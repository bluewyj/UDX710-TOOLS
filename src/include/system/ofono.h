/**
 * @file ofono.h
 * @brief ofono D-Bus 接口封装
 */

#ifndef OFONO_H
#define OFONO_H

#include <gio/gio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ofono D-Bus 常量 */
#define OFONO_SERVICE           "org.ofono"
#define OFONO_RADIO_SETTINGS    "org.ofono.RadioSettings"
#define OFONO_TIMEOUT_MS        30000

/**
 * 初始化 D-Bus 连接
 * @return 成功返回非0，失败返回0
 */
int ofono_init(void);

/**
 * 检查 D-Bus 连接是否已初始化
 * @return 已初始化返回1，否则返回0
 */
int ofono_is_initialized(void);

/**
 * 关闭 D-Bus 连接
 */
void ofono_deinit(void);

/**
 * 通过 modem 串口发送 USB share AT（对齐 enable-usb-share-at.sh）
 * 等待 /dev/stty_lte30 最多约 30s，最多 5 次写入；失败不阻断管理面启动
 * @return 成功返回 0，失败返回 -1
 */
int ofono_enable_usb_share_at(void);

/**
 * 获取网络模式
 * @param modem_path modem 路径，如 "/ril_0"
 * @param buffer 输出缓冲区
 * @param size 缓冲区大小
 * @param timeout_ms 超时时间(毫秒)
 * @return 成功返回0，失败返回错误码
 */
int ofono_network_get_mode_sync(const char* modem_path, char* buffer, int size, int timeout_ms);

/**
 * 获取数据卡路径
 * @return 数据卡路径字符串，需要调用 g_free 释放，失败返回 NULL
 */
char* ofono_get_datacard(void);

/**
 * 设置网络模式
 * @param modem_path modem 路径
 * @param mode 网络模式索引 (0-10)
 * @param timeout_ms 超时时间
 * @return 成功返回0，失败返回错误码
 */
int ofono_network_set_mode_sync(const char* modem_path, int mode, int timeout_ms);

/**
 * 获取网络模式名称
 * @param mode 模式索引
 * @return 模式名称字符串
 */
const char* ofono_get_mode_name(int mode);

/**
 * 获取支持的网络模式数量
 */
int ofono_get_mode_count(void);

/**
 * 设置 modem 在线状态
 * @param modem_path modem 路径
 * @param online 1=在线, 0=离线
 * @param timeout_ms 超时时间
 * @return 成功返回0，失败返回错误码
 */
int ofono_modem_set_online(const char* modem_path, int online, int timeout_ms);

/**
 * 设置数据卡
 * @param modem_path modem 路径
 * @return 成功返回1，失败返回0
 */
int ofono_set_datacard(const char* modem_path);

/**
 * 获取信号强度
 * @param modem_path modem 路径
 * @param strength 输出信号强度百分比 (0-100)
 * @param dbm 输出信号强度 dBm 值
 * @param timeout_ms 超时时间
 * @return 成功返回0，失败返回错误码
 */
int ofono_network_get_signal_strength(const char* modem_path, int* strength, int* dbm, int timeout_ms);

/**
 * 获取数据连接状态
 * @param active 输出数据连接状态 (1=激活, 0=未激活)
 * @return 成功返回0，失败返回错误码
 */
int ofono_get_data_status(int *active);

/**
 * 设置数据连接状态
 * @param active 1=开启数据连接, 0=关闭数据连接
 * @return 成功返回0，失败返回错误码
 */
int ofono_set_data_status(int active);

/**
 * 设置数据连接；user_request=1 表示来自用户 API（写/清 /mnt/data/user_data_off）
 * user_request=0 为内部自愈/bounce/APN，不得污染用户关闭意图；若用户已关闭则拒绝激活
 */
int ofono_set_data_status_ex(int active, int user_request);

/** 用户是否已显式关闭移动数据（持久标志存在） */
int ofono_user_data_disabled(void);

