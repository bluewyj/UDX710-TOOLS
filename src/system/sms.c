/**
 * @file sms.c
 * @brief 短信管理模块实现 - D-Bus监控、发送、Webhook转发
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <gio/gio.h>
#include "sms.h"
#include "database.h"
#include "exec_utils.h"

/* 短信模块专用互斥锁 */
static pthread_mutex_t g_sms_mutex = PTHREAD_MUTEX_INITIALIZER;
static GDBusConnection *g_sms_dbus_conn = NULL;
static guint g_signal_subscription_id = 0;
static guint g_name_watch_id = 0;
static int g_sms_initialized = 0;
static int g_ofono_available = 0;

/* Webhook配置 */
static WebhookConfig g_webhook_config = {0};

/* 最大短信存储数量 */
#define DEFAULT_MAX_SMS_COUNT 50
#define DEFAULT_MAX_SENT_COUNT 10
/* 同 sender+content 去重窗口（秒） */
#define SMS_DEDUP_WINDOW_S 60
static int g_max_sms_count = DEFAULT_MAX_SMS_COUNT;
static int g_max_sent_count = DEFAULT_MAX_SENT_COUNT;

/* Webhook发送日志（内存存储，重启后清空） */
#define MAX_WEBHOOK_LOGS 30
static SmsWebhookLog g_webhook_logs[MAX_WEBHOOK_LOGS];
static int g_webhook_log_count = 0;
static int g_webhook_log_id = 0;
static pthread_mutex_t g_webhook_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 添加Webhook日志 */
static void add_webhook_log(const char *sender, const char *request, const char *response, int result);

/* 前向声明 */
static void on_incoming_message(GDBusConnection *conn, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data);
static int save_sms_to_db(const char *sender, const char *content, time_t timestamp);
static int save_sent_sms_to_db(const char *recipient, const char *content, time_t timestamp, const char *status);
static int sms_is_duplicate(const char *sender, const char *content, time_t now);
static void send_webhook_notification(const SmsMessage *msg);
static void load_sms_config(void);
static void subscribe_sms_signal(void);
static void unsubscribe_sms_signal(void);
static void on_ofono_appeared(GDBusConnection *conn, const gchar *name, const gchar *name_owner, gpointer user_data);
static void on_ofono_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data);
static void apply_sms_fix_on_init(void);

/* Hex解码函数 - 将hex字符串解码为原始字节 */
static void hex_decode(const char *hex, char *out, size_t out_size) {
    size_t i = 0, j = 0;
    size_t hex_len = strlen(hex);
    
    while (i < hex_len && j < out_size - 1) {
        unsigned int byte;
        if (sscanf(hex + i, "%2x", &byte) == 1) {
            out[j++] = (char)byte;
            i += 2;
        } else {
            break;
        }
    }
    out[j] = '\0';
}

/* 保存短信到数据库 */
static int save_sms_to_db(const char *sender, const char *content, time_t timestamp) {
    char sql[2048];
    char escaped_content[1024];
    
    /* 转义单引号 */
    size_t j = 0;
    for (size_t i = 0; content[i] && j < sizeof(escaped_content) - 2; i++) {
        if (content[i] == '\'') {
            escaped_content[j++] = '\'';
            escaped_content[j++] = '\'';
        } else {
            escaped_content[j++] = content[i];
        }
    }
    escaped_content[j] = '\0';
    
    snprintf(sql, sizeof(sql),
        "INSERT INTO sms (sender, content, timestamp, is_read) VALUES ('%s', '%s', %ld, 0);",
        sender, escaped_content, (long)timestamp);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    /* 清理超出限制的旧短信 */
    if (ret == 0) {
        printf("[SMS] 短信保存成功，当前最大限制: %d\n", g_max_sms_count);
        snprintf(sql, sizeof(sql),
            "DELETE FROM sms WHERE id NOT IN (SELECT id FROM sms ORDER BY id DESC LIMIT %d);",
            g_max_sms_count);
        pthread_mutex_lock(&g_sms_mutex);
        db_execute(sql);
        pthread_mutex_unlock(&g_sms_mutex);
    } else {
        printf("[SMS] 短信保存失败!\n");
    }
    
    return ret;
}

/**
 * 60 秒窗口内同 sender+content 去重。
 * @return 1 重复；0 非重复或查询失败（fail-open，允许入库）
 */
static int sms_is_duplicate(const char *sender, const char *content, time_t now) {
    char sql[2048];
    char escaped_sender[128];
    char escaped_content[1024];
    time_t cutoff;

    if (!sender || !content) {
        return 0;
    }

    db_escape_string(sender, escaped_sender, sizeof(escaped_sender));
    db_escape_string(content, escaped_content, sizeof(escaped_content));

    cutoff = now - (time_t)SMS_DEDUP_WINDOW_S;
    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM sms WHERE sender='%s' AND content='%s' AND timestamp >= %ld LIMIT 1;",
        escaped_sender, escaped_content, (long)cutoff);

    /* default_val=0：查询失败/无行 → 非重复，失败开放 */
    return db_query_int(sql, 0) == 1 ? 1 : 0;
}

