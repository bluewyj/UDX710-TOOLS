/**
 * @file update.h
 * @brief OTA更新系统
 */

#ifndef UPDATE_H
#define UPDATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 当前固件版本 */
#define FIRMWARE_VERSION "2.3.5"

/* 更新文件路径 */
#define UPDATE_TMP_DIR "/tmp"
#define UPDATE_ZIP_PATH "/tmp/update.zip"
#define UPDATE_EXTRACT_DIR "/tmp/update"
#define UPDATE_INSTALL_SCRIPT "/tmp/update/install.sh"
#define UPDATE_META_PATH "/tmp/update/meta.json"
#define UPDATE_RUNTIME_BINARY "/home/root/udx710"
#define UPDATE_RUNTIME_WWW "/home/root/www"
#define UPDATE_STAGING_BINARY "/tmp/update/udx710"
#define UPDATE_STAGING_WWW "/tmp/update/www"
#define UPDATE_EXPECTED_ARCH "aarch64-unknown-linux-musl"

/* 版本检查URL（编译时嵌入） */
#define UPDATE_CHECK_URL "https://gitee.com/C_Rabe/leo/raw/master/version.json"

/* 安装脚本签名配置文件 */
#define UPDATE_CONFIG_FILE "/tmp/update/configuration.json"

/* 版本信息结构 */
typedef struct {
  char version[32];
  char url[512];
  char changelog[1024];
  size_t size;
  int required;
} update_info_t;

/* OTA 状态机：pending 摘要 */
typedef struct {
  char current_version[32];
  int pending;           /* 1=有待应用更新 */
  char kind[16];         /* "meta" | "legacy" | "" */
  char meta_version[32]; /* meta.json 中的 version，无则为空 */
} update_status_t;

/**
 * @brief 获取当前版本
 * @return 版本字符串
 */
const char *update_get_version(void);

/**
 * @brief 获取嵌入的版本检查URL
 * @return URL字符串
 */
const char *update_get_embedded_url(void);

/**
 * @brief 从URL下载更新包
 * @param url 下载链接
 * @return 0成功, -1失败
 */
int update_download(const char *url);

/**
 * @brief 解压更新包
 * @return 0成功, -1失败
 */
int update_extract(void);

/**
 * @brief 执行安装脚本
 * @param output 输出缓冲区
 * @param size 缓冲区大小
 * @return 0成功, -1失败
 */
int update_install(char *output, size_t size);

/**
 * @brief 清理更新临时文件
 */
void update_cleanup(void);

/**
 * @brief 检查远程版本
 * @param check_url 版本检查URL
 * @param info 版本信息输出
 * @return 0成功, -1失败
 */
int update_check_version(const char *check_url, update_info_t *info);

/**
 * @brief 获取 OTA 状态（当前版本 + pending 摘要）
 * @param st 状态输出
 * @return 0成功, -1失败
 */
int update_get_status(update_status_t *st);

/**
 * @brief 取消待应用更新（清理 zip + staging）
 * @return 0成功
 */
int update_cancel(void);

/**
 * @brief 应用待更新（meta 或 legacy）；restart_now 为真则延迟重启
 * @param restart_now 1=应用成功后重启, 0=不重启
 * @param output 输出缓冲区
 * @param size 缓冲区大小
 * @return 0成功, -1失败（失败时保留 staging）
 */
int update_apply(int restart_now, char *output, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* UPDATE_H */
