/**
 * @file ofono.c
 * @brief ofono D-Bus 接口实现 (合并自 dbus_core.c)
 *
 * 包含：
 * - D-Bus 连接管理
 * - AT 命令执行
 * - oFono 服务调用（网络模式、APN、信号等）
 */

#include "exec_utils.h"
#include "ofono.h"
#include "dbus_core.h"
#include "sysinfo.h"
#include "usb_mode.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ==================== 常量定义 ==================== */
#define OFONO_MODEM_IFACE "org.ofono.Modem"
#define DEFAULT_MODEM_PATH "/ril_0"
#define AT_COMMAND_TIMEOUT 8000 /* 8秒超时 (毫秒) */
#define MAX_RETRIES 1

/* ==================== 前向声明 ==================== */
int ofono_start_data_monitor(void);
void ofono_stop_data_monitor(void);
int ofono_is_data_monitor_running(void);

/* ==================== 全局变量 ==================== */
static GDBusConnection *g_dbus_conn = NULL;
static GDBusProxy *g_modem_proxy = NULL;
static pthread_mutex_t g_at_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_last_error[512] = {0};
static char g_modem_path[64] =
    DEFAULT_MODEM_PATH; /* 缓存路径，仅用于 proxy 切换检测 */

/* ==================== 内部辅助函数 ==================== */

/**
 * 动态获取当前 modem 路径（每次实时查询 oFono DataCard）
 * 切卡后自动返回正确的 /ril_0 或 /ril_1
 */
static const char *get_current_modem_path(void) {
  char slot[16], ril_path[32];
  if (get_current_slot(slot, ril_path) == 0 &&
      strcmp(ril_path, "unknown") != 0) {
    strncpy(g_modem_path, ril_path, sizeof(g_modem_path) - 1);
    g_modem_path[sizeof(g_modem_path) - 1] = '\0';
  }
  return g_modem_path;
}

/* 设置错误信息 */
static void set_error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
  va_end(args);
}

/* 检查 D-Bus 连接是否有效 */
static int is_connection_valid(void) {
  if (!g_dbus_conn) {
    return 0;
  }
  if (g_dbus_connection_is_closed(g_dbus_conn)) {
    g_object_unref(g_dbus_conn);
    g_dbus_conn = NULL;
    return 0;
  }
  return 1;
}

/* 确保 D-Bus 连接有效，如果无效则重新连接 */
static int ensure_connection(void) {
  GError *error = NULL;

  if (is_connection_valid()) {
    return 1;
  }

  g_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!g_dbus_conn) {
    if (error)
      g_error_free(error);
    return 0;
  }
  return 1;
}

/* 验证 AT 命令格式 */
static int validate_at_command(const char *cmd) {
  if (!cmd || strlen(cmd) < 2)
    return 0;
  /* 检查是否以 AT 开头 (不区分大小写) */
  if ((cmd[0] == 'A' || cmd[0] == 'a') && (cmd[1] == 'T' || cmd[1] == 't')) {
    return 1;
  }
  return 0;
}

/* ==================== dbus_core.h 接口实现 ==================== */

const char *dbus_get_last_error(void) { return g_last_error; }

int is_dbus_initialized(void) {
  return (g_dbus_conn != NULL && g_modem_proxy != NULL) ? 1 : 0;
}

int init_dbus(void) {
  GError *error = NULL;

  if (g_dbus_conn != NULL && g_modem_proxy != NULL) {
    return 0; /* 已初始化 */
  }

  /* 动态获取当前卡槽路径 */
  const char *modem_path = get_current_modem_path();
  printf("D-Bus 使用卡槽: %s\n", modem_path);

  /* 获取系统 D-Bus 连接 */
  if (!g_dbus_conn) {
    g_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!g_dbus_conn) {
      set_error("连接系统 D-Bus 失败: %s", error ? error->message : "unknown");
      if (error)
        g_error_free(error);
      return -1;
    }
  }

  /* 创建 oFono Modem 代理对象 */
  g_modem_proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE,
                                        NULL, OFONO_SERVICE, modem_path,
                                        OFONO_MODEM_IFACE, NULL, &error);

  if (!g_modem_proxy) {
    set_error("创建 oFono Modem 代理失败: %s",
              error ? error->message : "unknown");
    if (error)
      g_error_free(error);
    return -1;
  }

  printf("D-Bus 连接和 oFono Modem 对象初始化成功 (路径: %s)\n", modem_path);
  return 0;
}

void close_dbus(void) {
  if (g_modem_proxy) {
    g_object_unref(g_modem_proxy);
    g_modem_proxy = NULL;
  }
  if (g_dbus_conn) {
    g_object_unref(g_dbus_conn);
    g_dbus_conn = NULL;
  }
  printf("D-Bus 连接已关闭\n");
}

int execute_at(const char *command, char **result) {
  GError *error = NULL;
  GVariant *ret = NULL;
  int rc = -1;
  int retry;

  if (!command || !result) {
    set_error("无效的参数");
    return -1;
  }
  *result = NULL;

  /* 去除首尾空白 */
  while (*command == ' ' || *command == '\t')
    command++;

  /* 验证 AT 命令格式 */
  if (!validate_at_command(command)) {
    set_error("无效的 AT 命令格式: %s", command);
    return -1;
  }

  /* 检查 D-Bus 是否已初始化 */
  if (!is_dbus_initialized()) {
    printf("D-Bus 未初始化，尝试初始化...\n");
    if (init_dbus() != 0) {
      return -1;
    }
  }

  /* 动态检测 modem 路径变化，切卡后自动重建 proxy */
  const char *current_path = get_current_modem_path();
  if (g_modem_proxy) {
    const gchar *proxy_path = g_dbus_proxy_get_object_path(g_modem_proxy);
    if (proxy_path && strcmp(proxy_path, current_path) != 0) {
      printf("[AT] 检测到 modem 路径变化: %s -> %s，重建 proxy...\n",
             proxy_path, current_path);
      g_object_unref(g_modem_proxy);
      g_modem_proxy = NULL;
      GError *perr = NULL;
      g_modem_proxy = g_dbus_proxy_new_sync(
          g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, OFONO_SERVICE,
          current_path, OFONO_MODEM_IFACE, NULL, &perr);
      if (!g_modem_proxy) {
        printf("[AT] 重建 proxy 失败: %s\n", perr ? perr->message : "unknown");
        if (perr)
          g_error_free(perr);
        return -1;
      }
      printf("[AT] proxy 重建成功 (路径: %s)\n", current_path);
    }
  }

  /* 获取互斥锁，确保串行执行 */
  pthread_mutex_lock(&g_at_mutex);

  printf("准备发送 AT 命令: %s\n", command);

  /* 重试逻辑 */
  for (retry = 0; retry <= MAX_RETRIES; retry++) {
    error = NULL;

    /* 调用 oFono 的 SendAtcmd 方法 */
    ret = g_dbus_proxy_call_sync(
        g_modem_proxy, "SendAtcmd", g_variant_new("(s)", command),
        G_DBUS_CALL_FLAGS_NONE, AT_COMMAND_TIMEOUT, NULL, &error);

    if (!ret) {
      printf("调用 SendAtcmd 失败 (尝试 %d/%d) (%s): %s\n", retry + 1,
             MAX_RETRIES + 1, command, error ? error->message : "unknown");

      /* 检测连接关闭错误 */
      if (error && strstr(error->message, "connection closed")) {
        printf("检测到连接关闭，尝试重新初始化 D-Bus...\n");
        g_error_free(error);
        close_dbus();
        if (init_dbus() != 0) {
          set_error("重新初始化 D-Bus 失败");
          break;
        }
        continue;
      }

      /* 检测操作进行中错误 */
      if (error && strstr(error->message, "Operation already in progress")) {
        printf("检测到 'Operation already in progress'，500ms 后重试...\n");
        g_error_free(error);
        g_usleep(500000); /* 500ms */
        continue;
      }

      set_error("调用 SendAtcmd 失败: %s", error ? error->message : "unknown");
      if (error)
        g_error_free(error);
      break;
    }

    /* 提取结果字符串 */
    const gchar *res_str = NULL;
    g_variant_get(ret, "(s)", &res_str);

    if (res_str) {
      *result = g_strdup(res_str);
      g_strstrip(*result);
      printf("AT 命令 (%s) 响应: %s\n", command, *result);
      rc = 0;
    } else {
      set_error("空响应");
    }

    g_variant_unref(ret);
    break;
  }

  pthread_mutex_unlock(&g_at_mutex);
  return rc;
}

/* ==================== ofono.h 接口实现 ==================== */

int ofono_init(void) { return ensure_connection() ? 1 : 0; }

int ofono_is_initialized(void) { return is_connection_valid(); }

void ofono_deinit(void) {
  if (g_dbus_conn) {
    g_object_unref(g_dbus_conn);
    g_dbus_conn = NULL;
  }
}

int ofono_enable_usb_share_at(void) {
  const char *dev = "/dev/stty_lte30";
  const char *at = "AT+SPASENGMD=\"#dsm_usb_share_enable\",1\r";
  int n;

  for (n = 0; n < 30; n++) {
    struct stat st;
    if (stat(dev, &st) == 0 && S_ISCHR(st.st_mode))
      break;
    sleep(1);
  }
  if (n >= 30) {
    printf("[USBShare] stty_lte30 missing\n");
    return -1;
  }

  for (n = 0; n < 5; n++) {
    FILE *f = fopen(dev, "w");
    if (f) {
      fputs(at, f);
      fclose(f);
      printf("[USBShare] AT sent attempt=%d\n", n + 1);
      return 0;
    }
    sleep(2);
  }

  printf("[USBShare] failed to write AT after 5 attempts\n");
  return -1;
}

