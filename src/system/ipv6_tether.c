/**
 * @file ipv6_tether.c
 * @brief USB0 IPv6 NAT66 + ULA + periodic RA for Windows SLAAC
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define USB_IFACE "usb0"
#define RA_INTERVAL_SEC 30
#define RA_ROUTER_LIFETIME 1800

static pthread_t g_ra_tid;
static volatile int g_ra_running;
static int g_started;

/* #region agent log */
static void dbg_log(const char *hid, const char *loc, const char *msg,
                    const char *data_json) {
  FILE *f;
  struct timespec ts;
  long long ms;

  mkdir("/mnt/data/logs", 0755);
  f = fopen("/mnt/data/logs/debug-aa8e5b.log", "a");
  if (!f)
    return;
  clock_gettime(CLOCK_REALTIME, &ts);
  ms = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
  fprintf(f,
          "{\"sessionId\":\"aa8e5b\",\"runId\":\"ipv6-fix\",\"hypothesisId\":\"%s\","
          "\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
          hid, loc, msg, data_json ? data_json : "{}", ms);
  fclose(f);
}
/* #endregion */

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
struct ra_pkt {
  struct nd_router_advert ra;
  struct nd_opt_prefix_info pi;
};
#pragma pack(pop)

static int send_one_ra(int ifindex) {
  int fd;
  struct sockaddr_in6 dst;
  struct ra_pkt pkt;
  struct ipv6_mreq mreq;
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
  pkt.pi.nd_opt_pi_prefix_len = IPV6_TETHER_ULA_PLEN;
  pkt.pi.nd_opt_pi_flags_reserved = ND_OPT_PI_FLAG_ONLINK | ND_OPT_PI_FLAG_AUTO;
  pkt.pi.nd_opt_pi_valid_time = htonl(7200);
  pkt.pi.nd_opt_pi_preferred_time = htonl(3600);
  if (inet_pton(AF_INET6, IPV6_TETHER_ULA_PREFIX, prefix) != 1) {
    close(fd);
    return -1;
  }
  memcpy(&pkt.pi.nd_opt_pi_prefix, prefix, 16);

  memset(&dst, 0, sizeof(dst));
  dst.sin6_family = AF_INET6;
  dst.sin6_scope_id = (uint32_t)ifindex;
  inet_pton(AF_INET6, "ff02::1", &dst.sin6_addr);

  if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) <
      0) {
    close(fd);
    return -1;
  }

  (void)mreq;
  close(fd);
  return 0;
}

static void *ra_thread(void *arg) {
  unsigned ifindex = if_nametoindex(USB_IFACE);
  int ok = 0;
  int sent_logs = 0;
  (void)arg;

  /* #region agent log */
  dbg_log("H3", "ipv6_tether.c:ra_thread", "ra thread start",
          ifindex ? "{\"if\":\"usb0\"}" : "{\"if\":\"missing\"}");
  /* #endregion */

  while (g_ra_running) {
    ifindex = if_nametoindex(USB_IFACE);
    if (ifindex != 0) {
      ok = (send_one_ra((int)ifindex) == 0);
      /* #region agent log */
      if (sent_logs < 3) {
        char data[80];
        snprintf(data, sizeof(data), "{\"ok\":%d,\"ifindex\":%u}", ok, ifindex);
        dbg_log("H3", "ipv6_tether.c:ra_send", "ra sent", data);
        sent_logs++;
      }
      /* #endregion */
    }
    sleep(RA_INTERVAL_SEC);
  }
  return NULL;
}

int ipv6_tether_ensure(void) {
  const char *uplink;
  char cmd[256];
  char data[160];
  unsigned ifindex;

  ifindex = if_nametoindex(USB_IFACE);
  if (ifindex == 0) {
    /* #region agent log */
    dbg_log("H3", "ipv6_tether.c:ensure", "usb0 missing", "{}");
    /* #endregion */
    return -1;
  }

  uplink = pick_uplink();

  (void)write_proc("/proc/sys/net/ipv6/conf/all/forwarding", "1");
  (void)write_proc("/proc/sys/net/ipv6/conf/usb0/forwarding", "1");
  (void)write_proc("/proc/sys/net/ipv6/conf/usb0/accept_ra", "0");
  snprintf(cmd, sizeof(cmd), "/proc/sys/net/ipv6/conf/%s/forwarding", uplink);
  (void)write_proc(cmd, "1");

  snprintf(cmd, sizeof(cmd), "ip -6 addr replace %s/%d dev %s",
           IPV6_TETHER_ULA_GW, IPV6_TETHER_ULA_PLEN, USB_IFACE);
  if (run_sh(cmd) != 0) {
    /* #region agent log */
    dbg_log("H3", "ipv6_tether.c:ensure", "ula add fail", "{}");
    /* #endregion */
    return -1;
  }

  if (apply_nat66(uplink) != 0) {
    /* #region agent log */
    dbg_log("H3", "ipv6_tether.c:ensure", "nat66 fail", "{}");
    /* #endregion */
    return -1;
  }

  /* #region agent log */
  snprintf(data, sizeof(data),
           "{\"uplink\":\"%s\",\"ula\":\"%s\",\"forwarding\":1}", uplink,
           IPV6_TETHER_ULA_GW);
  dbg_log("H3", "ipv6_tether.c:ensure", "nat66 ok", data);
  /* #endregion */

  if (!g_started) {
    g_ra_running = 1;
    if (pthread_create(&g_ra_tid, NULL, ra_thread, NULL) == 0) {
      pthread_detach(g_ra_tid);
      g_started = 1;
    } else {
      g_ra_running = 0;
      /* #region agent log */
      dbg_log("H3", "ipv6_tether.c:ensure", "ra thread fail", "{}");
      /* #endregion */
      return -1;
    }
  }

  /* 立即立刻发一次 RA */
  (void)send_one_ra((int)ifindex);
  printf("[boot] ipv6 tether ensured (ULA %s via %s)\n", IPV6_TETHER_ULA_GW,
         uplink);
  return 0;
}
