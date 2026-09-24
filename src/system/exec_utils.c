/**
 * @file exec_utils.c
 * @brief Command execution utilities (Go: system/exec.go)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include "exec_utils.h"

int run_command(char *output, size_t size, const char *cmd, ...) {
    va_list args;
    char *argv[32];
    int argc = 0;

    /* 构建参数数组 */
    argv[argc++] = (char *)cmd;
    va_start(args, cmd);
    char *arg;
    while ((arg = va_arg(args, char *)) != NULL && argc < 31) {
        argv[argc++] = arg;
    }
    va_end(args);
    argv[argc] = NULL;

    /* 创建管道 */
    int pipefd[2];
    if (pipe(pipefd) == -1) return -1;

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* 子进程 */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(cmd, argv);
        _exit(127);
    }

    /* 父进程 */
    close(pipefd[1]);

    /* 读取输出 */
    size_t total = 0;
    ssize_t n;
    while (total < size - 1 && (n = read(pipefd[0], output + total, size - 1 - total)) > 0) {
        total += n;
    }
    output[total] = '\0';
    close(pipefd[0]);

    /* 等待子进程 */
    int status;
    waitpid(pid, &status, 0);

    /* 去除尾部空白 */
    while (total > 0 && (output[total-1] == '\n' || output[total-1] == '\r' || output[total-1] == ' ')) {
        output[--total] = '\0';
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int run_command_timeout(int timeout_sec, char *output, size_t size, const char *cmd, ...) {
    /* 简化实现：直接调用 run_command */
    /* TODO: 实现真正的超时机制 */
    va_list args;
    va_start(args, cmd);
    int ret = run_command(output, size, cmd, args);
    va_end(args);
    return ret;
}

void device_reboot(void) {
    char buf[512];
    /*
     * ImmortalWrt RNDIS sleep-death: soft reboot with cable in leaves host
     * rndis_host TX-watchdog; static 66.x cannot ARP. Simulate unplug first:
     * unbind UDC → host disconnect → reboot → fresh enum on boot.
     * Prefer /home/root/soft-reboot-safe.sh when installed by OTA.
     */
    run_command(buf, sizeof(buf), "sh", "-c",
                "if [ -x /home/root/soft-reboot-safe.sh ]; then "
                "  exec /home/root/soft-reboot-safe.sh; "
                "fi; "
                "echo none > /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null; "
                "sleep 2; "
                "rm -f /mnt/data/need-usb-renum; "
                "touch /mnt/data/usb-clean-reboot 2>/dev/null; "
                "sync; sleep 1; sync; reboot",
                NULL);
}

void device_poweroff(void) {
    char buf[64];
    run_command(buf, sizeof(buf), "poweroff", NULL);
}

int clear_cache(void) {
    char buf[64];
    run_command(buf, sizeof(buf), "sync", NULL);
    return run_command(buf, sizeof(buf), "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches", NULL);
}
