/**
 * @file main.c
 * @brief 服务器主程序入口 (对应 Go: main.go)
 */

#include "http_server.h"
#include "netif.h"
#include "ofono.h"
#include "platform_setup.h"
#include "usb_mode.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *ofono_usb_share_at_thread(void *arg) {
  (void)arg;
  if (ofono_enable_usb_share_at() != 0)
    fprintf(stderr, "警告: USB share AT 发送失败\n");
  return NULL;
}

/*
 * Boot path after absorb: 6677-boot starts server only; loader may be missing
 * or non-executable. Outage/APN code only touches tether refresh flags — so
 * ensure usb-tether.sh itself is running or PC never gets 192.168.66.x.
 */
static void ensure_usb_tether_daemon(void) {
  int running;

  if (access("/home/root/usb-tether.sh", F_OK) != 0) {
    fprintf(stderr, "警告: /home/root/usb-tether.sh 缺失，跳过 USB DHCP\n");
    return;
  }

  (void)system("chmod 755 /home/root/usb-tether.sh 2>/dev/null");

  running = (system("ps | grep '[u]sb-tether' >/dev/null 2>&1") == 0);
  if (running) {
    printf("[boot] usb-tether already running\n");
    return;
  }

  /* Clear stale pid so tether's own lock does not no-op after crash */
  (void)system("rm -f /tmp/usb-tether.pid 2>/dev/null");
  if (system("setsid /home/root/usb-tether.sh >> /tmp/usb-tether.log 2>&1 &") !=
      0) {
    fprintf(stderr, "警告: 启动 usb-tether.sh 失败\n");
    return;
  }
  printf("[boot] usb-tether started\n");
}

#define SERVER_HOME "/home/root/6677"

int main(int argc, char *argv[]) {
  const char *port = "80";
  char cwd_before[256] = {0};
  char cwd_after[256] = {0};

  /* 解析命令行参数：init 曾传字面量 "boot"（非端口），映射到生产端口 80 */
  if (argc > 1 && argv[1] && argv[1][0] != '\0') {
    if (strcmp(argv[1], "boot") == 0)
      port = "80";
    else
      port = argv[1];
  }

  printf("=== ofono-server (C version) ===\n");

  /*
   * 6677-boot 以 CWD=/ 启动 server；静态页在 ./dist。未 chdir 时
   * boot→80 能监听但主页 404。统一切到安装目录后再开 HTTP/DB。
   */
  if (getcwd(cwd_before, sizeof(cwd_before)) == NULL)
    snprintf(cwd_before, sizeof(cwd_before), "?");
  if (chdir(SERVER_HOME) != 0) {
    fprintf(stderr, "警告: chdir %s 失败: 静态页/DB 可能不可用\n", SERVER_HOME);
  } else {
    if (getcwd(cwd_after, sizeof(cwd_after)) == NULL)
      snprintf(cwd_after, sizeof(cwd_after), SERVER_HOME);
    printf("[boot] cwd %s -> %s\n", cwd_before, cwd_after);
  }

  /* 同步系统时间：镜像无 ntpdate，用 ntpd one-shot */
  system("killall ntpd 2>/dev/null; /usr/sbin/ntpd -gq -x >/dev/null 2>&1; "
         "/etc/init.d/ntpd start >/dev/null 2>&1 &");

  /* 初始化 ofono D-Bus 连接 */
  if (!ofono_init()) {
    fprintf(stderr, "警告: ofono D-Bus 连接失败，部分功能可能不可用\n");
  }

  /* USB share AT：后台发送，不阻塞 HTTP/管理面启动 */
  {
    pthread_t usb_share_tid;
    if (pthread_create(&usb_share_tid, NULL, ofono_usb_share_at_thread, NULL) ==
        0)
      pthread_detach(usb_share_tid);
  }

  /* 初始化网络接口监听（自动恢复之前启用的监听） */
  init_netif();

  /* 启动数据连接监听（无论当前状态） */
  printf("启动数据连接监听...\n");
  ofono_start_data_monitor();

  /* 定时巡检：PARTIAL/TOTAL outage streak（30s 对齐壳 INTERVAL） */
  printf("启动数据连接 Watchdog (30s)...\n");
  if (ofono_start_data_watchdog(30) != 0) {
    fprintf(stderr, "警告: 数据连接 Watchdog 启动失败\n");
  }

  /* 首启幂等写入 platform USB/APN 配置文件 */
  if (platform_setup_ensure() != 0) {
    fprintf(stderr, "警告: platform_setup_ensure 失败\n");
  }

  /* 启动时纠正 RNDIS class（必要时 UDC 周期，对齐 fix-rndis-link.sh） */
  if (usb_mode_ensure_rndis_link() != 0) {
    fprintf(stderr, "警告: usb_mode_ensure_rndis_link 失败\n");
  }

  /* RNDIS DHCP：保证 usb-tether 守护进程在跑（不依赖 loader cron） */
  ensure_usb_tether_daemon();

  /* 启动 HTTP 服务器 */
  if (http_server_start(port) != 0) {
    fprintf(stderr, "服务器启动失败\n");
    ofono_stop_data_watchdog();
    ofono_stop_data_monitor();
    ofono_deinit();
    return 1;
  }

  /* 运行事件循环 */
  http_server_run();

  /* 清理 */
  http_server_stop();
  ofono_stop_data_watchdog();
  ofono_stop_data_monitor();
  ofono_deinit();

  return 0;
}
