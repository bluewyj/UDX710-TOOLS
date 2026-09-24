/**
 * @file update.c
 * @brief OTA更新系统实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "update.h"
#include "exec_utils.h"
#include "mongoose.h"

/* 获取当前版本 */
const char* update_get_version(void) {
    return FIRMWARE_VERSION;
}

/* 从URL下载更新包 */
int update_download(const char *url) {
    char output[2048];
    
    if (!url || strlen(url) == 0) {
        return -1;
    }
    
    /* 清理旧文件 */
    update_cleanup();
    
    /* 优先使用curl（更常见），失败再用wget；保留错误输出便于排查 */
    int ret = run_command(output, sizeof(output), "curl", "-k", "-sS", "-L",
                          "--connect-timeout", "15", "--max-time", "120",
                          "-o", UPDATE_ZIP_PATH, url, NULL);
    if (ret != 0) {
        printf("[OTA] curl download failed: %s\n", output);
        ret = run_command(output, sizeof(output), "wget", "--no-check-certificate",
                          "-T", "30", "-q", "-O", UPDATE_ZIP_PATH, url, NULL);
        if (ret != 0) {
            printf("[OTA] wget download failed: %s\n", output);
            return -1;
        }
    }
    
    /* 检查文件是否存在 */
    struct stat st;
    if (stat(UPDATE_ZIP_PATH, &st) != 0 || st.st_size == 0) {
        printf("[OTA] download empty or missing\n");
        return -1;
    }
    printf("[OTA] downloaded %ld bytes from %s\n", (long)st.st_size, url);
    
    return 0;
}

/* 解压更新包 */
int update_extract(void) {
    char output[2048];
    struct stat st;

    /* 检查ZIP文件是否存在 */
    if (stat(UPDATE_ZIP_PATH, &st) != 0) {
        return -1;
    }
    
    /* 创建解压目录 */
    run_command(output, sizeof(output), "rm", "-rf", UPDATE_EXTRACT_DIR, NULL);
    run_command(output, sizeof(output), "mkdir", "-p", UPDATE_EXTRACT_DIR, NULL);
    
    /* 解压ZIP - 优先使用unzip，失败则尝试busybox unzip */
    int ret = run_command(output, sizeof(output), "unzip", "-o", UPDATE_ZIP_PATH, "-d", UPDATE_EXTRACT_DIR, NULL);
    if (ret != 0) {
        ret = run_command(output, sizeof(output), "busybox", "unzip", "-o", UPDATE_ZIP_PATH, "-d", UPDATE_EXTRACT_DIR, NULL);
        if (ret != 0) {
            return -1;
        }
    }
    
    return 0;
}


/* 执行安装脚本 */
int update_install(char *output, size_t size) {
    struct stat st;
    char cmd[512];
    char cmd_out[2048];
    
    /* 检查安装脚本是否存在 */
    if (stat(UPDATE_INSTALL_SCRIPT, &st) != 0) {
        snprintf(output, size, "安装脚本不存在");
        return -1;
    }
    
    /* 添加执行权限 */
    run_command(cmd_out, sizeof(cmd_out), "chmod", "+x", UPDATE_INSTALL_SCRIPT, NULL);

    /*
     * Critical: install.sh uses paths relative to the package root.
     * run_command("sh", "/tmp/update/install.sh") keeps cwd elsewhere, so
     * relative 6677/server is not found and OTA appears to "install" nothing.
     * Force cwd to UPDATE_EXTRACT_DIR via sh -c.
     */
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && /bin/sh '%s'", UPDATE_EXTRACT_DIR, UPDATE_INSTALL_SCRIPT);
    
    if (run_command(cmd_out, sizeof(cmd_out), "sh", "-c", cmd, NULL) != 0) {
        snprintf(output, size, "install failed: %s", cmd_out);
        return -1;
    }
    snprintf(output, size, "%s", cmd_out);
    return 0;
}

/* 清理更新临时文件 */
void update_cleanup(void) {
    char output[256];
    run_command(output, sizeof(output), "rm", "-rf", UPDATE_ZIP_PATH, NULL);
    run_command(output, sizeof(output), "rm", "-rf", UPDATE_EXTRACT_DIR, NULL);
}

/* 检查远程版本 - 使用mongoose JSON API解析响应 */
int update_check_version(const char *check_url, update_info_t *info) {
    char output[4096];
    
    if (!check_url || !info) {
        return -1;
    }
    
    memset(info, 0, sizeof(update_info_t));
    
    /* 优先使用curl获取版本信息，失败再用wget */
    int ret = run_command(output, sizeof(output), "curl", "-k", "-s", "-L", check_url, NULL);
    if (ret != 0) {
        ret = run_command(output, sizeof(output), "wget", "--no-check-certificate", "-q", "-O", "-", check_url, NULL);
        if (ret != 0) {
            return -1;
        }
    }
    
    /* 使用mongoose JSON API解析 */
    struct mg_str json = mg_str(output);
    
    /* 提取version字段 */
    char *version = mg_json_get_str(json, "$.version");
    if (version) {
        strncpy(info->version, version, sizeof(info->version) - 1);
        free(version);
    }
    
    /* 提取url字段 */
    char *url = mg_json_get_str(json, "$.url");
    if (url) {
        strncpy(info->url, url, sizeof(info->url) - 1);
        free(url);
    }
    
    /* 提取changelog字段 */
    char *changelog = mg_json_get_str(json, "$.changelog");
    if (changelog) {
        strncpy(info->changelog, changelog, sizeof(info->changelog) - 1);
        free(changelog);
    }
    
    /* 提取size字段 */
    info->size = (size_t)mg_json_get_long(json, "$.size", 0);
    
    /* 提取required字段 */
    bool required = false;
    mg_json_get_bool(json, "$.required", &required);
    info->required = required ? 1 : 0;
    
    if (strlen(info->version) == 0) {
        return -1;
    }
    
    return 0;
}


/* 获取嵌入的版本检查URL */
const char* update_get_embedded_url(void) {
    return UPDATE_CHECK_URL;
}
