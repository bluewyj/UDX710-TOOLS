/**
 * @file apn.c
 * @brief APN配置管理模块实现 - 模板管理、自动/手动模式切换、自启动功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include "apn.h"
#include "database.h"
#include "exec_utils.h"
#include "ofono.h"

#define APN_BOOT_STATE_PATH "/mnt/data/apn-boot-apply.state"
#define APN_BOOT_LOG_PATH "/mnt/data/logs/apn-boot-apply.log"
#define APN_PERSIST_BASE1 "/mnt/data/ofono"
#define APN_PERSIST_BASE2 "/mnt/userdata/data/ofono"

/* APN模块专用互斥锁 */
static pthread_mutex_t g_apn_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_apn_initialized = 0;

/* 当前配置缓存 */
static ApnConfig g_current_config = {0};

typedef struct {
    char apn[128];
    char auth[32];
    char user[128];
    char pass[128];
    char proto[32];
} ApnBootTemplate;

/* 前向声明 */
static int create_apn_tables(void);
static int load_apn_config(void);
static int apply_apn_to_ofono(const ApnTemplate *tpl);
static void apn_boot_log_line(const char *msg);
static int apn_boot_set_state(const char *state);
static int apn_boot_get_imsi(char *buf, size_t size);
static int apn_boot_modem_online(void);
static int apn_boot_wait_modem_online(int max_secs);
static int apn_boot_write_persist(const ApnBootTemplate *tpl);
static int apn_boot_load_template(ApnBootTemplate *tpl);
static int apn_boot_is_active(void);
static int apn_boot_activate_pdp_fallback(void);
static void apn_boot_touch_tether_refresh(void);
static int apn_boot_apply_full(const ApnBootTemplate *tpl);
static void *apn_boot_persist_only_thread(void *arg);

/**
 * 创建APN数据库表
 */
static int create_apn_tables(void) {
    const char *sql = 
        "CREATE TABLE IF NOT EXISTS apn_templates ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "apn TEXT NOT NULL,"
        "protocol TEXT DEFAULT 'dual',"
        "username TEXT,"
        "password TEXT,"
        "auth_method TEXT DEFAULT 'chap',"
        "created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS apn_config ("
        "id INTEGER PRIMARY KEY DEFAULT 1,"
        "mode INTEGER DEFAULT 0,"
        "template_id INTEGER,"
        "auto_start INTEGER DEFAULT 0"
        ");";
    
    return db_execute(sql);
}

/**
 * 加载APN配置
 */
static int load_apn_config(void) {
    char output[256];
    const char *sql = "SELECT mode || '|' || COALESCE(template_id, 0) || '|' || auto_start FROM apn_config WHERE id = 1;";
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_query_string(sql, output, sizeof(output));
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret == 0 && strlen(output) > 0) {
        char *p1 = strchr(output, '|');
        if (p1) {
            *p1 = '\0';
            char *p2 = strchr(p1 + 1, '|');
            if (p2) {
                *p2 = '\0';
                g_current_config.mode = atoi(output);
                g_current_config.template_id = atoi(p1 + 1);
                g_current_config.auto_start = atoi(p2 + 1);
            }
        }
    } else {
        /* 默认配置：自动模式 */
        g_current_config.mode = APN_MODE_AUTO;
        g_current_config.template_id = 0;
        g_current_config.auto_start = 0;
    }
    
    printf("[APN] 配置加载完成: 模式=%d, 模板ID=%d, 自启动=%d\n", 
           g_current_config.mode, g_current_config.template_id, g_current_config.auto_start);
    return 0;
}

/**
 * 应用APN模板到oFono
 */