int ofono_network_get_mode_sync(const char *modem_path, char *buffer, int size,
                                int timeout_ms) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!modem_path || !buffer || size <= 0) {
    return -1;
  }

  if (!ensure_connection()) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, modem_path, OFONO_RADIO_SETTINGS,
                                NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -1;
  }

  result =
      g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                             G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -1;
  }

  GVariant *props = g_variant_get_child_value(result, 0);
  if (props) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "TechnologyPreference") == 0) {
        const gchar *mode = g_variant_get_string(value, NULL);
        if (mode) {
          strncpy(buffer, mode, size - 1);
          buffer[size - 1] = '\0';
          ret = 0;
        }
        g_variant_unref(value);
        break;
      }
      g_variant_unref(value);
    }
    g_variant_unref(props);
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

char *ofono_get_datacard(void) {
  GError *error = NULL;
  GVariant *result = NULL;
  char *datacard_path = NULL;

  if (!g_dbus_conn) {
    return NULL;
  }

  result = g_dbus_connection_call_sync(
      g_dbus_conn, OFONO_SERVICE, "/", "org.ofono.Manager", "GetDataCard", NULL,
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    return NULL;
  }

  const gchar *path = NULL;
  g_variant_get(result, "(&o)", &path);
  if (path && strlen(path) > 0) {
    datacard_path = g_strdup(path);
  }

  g_variant_unref(result);
  return datacard_path;
}

/* 网络模式映射表 - 索引对应 ofono TechnologyPreference */
static const char *network_modes[] = {"WCDMA preferred",          /* 0 */
                                      "GSM only",                 /* 1 */
                                      "WCDMA only",               /* 2 */
                                      "GSM/WCDMA auto",           /* 3 */
                                      "LTE/GSM/WCDMA auto",       /* 4 */
                                      "LTE only",                 /* 5 */
                                      "LTE/WCDMA auto",           /* 6 */
                                      "NR 5G/LTE/GSM/WCDMA auto", /* 7 */
                                      "NR 5G only",               /* 8 */
                                      "NR 5G/LTE auto",           /* 9 */
                                      "NSA only",                 /* 10 */
                                      NULL};

const char *ofono_get_mode_name(int mode) {
  if (mode >= 0 && mode < 11) {
    return network_modes[mode];
  }
  return NULL;
}

int ofono_get_mode_count(void) { return 11; }

int ofono_network_set_mode_sync(const char *modem_path, int mode,
                                int timeout_ms) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  const char *mode_str;

  if (!modem_path || !ensure_connection()) {
    return -1;
  }

  mode_str = ofono_get_mode_name(mode);
  if (!mode_str) {
    return -2;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, modem_path, OFONO_RADIO_SETTINGS,
                                NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -3;
  }

  result =
      g_dbus_proxy_call_sync(proxy, "SetProperty",
                             g_variant_new("(sv)", "TechnologyPreference",
                                           g_variant_new_string(mode_str)),
                             G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -4;
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return 0;
}

int ofono_modem_set_online(const char *modem_path, int online, int timeout_ms) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;

  if (!modem_path || !ensure_connection()) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, modem_path, "org.ofono.Modem",
                                NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(
      proxy, "SetProperty",
      g_variant_new("(sv)", "Online",
                    g_variant_new_boolean(online ? TRUE : FALSE)),
      G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return 0;
}

int ofono_set_datacard(const char *modem_path) {
  GError *error = NULL;
  GVariant *result = NULL;

  if (!modem_path || !ensure_connection()) {
    return 0;
  }

  result = g_dbus_connection_call_sync(
      g_dbus_conn, OFONO_SERVICE, "/", "org.ofono.Manager", "SetDataCard",
      g_variant_new("(o)", modem_path), NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
      NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    return 0;
  }

  g_variant_unref(result);
  return 1;
}

int ofono_network_get_signal_strength(const char *modem_path, int *strength,
                                      int *dbm, int timeout_ms) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!modem_path || !ensure_connection()) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, modem_path,
                                "org.ofono.NetworkRegistration", NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result =
      g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                             G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  GVariant *props = g_variant_get_child_value(result, 0);
  gboolean got_dbm = FALSE;

  if (props) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "Strength") == 0 && strength) {
        *strength = g_variant_get_byte(value);
        ret = 0;
      } else if (g_strcmp0(key, "StrengthDbm") == 0 && dbm) {
        *dbm = g_variant_get_int32(value);
        got_dbm = TRUE;
      }
      g_variant_unref(value);
    }
    g_variant_unref(props);
  }

  // 如果没有获取到 StrengthDbm，则使用 Strength 计算
  if (ret == 0 && dbm && !got_dbm && strength) {
    *dbm = -113 + 2 * (*strength);
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

/* ==================== 数据连接和漫游 API ==================== */

#define OFONO_CONNECTION_CONTEXT "org.ofono.ConnectionContext"
#define OFONO_CONNECTION_MANAGER "org.ofono.ConnectionManager"
#define OFONO_NETWORK_REGISTRATION "org.ofono.NetworkRegistration"
#define DEFAULT_CONTEXT_PATH "/ril_0/context2"
#define DEFAULT_MODEM_PATH "/ril_0"

/**
 * 动态查找第一个有效的 internet 类型 context 路径
 * 遍历所有 context，优先返回配置了 APN 的 internet 类型 context
 *
 * @param path_buf 输出缓冲区，存储找到的 context 路径
 * @param buf_size 缓冲区大小
 * @return 0 成功，-1 失败
 */
static int find_internet_context_path(char *path_buf, size_t buf_size) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int found = 0;
  char first_internet_path[256] = {0};

  if (!path_buf || buf_size == 0 || !ensure_connection()) {
    return -1;
  }

  /* 创建 ConnectionManager 代理 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_CONNECTION_MANAGER, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    /* 回退到默认路径 */
    strncpy(path_buf, DEFAULT_CONTEXT_PATH, buf_size - 1);
    return 0;
  }

  /* 调用 GetContexts 获取所有 context */
  result =
      g_dbus_proxy_call_sync(proxy, "GetContexts", NULL, G_DBUS_CALL_FLAGS_NONE,
                             OFONO_TIMEOUT_MS, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    strncpy(path_buf, DEFAULT_CONTEXT_PATH, buf_size - 1);
    return 0;
  }

  /* 解析返回的 a(oa{sv}) 数组 */
  GVariant *array = g_variant_get_child_value(result, 0);
  GVariantIter iter;
  GVariant *child;

  g_variant_iter_init(&iter, array);
  while ((child = g_variant_iter_next_value(&iter)) != NULL) {
    const gchar *path = NULL;
    GVariant *props = NULL;

    g_variant_get(child, "(&o@a{sv})", &path, &props);

    if (props && path) {
      /* 获取 Type 属性 */
      GVariant *type_var =
          g_variant_lookup_value(props, "Type", G_VARIANT_TYPE_STRING);
      const gchar *context_type =
          type_var ? g_variant_get_string(type_var, NULL) : "";

      if (g_strcmp0(context_type, "internet") == 0) {
        /* 获取 AccessPointName 属性 */
        GVariant *apn_var = g_variant_lookup_value(props, "AccessPointName",
                                                   G_VARIANT_TYPE_STRING);
        const gchar *apn = apn_var ? g_variant_get_string(apn_var, NULL) : "";

        /* 记录第一个 internet context */
        if (first_internet_path[0] == '\0') {
          strncpy(first_internet_path, path, sizeof(first_internet_path) - 1);
        }

        /* 优先返回配置了 APN 的 context */
        if (apn && apn[0] != '\0') {
          strncpy(path_buf, path, buf_size - 1);
          found = 1;
          if (apn_var)
            g_variant_unref(apn_var);
          if (type_var)
            g_variant_unref(type_var);
          g_variant_unref(props);
          g_variant_unref(child);
          break;
        }

        if (apn_var)
          g_variant_unref(apn_var);
      }

      if (type_var)
        g_variant_unref(type_var);
      g_variant_unref(props);
    }
    g_variant_unref(child);
  }

  g_variant_unref(array);
  g_variant_unref(result);
  g_object_unref(proxy);

  /* 如果没找到配置了 APN 的，使用第一个 internet context */
  if (!found) {
    if (first_internet_path[0] != '\0') {
      strncpy(path_buf, first_internet_path, buf_size - 1);
    } else {
      strncpy(path_buf, DEFAULT_CONTEXT_PATH, buf_size - 1);
    }
  }

  return 0;
}

int ofono_get_data_status(int *active) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;
  char context_path[256] = {0};

  if (!active || !ensure_connection()) {
    return -1;
  }

  /* 动态获取 internet context 路径 */
  if (find_internet_context_path(context_path, sizeof(context_path)) != 0) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, context_path,
                                OFONO_CONNECTION_CONTEXT, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  GVariant *props = g_variant_get_child_value(result, 0);
  if (props) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "Active") == 0) {
        *active = g_variant_get_boolean(value) ? 1 : 0;
        ret = 0;
      }
      g_variant_unref(value);
    }
    g_variant_unref(props);
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

