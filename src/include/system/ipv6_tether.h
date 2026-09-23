/**
 * @file ipv6_tether.h
 * @brief USB RNDIS IPv6 共享：NAT66 + ULA + Router Advertisement
 *
 * 运营商给蜂窝口 /64 全局地址；PC 侧无 radvd。用 ULA fd66:6677::/64
 * + ip6tables MASQUERADE 让 PC 上网，并用 RA 做 SLAAC。
 */

#ifndef IPV6_TETHER_H
#define IPV6_TETHER_H

#ifdef __cplusplus
extern "C" {
#endif

#define IPV6_TETHER_ULA_PREFIX "fd66:6677::"
#define IPV6_TETHER_ULA_GW     "fd66:6677::1"
#define IPV6_TETHER_ULA_PLEN   64

/**
 * 确保 IPv6 tether：转发、usb0 ULA、NAT66、启动 RA 线程（幂等）。
 * @return 0 成功，-1 失败（usb0 未就绪等）
 */
int ipv6_tether_ensure(void);

#ifdef __cplusplus
}
#endif

#endif /* IPV6_TETHER_H */