static int apply_apn_to_ofono(const ApnTemplate *tpl) {
    if (!tpl) {
        printf("[APN] 模板参数无效\n");
        return -1;
    }
    
    printf("[APN] 开始应用模板: %s (APN: %s)\n", tpl->name, tpl->apn);
    
    /* 检查oFono是否已初始化，如果未初始化则尝试初始化 */
    if (!ofono_is_initialized()) {
        printf("[APN] oFono未初始化，尝试初始化...\n");
        if (!ofono_init()) {
            printf("[APN] oFono初始化失败\n");
            return -1;
        }
        printf("[APN] oFono初始化成功\n");
    }
    
    /* 获取所有APN Context */
    ApnContext contexts[MAX_APN_CONTEXTS];
    int count = ofono_get_all_apn_contexts(contexts, MAX_APN_CONTEXTS);
    
    if (count <= 0) {
        printf("[APN] 未找到可用的APN Context (count=%d)\n", count);
        return -1;
    }
    
    printf("[APN] 找到 %d 个APN Context\n", count);
    
    /* 使用第一个internet类型的context */
    const char *target_path = NULL;
    for (int i = 0; i < count; i++) {
        printf("[APN] Context[%d]: path=%s, type=%s, apn=%s\n", 
               i, contexts[i].path, contexts[i].context_type, contexts[i].apn);
        if (strcmp(contexts[i].context_type, "internet") == 0) {
            target_path = contexts[i].path;
            break;
        }
    }
    
    if (!target_path) {
        /* 如果没有internet类型，使用第一个 */
        target_path = contexts[0].path;
        printf("[APN] 未找到internet类型Context，使用第一个: %s\n", target_path);
    }
    
    printf("[APN] 应用模板到: %s\n", target_path);
    
    /* 批量设置APN属性 */
    int ret = ofono_set_apn_properties(
        target_path,
        tpl->apn,
        tpl->protocol,
        strlen(tpl->username) > 0 ? tpl->username : NULL,
        strlen(tpl->password) > 0 ? tpl->password : NULL,
        tpl->auth_method
    );
    
    if (ret != 0) {
        printf("[APN] 应用APN配置失败 (ret=%d)\n", ret);
        return -1;
    }
    
    printf("[APN] APN配置应用成功\n");
    return 0;
}

/**
 * 初始化APN模块
 */
int apn_init(const char *db_path) {
    if (g_apn_initialized) {
        return 0;
    }
    
    printf("[APN] 初始化APN模块\n");
    
    /* 初始化数据库模块（如果还未初始化） */
    if (db_path && strlen(db_path) > 0) {
        db_init(db_path);
    }
    
    /* 创建数据库表 */
    if (create_apn_tables() != 0) {
        printf("[APN] 创建数据库表失败\n");
        return -1;
    }
    
    /* 加载配置 */
    load_apn_config();
    
    /* 处理自启动：走 full boot（persist → wait modem → 连接） */
    if (g_current_config.mode == APN_MODE_MANUAL &&
        g_current_config.auto_start == 1 &&
        g_current_config.template_id > 0) {
        printf("[APN] 检测到自启动配置，模板ID: %d，启动 apn_boot_apply(full)\n",
               g_current_config.template_id);
        apn_boot_apply("full");
    }
    
    g_apn_initialized = 1;
    printf("[APN] APN模块初始化完成\n");
    return 0;
}

/**
 * 获取APN配置
 */
int apn_get_config(ApnConfig *config) {
    if (!config) {
        return -1;
    }
    
    pthread_mutex_lock(&g_apn_mutex);
    memcpy(config, &g_current_config, sizeof(ApnConfig));
    pthread_mutex_unlock(&g_apn_mutex);
    
    return 0;
}

/**
 * 设置APN模式
 */
int apn_set_mode(int mode, int template_id, int auto_start) {
    char sql[256];
    
    /* 参数校验 */
    if (mode != APN_MODE_AUTO && mode != APN_MODE_MANUAL) {
        printf("[APN] 无效的模式: %d\n", mode);
        return -1;
    }
    
    if (mode == APN_MODE_MANUAL && template_id <= 0) {
        printf("[APN] 手动模式必须指定模板ID\n");
        return -1;
    }
    
    printf("[APN] 设置模式: %d, 模板ID: %d, 自启动: %d\n", mode, template_id, auto_start);
    
    /* 保存配置 */
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO apn_config (id, mode, template_id, auto_start) "
        "VALUES (1, %d, %d, %d);",
        mode, template_id, auto_start);
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret != 0) {
        printf("[APN] 保存配置失败\n");
        return -1;
    }
    
    /* 更新内存配置 */
    g_current_config.mode = mode;
    g_current_config.template_id = template_id;
    g_current_config.auto_start = auto_start;
    
    printf("[APN] 配置保存成功\n");
    return 0;
}

/**
 * 获取模板列表
 */