int ofono_set_data_status(int active) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  char context_path[256] = {0};

  if (!ensure_connection()) {
    return -1;
  }

  /* 动态获取 internet context 路径 */
  if (find_internet_context_path(context_path, sizeof(context_path)) != 0) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, context_path,
                                OFONO_CONNECTION_CONTEXT, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(
      proxy, "SetProperty",
      g_variant_new("(sv)", "Active",
                    g_variant_new_boolean(active ? TRUE : FALSE)),
      G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  g_variant_unref(result);
  g_object_unref(proxy);

  /* 根据数据连接状态控制监听 */
  if (active) {
    /* 开启数据连接时启动监听 */
    if (!ofono_is_data_monitor_running()) {
      ofono_start_data_monitor();
    }
  } else {
    /* 关闭数据连接时停止监听 */
    if (ofono_is_data_monitor_running()) {
      ofono_stop_data_monitor();
    }
  }

  return 0;
}

int ofono_get_roaming_status(int *roaming_allowed, int *is_roaming) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!roaming_allowed || !is_roaming || !ensure_connection()) {
    return -1;
  }

  *roaming_allowed = 0;
  *is_roaming = 0;

  /* 1. 获取 ConnectionManager 的 RoamingAllowed 属性 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_CONNECTION_MANAGER, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (result) {
    GVariant *props = g_variant_get_child_value(result, 0);
    if (props) {
      GVariantIter iter;
      const gchar *key;
      GVariant *value;

      g_variant_iter_init(&iter, props);
      while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        if (g_strcmp0(key, "RoamingAllowed") == 0) {
          *roaming_allowed = g_variant_get_boolean(value) ? 1 : 0;
          ret = 0;
        }
        g_variant_unref(value);
      }
      g_variant_unref(props);
    }
    g_variant_unref(result);
  } else {
    if (error) {
      g_error_free(error);
      error = NULL;
    }
  }

  g_object_unref(proxy);

  /* 2. 获取 NetworkRegistration 的 Status 属性判断是否漫游中 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_NETWORK_REGISTRATION, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return ret; /* 返回已获取的 roaming_allowed */
  }

  result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (result) {
    GVariant *props = g_variant_get_child_value(result, 0);
    if (props) {
      GVariantIter iter;
      const gchar *key;
      GVariant *value;

      g_variant_iter_init(&iter, props);
      while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        if (g_strcmp0(key, "Status") == 0) {
          const gchar *status = g_variant_get_string(value, NULL);
          if (status && g_strcmp0(status, "roaming") == 0) {
            *is_roaming = 1;
          }
        }
        g_variant_unref(value);
      }
      g_variant_unref(props);
    }
    g_variant_unref(result);
  } else {
    if (error)
      g_error_free(error);
  }

  g_object_unref(proxy);
  return ret;
}

int ofono_set_roaming_allowed(int allowed) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;

  if (!ensure_connection()) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_CONNECTION_MANAGER, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(
      proxy, "SetProperty",
      g_variant_new("(sv)", "RoamingAllowed",
                    g_variant_new_boolean(allowed ? TRUE : FALSE)),
      G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return 0;
}

/* ==================== APN 管理 API ==================== */

int ofono_get_all_apn_contexts(ApnContext *contexts, int max_count) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int count = 0;

  if (!contexts || max_count <= 0 || !ensure_connection()) {
    return -1;
  }

  /* 创建 ConnectionManager 代理 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_CONNECTION_MANAGER, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  /* 调用 GetContexts */
  result =
      g_dbus_proxy_call_sync(proxy, "GetContexts", NULL, G_DBUS_CALL_FLAGS_NONE,
                             OFONO_TIMEOUT_MS, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  /* 解析返回的 a(oa{sv}) 数组 */
  GVariant *array = g_variant_get_child_value(result, 0);
  GVariantIter iter;
  GVariant *child;

  g_variant_iter_init(&iter, array);
  while ((child = g_variant_iter_next_value(&iter)) != NULL &&
         count < max_count) {
    const gchar *path = NULL;
    GVariant *props = NULL;

    g_variant_get(child, "(&o@a{sv})", &path, &props);

    if (props) {
      /* 获取 Type 属性 */
      GVariant *type_var =
          g_variant_lookup_value(props, "Type", G_VARIANT_TYPE_STRING);
      const gchar *context_type =
          type_var ? g_variant_get_string(type_var, NULL) : "";

      /* 只处理 internet 类型 */
      if (g_strcmp0(context_type, "internet") == 0) {
        ApnContext *ctx = &contexts[count];
        memset(ctx, 0, sizeof(ApnContext));

        /* 复制路径 */
        strncpy(ctx->path, path, APN_STRING_SIZE - 1);
        strncpy(ctx->context_type, context_type, sizeof(ctx->context_type) - 1);

        /* 获取各属性 */
        GVariant *v;

        v = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
        if (v) {
          strncpy(ctx->name, g_variant_get_string(v, NULL),
                  APN_STRING_SIZE - 1);
          g_variant_unref(v);
        } else {
          strcpy(ctx->name, "Internet");
        }

        v = g_variant_lookup_value(props, "Active", G_VARIANT_TYPE_BOOLEAN);
        if (v) {
          ctx->active = g_variant_get_boolean(v) ? 1 : 0;
          g_variant_unref(v);
        }

        v = g_variant_lookup_value(props, "AccessPointName",
                                   G_VARIANT_TYPE_STRING);
        if (v) {
          strncpy(ctx->apn, g_variant_get_string(v, NULL), APN_STRING_SIZE - 1);
          g_variant_unref(v);
        }

        v = g_variant_lookup_value(props, "Protocol", G_VARIANT_TYPE_STRING);
        if (v) {
          strncpy(ctx->protocol, g_variant_get_string(v, NULL),
                  sizeof(ctx->protocol) - 1);
          g_variant_unref(v);
        } else {
          strcpy(ctx->protocol, "ip");
        }

        v = g_variant_lookup_value(props, "Username", G_VARIANT_TYPE_STRING);
        if (v) {
          strncpy(ctx->username, g_variant_get_string(v, NULL),
                  APN_STRING_SIZE - 1);
          g_variant_unref(v);
        }

        v = g_variant_lookup_value(props, "Password", G_VARIANT_TYPE_STRING);
        if (v) {
          strncpy(ctx->password, g_variant_get_string(v, NULL),
                  APN_STRING_SIZE - 1);
          g_variant_unref(v);
        }

        v = g_variant_lookup_value(props, "AuthenticationMethod",
                                   G_VARIANT_TYPE_STRING);
        if (v) {
          strncpy(ctx->auth_method, g_variant_get_string(v, NULL),
                  sizeof(ctx->auth_method) - 1);
          g_variant_unref(v);
        } else {
          strcpy(ctx->auth_method, "chap");
        }

        count++;
      }

      if (type_var)
        g_variant_unref(type_var);
      g_variant_unref(props);
    }
    g_variant_unref(child);
  }

  g_variant_unref(array);
  g_variant_unref(result);
  g_object_unref(proxy);

  return count;
}

int ofono_set_apn_property(const char *context_path, const char *property,
                           const char *value) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;

  if (!context_path || !property || !value || !ensure_connection()) {
    return -1;
  }

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, context_path,
                                OFONO_CONNECTION_CONTEXT, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(
      proxy, "SetProperty",
      g_variant_new("(sv)", property, g_variant_new_string(value)),
      G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS, NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return 0;
}

int ofono_set_apn_properties(const char *context_path, const char *apn,
                             const char *protocol, const char *username,
                             const char *password, const char *auth_method) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int was_active = 0;

  if (!context_path || !ensure_connection()) {
    return -1;
  }

  /* 1. 检查 context 是否激活 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, context_path,
                                OFONO_CONNECTION_CONTEXT, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (result) {
    GVariant *props = g_variant_get_child_value(result, 0);
    GVariant *active_var =
        g_variant_lookup_value(props, "Active", G_VARIANT_TYPE_BOOLEAN);
    if (active_var) {
      was_active = g_variant_get_boolean(active_var) ? 1 : 0;
      g_variant_unref(active_var);
    }
    g_variant_unref(props);
    g_variant_unref(result);
  } else {
    if (error) {
      g_error_free(error);
      error = NULL;
    }
  }

  g_object_unref(proxy);

  /* 2. 如果激活中，先关闭 */
  if (was_active) {
    proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                  OFONO_SERVICE, context_path,
                                  OFONO_CONNECTION_CONTEXT, NULL, &error);
    if (proxy) {
      result = g_dbus_proxy_call_sync(
          proxy, "SetProperty",
          g_variant_new("(sv)", "Active", g_variant_new_boolean(FALSE)),
          G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS, NULL, &error);
      if (result)
        g_variant_unref(result);
      if (error) {
        g_error_free(error);
        error = NULL;
      }
      g_object_unref(proxy);
    }
    /* 等待状态稳定 */
    g_usleep(500000); /* 500ms */
  }

  /* 3. 设置各属性 */
  if (apn) {
    ofono_set_apn_property(context_path, "AccessPointName", apn);
  }
  if (protocol) {
    ofono_set_apn_property(context_path, "Protocol", protocol);
  }
  if (username) {
    ofono_set_apn_property(context_path, "Username", username);
  }
  if (password) {
    ofono_set_apn_property(context_path, "Password", password);
  }
  if (auth_method) {
    ofono_set_apn_property(context_path, "AuthenticationMethod", auth_method);
  }

  /* 4. 如果之前是激活状态，重新激活 */
  if (was_active) {
    g_usleep(500000); /* 500ms */
    proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                  OFONO_SERVICE, context_path,
                                  OFONO_CONNECTION_CONTEXT, NULL, &error);
    if (proxy) {
      result = g_dbus_proxy_call_sync(
          proxy, "SetProperty",
          g_variant_new("(sv)", "Active", g_variant_new_boolean(TRUE)),
          G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS, NULL, &error);
      if (result)
        g_variant_unref(result);
      if (error)
        g_error_free(error);
      g_object_unref(proxy);
    }
  }

  return 0;
}

