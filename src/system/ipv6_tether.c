/**
 * @file ipv6_tether.c
 * @brief USB0 IPv6 NAT66 + 合成全局前缀 + periodic RA (含 RDNSS)
 */

#include "ipv6_tether.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define USB_IFACE "usb0"
#define RA_INTERVAL_SEC 30
#define RA_ROUTER_LIFETIME 1800

static pthread_t g_ra_tid;
static volatile int g_ra_running;
static int g_started;

static int write_proc(const char *path, const char *val) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  fputs(val, f);
  fclose(f);
  return 0;
}

static const char *pick_uplink(void) {
  if (if_nametoindex("sipa_eth0") != 0)
    return "sipa_eth0";
  if (if_nametoindex("ccinet0") != 0)
    return "ccinet0";
  if (if_nametoindex("rmnet_data0") != 0)
    return "rmnet_data0";
  return "sipa_eth0";
}

static int run_sh(const char *cmd) {
  int st = system(cmd);
  if (st == -1)
    return -1;
  if (WIFEXITED(st))
    return WEXITSTATUS(st) == 0 ? 0 : -1;
  return -1;
}

static int apply_nat66(const char *uplink) {
  char cmd[384];

  snprintf(cmd, sizeof(cmd),
           "ip6tables -t nat -C POSTROUTING -o %s -j MASQUERADE 2>/dev/null || "
           "ip6tables -t nat -A POSTROUTING -o %s -j MASQUERADE",
           uplink, uplink);
  if (run_sh(cmd) != 0)
    return -1;

  snprintf(cmd, sizeof(cmd),
           "ip6tables -C FORWARD -i %s -o %s -j ACCEPT 2>/dev/null || "
           "ip6tables -A FORWARD -i %s -o %s -j ACCEPT",
           USB_IFACE, uplink, USB_IFACE, uplink);
  (void)run_sh(cmd);

  snprintf(cmd, sizeof(cmd),
           "ip6tables -C FORWARD -i %s -o %s -m state --state "
           "RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || "
           "ip6tables -A FORWARD -i %s -o %s -m state --state "
           "RELATED,ESTABLISHED -j ACCEPT",
           uplink, USB_IFACE, uplink, USB_IFACE);
  (void)run_sh(cmd);

  /* 清除旧 route_test 可能留下的 DROP */
  snprintf(cmd, sizeof(cmd),
           "ip6tables -D FORWARD -i %s -o %s -j DROP 2>/dev/null || true",
           USB_IFACE, uplink);
  (void)run_sh(cmd);
  return 0;
}

#pragma pack(push, 1)
/* RFC 6106 RDNSS — type 25, len=5 for two addresses */
struct nd_opt_rdnss2 {
  uint8_t type;
  uint8_t len;
  uint16_t reserved;
  uint32_t lifetime;
  uint8_t addr1[16];
  uint8_t addr2[16];
};

struct ra_pkt {
  struct nd_router_advert ra;
  struct nd_opt_prefix_info pi;
  struct nd_opt_rdnss2 rdnss;
};
#pragma pack(pop)

#ifndef ND_OPT_RDNSS
#define ND_OPT_RDNSS 25
#endif

/* Public recursive DNS so Windows NCSI/IPv6 name resolve works on NAT66 path */
static const char *k_rdnss1 = "2400:3200::1";
static const char *k_rdnss2 = "2001:4860:4860::8888";