int apn_template_list(ApnTemplate *templates, int max_count) {
    char *output = NULL;
    
    if (!templates || max_count <= 0) {
        return -1;
    }
    
    /* 分配大缓冲区 */
    output = (char *)malloc(64 * 1024);
    if (!output) {
        return -1;
    }
    
    const char *sql = 
        "SELECT id || '|' || name || '|' || apn || '|' || protocol || '|' || "
        "COALESCE(username, '') || '|' || COALESCE(password, '') || '|' || "
        "auth_method || '|' || created_at FROM apn_templates ORDER BY id DESC;";
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_query_string(sql, output, 64 * 1024);
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        free(output);
        return 0;
    }
    
    /* 解析输出 */
    int count = 0;
    char *line = output;
    char *next_line;
    
    while (line && *line && count < max_count) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        if (strlen(line) == 0) {
            line = next_line;
            continue;
        }
        
        /* 解析字段 */
        char *fields[8] = {NULL};
        int field_count = 0;
        char *p = line;
        char *start = p;
        
        while (*p && field_count < 8) {
            if (*p == '|') {
                *p = '\0';
                fields[field_count++] = start;
                start = p + 1;
            }
            p++;
        }
        if (field_count < 8 && start) {
            fields[field_count++] = start;
        }
        
        if (field_count >= 8) {
            templates[count].id = atoi(fields[0]);
            strncpy(templates[count].name, fields[1], sizeof(templates[count].name) - 1);
            templates[count].name[sizeof(templates[count].name) - 1] = '\0';
            strncpy(templates[count].apn, fields[2], sizeof(templates[count].apn) - 1);
            templates[count].apn[sizeof(templates[count].apn) - 1] = '\0';
            strncpy(templates[count].protocol, fields[3], sizeof(templates[count].protocol) - 1);
            templates[count].protocol[sizeof(templates[count].protocol) - 1] = '\0';
            strncpy(templates[count].username, fields[4], sizeof(templates[count].username) - 1);
            templates[count].username[sizeof(templates[count].username) - 1] = '\0';
            strncpy(templates[count].password, fields[5], sizeof(templates[count].password) - 1);
            templates[count].password[sizeof(templates[count].password) - 1] = '\0';
            strncpy(templates[count].auth_method, fields[6], sizeof(templates[count].auth_method) - 1);
            templates[count].auth_method[sizeof(templates[count].auth_method) - 1] = '\0';
            templates[count].created_at = (time_t)atol(fields[7]);
            count++;
        }
        
        line = next_line;
    }
    
    free(output);
    printf("[APN] 获取到 %d 个模板\n", count);
    return count;
}

/**
 * 创建模板
 */
int apn_template_create(const char *name, const char *apn, const char *protocol,
                       const char *username, const char *password, const char *auth_method) {
    char sql[2048];
    char escaped_name[256];
    char escaped_apn[256];
    char escaped_username[256];
    char escaped_password[256];
    
    /* 参数校验 */
    if (!name || !apn || strlen(name) == 0 || strlen(apn) == 0) {
        printf("[APN] 模板名称和APN不能为空\n");
        return -1;
    }
    
    /* 转义特殊字符 */
    db_escape_string(name, escaped_name, sizeof(escaped_name));
    db_escape_string(apn, escaped_apn, sizeof(escaped_apn));
    db_escape_string(username ? username : "", escaped_username, sizeof(escaped_username));
    db_escape_string(password ? password : "", escaped_password, sizeof(escaped_password));
    
    time_t now = time(NULL);
    
    snprintf(sql, sizeof(sql),
        "INSERT INTO apn_templates (name, apn, protocol, username, password, auth_method, created_at) "
        "VALUES ('%s', '%s', '%s', '%s', '%s', '%s', %ld);",
        escaped_name, escaped_apn, protocol ? protocol : "dual", 
        escaped_username, escaped_password, auth_method ? auth_method : "chap", (long)now);
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret == 0) {
        printf("[APN] 模板创建成功\n");
    } else {
        printf("[APN] 模板创建失败\n");
    }
    
    return ret;
}

/**
 * 更新模板
 */