/* ==================== NetworkMonitor API ==================== */

#define OFONO_NETWORK_MONITOR "org.ofono.NetworkMonitor"

int ofono_get_serving_cell_tech(char *tech, int size) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!tech || size <= 0 || !ensure_connection()) {
    return -1;
  }

  tech[0] = '\0';

  /* 创建 NetworkMonitor 代理 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_NETWORK_MONITOR, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  /* 调用 GetServingCellInformation */
  result = g_dbus_proxy_call_sync(proxy, "GetServingCellInformation", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  /* 解析返回的 a{sv} 字典 */
  GVariant *props = g_variant_get_child_value(result, 0);
  if (props) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "Technology") == 0) {
        const gchar *tech_str = g_variant_get_string(value, NULL);
        if (tech_str) {
          strncpy(tech, tech_str, size - 1);
          tech[size - 1] = '\0';
          ret = 0;
        }
        g_variant_unref(value);
        break;
      }
      g_variant_unref(value);
    }
    g_variant_unref(props);
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

/**
 * 获取服务小区信息（Technology和Band）
 * @param tech 网络类型输出缓冲区（如 "nr", "lte"）
 * @param tech_size 缓冲区大小
 * @param band 频段输出缓冲区（如 "41", "78"）
 * @param band_size 缓冲区大小
 * @return 0 成功, 负数 失败
 */
int ofono_get_serving_cell_info(char *tech, int tech_size, int *band) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!tech || tech_size <= 0 || !band || !ensure_connection()) {
    return -1;
  }

  tech[0] = '\0';
  *band = 0;

  /* 创建 NetworkMonitor 代理 */
  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_NETWORK_MONITOR, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  /* 调用 GetServingCellInformation */
  result = g_dbus_proxy_call_sync(proxy, "GetServingCellInformation", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  /* 解析返回的 a{sv} 字典 */
  GVariant *props = g_variant_get_child_value(result, 0);
  if (props) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;
    int got_tech = 0, got_band = 0;

    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "Technology") == 0) {
        const gchar *tech_str = g_variant_get_string(value, NULL);
        if (tech_str) {
          strncpy(tech, tech_str, tech_size - 1);
          tech[tech_size - 1] = '\0';
          got_tech = 1;
        }
      } else if (g_strcmp0(key, "Band") == 0) {
        /* Band 可能是 int32 或 uint32 */
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
          *band = g_variant_get_int32(value);
          got_band = 1;
        } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
          *band = (int)g_variant_get_uint32(value);
          got_band = 1;
        }
      }
      g_variant_unref(value);

      if (got_tech && got_band)
        break;
    }
    g_variant_unref(props);

    if (got_tech)
      ret = 0;
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

/* ==================== 数据连接 Watchdog 实现 ==================== */

#define OUTAGE_INTERVAL_S              30
#define PARTIAL_BOUNCE_STREAK          20
#define PARTIAL_BOUNCE_RETRY           26
#define PARTIAL_REBOOT_STREAK          30
#define TOTAL_REBOOT_STREAK            12
#define TOTAL_ESCALATE_WINDOW_S       3600
#define TOTAL_REBOOT_MAX_DAY           2
#define PARTIAL_GRACE_S              1800
#define OUTAGE_BOOT_GRACE_S            120
#define OUTAGE_PARTIAL_ACT_STREAK       3
#define OUTAGE_TOTAL_LIGHT_EVERY        3
#define OUTAGE_TOTAL_UDC_EVERY          4
#define OUTAGE_REBOOT_DAY_FILE     "/mnt/data/outage-watch-reboot-day"
#define OUTAGE_REBOOT_PENDING_FILE "/mnt/data/outage-watch-reboot-pending"
#define OUTAGE_PARTIAL_GRACE_FILE  "/mnt/data/outage-watch-partial-grace"
#define NR_LTE_SWITCH_ON_FILE      "/mnt/data/nr-lte-switch.on"
#define NR_LTE_BOOT_GRACE_S            300
#define NR_LTE_TICK_INTERVAL_S          60
#define NR_LTE_PHASE1_MAX_SWITCHES       3

static pthread_t g_watchdog_thread = 0;
static volatile int g_watchdog_running = 0;
static int g_watchdog_interval = 10; /* 默认10秒 */
static char g_last_watchdog_status[256] = {0};
static time_t g_outage_boot_ts = 0;
static int g_partial_streak = 0;
static int g_total_streak = 0;
static int g_partial_bounce_count = 0;
static int g_partial_bounce_retry_pending = 0;
static int g_nr_lte_phase = 1;
static int g_nr_lte_switch_count = 0;
static int g_nr_lte_cooldown = 0;
static int g_nr_lte_cooldown_counter = 0;
static int g_nr_lte_nr_streak = 0;
static int g_nr_lte_last_switch = 0;
static time_t g_nr_lte_last_tick_ts = 0;

/**
 * 获取网络注册状态
 */
int ofono_get_network_status(char *status, int size) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!status || size <= 0 || !ensure_connection()) {
    return -1;
  }

  status[0] = '\0';

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_NETWORK_REGISTRATION, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);

  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  GVariant *props = g_variant_get_child_value(result, 0);
  if (props) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "Status") == 0) {
        const gchar *stat = g_variant_get_string(value, NULL);
        if (stat) {
          strncpy(status, stat, size - 1);
          status[size - 1] = '\0';
          ret = 0;
        }
        g_variant_unref(value);
        break;
      }
      g_variant_unref(value);
    }
    g_variant_unref(props);
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

/* 壳侧 egress 探测：Active=true 不等于用户面可用（zombie Active）。 */
#define OFONO_EGRESS_BOUNCE_COOLDOWN_S 90

static time_t g_last_egress_bounce_ts = 0;

static int ofono_egress_reachable(void) {
  /* CN carrier DNS first; AliDNS then 8.8.8.8 as fallback */
  if (system("ping -c 1 -W 2 211.138.245.180 >/dev/null 2>&1") == 0) {
    return 1;
  }
  if (system("ping -c 1 -W 2 211.138.240.100 >/dev/null 2>&1") == 0) {
    return 1;
  }
  if (system("ping -c 1 -W 2 223.5.5.5 >/dev/null 2>&1") == 0) {
    return 1;
  }
  if (system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1") == 0) {
    return 1;
  }
  return 0;
}

int ofono_bounce_pdp_context(void) {
  (void)ofono_set_data_status(0);
  sleep(4);
  if (ofono_set_data_status(1) != 0) {
    return -1;
  }
  int active = 0;
  if (ofono_get_data_status(&active) == 0 && active) {
    return 0;
  }
  return -1;
}

/**
 * 强制 PDP 翻转（watchdog 用，含冷却与 egress 校验）。
 */
int ofono_bounce_pdp(void) {
  time_t now = time(NULL);
  if (g_last_egress_bounce_ts != 0 &&
      (now - g_last_egress_bounce_ts) < OFONO_EGRESS_BOUNCE_COOLDOWN_S) {
    return -2; /* 冷却中 */
  }
  g_last_egress_bounce_ts = now;

  printf("[DataRestore] egress dead while Active — bouncing PDP\n");
  if (ofono_bounce_pdp_context() != 0) {
    return -1;
  }
  sleep(2);
  return ofono_egress_reachable() ? 0 : -1;
}

/**
 * 检查并恢复数据连接
 * Active=false → 尝试激活；Active=true 仍须 egress 可达，否则 bounce PDP
 */
int ofono_check_and_restore_data(char *result, int size) {
  char net_status[64] = {0};
  char context_path[256] = {0};
  int active = 0;

  if (!result || size <= 0) {
    return -1;
  }

  /* 1. 检查网络注册状态 */
  if (ofono_get_network_status(net_status, sizeof(net_status)) != 0) {
    snprintf(result, size, "无法获取网络状态");
    return -1;
  }

  if (strcmp(net_status, "registered") != 0 &&
      strcmp(net_status, "roaming") != 0) {
    snprintf(result, size, "等待网络注册 (状态: %s)", net_status);
    return 0;
  }

  /* 2. 获取 internet context 路径 */
  if (find_internet_context_path(context_path, sizeof(context_path)) != 0) {
    snprintf(result, size, "未找到 internet context");
    return -1;
  }

  /* 3. 获取 context 属性 */
  GError *error = NULL;
  GVariant *ctx_result = NULL;
  GDBusProxy *proxy = NULL;
  char apn[128] = {0};

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, context_path,
                                OFONO_CONNECTION_CONTEXT, NULL, &error);

  if (!proxy) {
    if (error)
      g_error_free(error);
    snprintf(result, size, "创建 context 代理失败");
    return -1;
  }

  ctx_result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                      G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                      NULL, &error);

  if (!ctx_result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    snprintf(result, size, "获取 context 属性失败");
    return -1;
  }

  GVariant *props = g_variant_get_child_value(ctx_result, 0);
  if (props) {
    GVariant *v;

    v = g_variant_lookup_value(props, "Active", G_VARIANT_TYPE_BOOLEAN);
    if (v) {
      active = g_variant_get_boolean(v) ? 1 : 0;
      g_variant_unref(v);
    }

    v = g_variant_lookup_value(props, "AccessPointName", G_VARIANT_TYPE_STRING);
    if (v) {
      strncpy(apn, g_variant_get_string(v, NULL), sizeof(apn) - 1);
      g_variant_unref(v);
    }

    g_variant_unref(props);
  }

  g_variant_unref(ctx_result);
  g_object_unref(proxy);

  /* 4. 检查 APN 是否配置 */
  if (strlen(apn) == 0) {
    snprintf(result, size, "APN 未配置，跳过自动连接");
    return 0;
  }

  /* 5. Active=true：egress 由 watchdog streak 触发 bounce，此处不立即 bounce */
  if (active) {
    if (ofono_egress_reachable()) {
      snprintf(result, size, "已连接 (APN: %s)", apn);
      return 0;
    }
    snprintf(result, size, "egress dead (APN: %s), wait streak bounce", apn);
    return 0;
  }

  /* 6. Active=false：尝试激活数据连接 */
  if (ofono_set_data_status(1) == 0) {
    snprintf(result, size, "连接已恢复 (APN: %s)", apn);
    return 0;
  } else {
    snprintf(result, size, "激活失败 (APN: %s)", apn);
    return -1;
  }
}