/* 订阅短信信号 */
static void subscribe_sms_signal(void) {
    if (!g_sms_dbus_conn) {
        printf("[SMS] D-Bus未连接，无法订阅信号\n");
        return;
    }
    
    /* 先取消旧的订阅 */
    unsubscribe_sms_signal();
    
    /* 添加D-Bus match规则 - 与Go版本保持一致 */
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        g_sms_dbus_conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "AddMatch",
        g_variant_new("(s)", "type='signal',interface='org.ofono.MessageManager',member='IncomingMessage'"),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &error
    );
    
    if (error) {
        printf("[SMS] 添加D-Bus match规则失败: %s\n", error->message);
        g_error_free(error);
    } else {
        printf("[SMS] D-Bus match规则添加成功\n");
        if (result) g_variant_unref(result);
    }
    
    /* 订阅短信信号 */
    g_signal_subscription_id = g_dbus_connection_signal_subscribe(
        g_sms_dbus_conn,
        "org.ofono",
        "org.ofono.MessageManager",
        "IncomingMessage",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_incoming_message,
        NULL, NULL
    );
    
    printf("[SMS] 短信信号订阅ID: %u\n", g_signal_subscription_id);
}

/* 取消短信信号订阅 */
static void unsubscribe_sms_signal(void) {
    if (g_signal_subscription_id > 0 && g_sms_dbus_conn) {
        g_dbus_connection_signal_unsubscribe(g_sms_dbus_conn, g_signal_subscription_id);
        printf("[SMS] 已取消信号订阅ID: %u\n", g_signal_subscription_id);
    }
    g_signal_subscription_id = 0;
}

/* oFono服务出现回调 */
static void on_ofono_appeared(GDBusConnection *conn, const gchar *name, 
    const gchar *name_owner, gpointer user_data) {
    (void)conn; (void)user_data;
    
    printf("[SMS] oFono服务已启动: %s (owner: %s)\n", name, name_owner);
    g_ofono_available = 1;
    
    /* 重新订阅短信信号 */
    subscribe_sms_signal();
}

/* oFono服务消失回调 */
static void on_ofono_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    (void)conn; (void)user_data;
    
    printf("[SMS] oFono服务已停止: %s\n", name);
    g_ofono_available = 0;
    
    /* 取消信号订阅 */
    unsubscribe_sms_signal();
}

/* D-Bus连接关闭回调 */
static void on_dbus_connection_closed(GDBusConnection *conn, gboolean remote_peer_vanished,
    GError *error, gpointer user_data) {
    (void)conn; (void)user_data;
    
    printf("[SMS] D-Bus连接已关闭! remote_peer_vanished=%d, error=%s\n", 
           remote_peer_vanished, error ? error->message : "无");
    
    /* 清理状态 */
    g_signal_subscription_id = 0;
    g_name_watch_id = 0;
    g_ofono_available = 0;
    g_sms_dbus_conn = NULL;
}

/* D-Bus信号处理 - 接收新短信 */
static void on_incoming_message(GDBusConnection *conn, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data) {
    
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name; (void)user_data;
    
    printf("[SMS] 收到新短信信号! path=%s\n", object_path);
    
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv})"))) {
        printf("[SMS] 短信信号参数类型不匹配\n");
        return;
    }
    
    const gchar *content = NULL;
    GVariant *props = NULL;
    g_variant_get(parameters, "(&s@a{sv})", &content, &props);
    
    if (!content || !props) {
        printf("[SMS] 解析短信内容失败\n");
        if (props) g_variant_unref(props);
        return;
    }
    
    /* 提取发送者 */
    char sender[64] = "未知";
    GVariant *sender_var = g_variant_lookup_value(props, "Sender", G_VARIANT_TYPE_STRING);
    if (sender_var) {
        const gchar *s = g_variant_get_string(sender_var, NULL);
        if (s) strncpy(sender, s, sizeof(sender) - 1);
        g_variant_unref(sender_var);
    }
    
    printf("[SMS] 新短信 - 发件人: %s, 内容: %s\n", sender, content);
    
    time_t now = time(NULL);
    if (sms_is_duplicate(sender, content, now)) {
        printf("[SMS] 60秒内重复短信，跳过入库与Webhook: sender=%s\n", sender);
        g_variant_unref(props);
        return;
    }

    /* 保存到数据库 */
    if (save_sms_to_db(sender, content, now) == 0) {
        printf("[SMS] 短信已保存到数据库\n");
        
        /* 发送Webhook通知 */
        if (g_webhook_config.enabled && strlen(g_webhook_config.url) > 0) {
            SmsMessage msg = {0};
            strncpy(msg.sender, sender, sizeof(msg.sender) - 1);
            strncpy(msg.content, content, sizeof(msg.content) - 1);
            msg.timestamp = now;
            send_webhook_notification(&msg);
        }
    }
    g_variant_unref(props);
}

