/**
 * @file usb_mode.h
 * @brief USB模式切换头文件
 */

#ifndef USB_MODE_H
#define USB_MODE_H

#include "mongoose.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB模式定义 */
#define USB_MODE_CDC_NCM  1
#define USB_MODE_CDC_ECM  2
#define USB_MODE_RNDIS    3

/* 配置文件路径 */
#define USB_MODE_CFG_PATH     "/mnt/data/mode.cfg"
#define USB_MODE_TMP_CFG_PATH "/mnt/data/mode_tmp.cfg"

/* USB configfs 路径 */
#define USB_GADGET_PATH       "/sys/kernel/config/usb_gadget/g1"
#define USB_CONFIG_PATH       "/sys/kernel/config/usb_gadget/g1/configs/b.1"
#define USB_FUNCTIONS_PATH    "/sys/kernel/config/usb_gadget/g1/functions"
#define USB_UDC_PATH          "/sys/kernel/config/usb_gadget/g1/UDC"

/* IPA 硬件加速路径 */
#define PAMU3_PROTOCOL_PATH   "/sys/devices/platform/soc/soc:ipa/2b300000.pamu3/pamu3_protocol"

/* USB 网络接口配置 */
#define USB_INTERFACE_IP      "192.168.66.1"
#define USB_INTERFACE_MAC     "CC:E8:AC:C0:00:00"
#define DEFAULT_UDC           "29100000.dwc3"

/**
 * @brief 设置USB模式
 * @param mode 模式值 (1=CDC-NCM, 2=CDC-ECM, 3=RNDIS)
 * @param permanent 是否永久保存 (1=永久, 0=临时)
 * @return 0成功, -1失败
 */
int usb_mode_set(int mode, int permanent);

/**
 * @brief 获取当前USB模式
 * @return 模式值 (1=CDC-NCM, 2=CDC-ECM, 3=RNDIS), -1表示未设置
 */
int usb_mode_get(void);

/**
 * @brief 获取模式名称
 * @param mode 模式值
 * @return 模式名称字符串
 */
const char* usb_mode_name(int mode);

/**
 * @brief USB模式热切换（立即生效，无需重启）
 * @param mode 模式值 (1=CDC-NCM, 2=CDC-ECM, 3=RNDIS)
 * @return 0成功, 负数失败
 */
int usb_mode_switch_advanced(int mode);

/**
 * @brief 获取当前硬件USB模式（从configfs读取）
 * @return 模式值, -1表示无法读取
 */
int usb_mode_get_current_hardware(void);

/**
 * @brief 启动/运行时确保 RNDIS class ef/04/01（必要时 UDC 解绑→写入→重绑）
 * @return 0成功或已正确, 负数失败
 */
int usb_mode_ensure_rndis_link(void);

/**
 * @brief 恢复永久 RNDIS 安全档：写永久配置、清临时、尽力热切并 ensure class
 * @param applied_immediately 非 NULL 时写出是否立即生效 (1=是, 0=否/需重启)
 * @return 0=ok立即生效, 1=ok需重启, <0=失败
 */
int usb_mode_restore_safe(int *applied_immediately);

/* HTTP API处理函数 */
void handle_usb_mode_get(struct mg_connection *c, struct mg_http_message *hm);
void handle_usb_mode_set(struct mg_connection *c, struct mg_http_message *hm);
void handle_usb_advance(struct mg_connection *c, struct mg_http_message *hm);
void handle_usb_mode_restore_safe(struct mg_connection *c, struct mg_http_message *hm);

#ifdef __cplusplus
}
#endif

#endif /* USB_MODE_H */