static int outage_has_gateway(void) {
  return system("ifconfig usb0 2>/dev/null | grep -q 'inet addr:192.168.66.1'") ==
         0;
}

static int outage_dhcp_ok(void) {
  return system("netstat -uln 2>/dev/null | grep -q ':67'") == 0;
}

static int outage_cell_active(void) {
  int active = 0;
  return ofono_get_data_status(&active) == 0 && active;
}

static int nr_lte_uptime_sec(void) {
  FILE *f = fopen("/proc/uptime", "r");
  if (!f)
    return -1;
  double up = 0;
  if (fscanf(f, "%lf", &up) != 1) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return (int)up;
}

static int nr_lte_get_registration_technology(char *tech, size_t size) {
  GError *error = NULL;
  GVariant *result = NULL;
  GDBusProxy *proxy = NULL;
  int ret = -1;

  if (!tech || size == 0 || !ensure_connection())
    return -1;

  tech[0] = '\0';

  proxy = g_dbus_proxy_new_sync(g_dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                OFONO_SERVICE, get_current_modem_path(),
                                OFONO_NETWORK_REGISTRATION, NULL, &error);
  if (!proxy) {
    if (error)
      g_error_free(error);
    return -2;
  }

  result = g_dbus_proxy_call_sync(proxy, "GetProperties", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, OFONO_TIMEOUT_MS,
                                  NULL, &error);
  if (!result) {
    if (error)
      g_error_free(error);
    g_object_unref(proxy);
    return -3;
  }

  GVariant *props = g_variant_get_child_value(result, 0);
  if (props) {
    GVariant *v =
        g_variant_lookup_value(props, "Technology", G_VARIANT_TYPE_STRING);
    if (v) {
      const gchar *tech_str = g_variant_get_string(v, NULL);
      if (tech_str) {
        strncpy(tech, tech_str, size - 1);
        tech[size - 1] = '\0';
        ret = 0;
      }
      g_variant_unref(v);
    }
    g_variant_unref(props);
  }

  g_variant_unref(result);
  g_object_unref(proxy);
  return ret;
}

static void nr_lte_apply_nr_preference(void) {
  const char *modem = get_current_modem_path();
  printf("[NrLteSwitch] TechnologyPreference: NR 5G only -> NR 5G/LTE auto\n");
  (void)ofono_network_set_mode_sync(modem, 8, OFONO_TIMEOUT_MS);
  (void)ofono_network_set_mode_sync(modem, 9, OFONO_TIMEOUT_MS);
}

static void nr_lte_reset_phase1(void) {
  g_nr_lte_phase = 1;
  g_nr_lte_switch_count = 0;
  g_nr_lte_cooldown = 0;
  g_nr_lte_cooldown_counter = 0;
  g_nr_lte_nr_streak = 0;
  g_nr_lte_last_switch = 0;
}

static void nr_lte_switch_tick(void) {
  if (access(NR_LTE_SWITCH_ON_FILE, F_OK) != 0)
    return;

  int up = nr_lte_uptime_sec();
  if (up >= 0 && up < NR_LTE_BOOT_GRACE_S)
    return;

  char tech[32];
  if (nr_lte_get_registration_technology(tech, sizeof(tech)) != 0 ||
      tech[0] == '\0')
    return;

  if (strcmp(tech, "NR") == 0) {
    g_nr_lte_nr_streak++;
    if (g_nr_lte_nr_streak >= 2) {
      printf("[NrLteSwitch] consecutive NR -> reset phase 1\n");
      nr_lte_reset_phase1();
    }
    return;
  }

  if (strcmp(tech, "LTE") != 0)
    return;

  g_nr_lte_nr_streak = 0;

  if (outage_cell_active())
    return;

  if (g_nr_lte_phase == 1) {
    if (g_nr_lte_switch_count < NR_LTE_PHASE1_MAX_SWITCHES) {
      nr_lte_apply_nr_preference();
      g_nr_lte_switch_count++;
      printf("[NrLteSwitch] phase1 switch %d/%d\n", g_nr_lte_switch_count,
             NR_LTE_PHASE1_MAX_SWITCHES);
    } else {
      g_nr_lte_phase = 2;
      g_nr_lte_cooldown = 10;
      g_nr_lte_cooldown_counter = g_nr_lte_cooldown;
      g_nr_lte_last_switch = 0;
      printf("[NrLteSwitch] phase1 exhausted -> phase2 cooldown=%d min\n",
             g_nr_lte_cooldown);
    }
  } else if (g_nr_lte_cooldown_counter > 0) {
    g_nr_lte_cooldown_counter--;
    if (g_nr_lte_cooldown_counter == 0)
      g_nr_lte_last_switch = 0;
  } else if (g_nr_lte_last_switch == 0) {
    nr_lte_apply_nr_preference();
    g_nr_lte_last_switch = 1;
  } else {
    if (g_nr_lte_cooldown == 10)
      g_nr_lte_cooldown = 30;
    else
      g_nr_lte_cooldown = 60;
    g_nr_lte_cooldown_counter = g_nr_lte_cooldown;
    g_nr_lte_last_switch = 0;
    printf("[NrLteSwitch] phase2 cooldown escalate -> %d min\n",
           g_nr_lte_cooldown);
  }
}

static int outage_usb0_has_inet(void) {
  return system("ifconfig usb0 2>/dev/null | grep -q 'inet addr:'") == 0;
}

static int outage_total_due(int streak, int first, int every) {
  if (streak < first || every <= 0)
    return 0;
  return ((streak - first) % every) == 0;
}

static int outage_reboot_count_for_day(const char *want_day) {
  FILE *f = fopen(OUTAGE_REBOOT_DAY_FILE, "r");
  if (!f)
    return 0;
  char stored_day[16] = {0};
  int stored_cnt = 0;
  if (fscanf(f, "%15s %d", stored_day, &stored_cnt) == 2 &&
      strcmp(stored_day, want_day) == 0) {
    fclose(f);
    return stored_cnt;
  }
  fclose(f);
  return 0;
}

static void outage_today_str(char *buf, size_t size) {
  time_t now = time(NULL);
  struct tm tm;
  if (now == (time_t)-1 || !localtime_r(&now, &tm)) {
    strncpy(buf, "unknown", size - 1);
    buf[size - 1] = '\0';
    return;
  }
  strftime(buf, size, "%Y-%m-%d", &tm);
}

static int outage_is_iso_day(const char *day) {
  return day && strlen(day) == 10 && day[4] == '-' && day[7] == '-';
}

static void outage_budget_accounting_day(char *chosen, size_t size) {
  char clock_day[16];
  char anchor[16] = {0};
  outage_today_str(clock_day, sizeof(clock_day));

  FILE *pf = fopen(OUTAGE_REBOOT_PENDING_FILE, "r");
  if (pf) {
    char pending_day[16] = {0};
    if (fscanf(pf, "%15s", pending_day) == 1 && outage_is_iso_day(pending_day))
      strncpy(anchor, pending_day, sizeof(anchor) - 1);
    fclose(pf);
  }
  if (anchor[0] == '\0') {
    FILE *df = fopen(OUTAGE_REBOOT_DAY_FILE, "r");
    if (df) {
      char stored_day[16] = {0};
      int cnt = 0;
      if (fscanf(df, "%15s %d", stored_day, &cnt) == 2 &&
          outage_is_iso_day(stored_day))
        strncpy(anchor, stored_day, sizeof(anchor) - 1);
      fclose(df);
    }
  }

  strncpy(chosen, clock_day, size - 1);
  chosen[size - 1] = '\0';
  if (anchor[0] != '\0' && strcmp(clock_day, "unknown") != 0 &&
      strcmp(clock_day, anchor) != 0 && strcmp(clock_day, anchor) < 0)
    strncpy(chosen, anchor, size - 1);
}

static int outage_total_reboot_count_today(void) {
  char day[16];
  outage_budget_accounting_day(day, sizeof(day));
  return outage_reboot_count_for_day(day);
}

static int outage_total_reboot_effective_count(void) {
  int count = outage_total_reboot_count_today();
  if (access(OUTAGE_REBOOT_PENDING_FILE, F_OK) == 0)
    count++;
  return count;
}

static int outage_total_reboot_budget_exhausted(void) {
  return outage_total_reboot_effective_count() >= TOTAL_REBOOT_MAX_DAY;
}