/* 发送Webhook通知 */
static void send_webhook_notification_ext(const SmsMessage *msg, int force) {
    if (!force && (!g_webhook_config.enabled || strlen(g_webhook_config.url) == 0)) {
        return;
    }
    
    if (strlen(g_webhook_config.url) == 0) {
        printf("[SMS] Webhook URL未配置\n");
        return;
    }
    
    printf("[SMS] 发送Webhook通知到: %s (force=%d)\n", g_webhook_config.url, force);
    
    /* 替换变量 */
    char body[4096];
    strncpy(body, g_webhook_config.body, sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    
    /* 简单的变量替换 */
    char *p;
    char temp[4096];
    
    /* 替换 #{sender} */
    while ((p = strstr(body, "#{sender}")) != NULL) {
        *p = '\0';
        snprintf(temp, sizeof(temp), "%s%s%s", body, msg->sender, p + 9);
        strncpy(body, temp, sizeof(body) - 1);
    }
    
    /* 替换 #{content} */
    while ((p = strstr(body, "#{content}")) != NULL) {
        *p = '\0';
        snprintf(temp, sizeof(temp), "%s%s%s", body, msg->content, p + 10);
        strncpy(body, temp, sizeof(body) - 1);
    }
    
    /* 替换 #{time} */
    char time_str[32];
    struct tm *tm_info = localtime(&msg->timestamp);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    while ((p = strstr(body, "#{time}")) != NULL) {
        *p = '\0';
        snprintf(temp, sizeof(temp), "%s%s%s", body, time_str, p + 7);
        strncpy(body, temp, sizeof(body) - 1);
    }
    
    /* 将body写入临时文件，避免shell转义问题 */
    const char *tmp_file = "/tmp/webhook_body.json";
    FILE *fp = fopen(tmp_file, "w");
    if (fp) {
        fputs(body, fp);
        fclose(fp);
    } else {
        printf("[SMS] 无法创建临时文件\n");
        add_webhook_log(msg->sender, body, "创建临时文件失败", 0);
        return;
    }
    
    /* 构建curl命令 */
    char cmd[8192];
    char headers_part[1024] = "";
    
    /* 解析自定义headers */
    if (strlen(g_webhook_config.headers) > 0) {
        char headers_copy[512];
        strncpy(headers_copy, g_webhook_config.headers, sizeof(headers_copy) - 1);
        headers_copy[sizeof(headers_copy) - 1] = '\0';
        
        char *line = strtok(headers_copy, "\n");
        while (line) {
            while (*line == ' ' || *line == '\r') line++;
            if (strlen(line) > 0 && strchr(line, ':')) {
                char header_arg[256];
                char *cr = strchr(line, '\r');
                if (cr) *cr = '\0';
                snprintf(header_arg, sizeof(header_arg), " -H '%s'", line);
                strncat(headers_part, header_arg, sizeof(headers_part) - strlen(headers_part) - 1);
            }
            line = strtok(NULL, "\n");
        }
    }
    
    /* 同步执行curl获取响应 */
    if (strstr(headers_part, "Content-Type") == NULL) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 10 -X POST '%s' -H 'Content-Type: application/json'%s -d @%s 2>&1",
            g_webhook_config.url, headers_part, tmp_file);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 10 -X POST '%s'%s -d @%s 2>&1",
            g_webhook_config.url, headers_part, tmp_file);
    }
    
    printf("[SMS] 执行: %s\n", cmd);
    
    /* 使用popen捕获响应 */
    char response[1024] = "";
    FILE *pipe = popen(cmd, "r");
    if (pipe) {
        size_t total = 0;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe) && total < sizeof(response) - 1) {
            size_t len = strlen(buf);
            if (total + len < sizeof(response)) {
                strcat(response, buf);
                total += len;
            }
        }
        pclose(pipe);
    } else {
        strncpy(response, "执行curl失败", sizeof(response) - 1);
    }
    
    /* 删除临时文件 */
    unlink(tmp_file);
    
    /* 判断是否成功 */
    int result = (strlen(response) > 0 && strstr(response, "curl:") == NULL) ? 1 : 0;
    
    printf("[SMS] Webhook响应: %s\n", response);
    
    /* 记录日志 */
    add_webhook_log(msg->sender, body, response, result);
}

/* 发送Webhook通知 */
static void send_webhook_notification(const SmsMessage *msg) {
    send_webhook_notification_ext(msg, 0);
}

