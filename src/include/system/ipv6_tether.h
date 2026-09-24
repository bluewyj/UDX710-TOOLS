/**
 * @file ipv6_tether.h
 * @brief USB RNDIS IPv6 共享：NAT66 + 合成全局前缀 + Router Advertisement
 *
 * 运营商给蜂窝口 /64 全局地址，无法再拆给 PC。对 PC 使用合成全局前缀
 * （非 ULA：Windows 对 fd00::/8 会显示「无 Internet」）+ ip6tables MASQUERADE，
 * 并用 RA（含 RDNSS）做 SLAAC / DNS。
 */

#ifndef IPV6_TETHER_H
#define IPV6_TETHER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 合成全局前缀（2000::/3）。勿用 fd00::/8 ULA，否则 Win NLA=LocalNetwork */
#define IPV6_TETHER_LAN_PREFIX "2406:6677:66:77::"
#define IPV6_TETHER_LAN_GW     "2406:6677:66:77::1"
#define IPV6_TETHER_LAN_PLEN   64

/* 兼容旧宏名 */
#define IPV6_TETHER_ULA_PREFIX IPV6_TETHER_LAN_PREFIX
#define IPV6_TETHER_ULA_GW     IPV6_TETHER_LAN_GW
#define IPV6_TETHER_ULA_PLEN   IPV6_TETHER_LAN_PLEN

/**
 * 确保 IPv6 tether：转发、usb0 前缀、NAT66、启动 RA 线程（幂等）。
 * @return 0 成功，-1 失败（usb0 未就绪等）
 */
int ipv6_tether_ensure(void);

#ifdef __cplusplus
}
#endif

#endif /* IPV6_TETHER_H */