static int outage_commit_pending_reboot_budget(void) {
  FILE *pf = fopen(OUTAGE_REBOOT_PENDING_FILE, "r");
  if (!pf)
    return 0;
  char pending_day[16] = {0};
  char rest[128] = {0};
  if (fscanf(pf, "%15s %127[^\n]", pending_day, rest) < 1) {
    fclose(pf);
    return -1;
  }
  fclose(pf);

  char clock_day[16];
  outage_today_str(clock_day, sizeof(clock_day));
  const char *day =
      outage_is_iso_day(pending_day) ? pending_day : clock_day;
  int count = outage_reboot_count_for_day(day) + 1;

  for (int i = 0; i < 15; i++) {
    FILE *wf = fopen(OUTAGE_REBOOT_DAY_FILE, "w");
    if (!wf) {
      sleep(1);
      continue;
    }
    fprintf(wf, "%s %d\n", day, count);
    fclose(wf);

    FILE *rf = fopen(OUTAGE_REBOOT_DAY_FILE, "r");
    if (!rf) {
      sleep(1);
      continue;
    }
    char got_day[16] = {0};
    int got_cnt = 0;
    int ok = fscanf(rf, "%15s %d", got_day, &got_cnt) == 2 &&
             strcmp(got_day, day) == 0 && got_cnt == count;
    fclose(rf);
    if (ok) {
      unlink(OUTAGE_REBOOT_PENDING_FILE);
      printf("[Watchdog] TOTAL: commit escalate reboot budget (%d/%d day=%s)\n",
             count, TOTAL_REBOOT_MAX_DAY, day);
      return 0;
    }
    sleep(1);
  }
  printf("[Watchdog] TOTAL: commit escalate reboot budget FAILED day=%s count=%d\n",
         day, count);
  return -1;
}

static int outage_in_escalate_window(void) {
  time_t now = time(NULL);
  if (now == (time_t)-1 || g_outage_boot_ts == 0)
    return 0;
  return now < g_outage_boot_ts + TOTAL_ESCALATE_WINDOW_S;
}

static int outage_partial_grace_active(void) {
  FILE *f = fopen(OUTAGE_PARTIAL_GRACE_FILE, "r");
  if (!f)
    return 0;
  long gs = 0;
  if (fscanf(f, "%ld", &gs) != 1) {
    fclose(f);
    return 0;
  }
  fclose(f);
  time_t now = time(NULL);
  if (now == (time_t)-1)
    return 0;
  return now < (time_t)(gs + PARTIAL_GRACE_S);
}

static void outage_partial_grace_clear_if_usb_ok(void) {
  if (access(OUTAGE_PARTIAL_GRACE_FILE, F_OK) != 0)
    return;
  if (outage_usb0_has_inet()) {
    unlink(OUTAGE_PARTIAL_GRACE_FILE);
    printf("[Watchdog] PARTIAL grace cleared - usb0 inet ready\n");
  }
}

static int outage_write_pending(const char *day, const char *kind, int streak) {
  FILE *f = fopen(OUTAGE_REBOOT_PENDING_FILE, "w");
  if (!f)
    return -1;
  fprintf(f, "%s pending %s streak=%d\n", day, kind, streak);
  fclose(f);
  return 0;
}

static void outage_write_partial_grace(void) {
  time_t now = time(NULL);
  if (now == (time_t)-1)
    return;
  FILE *f = fopen(OUTAGE_PARTIAL_GRACE_FILE, "w");
  if (!f)
    return;
  fprintf(f, "%ld\n", (long)now);
  fclose(f);
}

static void outage_try_total_escalate_reboot(void) {
  if (access("/mnt/data/need-usb-renum", F_OK) == 0) {
    printf("[Watchdog] TOTAL: escalate deferred - waiting USB renum (streak=%d)\n",
           g_total_streak);
    return;
  }
  if (outage_partial_grace_active()) {
    printf("[Watchdog] TOTAL: escalate deferred - PARTIAL grace active (streak=%d)\n",
           g_total_streak);
    return;
  }
  if (!outage_usb0_has_inet() && outage_total_reboot_count_today() >= 1) {
    printf("[Watchdog] TOTAL: escalate deferred - usb0 no inet after spend (streak=%d)\n",
           g_total_streak);
    return;
  }
  if (!outage_in_escalate_window()) {
    printf("[Watchdog] TOTAL: escalate skipped - outside window %ds (streak=%d)\n",
           TOTAL_ESCALATE_WINDOW_S, g_total_streak);
    return;
  }
  if (access(OUTAGE_REBOOT_PENDING_FILE, F_OK) == 0) {
    printf("[Watchdog] TOTAL: escalate skipped - pending exists (streak=%d)\n",
           g_total_streak);
    return;
  }
  char day[16];
  outage_budget_accounting_day(day, sizeof(day));
  int count = outage_total_reboot_count_today();
  if (count >= TOTAL_REBOOT_MAX_DAY) {
    printf("[Watchdog] TOTAL: reboot budget exhausted (%d/%d day=%s)\n", count,
           TOTAL_REBOOT_MAX_DAY, day);
    return;
  }
  if (outage_write_pending(day, "streak", g_total_streak) != 0)
    return;
  printf("[Watchdog] TOTAL: escalate soft reboot need-usb-renum (streak=%d pending->%d/%d)\n",
         g_total_streak, count + 1, TOTAL_REBOOT_MAX_DAY);
  sync();
  sleep(2);
  device_reboot();
}

static void outage_try_partial_escalate_reboot(void) {
  if (access(OUTAGE_REBOOT_PENDING_FILE, F_OK) == 0) {
    printf("[Watchdog] PARTIAL: escalate skipped - pending exists (streak=%d)\n",
           g_partial_streak);
    return;
  }
  char day[16];
  outage_budget_accounting_day(day, sizeof(day));
  int count = outage_total_reboot_count_today();
  if (count >= TOTAL_REBOOT_MAX_DAY) {
    printf("[Watchdog] PARTIAL: reboot budget exhausted (%d/%d day=%s streak=%d)\n",
           count, TOTAL_REBOOT_MAX_DAY, day, g_partial_streak);
    return;
  }
  if (outage_write_pending(day, "partial", g_partial_streak) != 0)
    return;
  printf("[Watchdog] PARTIAL: escalate soft reboot need-usb-renum (streak=%d pending->%d/%d)\n",
         g_partial_streak, count + 1, TOTAL_REBOOT_MAX_DAY);
  outage_write_partial_grace();
  sync();
  sleep(2);
  device_reboot();
}

static void outage_watchdog_tick(void) {
  int gw = outage_has_gateway();
  int dhcp = outage_dhcp_ok();
  int net = ofono_egress_reachable();
  int cell = outage_cell_active();
  time_t now = time(NULL);
  int in_grace =
      g_outage_boot_ts != 0 && now != (time_t)-1 &&
      now < g_outage_boot_ts + OUTAGE_BOOT_GRACE_S;

  if (gw)
    outage_partial_grace_clear_if_usb_ok();

  if (!gw || !dhcp) {
    g_total_streak++;
    g_partial_streak = 0;
    printf("[Watchdog] TOTAL gw=%d dhcp=%d streak=%d grace=%d\n", gw, dhcp,
           g_total_streak, in_grace);

    if (in_grace) {
      system("touch /tmp/usb-tether-refresh 2>/dev/null");
    } else if (outage_total_reboot_budget_exhausted()) {
      if (g_total_streak == TOTAL_REBOOT_STREAK ||
          (g_total_streak % TOTAL_REBOOT_STREAK) == 0) {
        printf("[Watchdog] TOTAL: budget exhausted - skip repair (streak=%d)\n",
               g_total_streak);
      }
    } else {
      if (outage_total_due(g_total_streak, 2, OUTAGE_TOTAL_LIGHT_EVERY)) {
        system("touch /tmp/usb-tether-refresh 2>/dev/null");
        system("touch /tmp/usb-tether-force-repair 2>/dev/null");
        printf("[Watchdog] TOTAL: light repair (streak=%d)\n", g_total_streak);
      }
      if (outage_total_due(g_total_streak, 6, OUTAGE_TOTAL_UDC_EVERY)) {
        printf("[Watchdog] TOTAL: udc/fix-rndis (streak=%d)\n", g_total_streak);
        system("touch /tmp/usb-tether-force-repair 2>/dev/null");
        (void)usb_mode_ensure_rndis_link();
      }
      if (g_total_streak == TOTAL_REBOOT_STREAK ||
          g_total_streak == TOTAL_REBOOT_STREAK * 2) {
        outage_try_total_escalate_reboot();
      }
    }
  } else if (!net) {
    g_partial_streak++;
    g_total_streak = 0;

    if (g_partial_streak < OUTAGE_PARTIAL_ACT_STREAK) {
      printf("[Watchdog] PARTIAL_BLIP gw=1 net=0 cell=%d streak=%d\n", cell,
             g_partial_streak);
    } else {
      printf("[Watchdog] PARTIAL gw=1 net=0 cell=%d streak=%d\n", cell,
             g_partial_streak);

      if (g_partial_streak == OUTAGE_PARTIAL_ACT_STREAK)
        system("touch /tmp/usb-tether-refresh 2>/dev/null");

      if (g_partial_streak == PARTIAL_BOUNCE_STREAK ||
          g_partial_streak == PARTIAL_BOUNCE_RETRY) {
        int br = ofono_bounce_pdp();
        if (br == -2)
          g_partial_bounce_retry_pending = 1;
        else if (br == 0)
          g_partial_bounce_count++;
      } else if (g_partial_bounce_retry_pending &&
                 g_partial_streak >= PARTIAL_BOUNCE_STREAK &&
                 g_partial_streak < PARTIAL_REBOOT_STREAK) {
        int br = ofono_bounce_pdp();
        if (br != -2) {
          g_partial_bounce_count++;
          g_partial_bounce_retry_pending = 0;
        }
      }

      if (g_partial_streak == PARTIAL_REBOOT_STREAK) {
        if (outage_total_reboot_budget_exhausted()) {
          printf("[Watchdog] PARTIAL: budget exhausted - skip reboot (streak=%d)\n",
                 g_partial_streak);
        } else {
          outage_try_partial_escalate_reboot();
        }
      }
    }
  } else {
    if (g_total_streak > 0 || g_partial_streak > 0) {
      printf("[Watchdog] RECOVERED gw=%d dhcp=%d net=%d\n", gw, dhcp, net);
    }
    g_total_streak = 0;
    g_partial_streak = 0;
    g_partial_bounce_count = 0;
    g_partial_bounce_retry_pending = 0;
  }
}

