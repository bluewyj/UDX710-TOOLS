/**
 * @file update.c
 * @brief OTA更新系统实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    char output[1024];
    
    if (!url || strlen(url) == 0) {
        return -1;
    }
    
    /* 清理旧文件 */
    update_cleanup();
    
    /* 优先使用curl（更常见），失败再用wget */
    int ret = run_command(output, sizeof(output), "curl", "-k", "-s", "-L", "-o", UPDATE_ZIP_PATH, url, NULL);
    if (ret != 0) {
        ret = run_command(output, sizeof(output), "wget", "--no-check-certificate", "-q", "-O", UPDATE_ZIP_PATH, url, NULL);
        if (ret != 0) {
            return -1;
        }
    }
    
    /* 检查文件是否存在 */
    struct stat st;
    if (stat(UPDATE_ZIP_PATH, &st) != 0 || st.st_size == 0) {
        return -1;
    }
    
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
    
    /* 检查安装脚本是否存在 */
    if (stat(UPDATE_INSTALL_SCRIPT, &st) != 0) {
        snprintf(output, size, "安装脚本不存在");
        return -1;
    }
    
    /* 添加执行权限 */
    run_command(output, size, "chmod", "+x", UPDATE_INSTALL_SCRIPT, NULL);
    
    /* 执行安装脚本 */
    if (run_command(output, size, "sh", UPDATE_INSTALL_SCRIPT, NULL) != 0) {
        return -1;
    }
    
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

/* ---------- OTA 状态机：status / cancel / apply ---------- */