/* 初始化短信模块 */
int sms_init(const char *db_path) {
    GError *error = NULL;
    const char *tz;
    
    if (g_sms_initialized) {
        return 0;
    }

    /* 未设置 TZ 时默认 Asia/Shanghai，供 webhook #{time}/localtime 使用 */
    tz = getenv("TZ");
    if (tz == NULL || tz[0] == '\0') {
        setenv("TZ", "Asia/Shanghai", 0);
        tzset();
        printf("[SMS] TZ 未设置，默认 Asia/Shanghai\n");
    }
    
    printf("[SMS] 初始化短信模块\n");
    
    /* 初始化数据库模块 */
    if (db_init(db_path) != 0) {
        printf("[SMS] 数据库初始化失败\n");
        return -1;
    }
    
    printf("[SMS] 数据库路径: %s\n", db_get_path());
    
    /* 加载配置 */
    load_sms_config();
    sms_get_webhook_config(&g_webhook_config);
    
    /* 连接D-Bus */
    g_sms_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!g_sms_dbus_conn) {
        printf("[SMS] D-Bus连接失败: %s\n", error ? error->message : "未知错误");
        if (error) g_error_free(error);
        return -1;
    }
    
    /* 监听D-Bus连接关闭事件 */
    g_signal_connect(g_sms_dbus_conn, "closed", G_CALLBACK(on_dbus_connection_closed), NULL);
    
    /* 监控oFono服务状态 - 当服务重启时自动重新订阅 */
    g_name_watch_id = g_bus_watch_name_on_connection(
        g_sms_dbus_conn,
        "org.ofono",
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_ofono_appeared,
        on_ofono_vanished,
        NULL, NULL
    );
    
    printf("[SMS] oFono服务监控ID: %u\n", g_name_watch_id);
    
    /* 应用短信修复设置 - 在D-Bus连接后执行 */
    apply_sms_fix_on_init();
    
    /* 立即尝试订阅信号（如果oFono已经运行） */
    subscribe_sms_signal();
    g_ofono_available = 1;  /* 假设oFono可用，后续会通过监控更新 */
    
    printf("[SMS] 短信模块初始化成功\n");
    g_sms_initialized = 1;
    return 0;
}

/* 关闭短信模块 */
void sms_deinit(void) {
    if (!g_sms_initialized) return;
    
    /* 取消信号订阅 */
    unsubscribe_sms_signal();
    
    /* 取消oFono服务监控 */
    if (g_name_watch_id > 0) {
        g_bus_unwatch_name(g_name_watch_id);
        g_name_watch_id = 0;
    }
    
    if (g_sms_dbus_conn) {
        g_object_unref(g_sms_dbus_conn);
        g_sms_dbus_conn = NULL;
    }
    
    g_ofono_available = 0;
    g_sms_initialized = 0;
    printf("[SMS] 短信模块已关闭\n");
}


/* 发送短信 */
int sms_send(const char *recipient, const char *content, char *result_path, size_t path_size) {
    GError *error = NULL;
    GVariant *result = NULL;
    
    if (!recipient || !content || strlen(recipient) == 0 || strlen(content) == 0) {
        printf("发送短信参数无效\n");
        return -1;
    }
    
    if (!g_sms_dbus_conn || !g_ofono_available) {
        printf("[SMS] D-Bus未连接或oFono服务不可用\n");
        return -1;
    }
    
    printf("[SMS] 发送短信到 %s: %s\n", recipient, content);
    
    /* 调用 org.ofono.MessageManager.SendMessage */
    result = g_dbus_connection_call_sync(
        g_sms_dbus_conn,
        "org.ofono",
        "/ril_0",
        "org.ofono.MessageManager",
        "SendMessage",
        g_variant_new("(ss)", recipient, content),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        15000,  /* 15秒超时 */
        NULL,
        &error
    );
    
    if (!result) {
        printf("[SMS] 发送短信失败: %s\n", error ? error->message : "未知错误");
        if (error) g_error_free(error);
        return -1;
    }
    
    /* 获取返回的消息路径 */
    const gchar *path = NULL;
    g_variant_get(result, "(&o)", &path);
    
    if (result_path && path_size > 0 && path) {
        strncpy(result_path, path, path_size - 1);
        result_path[path_size - 1] = '\0';
    }
    
    printf("[SMS] 短信发送成功，路径: %s\n", path ? path : "N/A");
    g_variant_unref(result);
    
    /* 保存发送记录到数据库 */
    save_sent_sms_to_db(recipient, content, time(NULL), "sent");
    
    return 0;
}