int apn_template_update(int id, const char *name, const char *apn, const char *protocol,
                       const char *username, const char *password, const char *auth_method) {
    char sql[2048];
    char escaped_name[256];
    char escaped_apn[256];
    char escaped_username[256];
    char escaped_password[256];
    
    if (id <= 0) {
        return -1;
    }
    
    /* 参数校验 */
    if (!name || !apn || strlen(name) == 0 || strlen(apn) == 0) {
        printf("[APN] 模板名称和APN不能为空\n");
        return -1;
    }
    
    /* 转义特殊字符 */
    db_escape_string(name, escaped_name, sizeof(escaped_name));
    db_escape_string(apn, escaped_apn, sizeof(escaped_apn));
    db_escape_string(username ? username : "", escaped_username, sizeof(escaped_username));
    db_escape_string(password ? password : "", escaped_password, sizeof(escaped_password));
    
    snprintf(sql, sizeof(sql),
        "UPDATE apn_templates SET name='%s', apn='%s', protocol='%s', "
        "username='%s', password='%s', auth_method='%s' WHERE id=%d;",
        escaped_name, escaped_apn, protocol ? protocol : "dual",
        escaped_username, escaped_password, auth_method ? auth_method : "chap", id);
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret == 0) {
        printf("[APN] 模板更新成功\n");
    } else {
        printf("[APN] 模板更新失败\n");
    }
    
    return ret;
}

/**
 * 删除模板
 */
int apn_template_delete(int id) {
    char sql[256];
    
    if (id <= 0) {
        return -1;
    }
    
    /* 检查是否正在使用 */
    if (g_current_config.mode == APN_MODE_MANUAL && g_current_config.template_id == id) {
        printf("[APN] 模板正在使用中，无法删除\n");
        return -1;
    }
    
    snprintf(sql, sizeof(sql), "DELETE FROM apn_templates WHERE id = %d;", id);
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret == 0) {
        printf("[APN] 模板删除成功\n");
    } else {
        printf("[APN] 模板删除失败\n");
    }
    
    return ret;
}

/**
 * 应用模板
 */
int apn_apply_template(int template_id) {
    char sql[256];
    char output[1024];
    ApnTemplate tpl;
    
    if (template_id <= 0) {
        printf("[APN] 无效的模板ID\n");
        return -1;
    }
    
    /* 查询模板 */
    snprintf(sql, sizeof(sql),
        "SELECT id, name, apn, protocol, username, password, auth_method, created_at "
        "FROM apn_templates WHERE id = %d;", template_id);
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_query_rows(sql, "|", output, sizeof(output));
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        printf("[APN] 模板不存在: %d\n", template_id);
        return -1;
    }
    
    /* 解析模板数据 */
    char *fields[8] = {NULL};
    int field_count = 0;
    char *p = output;
    char *start = p;
    
    while (*p && field_count < 8) {
        if (*p == '|') {
            *p = '\0';
            fields[field_count++] = start;
            start = p + 1;
        }
        p++;
    }
    if (field_count < 8 && start) {
        fields[field_count++] = start;
    }
    
    if (field_count < 8) {
        printf("[APN] 模板数据解析失败\n");
        return -1;
    }
    
    tpl.id = atoi(fields[0]);
    strncpy(tpl.name, fields[1], sizeof(tpl.name) - 1);
    tpl.name[sizeof(tpl.name) - 1] = '\0';
    strncpy(tpl.apn, fields[2], sizeof(tpl.apn) - 1);
    tpl.apn[sizeof(tpl.apn) - 1] = '\0';
    strncpy(tpl.protocol, fields[3], sizeof(tpl.protocol) - 1);
    tpl.protocol[sizeof(tpl.protocol) - 1] = '\0';
    strncpy(tpl.username, fields[4], sizeof(tpl.username) - 1);
    tpl.username[sizeof(tpl.username) - 1] = '\0';
    strncpy(tpl.password, fields[5], sizeof(tpl.password) - 1);
    tpl.password[sizeof(tpl.password) - 1] = '\0';
    strncpy(tpl.auth_method, fields[6], sizeof(tpl.auth_method) - 1);
    tpl.auth_method[sizeof(tpl.auth_method) - 1] = '\0';
    tpl.created_at = (time_t)atol(fields[7]);
    
    /* 应用到oFono */
    return apply_apn_to_ofono(&tpl);
}

/**
 * 清除所有APN配置（自动模式使用）
 */