static int path_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static int path_is_dir(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

static int meta_readable(void) {
    return (access(UPDATE_META_PATH, R_OK) == 0) ? 1 : 0;
}

static int has_pending_update(void) {
    return meta_readable() || path_exists(UPDATE_INSTALL_SCRIPT);
}

/* 从 md5sum 输出提取哈希（首个空白前字段） */
static void extract_md5_token(const char *cmd_out, char *hash, size_t hash_size) {
    size_t i = 0;
    if (!cmd_out || !hash || hash_size == 0) return;
    hash[0] = '\0';
    while (cmd_out[i] && cmd_out[i] != ' ' && cmd_out[i] != '\t' &&
           cmd_out[i] != '\n' && cmd_out[i] != '\r' && i + 1 < hash_size) {
        hash[i] = cmd_out[i];
        i++;
    }
    hash[i] = '\0';
}

static int file_md5(const char *path, char *hash, size_t hash_size) {
    char output[256];
    int ret;

    if (!path || !hash || hash_size == 0) return -1;
    hash[0] = '\0';

    ret = run_command(output, sizeof(output), "md5sum", path, NULL);
    if (ret != 0) {
        ret = run_command(output, sizeof(output), "busybox", "md5sum", path, NULL);
        if (ret != 0) {
            return -1;
        }
    }
    extract_md5_token(output, hash, hash_size);
    return (hash[0] != '\0') ? 0 : -1;
}

/*
 * 校验 meta 包：必需 udx710 + www/；若有 binary_md5 / arch 则比对。
 * 失败返回 -1 并写入 output；成功返回 0，meta_version 可选填出。
 */
static int update_validate_meta(char *output, size_t size, char *meta_version,
                                size_t meta_version_size) {
    char buf[4096];
    FILE *fp;
    size_t n;
    struct mg_str json;
    char *version = NULL;
    char *binary_md5 = NULL;
    char *arch = NULL;
    char actual_md5[64];

    if (output && size > 0) output[0] = '\0';
    if (meta_version && meta_version_size > 0) meta_version[0] = '\0';

    if (!meta_readable()) {
        snprintf(output, size, "meta.json 不可读");
        return -1;
    }

    fp = fopen(UPDATE_META_PATH, "r");
    if (!fp) {
        snprintf(output, size, "无法打开 meta.json");
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    json = mg_str(buf);

    version = mg_json_get_str(json, "$.version");
    if (!version || version[0] == '\0') {
        if (version) free(version);
        snprintf(output, size, "meta.json 缺少 version");
        return -1;
    }
    if (meta_version && meta_version_size > 0) {
        strncpy(meta_version, version, meta_version_size - 1);
        meta_version[meta_version_size - 1] = '\0';
    }

    if (!path_exists(UPDATE_STAGING_BINARY)) {
        free(version);
        snprintf(output, size, "缺少 staging 二进制 udx710");
        return -1;
    }
    if (!path_is_dir(UPDATE_STAGING_WWW)) {
        free(version);
        snprintf(output, size, "缺少 staging www 目录");
        return -1;
    }

    binary_md5 = mg_json_get_str(json, "$.binary_md5");
    if (binary_md5 && binary_md5[0] != '\0') {
        if (file_md5(UPDATE_STAGING_BINARY, actual_md5, sizeof(actual_md5)) != 0) {
            free(version);
            free(binary_md5);
            snprintf(output, size, "无法计算二进制 MD5");
            return -1;
        }
        if (strcasecmp(actual_md5, binary_md5) != 0) {
            snprintf(output, size, "二进制 MD5 不匹配: expected=%s actual=%s",
                     binary_md5, actual_md5);
            free(version);
            free(binary_md5);
            return -1;
        }
    }
    if (binary_md5) free(binary_md5);

    arch = mg_json_get_str(json, "$.arch");
    if (arch && arch[0] != '\0') {
        if (strcmp(arch, UPDATE_EXPECTED_ARCH) != 0) {
            snprintf(output, size, "架构不匹配: expected=%s actual=%s",
                     UPDATE_EXPECTED_ARCH, arch);
            free(version);
            free(arch);
            return -1;
        }
    }
    if (arch) free(arch);

    free(version);
    return 0;
}

static int update_apply_meta(char *output, size_t size) {
    char cmd_out[512];
    char meta_version[32] = {0};

    if (update_validate_meta(output, size, meta_version, sizeof(meta_version)) != 0) {
        return -1; /* 保留 staging */
    }

    if (run_command(cmd_out, sizeof(cmd_out), "cp", "-f", UPDATE_STAGING_BINARY,
                    UPDATE_RUNTIME_BINARY, NULL) != 0) {
        snprintf(output, size, "复制二进制失败: %s", cmd_out);
        return -1;
    }
    run_command(cmd_out, sizeof(cmd_out), "chmod", "755", UPDATE_RUNTIME_BINARY, NULL);

    run_command(cmd_out, sizeof(cmd_out), "rm", "-rf", UPDATE_RUNTIME_WWW, NULL);
    if (run_command(cmd_out, sizeof(cmd_out), "cp", "-a", UPDATE_STAGING_WWW,
                    UPDATE_RUNTIME_WWW, NULL) != 0) {
        snprintf(output, size, "复制 www 失败: %s", cmd_out);
        return -1;
    }

    update_cleanup();
    if (meta_version[0]) {
        snprintf(output, size, "已应用版本 %s", meta_version);
    } else {
        snprintf(output, size, "meta 更新已应用");
    }
    return 0;
}

static void schedule_device_reboot(void) {
    pid_t pid = fork();
    if (pid == 0) {
        sleep(2);
        device_reboot();
        _exit(0);
    }
}

int update_get_status(update_status_t *st) {
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    strncpy(st->current_version, FIRMWARE_VERSION, sizeof(st->current_version) - 1);

    if (!has_pending_update()) {
        st->pending = 0;
        return 0;
    }

    st->pending = 1;
    if (meta_readable()) {
        char buf[4096];
        FILE *fp = fopen(UPDATE_META_PATH, "r");
        strncpy(st->kind, "meta", sizeof(st->kind) - 1);
        if (fp) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            buf[n] = '\0';
            {
                char *ver = mg_json_get_str(mg_str(buf), "$.version");
                if (ver) {
                    strncpy(st->meta_version, ver, sizeof(st->meta_version) - 1);
                    free(ver);
                }
            }
        }
    } else {
        strncpy(st->kind, "legacy", sizeof(st->kind) - 1);
    }
    return 0;
}

int update_cancel(void) {
    update_cleanup();
    return 0;
}

int update_apply(int restart_now, char *output, size_t size) {
    int ret;

    if (output && size > 0) output[0] = '\0';

    if (!has_pending_update()) {
        snprintf(output, size, "无待应用更新");
        return -1;
    }

    if (meta_readable()) {
        ret = update_apply_meta(output, size);
    } else if (path_exists(UPDATE_INSTALL_SCRIPT)) {
        ret = update_install(output, size);
        if (ret == 0) {
            update_cleanup();
        }
        /* install 失败保留 staging */
    } else {
        snprintf(output, size, "无待应用更新");
        return -1;
    }

    if (ret == 0 && restart_now) {
        schedule_device_reboot();
    }
    return ret;
}