/**
 * Watchdog 线程函数
 */
static void *data_watchdog_thread(void *arg) {
  (void)arg;
  char status[256];

  g_outage_boot_ts = time(NULL);
  g_partial_streak = 0;
  g_total_streak = 0;
  g_partial_bounce_count = 0;
  g_partial_bounce_retry_pending = 0;
  nr_lte_reset_phase1();
  g_nr_lte_last_tick_ts = 0;
  outage_commit_pending_reboot_budget();

  printf("[Watchdog] 数据连接监控线程已启动 (间隔: %d秒, PARTIAL bounce@%d/%d reboot@%d, TOTAL reboot@%d)\n",
         g_watchdog_interval, PARTIAL_BOUNCE_STREAK, PARTIAL_BOUNCE_RETRY,
         PARTIAL_REBOOT_STREAK, TOTAL_REBOOT_STREAK);

  while (g_watchdog_running) {
    outage_watchdog_tick();

    {
      time_t nr_now = time(NULL);
      if (nr_now != (time_t)-1) {
        if (g_nr_lte_last_tick_ts == 0)
          g_nr_lte_last_tick_ts = nr_now;
        else if (nr_now >= g_nr_lte_last_tick_ts + NR_LTE_TICK_INTERVAL_S) {
          g_nr_lte_last_tick_ts = nr_now;
          nr_lte_switch_tick();
        }
      }
    }

    if (ofono_check_and_restore_data(status, sizeof(status)) >= 0) {
      if (strcmp(status, g_last_watchdog_status) != 0) {
        printf("[Watchdog] %s\n", status);
        strncpy(g_last_watchdog_status, status,
                sizeof(g_last_watchdog_status) - 1);
      }
    }

    for (int i = 0; i < g_watchdog_interval && g_watchdog_running; i++) {
      sleep(1);
    }
  }

  printf("[Watchdog] 数据连接监控线程已停止\n");
  return NULL;
}

/**
 * 启动数据连接 Watchdog 线程
 */
int ofono_start_data_watchdog(int interval_secs) {
  if (g_watchdog_running) {
    printf("[Watchdog] 已在运行中\n");
    return 0;
  }

  g_watchdog_interval = (interval_secs > 0) ? interval_secs : 10;
  g_watchdog_running = 1;
  g_last_watchdog_status[0] = '\0';

  if (pthread_create(&g_watchdog_thread, NULL, data_watchdog_thread, NULL) !=
      0) {
    g_watchdog_running = 0;
    printf("[Watchdog] 创建线程失败\n");
    return -1;
  }

  pthread_detach(g_watchdog_thread);
  return 0;
}

/**
 * 停止数据连接 Watchdog 线程
 */
void ofono_stop_data_watchdog(void) {
  if (!g_watchdog_running) {
    return;
  }

  g_watchdog_running = 0;
  /* 线程会在下一次循环时自动退出 */
}

/**
 * 检查 Watchdog 是否运行中
 */
int ofono_is_watchdog_running(void) { return g_watchdog_running ? 1 : 0; }

/* ==================== 数据连接监听实现 (DBus 信号驱动) ==================== */

static guint g_context_signal_id = 0;      /* ConnectionContext 信号订阅 ID */
static guint g_network_signal_id = 0;      /* NetworkRegistration 信号订阅 ID */
static guint g_manager_signal_id = 0;      /* Manager 信号订阅 ID (监听切卡) */
static guint g_ofono_monitor_watch_id = 0; /* oFono 服务监控 ID */
static volatile int g_data_monitor_running = 0;
static GDBusConnection *g_monitor_dbus_conn = NULL;
static guint g_restore_timeout_id = 0; /* 延迟恢复定时器 ID */

/* 前向声明 */
static void subscribe_data_monitor_signals(void);
static void unsubscribe_data_monitor_signals(void);

/**
 * 延迟恢复数据连接的回调函数
 */
static gboolean delayed_restore_data(gpointer user_data) {
  (void)user_data;

  g_restore_timeout_id = 0; /* 清除定时器 ID */

  char result[256];
  if (ofono_check_and_restore_data(result, sizeof(result)) >= 0) {
    printf("[DataMonitor] 恢复结果: %s\n", result);
  }

  return G_SOURCE_REMOVE; /* 只执行一次 */
}

/**
 * ConnectionContext PropertyChanged 信号回调
 * 监听 Active 属性变化，断开时尝试重连
 */
static void on_context_property_changed(
    GDBusConnection *conn, const gchar *sender_name, const gchar *object_path,
    const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
    gpointer user_data) {

  (void)conn;
  (void)sender_name;
  (void)interface_name;
  (void)signal_name;
  (void)user_data;

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sv)"))) {
    return;
  }

  const gchar *prop_name = NULL;
  GVariant *prop_value = NULL;
  g_variant_get(parameters, "(&sv)", &prop_name, &prop_value);

  if (!prop_name || !prop_value) {
    if (prop_value)
      g_variant_unref(prop_value);
    return;
  }

  /* 只关注 Active 属性 */
  if (g_strcmp0(prop_name, "Active") == 0) {
    gboolean active = g_variant_get_boolean(prop_value);
    printf("[DataMonitor] Context %s Active 变化: %s\n", object_path,
           active ? "true" : "false");

    if (!active) {
      /* 数据连接断开，使用 g_timeout_add 延迟恢复（非阻塞） */
      printf("[DataMonitor] 数据连接断开，2秒后尝试恢复...\n");

      /* 取消之前的定时器（如果有） */
      if (g_restore_timeout_id > 0) {
        g_source_remove(g_restore_timeout_id);
      }

      /* 2秒后尝试恢复 */
      g_restore_timeout_id = g_timeout_add(2000, delayed_restore_data, NULL);
    }
  }

  g_variant_unref(prop_value);
}

/**
 * NetworkRegistration PropertyChanged 信号回调
 * 监听 Status 属性变化，注册成功时尝试激活数据连接
 */
static void on_network_property_changed(
    GDBusConnection *conn, const gchar *sender_name, const gchar *object_path,
    const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
    gpointer user_data) {

  (void)conn;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  (void)user_data;

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sv)"))) {
    return;
  }

  const gchar *prop_name = NULL;
  GVariant *prop_value = NULL;
  g_variant_get(parameters, "(&sv)", &prop_name, &prop_value);

  if (!prop_name || !prop_value) {
    if (prop_value)
      g_variant_unref(prop_value);
    return;
  }

  /* 只关注 Status 属性 */
  if (g_strcmp0(prop_name, "Status") == 0) {
    const gchar *status = g_variant_get_string(prop_value, NULL);
    printf("[DataMonitor] 网络注册状态变化: %s\n", status);

    if (g_strcmp0(status, "registered") == 0 ||
        g_strcmp0(status, "roaming") == 0) {
      /* 网络注册成功，立即检查数据连接 */
      printf("[DataMonitor] 网络已注册，检查数据连接...\n");

      char result[256];
      if (ofono_check_and_restore_data(result, sizeof(result)) >= 0) {
        printf("[DataMonitor] 检查结果: %s\n", result);
      }
    }
  }

  g_variant_unref(prop_value);
}

/**
 * Manager PropertyChanged 信号回调
 * 监听 DataCard 属性变化（外部切卡）
 */
static void on_manager_property_changed(
    GDBusConnection *conn, const gchar *sender_name, const gchar *object_path,
    const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
    gpointer user_data) {

  (void)conn;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  (void)user_data;

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sv)"))) {
    return;
  }

  const gchar *prop_name = NULL;
  GVariant *prop_value = NULL;
  g_variant_get(parameters, "(&sv)", &prop_name, &prop_value);

  if (!prop_name || !prop_value) {
    if (prop_value)
      g_variant_unref(prop_value);
    return;
  }

  /* 监听 DataCard 属性变化（切卡） */
  if (g_strcmp0(prop_name, "DataCard") == 0) {
    const gchar *new_datacard = g_variant_get_string(prop_value, NULL);
    printf("[DataMonitor] 检测到切卡: %s\n", new_datacard);

    /* 动态检测方案：不再 close_dbus/init_dbus，所有函数已自动使用
     * get_current_modem_path() 获取正确路径，无需重建连接 */
    printf("[DataMonitor] modem 路径将在下次调用时自动切换到: %s\n",
           new_datacard);

    /* 重新订阅信号（使用新的卡槽路径） */
    printf("[DataMonitor] 重新订阅信号...\n");

    /* 先取消旧的 NetworkRegistration 订阅 */
    if (g_network_signal_id > 0 && g_monitor_dbus_conn) {
      g_dbus_connection_signal_unsubscribe(g_monitor_dbus_conn,
                                           g_network_signal_id);
      g_network_signal_id = 0;
    }

    /* 重新订阅 NetworkRegistration 信号（使用新路径） */
    g_network_signal_id = g_dbus_connection_signal_subscribe(
        g_monitor_dbus_conn, OFONO_SERVICE, "org.ofono.NetworkRegistration",
        "PropertyChanged", new_datacard, /* 使用新的卡槽路径 */
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_network_property_changed, NULL,
        NULL);
    printf("[DataMonitor] NetworkRegistration 信号重新订阅 ID: %u (路径: %s)\n",
           g_network_signal_id, new_datacard);

    /* 等待一小段时间让 oFono 内部状态同步 */
    usleep(300000); /* 300ms */

    /* 立即检查数据连接状态 */
    char result[256];
    if (ofono_check_and_restore_data(result, sizeof(result)) >= 0) {
      printf("[DataMonitor] 切卡后检查: %s\n", result);
    }
  }

  g_variant_unref(prop_value);
}

