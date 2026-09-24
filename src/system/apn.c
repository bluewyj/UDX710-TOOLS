/**
 * @file apn.c
 * @brief APN配置管理模块实现 - 模板管理、自动/手动模式切换、自启动功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "apn.h"
#include "database.h"
#include "ofono.h"
#include "exec_utils.h"

/* APN模块专用互斥锁 */
static pthread_mutex_t g_apn_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_apn_initialized = 0;

/* 当前配置缓存 */
static ApnConfig g_current_config = {0};

/* 前向声明 */
static int create_apn_tables(void);
static int load_apn_config(void);
static int apply_apn_to_ofono(const ApnTemplate *tpl);

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
    char out[512];

    if (!tpl) {
        printf("[APN] 模板参数无效\n");
        return -1;
    }
    
    printf("[APN] 开始应用模板: %s (APN: %s)\n", tpl->name, tpl->apn);

    /* Prefer on-device helper so apply works even before ofono-server rebuild. */
    if (access("/home/root/apn-apply-now.sh", X_OK) == 0) {
        printf("[APN] apply via apn-apply-now.sh\n");
        if (run_command(out, sizeof(out), "/home/root/apn-apply-now.sh", tpl->apn,
                        tpl->auth_method[0] ? tpl->auth_method : "none",
                        tpl->username, tpl->password,
                        tpl->protocol[0] ? tpl->protocol : "dual", NULL) == 0) {
            printf("[APN] APN配置应用成功 (apply-now)\n");
            return 0;
        }
        printf("[APN] apply-now failed, fallback ofono dbus\n");
    }
    
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
    
    /* 处理自启动 */
    if (g_current_config.mode == APN_MODE_MANUAL && 
        g_current_config.auto_start == 1 && 
        g_current_config.template_id > 0) {
        
        printf("[APN] 检测到自启动配置，应用模板ID: %d\n", g_current_config.template_id);
        
        /* 获取模板 */
        ApnTemplate tpl;
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT id, name, apn, protocol, username, password, auth_method, created_at "
            "FROM apn_templates WHERE id = %d;", g_current_config.template_id);
        
        char output[1024];
        pthread_mutex_lock(&g_apn_mutex);
        int ret = db_query_rows(sql, "|", output, sizeof(output));
        pthread_mutex_unlock(&g_apn_mutex);
        
        if (ret == 0 && strlen(output) > 0) {
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
            
            if (field_count >= 8) {
                tpl.id = atoi(fields[0]);
                strncpy(tpl.name, fields[1], sizeof(tpl.name) - 1);
                strncpy(tpl.apn, fields[2], sizeof(tpl.apn) - 1);
                strncpy(tpl.protocol, fields[3], sizeof(tpl.protocol) - 1);
                strncpy(tpl.username, fields[4], sizeof(tpl.username) - 1);
                strncpy(tpl.password, fields[5], sizeof(tpl.password) - 1);
                strncpy(tpl.auth_method, fields[6], sizeof(tpl.auth_method) - 1);
                tpl.created_at = (time_t)atol(fields[7]);
                
                /* 应用模板 */
                apply_apn_to_ofono(&tpl);
            }
        }
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
    char out[512];
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
    
    /* Prefer helper: clear persist + cellular bounce (plain SetProperty hits InUse). */
    if (access("/home/root/apn-apply-now.sh", X_OK) == 0) {
        if (run_command(out, sizeof(out), "/home/root/apn-apply-now.sh", "--clear",
                        NULL) == 0) {
            printf("[APN] APN配置清除完成 (apply-now --clear)\n");
            return 0;
        }
        printf("[APN] apply-now --clear failed, fallback ofono clear\n");
    }

    /* 清除oFono APN配置 */
    if (ofono_is_initialized()) {
        ApnContext contexts[MAX_APN_CONTEXTS];
        int count = ofono_get_all_apn_contexts(contexts, MAX_APN_CONTEXTS);
        
        for (int i = 0; i < count; i++) {
            if (strcmp(contexts[i].context_type, "internet") == 0) {
                ofono_set_apn_properties(
                    contexts[i].path,
                    "",      /* 清空APN */
                    "dual",
                    NULL,
                    NULL,
                    "chap"
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