/* 获取短信列表 - 使用hex编码避免特殊字符问题，兼容无JSON扩展的SQLite */
int sms_get_list(SmsMessage *messages, int max_count) {
    char sql[512];
    char *output = NULL;
    
    if (!messages || max_count <= 0) return -1;
    
    /* 分配大缓冲区 */
    output = (char *)malloc(256 * 1024);
    if (!output) return -1;
    
    /* 使用hex编码content字段，用|分隔，每行一条记录 */
    snprintf(sql, sizeof(sql),
        "SELECT id || '|' || sender || '|' || hex(content) || '|' || timestamp || '|' || is_read FROM sms ORDER BY id DESC LIMIT %d;",
        max_count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_string(sql, output, 256 * 1024);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        printf("[SMS] 获取短信列表失败或为空\n");
        free(output);
        return 0;
    }
    
    /* 解析输出 - 格式: id|sender|hex_content|timestamp|is_read\n */
    int count = 0;
    char *line = output;
    char *next_line;
    
    while (line && *line && count < max_count) {
        /* 找到行尾 */
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        /* 跳过空行 */
        if (strlen(line) == 0) {
            line = next_line;
            continue;
        }
        
        /* 解析字段: id|sender|hex_content|timestamp|is_read */
        char *fields[5] = {NULL};
        int field_count = 0;
        char *p = line;
        char *field_start = p;
        
        while (*p && field_count < 5) {
            if (*p == '|') {
                *p = '\0';
                fields[field_count++] = field_start;
                field_start = p + 1;
            }
            p++;
        }
        if (field_count < 5 && field_start) {
            fields[field_count++] = field_start;
        }
        
        if (field_count >= 5) {
            messages[count].id = atoi(fields[0]);
            strncpy(messages[count].sender, fields[1], sizeof(messages[count].sender) - 1);
            messages[count].sender[sizeof(messages[count].sender) - 1] = '\0';
            
            /* hex解码content */
            hex_decode(fields[2], messages[count].content, sizeof(messages[count].content));
            
            messages[count].timestamp = (time_t)atol(fields[3]);
            messages[count].is_read = atoi(fields[4]);
            count++;
        }
        
        line = next_line;
    }
    
    free(output);
    printf("[SMS] 获取到 %d 条短信\n", count);
    return count;
}

/* 获取短信总数 */
int sms_get_count(void) {
    const char *sql = "SELECT COUNT(*) FROM sms;";
    return db_query_int(sql, -1);
}

/* 删除短信 */
int sms_delete(int id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM sms WHERE id = %d;", id);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    return ret;
}

/* 清空所有短信 */
int sms_clear_all(void) {
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute("DELETE FROM sms;");
    pthread_mutex_unlock(&g_sms_mutex);
    return ret;
}

/* 获取Webhook配置 */
int sms_get_webhook_config(WebhookConfig *config) {
    char output[4096];
    
    if (!config) return -1;
    
    memset(config, 0, sizeof(WebhookConfig));
    
    const char *sql = "SELECT enabled, platform, url, body, headers FROM webhook_config WHERE id = 1;";
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_rows(sql, "|", output, sizeof(output));
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        /* 使用默认配置 */
        config->enabled = 0;
        strcpy(config->platform, "pushplus");
        return 0;
    }
    
    /* 解析输出 */
    char *fields[5] = {NULL};
    int field_count = 0;
    char *p = output;
    char *start = p;
    
    while (*p && field_count < 5) {
        if (*p == '|') {
            *p = '\0';
            fields[field_count++] = start;
            start = p + 1;
        }
        p++;
    }
    if (field_count < 5 && start) {
        fields[field_count++] = start;
    }
    
    if (field_count >= 5) {
        config->enabled = atoi(fields[0]);
        strncpy(config->platform, fields[1], sizeof(config->platform) - 1);
        strncpy(config->url, fields[2], sizeof(config->url) - 1);
        strncpy(config->body, fields[3], sizeof(config->body) - 1);
        strncpy(config->headers, fields[4], sizeof(config->headers) - 1);
        
        /* 去除末尾换行符 */
        char *nl = strchr(config->headers, '\n');
        if (nl) *nl = '\0';
        
        /* 反转义特殊字符 */
        db_unescape_string(config->url);
        db_unescape_string(config->body);
        db_unescape_string(config->headers);
    }
    
    return 0;
}

/* 保存Webhook配置 */
int sms_save_webhook_config(const WebhookConfig *config) {
    char sql[8192];
    char escaped_body[4096];
    char escaped_headers[1024];
    char escaped_url[1024];
    
    if (!config) return -1;
    
    /* 转义特殊字符 */
    db_escape_string(config->body, escaped_body, sizeof(escaped_body));
    db_escape_string(config->headers, escaped_headers, sizeof(escaped_headers));
    db_escape_string(config->url, escaped_url, sizeof(escaped_url));
    
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO webhook_config (id, enabled, platform, url, body, headers) "
        "VALUES (1, %d, '%s', '%s', '%s', '%s');",
        config->enabled, config->platform, escaped_url, escaped_body, escaped_headers);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0) {
        /* 更新内存中的配置 */
        memcpy(&g_webhook_config, config, sizeof(WebhookConfig));
        printf("[SMS] Webhook配置保存成功\n");
    } else {
        printf("[SMS] Webhook配置保存失败\n");
    }
    
    return ret;
}