/**
 * 获取漫游状态
 * @param roaming_allowed 输出漫游允许状态 (1=允许, 0=禁止)
 * @param is_roaming 输出当前是否漫游中 (1=漫游中, 0=非漫游)
 * @return 成功返回0，失败返回错误码
 */
int ofono_get_roaming_status(int *roaming_allowed, int *is_roaming);

/**
 * 设置漫游允许状态
 * @param allowed 1=允许漫游, 0=禁止漫游
 * @return 成功返回0，失败返回错误码
 */
int ofono_set_roaming_allowed(int allowed);

/* ==================== APN 管理 API ==================== */

#define MAX_APN_CONTEXTS 16
#define APN_STRING_SIZE 128

/**
 * APN Context 结构体
 */
typedef struct {
    char path[APN_STRING_SIZE];        /* D-Bus 路径 (如 /ril_0/context2) */
    char name[APN_STRING_SIZE];        /* 名称 */
    int active;                        /* 是否激活 */
    char apn[APN_STRING_SIZE];         /* APN 名称 (如 cbnet, cmnet) */
    char protocol[32];                 /* 协议: ip/ipv6/dual */
    char username[APN_STRING_SIZE];    /* 用户名 */
    char password[APN_STRING_SIZE];    /* 密码 */
    char auth_method[32];              /* 认证方式: none/pap/chap */
    char context_type[32];             /* 类型: internet/mms/ims */
} ApnContext;

/**
 * 获取所有 APN Context 列表
 * @param contexts 输出 context 数组
 * @param max_count 数组最大容量
 * @return 成功返回 context 数量，失败返回负数错误码
 */
int ofono_get_all_apn_contexts(ApnContext *contexts, int max_count);

/**
 * 设置 APN 单个属性
 * @param context_path context 的 D-Bus 路径
 * @param property 属性名
 * @param value 属性值
 * @return 成功返回0，失败返回错误码
 */
int ofono_set_apn_property(const char *context_path, const char *property, const char *value);

/**
 * 批量设置 APN 属性
 * @param context_path context 的 D-Bus 路径
 * @param apn APN 名称 (NULL 表示不修改)
 * @param protocol 协议 (NULL 表示不修改)
 * @param username 用户名 (NULL 表示不修改)
 * @param password 密码 (NULL 表示不修改)
 * @param auth_method 认证方式 (NULL 表示不修改)
 * @return 成功返回0，失败返回错误码
 */
int ofono_set_apn_properties(const char *context_path, 
                             const char *apn,
                             const char *protocol,
                             const char *username,
                             const char *password,
                             const char *auth_method);

/**
 * 获取当前服务小区的网络技术类型
 * 通过 NetworkMonitor.GetServingCellInformation 获取
 * @param tech 输出技术类型字符串 (如 "nr", "lte", "umts", "gsm")
 * @param size 缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int ofono_get_serving_cell_tech(char *tech, int size);

/**
 * 获取当前服务小区信息（技术类型和频段）
 * 通过 NetworkMonitor.GetServingCellInformation 获取
 * @param tech 输出技术类型字符串 (如 "nr", "lte", "umts", "gsm")
 * @param tech_size 技术类型缓冲区大小
 * @param band 输出频段号 (如 41, 78)
 * @return 成功返回0，失败返回错误码
 */
int ofono_get_serving_cell_info(char *tech, int tech_size, int *band);

/* ==================== 数据连接 Watchdog API ==================== */

typedef struct {
  int success;           /* 1/0 */
  char target[64];
  double latency_ms;     /* 成功时填；失败可 0 */
  char error[128];       /* 失败时非空 */
} OfonoProbeResult;

typedef struct {
  OfonoProbeResult ipv4;
  OfonoProbeResult ipv6;
} OfonoConnectivityProbe;