int apn_clear_all(void) {
    printf("[APN] 清除所有APN配置\n");
    
    /* 重置数据库配置为自动模式 */
    const char *sql = "INSERT OR REPLACE INTO apn_config (id, mode, template_id, auto_start) "
                      "VALUES (1, 0, 0, 0);";
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret != 0) {
        printf("[APN] 重置配置失败\n");
        return -1;
    }
    
    /* 更新内存配置 */
    g_current_config.mode = APN_MODE_AUTO;
    g_current_config.template_id = 0;
    g_current_config.auto_start = 0;
    
    /* 清除oFono APN配置 */
    if (ofono_is_initialized()) {
        ApnContext contexts[MAX_APN_CONTEXTS];
        int count = ofono_get_all_apn_contexts(contexts, MAX_APN_CONTEXTS);
        
        for (int i = 0; i < count; i++) {
            if (strcmp(contexts[i].context_type, "internet") == 0) {
                /* 重置为空配置 */
                ofono_set_apn_properties(
                    contexts[i].path,
                    "",      /* 清空APN */
                    "dual",  /* 默认协议 */
                    NULL,    /* 清空用户名 */
                    NULL,    /* 清空密码 */
                    "chap"   /* 默认认证 */
                );
                printf("[APN] 已清除 %s 的APN配置\n", contexts[i].path);
            }
        }
    }
    
    printf("[APN] APN配置清除完成\n");
    return 0;
}

/**
 * 获取模板详情
 */
int apn_template_get(int id, ApnTemplate *tpl) {
    char sql[256];
    char output[1024];
    
    if (id <= 0 || !tpl) {
        return -1;
    }
    
    memset(tpl, 0, sizeof(ApnTemplate));
    
    snprintf(sql, sizeof(sql),
        "SELECT id, name, apn, protocol, username, password, auth_method, created_at "
        "FROM apn_templates WHERE id = %d;", id);
    
    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_query_rows(sql, "|", output, sizeof(output));
    pthread_mutex_unlock(&g_apn_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        return -1;
    }
    
    /* 解析模板数据 */
    char *fields[8] = {NULL};
    int field_count = 0;
    char *p = output;
    char *start = p;
    
    while (*p && field_count < 8) {
        if (*p == '|') {
            *p = '\0';
            fields[field_count++] = start;
            start = p + 1;
        }
        p++;
    }
    if (field_count < 8 && start) {
        fields[field_count++] = start;
    }
    
    if (field_count < 8) {
        return -1;
    }
    
    tpl->id = atoi(fields[0]);
    strncpy(tpl->name, fields[1], sizeof(tpl->name) - 1);
    strncpy(tpl->apn, fields[2], sizeof(tpl->apn) - 1);
    strncpy(tpl->protocol, fields[3], sizeof(tpl->protocol) - 1);
    strncpy(tpl->username, fields[4], sizeof(tpl->username) - 1);
    strncpy(tpl->password, fields[5], sizeof(tpl->password) - 1);
    strncpy(tpl->auth_method, fields[6], sizeof(tpl->auth_method) - 1);
    tpl->created_at = (time_t)atol(fields[7]);
    
    return 0;
}

/**
 * 获取模板详情并与oFono当前配置对比
 */
int apn_template_get_status(int id, ApnTemplateStatus *status) {
    if (id <= 0 || !status) {
        return -1;
    }
    
    memset(status, 0, sizeof(ApnTemplateStatus));
    
    /* 1. 获取模板数据 */
    if (apn_template_get(id, &status->template) != 0) {
        return -1;
    }
    
    /* 2. 获取oFono当前所有APN Context */
    ApnContext contexts[MAX_APN_CONTEXTS];
    int count = ofono_get_all_apn_contexts(contexts, MAX_APN_CONTEXTS);
    
    if (count <= 0) {
        /* oFono查询失败或无context，模板未应用 */
        status->is_applied = 0;
        return 0;
    }
    
    /* 3. 遍历对比，查找匹配的context */
    for (int i = 0; i < count; i++) {
        /* 对比APN名称（忽略大小写） */
        if (strcasecmp(contexts[i].apn, status->template.apn) == 0) {
            status->is_applied = 1;
            strncpy(status->applied_context, contexts[i].path, sizeof(status->applied_context) - 1);
            status->is_active = contexts[i].active;
            break;
        }
    }
    
    return 0;
}

/* ==================== APN boot apply（apn-boot-apply.sh 语义） ==================== */

static void apn_boot_log_line(const char *msg) {
    FILE *fp;
    time_t now;
    struct tm tm_info;
    char ts[32];

    if (!msg) {
        return;
    }
    mkdir("/mnt/data/logs", 0755);
    fp = fopen(APN_BOOT_LOG_PATH, "a");
    if (!fp) {
        return;
    }
    now = time(NULL);
    localtime_r(&now, &tm_info);
    strftime(ts, sizeof(ts), "%F %T", &tm_info);
    fprintf(fp, "%s %s\n", ts, msg);
    fclose(fp);
}

