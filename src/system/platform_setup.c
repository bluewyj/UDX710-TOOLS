/**
 * @file platform_setup.c
 * @brief Idempotent USB/APN platform files (patch-platform-setup.sh +
 *        fix-usb-rndis.sh intent, single server entry point)
 */

#include "platform_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define PLATFORM_MARK_PATH        "/mnt/data/patch-platform.ok"
#define PLATFORM_LOG_PATH         "/mnt/data/logs/patch-platform.log"
#define PLATFORM_USBENUM_PATH     "/mnt/data/usbenum.ini"
#define PLATFORM_MODE_CFG_PATH    "/mnt/data/mode.cfg"
#define PLATFORM_MODE_TMP_PATH    "/mnt/data/mode_tmp.cfg"
#define PLATFORM_CONNMAN_PATH     "/etc/connman/main.conf"
#define PLATFORM_SYS_USBENUM_PATH "/etc/usbenum/usbenum.ini"

static const char USBENUM_INI[] =
    "[machine]\n"
    "machine=udx710-module\n"
    "[property]\n"
    "virtualcn=0\n"
    "diag=1\n"
    "log=1\n"
    "debug=1\n"
    "usbch=mode0\n"
    "iq_vser=open\n"
    "udc=29100000.dwc3\n"
    "AfterPowerLoss=1\n";

static const char CONNMAN_CONF[] =
    "[General]\n"
    "TetheringTechnologies=ethernet,wifi,bluetooth,gadget\n"
    "PersistentTetheringMode=true\n";

static void platform_log(const char *msg) {
    FILE *f;
    time_t now;
    struct tm tm_buf;

    mkdir("/mnt/data/logs", 0755);
    f = fopen(PLATFORM_LOG_PATH, "a");
    if (!f)
        return;
    time(&now);
    localtime_r(&now, &tm_buf);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msg);
    fclose(f);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f;
    char line[256];

    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle) != NULL) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int write_text_file(const char *path, const char *content) {
    FILE *f;

    f = fopen(path, "w");
    if (!f) {
        printf("[platform_setup] ERROR: cannot write %s: %s\n", path,
               strerror(errno));
        return -1;
    }
    if (fputs(content, f) == EOF) {
        printf("[platform_setup] ERROR: write failed %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int ensure_mode_cfg(void) {
    if (access(PLATFORM_MODE_CFG_PATH, F_OK) != 0) {
        if (write_text_file(PLATFORM_MODE_CFG_PATH, "3\n") != 0)
            return -1;
        platform_log("created mode.cfg=3");
    }
    unlink(PLATFORM_MODE_TMP_PATH);
    return 0;
}

static int ensure_usbenum_ini(void) {
    if (access(PLATFORM_USBENUM_PATH, F_OK) == 0 &&
        file_contains(PLATFORM_USBENUM_PATH, "AfterPowerLoss=1")) {
        return 0;
    }
    if (write_text_file(PLATFORM_USBENUM_PATH, USBENUM_INI) != 0)
        return -1;
    platform_log("wrote usbenum.ini (virtualcn=0 AfterPowerLoss=1)");
    return 0;
}

static int ensure_connman_conf(void) {
    if (access(PLATFORM_CONNMAN_PATH, F_OK) == 0)
        return 0;
    if (write_text_file(PLATFORM_CONNMAN_PATH, CONNMAN_CONF) != 0)
        return -1;
    platform_log("created connman main.conf");
    return 0;
}

static int fix_sys_usbenum_virtualcn(void) {
    FILE *in, *out;
    char line[256];
    char tmp_path[512];
    int changed = 0;
    int has_virtualcn = 0;

    if (access(PLATFORM_SYS_USBENUM_PATH, F_OK) != 0)
        return 0;

    in = fopen(PLATFORM_SYS_USBENUM_PATH, "r");
    if (!in)
        return 0;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", PLATFORM_SYS_USBENUM_PATH);
    out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        return 0;
    }

    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "virtualcn=", 10) == 0) {
            has_virtualcn = 1;
            if (strcmp(line, "virtualcn=0\n") != 0) {
                fputs("virtualcn=0\n", out);
                changed = 1;
            } else {
                fputs(line, out);
            }
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);

    if (!has_virtualcn) {
        unlink(tmp_path);
        return 0;
    }
    if (!changed) {
        unlink(tmp_path);
        return 0;
    }
    if (rename(tmp_path, PLATFORM_SYS_USBENUM_PATH) != 0) {
        unlink(tmp_path);
        printf("[platform_setup] WARN: failed to update %s\n",
               PLATFORM_SYS_USBENUM_PATH);
        return 0;
    }
    platform_log("fixed /etc/usbenum/usbenum.ini virtualcn=0");
    return 0;
}

static int touch_mark(void) {
    FILE *f;

    f = fopen(PLATFORM_MARK_PATH, "w");
    if (!f) {
        printf("[platform_setup] ERROR: cannot create %s\n", PLATFORM_MARK_PATH);
        return -1;
    }
    fclose(f);
    sync();
    return 0;
}

int platform_setup_ensure(void) {
    mkdir("/mnt/data", 0755);
    mkdir("/etc/connman", 0755);

    if (access(PLATFORM_MARK_PATH, F_OK) == 0 &&
        access(PLATFORM_USBENUM_PATH, F_OK) == 0 &&
        file_contains(PLATFORM_USBENUM_PATH, "AfterPowerLoss=1")) {
        printf("[platform_setup] skip (already patched)\n");
        platform_log("skip (already patched)");
        return 0;
    }

    if (ensure_mode_cfg() != 0)
        return -1;
    if (ensure_usbenum_ini() != 0)
        return -1;
    if (ensure_connman_conf() != 0)
        return -1;
    (void)fix_sys_usbenum_virtualcn();
    if (touch_mark() != 0)
        return -1;

    printf("[platform_setup] platform files ensured\n");
    return 0;
}
