/**
 * @file apn.h
 * @brief APN配置管理模块
 */

#ifndef APN_H
#define APN_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 常量定义 */
#define MAX_APN_TEMPLATES 50
#define APN_MODE_AUTO 0
#define APN_MODE_MANUAL 1

/**
 * APN模板结构体
 */
typedef struct {
    int id;
    char name[128];          /* 模板名称（支持中文） */
    char apn[128];           /* APN名称 */
    char protocol[32];       /* 协议: ip/ipv6/dual */
    char username[128];      /* 用户名 */
    char password[128];      /* 密码 */
    char auth_method[32];    /* 认证方式: none/pap/chap */
    time_t created_at;       /* 创建时间 */
} ApnTemplate;

/**
 * APN配置结构体
 */
typedef struct {
    int mode;                /* 0=自动, 1=手动 */
    int template_id;         /* 绑定的模板ID */
    int auto_start;          /* 0=关闭, 1=开启 */
} ApnConfig;

/**
 * 初始化APN模块
 * @param db_path 数据库文件路径
 * @return 成功返回0，失败返回-1
 */
int apn_init(const char *db_path);

/**
 * 获取当前APN配置
 * @param config 输出配置结构体
 * @return 成功返回0，失败返回-1
 */
int apn_get_config(ApnConfig *config);

/**
 * 设置APN模式
 * @param mode 模式（0=自动, 1=手动）
 * @param template_id 模板ID（手动模式时有效）
 * @param auto_start 自启动开关（0=关闭, 1=开启）
 * @return 成功返回0，失败返回-1
 */
int apn_set_mode(int mode, int template_id, int auto_start);

/**
 * 获取模板列表
 * @param templates 输出模板数组
 * @param max_count 数组最大容量
 * @return 成功返回模板数量，失败返回-1
 */
int apn_template_list(ApnTemplate *templates, int max_count);

/**
 * 创建APN模板
 * @param name 模板名称
 * @param apn APN名称
 * @param protocol 协议类型
 * @param username 用户名（可为NULL）
 * @param password 密码（可为NULL）
 * @param auth_method 认证方式
 * @return 成功返回新模板ID，失败返回-1
 */
int apn_template_create(const char *name, const char *apn, const char *protocol,
                        const char *username, const char *password, const char *auth_method);

/**
 * 更新APN模板
 * @param id 模板ID
 * @param name 模板名称
 * @param apn APN名称
 * @param protocol 协议类型
 * @param username 用户名（可为NULL）
 * @param password 密码（可为NULL）
 * @param auth_method 认证方式
 * @return 成功返回0，失败返回-1
 */
int apn_template_update(int id, const char *name, const char *apn, const char *protocol,
                        const char *username, const char *password, const char *auth_method);

/**
 * 删除APN模板
 * @param id 模板ID
 * @return 成功返回0，失败返回-1
 */
int apn_template_delete(int id);

/**
 * 应用模板到系统
 * @param template_id 模板ID
 * @return 成功返回0，失败返回-1
 */
int apn_apply_template(int template_id);

/**
 * 清除所有APN配置（自动模式使用）
 * 重置为系统默认配置，清除自定义APN设置
 * @return 成功返回0，失败返回-1
 */
int apn_clear_all(void);

/**
 * 获取模板详情
 * @param id 模板ID
 * @param tpl 输出模板结构体
 * @return 成功返回0，失败返回-1
 */
int apn_template_get(int id, ApnTemplate *tpl);

/**
 * APN模板状态结构体（含oFono对比结果）
 */
typedef struct {
    ApnTemplate template;      /* 模板数据 */
    int is_applied;            /* 是否已应用到oFono (0/1) */
    char applied_context[128]; /* 应用到的context路径 */
    int is_active;             /* 该context是否激活 (0/1) */
} ApnTemplateStatus;

/**
 * 获取模板详情并与oFono当前配置对比
 * @param id 模板ID
 * @param status 输出模板状态结构体
 * @return 成功返回0，失败返回-1
 */
int apn_template_get_status(int id, ApnTemplateStatus *status);

/**
 * APN 开机 boot 应用（对齐 apn-boot-apply.sh）
 * @param mode "full" | "persist-only" | "reactivate-only" | "bounce-pdp"
 * @return 成功返回 0，失败返回 -1；persist-only 异步启动返回 0
 */
int apn_boot_apply(const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* APN_H */