static int apn_boot_set_state(const char *state) {
    FILE *fp;

    if (!state) {
        return -1;
    }
    mkdir("/mnt/data", 0755);
    fp = fopen(APN_BOOT_STATE_PATH, "w");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "%s", state);
    fclose(fp);
    return 0;
}

static int apn_boot_get_imsi(char *buf, size_t size) {
    char output[256];

    if (!buf || size == 0) {
        return -1;
    }
    buf[0] = '\0';
    if (run_command(output, sizeof(output), "sh", "-c",
                    "ls " APN_PERSIST_BASE1 " 2>/dev/null | grep -E '^[0-9]{15}$' | head -1",
                    NULL) == 0 &&
        output[0] != '\0') {
        strncpy(buf, output, size - 1);
        buf[size - 1] = '\0';
        return 0;
    }
    if (run_command(output, sizeof(output), "sh", "-c",
                    "connmanctl services 2>/dev/null | grep -oE '460[0-9]{12}' | head -1",
                    NULL) == 0 &&
        output[0] != '\0') {
        strncpy(buf, output, size - 1);
        buf[size - 1] = '\0';
        return 0;
    }
    return -1;
}

static int apn_boot_modem_online(void) {
    int active = 0;
    return ofono_get_data_status(&active) != -2;
}

static int apn_boot_wait_modem_online(int max_secs) {
    int waited = 0;

    while (waited < max_secs) {
        if (apn_boot_modem_online()) {
            return 0;
        }
        sleep(2);
        waited += 2;
    }
    return -1;
}

static int apn_boot_write_persist_file(const char *base, const char *imsi,
                                       const ApnBootTemplate *tpl) {
    char dir[256];
    char path[320];
    FILE *fp;
    const char *auth;
    const char *user;
    const char *pass;
    const char *proto;

    snprintf(dir, sizeof(dir), "%s/%s", base, imsi);
    mkdir(base, 0755);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/defult_apn", dir);
    auth = (tpl->auth[0] != '\0') ? tpl->auth : "none";
    user = tpl->user;
    pass = tpl->pass;
    proto = (tpl->proto[0] != '\0') ? tpl->proto : "dual";

    fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "[userDefultApn]\n");
    fprintf(fp, "AccessPointName=%s\n", tpl->apn);
    fprintf(fp, "Username=%s\n", user ? user : "");
    fprintf(fp, "Password=%s\n", pass ? pass : "");
    fprintf(fp, "AuthenticationMethod=%s\n", auth);
    fprintf(fp, "Protocol=%s\n", proto);
    fclose(fp);
    sync();
    return 0;
}

static int apn_boot_write_persist(const ApnBootTemplate *tpl) {
    char imsi[32];

    if (!tpl || tpl->apn[0] == '\0') {
        return -1;
    }
    if (apn_boot_get_imsi(imsi, sizeof(imsi)) != 0) {
        apn_boot_log_line("persist: no imsi");
        return -1;
    }
    if (apn_boot_write_persist_file(APN_PERSIST_BASE1, imsi, tpl) != 0 &&
        apn_boot_write_persist_file(APN_PERSIST_BASE2, imsi, tpl) != 0) {
        return -1;
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "persist ok imsi=%s apn=%s auth=%s", imsi,
                 tpl->apn, tpl->auth[0] ? tpl->auth : "none");
        apn_boot_log_line(msg);
    }
    return 0;
}