/**
 * 订阅数据监听信号
 */
static void subscribe_data_monitor_signals(void) {
  GError *error = NULL;
  GVariant *result = NULL;

  if (!g_monitor_dbus_conn) {
    printf("[DataMonitor] D-Bus 未连接，无法订阅信号\n");
    return;
  }

  /* 先取消旧的订阅 */
  unsubscribe_data_monitor_signals();

  /* 添加 D-Bus match 规则 - ConnectionContext PropertyChanged */
  result = g_dbus_connection_call_sync(
      g_monitor_dbus_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "AddMatch",
      g_variant_new("(s)", "type='signal',interface='org.ofono."
                           "ConnectionContext',member='PropertyChanged'"),
      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if (error) {
    printf("[DataMonitor] 添加 ConnectionContext match 规则失败: %s\n",
           error->message);
    g_error_free(error);
    error = NULL;
  } else {
    printf("[DataMonitor] ConnectionContext match 规则添加成功\n");
    if (result)
      g_variant_unref(result);
  }

  /* 添加 D-Bus match 规则 - NetworkRegistration PropertyChanged */
  result = g_dbus_connection_call_sync(
      g_monitor_dbus_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "AddMatch",
      g_variant_new("(s)", "type='signal',interface='org.ofono."
                           "NetworkRegistration',member='PropertyChanged'"),
      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if (error) {
    printf("[DataMonitor] 添加 NetworkRegistration match 规则失败: %s\n",
           error->message);
    g_error_free(error);
    error = NULL;
  } else {
    printf("[DataMonitor] NetworkRegistration match 规则添加成功\n");
    if (result)
      g_variant_unref(result);
  }

  /* 订阅 ConnectionContext PropertyChanged 信号 (所有 context) */
  g_context_signal_id = g_dbus_connection_signal_subscribe(
      g_monitor_dbus_conn, OFONO_SERVICE, "org.ofono.ConnectionContext",
      "PropertyChanged", NULL, /* 监听所有路径 */
      NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_context_property_changed, NULL, NULL);
  printf("[DataMonitor] ConnectionContext 信号订阅 ID: %u\n",
         g_context_signal_id);

  /* 动态获取当前卡槽路径 */
  char slot[16], ril_path[32];
  const char *modem_path = DEFAULT_MODEM_PATH;
  if (get_current_slot(slot, ril_path) == 0 &&
      strcmp(ril_path, "unknown") != 0) {
    modem_path = ril_path;
    printf("[DataMonitor] 使用当前卡槽: %s (%s)\n", slot, modem_path);
  } else {
    printf("[DataMonitor] 使用默认卡槽: %s\n", modem_path);
  }

  /* 订阅 NetworkRegistration PropertyChanged 信号 */
  g_network_signal_id = g_dbus_connection_signal_subscribe(
      g_monitor_dbus_conn, OFONO_SERVICE, "org.ofono.NetworkRegistration",
      "PropertyChanged", modem_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
      on_network_property_changed, NULL, NULL);
  printf("[DataMonitor] NetworkRegistration 信号订阅 ID: %u\n",
         g_network_signal_id);

  /* 添加 D-Bus match 规则 - Manager PropertyChanged (监听切卡) */
  result = g_dbus_connection_call_sync(
      g_monitor_dbus_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "AddMatch",
      g_variant_new("(s)", "type='signal',interface='org.ofono.Manager',member="
                           "'PropertyChanged'"),
      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if (error) {
    printf("[DataMonitor] 添加 Manager match 规则失败: %s\n", error->message);
    g_error_free(error);
    error = NULL;
  } else {
    printf("[DataMonitor] Manager match 规则添加成功\n");
    if (result)
      g_variant_unref(result);
  }

  /* 订阅 Manager PropertyChanged 信号 (监听切卡) */
  g_manager_signal_id = g_dbus_connection_signal_subscribe(
      g_monitor_dbus_conn, OFONO_SERVICE, "org.ofono.Manager",
      "PropertyChanged", "/", /* Manager 路径是根路径 */
      NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_manager_property_changed, NULL, NULL);
  printf("[DataMonitor] Manager 信号订阅 ID: %u (监听切卡)\n",
         g_manager_signal_id);
}

/**
 * 取消数据监听信号订阅
 */
static void unsubscribe_data_monitor_signals(void) {
  if (g_context_signal_id > 0 && g_monitor_dbus_conn) {
    g_dbus_connection_signal_unsubscribe(g_monitor_dbus_conn,
                                         g_context_signal_id);
    printf("[DataMonitor] 已取消 ConnectionContext 信号订阅\n");
  }
  g_context_signal_id = 0;

  if (g_network_signal_id > 0 && g_monitor_dbus_conn) {
    g_dbus_connection_signal_unsubscribe(g_monitor_dbus_conn,
                                         g_network_signal_id);
    printf("[DataMonitor] 已取消 NetworkRegistration 信号订阅\n");
  }
  g_network_signal_id = 0;

  if (g_manager_signal_id > 0 && g_monitor_dbus_conn) {
    g_dbus_connection_signal_unsubscribe(g_monitor_dbus_conn,
                                         g_manager_signal_id);
    printf("[DataMonitor] 已取消 Manager 信号订阅\n");
  }
  g_manager_signal_id = 0;
}

/**
 * oFono 服务出现回调
 */
static void on_ofono_monitor_appeared(GDBusConnection *conn, const gchar *name,
                                      const gchar *name_owner,
                                      gpointer user_data) {
  (void)conn;
  (void)user_data;

  printf("[DataMonitor] oFono 服务已启动: %s (owner: %s)\n", name, name_owner);

  /* 重新订阅信号 */
  subscribe_data_monitor_signals();

  /* 立即检查一次数据连接状态 */
  char result[256];
  if (ofono_check_and_restore_data(result, sizeof(result)) >= 0) {
    printf("[DataMonitor] 初始检查: %s\n", result);
  }
}

/**
 * oFono 服务消失回调
 */
static void on_ofono_monitor_vanished(GDBusConnection *conn, const gchar *name,
                                      gpointer user_data) {
  (void)conn;
  (void)user_data;

  printf("[DataMonitor] oFono 服务已停止: %s\n", name);

  /* 取消信号订阅 */
  unsubscribe_data_monitor_signals();
}

/**
 * 启动数据连接监听（基于 DBus 信号）
 */
int ofono_start_data_monitor(void) {
  GError *error = NULL;

  if (g_data_monitor_running) {
    printf("[DataMonitor] 已在运行中\n");
    return 0;
  }

  printf("[DataMonitor] 启动数据连接监听...\n");

  /* 获取 D-Bus 连接 */
  g_monitor_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!g_monitor_dbus_conn) {
    printf("[DataMonitor] 获取 D-Bus 连接失败: %s\n",
           error ? error->message : "unknown");
    if (error)
      g_error_free(error);
    return -1;
  }

  /* 监控 oFono 服务可用性 */
  g_ofono_monitor_watch_id = g_bus_watch_name_on_connection(
      g_monitor_dbus_conn, OFONO_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
      on_ofono_monitor_appeared, on_ofono_monitor_vanished, NULL, NULL);

  if (g_ofono_monitor_watch_id == 0) {
    printf("[DataMonitor] 监控 oFono 服务失败\n");
    g_object_unref(g_monitor_dbus_conn);
    g_monitor_dbus_conn = NULL;
    return -1;
  }

  g_data_monitor_running = 1;
  printf("[DataMonitor] 数据连接监听已启动 (watch_id: %u)\n",
         g_ofono_monitor_watch_id);

  return 0;
}

/**
 * 停止数据连接监听
 */
void ofono_stop_data_monitor(void) {
  if (!g_data_monitor_running) {
    return;
  }

  printf("[DataMonitor] 停止数据连接监听...\n");

  /* 取消延迟恢复定时器 */
  if (g_restore_timeout_id > 0) {
    g_source_remove(g_restore_timeout_id);
    g_restore_timeout_id = 0;
  }

  /* 取消信号订阅 */
  unsubscribe_data_monitor_signals();

  /* 取消服务监控 */
  if (g_ofono_monitor_watch_id > 0) {
    g_bus_unwatch_name(g_ofono_monitor_watch_id);
    g_ofono_monitor_watch_id = 0;
  }

  /* 释放 D-Bus 连接 */
  if (g_monitor_dbus_conn) {
    g_object_unref(g_monitor_dbus_conn);
    g_monitor_dbus_conn = NULL;
  }

  g_data_monitor_running = 0;
  printf("[DataMonitor] 数据连接监听已停止\n");
}

/**
 * 检查数据连接监听是否运行中
 */
int ofono_is_data_monitor_running(void) {
  return g_data_monitor_running ? 1 : 0;
}