/* 测试Webhook */
int sms_test_webhook(void) {
    SmsMessage test_msg = {
        .id = 0,
        .sender = "+8613800138000",
        .content = "这是一条测试短信",
        .timestamp = time(NULL),
        .is_read = 0
    };
    
    if (strlen(g_webhook_config.url) == 0) {
        printf("[SMS] Webhook URL为空\n");
        return -1;
    }
    
    /* 测试时强制发送，无需检查enabled状态 */
    send_webhook_notification_ext(&test_msg, 1);
    return 0;
}

/* 检查短信模块状态 */
int sms_check_status(void) {
    printf("[SMS] 状态检查 - 初始化: %d, D-Bus连接: %p, oFono可用: %d, 信号订阅: %u\n",
           g_sms_initialized, (void*)g_sms_dbus_conn, g_ofono_available, g_signal_subscription_id);
    return g_sms_initialized && g_sms_dbus_conn && g_ofono_available && g_signal_subscription_id > 0;
}

/* 定期维护短信模块 - 增强版 */
void sms_maintenance(void) {
    static int check_count = 0;
    check_count++;
    
    /* 每10次检查输出一次状态（约10秒一次，假设1秒调用一次） */
    if (check_count % 10 == 0) {
        printf("[SMS] 维护检查 #%d - D-Bus: %p, oFono: %d, 订阅ID: %u\n",
               check_count, (void*)g_sms_dbus_conn, g_ofono_available, g_signal_subscription_id);
    }
    
    if (!g_sms_initialized) {
        return;
    }
    
    /* 检查D-Bus连接是否有效 */
    if (!g_sms_dbus_conn || g_dbus_connection_is_closed(g_sms_dbus_conn)) {
        printf("[SMS] D-Bus连接无效，尝试重新连接...\n");
        GError *error = NULL;
        g_sms_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (g_sms_dbus_conn) {
            printf("[SMS] D-Bus重新连接成功\n");
            g_signal_connect(g_sms_dbus_conn, "closed", G_CALLBACK(on_dbus_connection_closed), NULL);
            subscribe_sms_signal();
        } else {
            printf("[SMS] D-Bus重新连接失败: %s\n", error ? error->message : "未知错误");
            if (error) g_error_free(error);
        }
        return;
    }
    
    /* 检查信号订阅是否丢失 */
    if (g_signal_subscription_id == 0) {
        printf("[SMS] 检测到信号订阅丢失，重新订阅...\n");
        subscribe_sms_signal();
    }
}

/* 获取短信接收修复开关状态 */
int sms_get_fix_enabled(void) {
    const char *sql = "SELECT sms_fix_enabled FROM sms_config WHERE id = 1;";
    return db_query_int(sql, 0);  /* 默认关闭 */
}

/* 设置短信接收修复开关 */
int sms_set_fix_enabled(int enabled) {
    extern int execute_at(const char *command, char **result);
    char sql[256];
    char *at_result = NULL;
    
    /* 发送AT命令 */
    const char *at_cmd = enabled ? "AT+CNMI=3,2,0,1,0" : "AT+CNMI=3,1,0,1,0";
    printf("[SMS] 发送AT命令: %s\n", at_cmd);
    
    if (execute_at(at_cmd, &at_result) != 0) {
        printf("[SMS] AT命令执行失败\n");
        if (at_result) g_free(at_result);
        return -1;
    }
    
    printf("[SMS] AT命令执行成功: %s\n", at_result ? at_result : "OK");
    if (at_result) g_free(at_result);
    
    /* 保存到数据库 */
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO sms_config (id, max_count, max_sent_count, sms_fix_enabled) "
        "VALUES (1, %d, %d, %d);", 
        g_max_sms_count, g_max_sent_count, enabled ? 1 : 0);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    return ret;
}

/* 初始化时应用短信修复设置 */
static void apply_sms_fix_on_init(void) {
    extern int execute_at(const char *command, char **result);
    
    int enabled = sms_get_fix_enabled();
    printf("[SMS] 短信接收修复开关状态: %s\n", enabled ? "开启" : "关闭");
    
    if (enabled) {
        char *at_result = NULL;
        printf("[SMS] 开机应用短信修复AT命令: AT+CNMI=3,2,0,1,0\n");
        
        if (execute_at("AT+CNMI=3,2,0,1,0", &at_result) == 0) {
            printf("[SMS] AT命令执行成功\n");
        } else {
            printf("[SMS] AT命令执行失败\n");
        }
        
        if (at_result) g_free(at_result);
    }
}