static int apn_boot_load_template(ApnBootTemplate *tpl) {
    char sql[256];
    char output[512];
    char *fields[5];
    int field_count = 0;
    char *p;
    char *start;

    if (!tpl) {
        return -1;
    }
    memset(tpl, 0, sizeof(*tpl));

    if (g_current_config.mode != APN_MODE_MANUAL ||
        g_current_config.auto_start != 1 ||
        g_current_config.template_id <= 0) {
        return -1;
    }

    snprintf(sql, sizeof(sql),
             "SELECT apn, auth_method, username, password, protocol "
             "FROM apn_templates WHERE id = %d;",
             g_current_config.template_id);

    pthread_mutex_lock(&g_apn_mutex);
    int ret = db_query_rows(sql, "|", output, sizeof(output));
    pthread_mutex_unlock(&g_apn_mutex);

    if (ret != 0 || output[0] == '\0') {
        return -1;
    }

    p = output;
    start = p;
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
    if (field_count < 5) {
        return -1;
    }

    strncpy(tpl->apn, fields[0], sizeof(tpl->apn) - 1);
    strncpy(tpl->auth, fields[1], sizeof(tpl->auth) - 1);
    strncpy(tpl->user, fields[2], sizeof(tpl->user) - 1);
    strncpy(tpl->pass, fields[3], sizeof(tpl->pass) - 1);
    strncpy(tpl->proto, fields[4], sizeof(tpl->proto) - 1);
    if (tpl->proto[0] == '\0') {
        strncpy(tpl->proto, "dual", sizeof(tpl->proto) - 1);
    }
    return 0;
}

static int apn_boot_is_active(void) {
    int active = 0;
    if (apn_boot_modem_online() &&
        ofono_get_data_status(&active) == 0 && active) {
        return 1;
    }
    return 0;
}

static int apn_boot_activate_pdp_fallback(void) {
    char dummy[64];

    run_command(dummy, sizeof(dummy), "connmanctl", "setautoconnect", "on", NULL);
    run_command(dummy, sizeof(dummy), "connmanctl", "ActivatePdp", "1", NULL);
    sleep(5);
    return apn_boot_is_active() ? 0 : -1;
}

static void apn_boot_disconnect_context(void) {
    char out[256];
    /* Best-effort: disconnect active connman cellular service if present */
    (void)run_command(out, sizeof(out), "sh", "-c",
        "svc=$(connmanctl services 2>/dev/null | awk '/cellular/ {print $NF; exit}'); "
        "[ -n \"$svc\" ] && connmanctl disconnect \"$svc\"",
        NULL);
}

static void apn_boot_touch_tether_refresh(void) {
    FILE *fp = fopen("/tmp/usb-tether-refresh", "w");
    if (fp) {
        fclose(fp);
    }
}

static int apn_boot_apply_full(const ApnBootTemplate *tpl) {
    ApnTemplate atpl;
    int active = 0;
    int i;
    int attempt;
    char msg[128];

    apn_boot_set_state("running");
    apn_boot_log_line("config ok, sleeping 20s");
    sleep(20);

    for (i = 0; i < 12; i++) {
        char ps_out[64];
        if (run_command(ps_out, sizeof(ps_out), "sh", "-c", "ps | grep -q '[o]fonod'",
                        NULL) == 0) {
            break;
        }
        sleep(5);
    }

    if (apn_boot_write_persist(tpl) != 0) {
        apn_boot_set_state("failed");
        return -1;
    }
    if (apn_boot_wait_modem_online(90) != 0) {
        apn_boot_log_line("modem not online");
        apn_boot_set_state("failed");
        return -1;
    }

    if (apn_boot_is_active()) {
        ApnContext contexts[MAX_APN_CONTEXTS];
        int count = ofono_get_all_apn_contexts(contexts, MAX_APN_CONTEXTS);
        for (i = 0; i < count; i++) {
            if (strcmp(contexts[i].context_type, "internet") == 0 &&
                strcmp(contexts[i].apn, tpl->apn) == 0) {
                apn_boot_log_line("already applied");
                apn_boot_set_state("activated");
                apn_boot_touch_tether_refresh();
                return 0;
            }
        }
    }

    for (attempt = 1; attempt <= 3; attempt++) {
        snprintf(msg, sizeof(msg), "apply attempt=%d", attempt);
        apn_boot_log_line(msg);
        apn_boot_disconnect_context();

        memset(&atpl, 0, sizeof(atpl));
        strncpy(atpl.apn, tpl->apn, sizeof(atpl.apn) - 1);
        strncpy(atpl.protocol, tpl->proto, sizeof(atpl.protocol) - 1);
        strncpy(atpl.username, tpl->user, sizeof(atpl.username) - 1);
        strncpy(atpl.password, tpl->pass, sizeof(atpl.password) - 1);
        strncpy(atpl.auth_method, tpl->auth, sizeof(atpl.auth_method) - 1);

        if (apply_apn_to_ofono(&atpl) != 0) {
            apn_boot_log_line("dbus apply skipped or failed");
            continue;
        }

        ofono_set_data_status(1);
        sleep(8);
        if (ofono_get_data_status(&active) == 0 && active) {
            apn_boot_set_state("activated");
            apn_boot_touch_tether_refresh();
            return 0;
        }
    }

    apn_boot_set_state("failed");
    return -1;
}