/**
 * 双栈连通性探测：短锁拷贝共享 TTL 快照（不在调用线程 ping；不得 bounce）
 * 冷启动无快照时返回 success=0 + cache miss，并 kick 后台刷新。
 * @param out 输出探测结果
 * @return 成功返回 0，参数无效返回 -1
 */
int ofono_probe_connectivity(OfonoConnectivityProbe *out);

/**
 * 启动出口探测共享缓存 worker（锁外 ping；TTL 刷新）
 * @return 成功 0，创建线程失败 -1
 */
int ofono_start_egress_probe_cache(void);

/**
 * 停止出口探测共享缓存 worker（broadcast + join）
 */
void ofono_stop_egress_probe_cache(void);

/**
 * 强制失效快照并等待新一代刷新完成（供 bounce 末尾校验）
 * @param timeout_ms 等待上限毫秒
 * @return 1 可达，0 不可达或超时
 */
int ofono_egress_reachable_fresh(int timeout_ms);

/**
 * 获取网络注册状态
 * @param status 输出状态字符串 (如 "registered", "roaming", "searching")
 * @param size 缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int ofono_get_network_status(char *status, int size);

/**
 * PDP context Active false→true（等同 apn-boot-apply bounce-pdp dbus 序列）
 * @return 成功且 Active 返回 0，否则 -1
 */
int ofono_bounce_pdp_context(void);

/**
 * 强制 PDP 翻转（watchdog 用，含 90s 冷却与 egress 校验）
 * @return 成功 0，失败 -1，冷却中 -2
 */
int ofono_bounce_pdp(void);

/**
 * 检查并恢复数据连接
 * - 用户已显式关闭移动数据 → 跳过激活（不报失败）
 * - APN 已配置且 Active=false → 尝试激活
 * - Active=true 但公网 ICMP 不可达 → 由 watchdog streak 触发 bounce（本函数不立即 bounce）
 * @param result 输出结果描述字符串
 * @param size 缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int ofono_check_and_restore_data(char *result, int size);

/**
 * 启动数据连接 Watchdog 线程
 * 后台定时检查数据连接状态，断开时自动重连
 * @param interval_secs 检查间隔（秒），默认10秒
 * @return 成功返回0，失败返回-1
 */
int ofono_start_data_watchdog(int interval_secs);

/**
 * 停止数据连接 Watchdog 线程
 */
void ofono_stop_data_watchdog(void);

/**
 * 检查 Watchdog 是否运行中
 * @return 运行中返回1，否则返回0
 * @deprecated 请使用 ofono_is_data_monitor_running()
 */
int ofono_is_watchdog_running(void);

/**
 * Watchdog 可观测性快照（只读；不触发 heal/escalate）
 */
typedef struct {
  int running;
  int partial_streak;
  int total_streak;
  int reboot_used;
  int reboot_max;
  int pending;
  char status[256];
  char accounting_day[16];
} OfonoWatchdogSnapshot;

/**
 * 获取 Watchdog 运行态快照（短临界区拷贝标量/status；预算文件在锁外读取）
 * @param out 输出快照
 * @return 成功返回 0，out 为 NULL 返回 -1
 */
int ofono_get_watchdog_snapshot(OfonoWatchdogSnapshot *out);

/* ==================== 数据连接监听 API (DBus 信号驱动) ==================== */

/**
 * 启动数据连接监听（基于 DBus 信号）
 * 订阅 ConnectionContext.PropertyChanged 和 NetworkRegistration.PropertyChanged 信号
 * 当数据连接断开或网络注册成功时自动尝试恢复连接
 * @return 成功返回0，失败返回-1
 */
int ofono_start_data_monitor(void);

/**
 * 停止数据连接监听
 * 取消所有信号订阅和服务监控
 */
void ofono_stop_data_monitor(void);

/**
 * 检查数据连接监听是否运行中
 * @return 运行中返回1，否则返回0
 */
int ofono_is_data_monitor_running(void);

#ifdef __cplusplus
}
#endif

#endif /* OFONO_H */