static int send_one_ra(int ifindex) {
  int fd;
  struct sockaddr_in6 dst;
  struct ra_pkt pkt;
  int hops = 255;
  unsigned char prefix[16];

  fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
  if (fd < 0)
    return -1;

  if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex,
                 sizeof(ifindex)) != 0) {
    close(fd);
    return -1;
  }
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) !=
      0) {
    close(fd);
    return -1;
  }

  memset(&pkt, 0, sizeof(pkt));
  pkt.ra.nd_ra_type = ND_ROUTER_ADVERT;
  pkt.ra.nd_ra_code = 0;
  pkt.ra.nd_ra_cksum = 0;
  pkt.ra.nd_ra_curhoplimit = 64;
  pkt.ra.nd_ra_flags_reserved = 0;
  pkt.ra.nd_ra_router_lifetime = htons(RA_ROUTER_LIFETIME);
  pkt.ra.nd_ra_reachable = 0;
  pkt.ra.nd_ra_retransmit = 0;

  pkt.pi.nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION;
  pkt.pi.nd_opt_pi_len = 4;
  pkt.pi.nd_opt_pi_prefix_len = IPV6_TETHER_LAN_PLEN;
  pkt.pi.nd_opt_pi_flags_reserved = ND_OPT_PI_FLAG_ONLINK | ND_OPT_PI_FLAG_AUTO;
  pkt.pi.nd_opt_pi_valid_time = htonl(7200);
  pkt.pi.nd_opt_pi_preferred_time = htonl(3600);
  if (inet_pton(AF_INET6, IPV6_TETHER_LAN_PREFIX, prefix) != 1) {
    close(fd);
    return -1;
  }
  memcpy(&pkt.pi.nd_opt_pi_prefix, prefix, 16);

  pkt.rdnss.type = ND_OPT_RDNSS;
  pkt.rdnss.len = 5; /* 8 + 32 bytes */
  pkt.rdnss.reserved = 0;
  pkt.rdnss.lifetime = htonl(RA_ROUTER_LIFETIME);
  if (inet_pton(AF_INET6, k_rdnss1, pkt.rdnss.addr1) != 1 ||
      inet_pton(AF_INET6, k_rdnss2, pkt.rdnss.addr2) != 1) {
    close(fd);
    return -1;
  }

  memset(&dst, 0, sizeof(dst));
  dst.sin6_family = AF_INET6;
  dst.sin6_scope_id = (uint32_t)ifindex;
  inet_pton(AF_INET6, "ff02::1", &dst.sin6_addr);

  if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) <
      0) {
    close(fd);
    return -1;
  }

  close(fd);
  return 0;
}

static void *ra_thread(void *arg) {
  unsigned ifindex;
  (void)arg;

  while (g_ra_running) {
    ifindex = if_nametoindex(USB_IFACE);
    if (ifindex != 0)
      (void)send_one_ra((int)ifindex);
    sleep(RA_INTERVAL_SEC);
  }
  return NULL;
}

int ipv6_tether_ensure(void) {
  const char *uplink;
  char cmd[256];
  unsigned ifindex;

  ifindex = if_nametoindex(USB_IFACE);
  if (ifindex == 0)
    return -1;

  uplink = pick_uplink();

  (void)write_proc("/proc/sys/net/ipv6/conf/all/forwarding", "1");
  (void)write_proc("/proc/sys/net/ipv6/conf/usb0/forwarding", "1");
  (void)write_proc("/proc/sys/net/ipv6/conf/usb0/accept_ra", "0");
  snprintf(cmd, sizeof(cmd), "/proc/sys/net/ipv6/conf/%s/forwarding", uplink);
  (void)write_proc(cmd, "1");

  snprintf(cmd, sizeof(cmd), "ip -6 addr replace %s/%d dev %s",
           IPV6_TETHER_LAN_GW, IPV6_TETHER_LAN_PLEN, USB_IFACE);
  if (run_sh(cmd) != 0)
    return -1;

  /* 清掉旧 ULA，避免 PC 仍挂 fd66（Win NLA 会判 LocalNetwork） */
  (void)run_sh("ip -6 addr del fd66:6677::1/64 dev usb0 2>/dev/null || true");

  if (apply_nat66(uplink) != 0)
    return -1;

  if (!g_started) {
    g_ra_running = 1;
    if (pthread_create(&g_ra_tid, NULL, ra_thread, NULL) == 0) {
      pthread_detach(g_ra_tid);
      g_started = 1;
    } else {
      g_ra_running = 0;
      return -1;
    }
  }

  (void)send_one_ra((int)ifindex);
  printf("[boot] ipv6 tether ensured (LAN %s via %s)\n", IPV6_TETHER_LAN_GW,
         uplink);
  return 0;
}