static void *apn_boot_persist_only_thread(void *arg) {
    ApnBootTemplate *tpl = (ApnBootTemplate *)arg;
    int waited = 0;

    while (waited < 120) {
        if (apn_boot_write_persist(tpl) == 0) {
            apn_boot_log_line("persist-only done");
            apn_boot_set_state("persist-only-ok");
            break;
        }
        sleep(2);
        waited += 2;
    }
    free(tpl);
    return NULL;
}

int apn_boot_apply(const char *mode) {
    ApnBootTemplate tpl;
    const char *m = mode ? mode : "full";
    char logbuf[128];
    int waited;

    snprintf(logbuf, sizeof(logbuf), "apn-boot-apply start mode=%s", m);
    apn_boot_log_line(logbuf);

    if (strcmp(m, "bounce-pdp") == 0) {
        apn_boot_log_line("bounce-pdp start");
        if (!apn_boot_modem_online()) {
            apn_boot_log_line("bounce-pdp modem offline");
            apn_boot_set_state("failed");
            return -1;
        }
        apn_boot_log_line("bounce-pdp dbus Active=false");
        if (ofono_bounce_pdp_context() == 0) {
            apn_boot_log_line("bounce-pdp ok (dbus)");
            apn_boot_set_state("activated");
            apn_boot_touch_tether_refresh();
            return 0;
        }
        apn_boot_log_line("bounce-pdp fallback ActivatePdp");
        if (apn_boot_activate_pdp_fallback() == 0) {
            apn_boot_log_line("bounce-pdp ok (ActivatePdp)");
            apn_boot_set_state("activated");
            apn_boot_touch_tether_refresh();
            return 0;
        }
        apn_boot_log_line("bounce-pdp failed");
        apn_boot_set_state("failed");
        return -1;
    }

    if (strcmp(m, "reactivate-only") == 0) {
        apn_boot_log_line("reactivate-only start");
        if (apn_boot_wait_modem_online(90) != 0) {
            apn_boot_log_line("reactivate-only modem offline");
            apn_boot_set_state("failed");
            return -1;
        }
        if (apn_boot_is_active()) {
            apn_boot_log_line("reactivate-only already active");
            apn_boot_set_state("activated");
            apn_boot_touch_tether_refresh();
            return 0;
        }
        apn_boot_log_line("reactivate-only dbus Active=true");
        ofono_set_data_status(1);
        if (apn_boot_is_active()) {
            apn_boot_log_line("reactivate-only ok (dbus)");
            apn_boot_set_state("activated");
            apn_boot_touch_tether_refresh();
            return 0;
        }
        waited = 0;
        while (waited < 30) {
            if (apn_boot_is_active()) {
                apn_boot_log_line("reactivate-only ok (dbus)");
                apn_boot_set_state("activated");
                apn_boot_touch_tether_refresh();
                return 0;
            }
            sleep(2);
            waited += 2;
        }
        apn_boot_log_line("reactivate-only fallback ActivatePdp");
        if (apn_boot_activate_pdp_fallback() == 0) {
            apn_boot_log_line("reactivate-only ok (ActivatePdp)");
            apn_boot_set_state("activated");
            apn_boot_touch_tether_refresh();
            return 0;
        }
        apn_boot_log_line("reactivate-only failed");
        apn_boot_set_state("failed");
        return -1;
    }

    if (apn_boot_load_template(&tpl) != 0) {
        apn_boot_set_state("skip");
        return 0;
    }

    if (strcmp(m, "persist-only") == 0) {
        ApnBootTemplate *heap_tpl = (ApnBootTemplate *)malloc(sizeof(ApnBootTemplate));
        pthread_t tid;

        apn_boot_set_state("persist-only-ok");
        if (!heap_tpl) {
            return -1;
        }
        *heap_tpl = tpl;
        if (pthread_create(&tid, NULL, apn_boot_persist_only_thread, heap_tpl) == 0) {
            pthread_detach(tid);
            return 0;
        }
        free(heap_tpl);
        return -1;
    }

    if (strcmp(m, "full") == 0) {
        return apn_boot_apply_full(&tpl);
    }

    apn_boot_log_line("unknown mode");
    return -1;
}