/* 保存发送记录到数据库 */
static int save_sent_sms_to_db(const char *recipient, const char *content, time_t timestamp, const char *status) {
    char sql[2048];
    char escaped_content[1024];
    
    size_t j = 0;
    for (size_t i = 0; content[i] && j < sizeof(escaped_content) - 2; i++) {
        if (content[i] == '\'') {
            escaped_content[j++] = '\'';
            escaped_content[j++] = '\'';
        } else {
            escaped_content[j++] = content[i];
        }
    }
    escaped_content[j] = '\0';
    
    snprintf(sql, sizeof(sql),
        "INSERT INTO sent_sms (recipient, content, timestamp, status) VALUES ('%s', '%s', %ld, '%s');",
        recipient, escaped_content, (long)timestamp, status);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    /* 清理超出限制的旧发送记录 */
    if (ret == 0) {
        snprintf(sql, sizeof(sql),
            "DELETE FROM sent_sms WHERE id NOT IN (SELECT id FROM sent_sms ORDER BY id DESC LIMIT %d);",
            g_max_sent_count);
        pthread_mutex_lock(&g_sms_mutex);
        db_execute(sql);
        pthread_mutex_unlock(&g_sms_mutex);
    }
    
    return ret;
}

/* 删除发送记录 */
int sms_delete_sent(int id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM sent_sms WHERE id = %d;", id);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    return ret;
}

/* 获取发送记录列表 - 使用hex编码避免特殊字符问题，兼容无JSON扩展的SQLite */
int sms_get_sent_list(SentSmsMessage *messages, int max_count) {
    char sql[512];
    char *output = NULL;
    
    if (!messages || max_count <= 0) return -1;
    
    /* 分配大缓冲区 */
    output = (char *)malloc(256 * 1024);
    if (!output) return -1;
    
    /* 使用hex编码content字段，用|分隔，每行一条记录 */
    snprintf(sql, sizeof(sql),
        "SELECT id || '|' || recipient || '|' || hex(content) || '|' || timestamp || '|' || status FROM sent_sms ORDER BY id DESC LIMIT %d;",
        max_count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_string(sql, output, 256 * 1024);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        printf("[SMS] 获取发送记录列表失败或为空\n");
        free(output);
        return 0;
    }
    
    /* 解析输出 - 格式: id|recipient|hex_content|timestamp|status\n */
    int count = 0;
    char *line = output;
    char *next_line;
    
    while (line && *line && count < max_count) {
        /* 找到行尾 */
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        /* 跳过空行 */
        if (strlen(line) == 0) {
            line = next_line;
            continue;
        }
        
        /* 解析字段: id|recipient|hex_content|timestamp|status */
        char *fields[5] = {NULL};
        int field_count = 0;
        char *p = line;
        char *field_start = p;
        
        while (*p && field_count < 5) {
            if (*p == '|') {
                *p = '\0';
                fields[field_count++] = field_start;
                field_start = p + 1;
            }
            p++;
        }
        if (field_count < 5 && field_start) {
            fields[field_count++] = field_start;
        }
        
        if (field_count >= 5) {
            messages[count].id = atoi(fields[0]);
            strncpy(messages[count].recipient, fields[1], sizeof(messages[count].recipient) - 1);
            messages[count].recipient[sizeof(messages[count].recipient) - 1] = '\0';
            
            /* hex解码content */
            hex_decode(fields[2], messages[count].content, sizeof(messages[count].content));
            
            messages[count].timestamp = (time_t)atol(fields[3]);
            strncpy(messages[count].status, fields[4], sizeof(messages[count].status) - 1);
            messages[count].status[sizeof(messages[count].status) - 1] = '\0';
            count++;
        }
        
        line = next_line;
    }
    
    free(output);
    printf("[SMS] 获取到 %d 条发送记录\n", count);
    return count;
}

/* 获取最大存储数量 */
int sms_get_max_count(void) {
    return g_max_sms_count;
}

/* 获取发送记录最大存储数量 */
int sms_get_max_sent_count(void) {
    return g_max_sent_count;
}

/* 加载配置 */
static void load_sms_config(void) {
    char output[128];
    const char *sql = "SELECT max_count || '|' || max_sent_count FROM sms_config WHERE id = 1;";
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_string(sql, output, sizeof(output));
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0 && strlen(output) > 0) {
        char *sep = strchr(output, '|');
        if (sep) {
            *sep = '\0';
            int mc = atoi(output);
            int msc = atoi(sep + 1);
            if (mc > 0) g_max_sms_count = mc;
            if (msc > 0) g_max_sent_count = msc;
        }
    }
    
    printf("[SMS] 配置加载完成: 收件箱最大=%d, 发件箱最大=%d\n", g_max_sms_count, g_max_sent_count);
}

/* 设置最大存储数量 */
int sms_set_max_count(int count) {
    if (count < 10 || count > 150) {
        printf("最大存储数量必须在10-150之间\n");
        return -1;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO sms_config (id, max_count, max_sent_count) VALUES (1, %d, %d);", 
        count, g_max_sent_count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0) {
        g_max_sms_count = count;
    }
    
    return ret;
}

/* 设置发送记录最大存储数量 */
int sms_set_max_sent_count(int count) {
    if (count < 1 || count > 50) {
        printf("发送记录最大存储数量必须在1-50之间\n");
        return -1;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO sms_config (id, max_count, max_sent_count) VALUES (1, %d, %d);", 
        g_max_sms_count, count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0) {
        g_max_sent_count = count;
    }
    
    return ret;
}

/* 添加Webhook发送日志（内存存储） */
static void add_webhook_log(const char *sender, const char *request, const char *response, int result) {
    pthread_mutex_lock(&g_webhook_log_mutex);
    
    /* 循环缓冲区 */
    int idx = g_webhook_log_count % MAX_WEBHOOK_LOGS;
    
    g_webhook_logs[idx].id = ++g_webhook_log_id;
    strncpy(g_webhook_logs[idx].sender, sender ? sender : "", sizeof(g_webhook_logs[idx].sender) - 1);
    strncpy(g_webhook_logs[idx].request, request ? request : "", sizeof(g_webhook_logs[idx].request) - 1);
    strncpy(g_webhook_logs[idx].response, response ? response : "", sizeof(g_webhook_logs[idx].response) - 1);
    g_webhook_logs[idx].result = result;
    g_webhook_logs[idx].created_at = time(NULL);
    
    if (g_webhook_log_count < MAX_WEBHOOK_LOGS) {
        g_webhook_log_count++;
    }
    
    pthread_mutex_unlock(&g_webhook_log_mutex);
    
    printf("[SMS] Webhook日志已添加, ID=%d, 结果=%d\n", g_webhook_log_id, result);
}

/* JSON字符串转义工具 */
static void json_escape(char *dest, const char *src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return;
    
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = '"'; } else break;
        } else if (c == '\\') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = '\\'; } else break;
        } else if (c == '\b') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'b'; } else break;
        } else if (c == '\f') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'f'; } else break;
        } else if (c == '\n') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'n'; } else break;
        } else if (c == '\r') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'r'; } else break;
        } else if (c == '\t') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 't'; } else break;
        } else if (c < 32) {
            if (j + 6 < dest_size) {
                j += snprintf(dest + j, 7, "\\u%04x", c);
            } else break;
        } else {
            dest[j++] = c;
        }
        i++;
    }
    dest[j] = '\0';
}

/* 获取Webhook发送日志 */
int sms_get_webhook_logs(char *json_output, size_t size, int max_count) {
    if (!json_output || size == 0) {
        return -1;
    }
    
    if (max_count <= 0 || max_count > MAX_WEBHOOK_LOGS) {
        max_count = 20;
    }
    
    pthread_mutex_lock(&g_webhook_log_mutex);
    
    int offset = 0;
    offset += snprintf(json_output + offset, size - offset, "[");
    
    int count = (g_webhook_log_count < max_count) ? g_webhook_log_count : max_count;
    int first = 1;
    
    /* 从最新的日志开始输出 */
    for (int i = 0; i < count && offset < (int)size - 10; i++) {
        int idx;
        if (g_webhook_log_count <= MAX_WEBHOOK_LOGS) {
            idx = g_webhook_log_count - 1 - i;
        } else {
            idx = (g_webhook_log_count - 1 - i) % MAX_WEBHOOK_LOGS;
        }
        
        if (idx < 0) break;
        
        if (!first) offset += snprintf(json_output + offset, size - offset, ",");
        first = 0;
        
        char escaped_req[3072] = "";
        char escaped_resp[3072] = "";
        
        json_escape(escaped_req, g_webhook_logs[idx].request, sizeof(escaped_req));
        json_escape(escaped_resp, g_webhook_logs[idx].response, sizeof(escaped_resp));
        
        offset += snprintf(json_output + offset, size - offset,
            "{\"id\":%d,\"sender\":\"%s\",\"request\":\"%s\",\"response\":\"%s\",\"result\":%d,\"created_at\":%ld}",
            g_webhook_logs[idx].id,
            g_webhook_logs[idx].sender,
            escaped_req,
            escaped_resp,
            g_webhook_logs[idx].result,
            (long)g_webhook_logs[idx].created_at);
            
        if (offset >= (int)size - 10) break;
    }
    
    offset += snprintf(json_output + offset, size - offset, "]");
    
    pthread_mutex_unlock(&g_webhook_log_mutex);
    
    return 0;
}
