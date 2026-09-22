/**
 * @file security.c
 * @brief 密保功能模块实现
 * @description 提供密保问题设置、验证、找回密码、重置数据等功能
 */

#include "security.h"
#include "auth.h"
#include "database.h"
#include "exec_utils.h"
#include "sha256.h"

#define SECURITY_ICCID_MAX_LEN 24 /* ICCID最大长度(仅内部使用) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 外部函数 - 获取ICCID */
extern int get_iccid(char *iccid, size_t size);

/*============================================================================
 * 内部函数
 *============================================================================*/

/**
 * @brief 计算MD5哈希（使用SHA256前16字节模拟）
 * @note 为保持与现有系统一致，使用SHA256
 */
static void compute_answer_hash(const char *answer, char *hash_out) {
  sha256_hash_string(answer, hash_out);
}

/**
 * @brief 创建密保数据表
 */
static int create_security_table(void) {
  const char *sql = "CREATE TABLE IF NOT EXISTS security_questions ("
                    "id INTEGER PRIMARY KEY,"
                    "question1 TEXT NOT NULL,"
                    "question2 TEXT NOT NULL,"
                    "answer1_hash TEXT NOT NULL,"
                    "answer2_hash TEXT NOT NULL,"
                    "iccid TEXT NOT NULL,"
                    "created_at INTEGER NOT NULL,"
                    "locked INTEGER DEFAULT 1"
                    ");";

  return db_execute(sql);
}

/*============================================================================
 * 公共接口实现
 *============================================================================*/

int security_init(void) {
  printf("[Security] 初始化密保模块\n");

  if (create_security_table() != 0) {
    printf("[Security] 创建数据表失败\n");
    return -1;
  }

  printf("[Security] 密保模块初始化完成\n");
  return 0;
}

int security_get_status(SecurityStatus *status) {
  char output[256] = {0};

  if (!status) {
    return -1;
  }

  memset(status, 0, sizeof(SecurityStatus));

  /* 查询密保记录，检查 answer1_hash 是否有效值来判断是否真正设置过 */
  const char *sql =
      "SELECT answer1_hash, created_at FROM security_questions WHERE id = 1;";

  if (db_query_string(sql, output, sizeof(output)) == 0 && strlen(output) > 0) {
    /* 解析输出: answer1_hash|created_at */
    char *p = strchr(output, '|');
    if (p) {
      *p = '\0';
      /* answer1_hash 必须是有效的SHA256哈希(64字符) */
      if (strlen(output) == 64) {
        status->is_set = 1;
        status->created_at = atol(p + 1);
      } else {
        status->is_set = 0;
      }
    } else {
      /* 没有分隔符，格式无效 */
      status->is_set = 0;
    }
  } else {
    status->is_set = 0;
  }

  return 0;
}

int security_setup(const SecuritySetupRequest *req) {
  char sql[2048];
  char answer1_hash[SHA256_HEX_SIZE] = {0};
  char answer2_hash[SHA256_HEX_SIZE] = {0};
  char escaped_q1[SECURITY_QUESTION_MAX_LEN * 2];
  char escaped_q2[SECURITY_QUESTION_MAX_LEN * 2];
  char current_iccid[SECURITY_ICCID_MAX_LEN] = {0};
  SecurityStatus status;

  if (!req) {
    return -2;
  }

  /* 检查参数 */
  if (strlen(req->question1) == 0 || strlen(req->answer1) == 0 ||
      strlen(req->question2) == 0 || strlen(req->answer2) == 0) {
    printf("[Security] 设置失败：问题或答案不能为空\n");
    return -2;
  }

  /* 检查是否已设置 */
  if (security_get_status(&status) == 0 && status.is_set) {
    printf("[Security] 设置失败：密保已设置，不可更改\n");
    return -1;
  }

  /* 获取当前ICCID（仅作为记录，获取失败不影响设置） */
  if (get_iccid(current_iccid, sizeof(current_iccid)) != 0) {
    printf("[Security] 警告：无法获取ICCID，继续设置\n");
    current_iccid[0] = '\0';
  }

  /* 计算答案哈希 */
  compute_answer_hash(req->answer1, answer1_hash);
  compute_answer_hash(req->answer2, answer2_hash);

  /* 转义问题 */
  db_escape_string(req->question1, escaped_q1, sizeof(escaped_q1));
  db_escape_string(req->question2, escaped_q2, sizeof(escaped_q2));

  /* 插入数据 */
  snprintf(sql, sizeof(sql),
           "INSERT OR REPLACE INTO security_questions "
           "(id, question1, question2, answer1_hash, answer2_hash, iccid, "
           "created_at, locked) "
           "VALUES (1, '%s', '%s', '%s', '%s', '%s', %ld, 1);",
           escaped_q1, escaped_q2, answer1_hash, answer2_hash, current_iccid,
           (long)time(NULL));

  if (db_execute(sql) != 0) {
    printf("[Security] 设置失败：数据库错误\n");
    return -2;
  }

  printf("[Security] 密保设置成功\n");
  return 0;
}

int security_get_questions(SecurityQuestions *questions) {
  char output[1024] = {0};

  if (!questions) {
    return -1;
  }

  memset(questions, 0, sizeof(SecurityQuestions));

  const char *sql = "SELECT question1 || '|' || question2 FROM "
                    "security_questions WHERE id = 1;";

  if (db_query_string(sql, output, sizeof(output)) != 0 ||
      strlen(output) == 0) {
    return -1; /* 未设置 */
  }

  /* 解析输出 */
  char *p = strchr(output, '|');
  if (p) {
    *p = '\0';
    strncpy(questions->question1, output, sizeof(questions->question1) - 1);
    strncpy(questions->question2, p + 1, sizeof(questions->question2) - 1);

    /* 去除换行符 */
    char *nl = strchr(questions->question2, '\n');
    if (nl)
      *nl = '\0';
  }

  /* 反转义 */
  db_unescape_string(questions->question1);
  db_unescape_string(questions->question2);

  return 0;
}

int security_verify(const SecurityVerifyRequest *req) {
  char output[512] = {0};
  char answer1_hash[SHA256_HEX_SIZE] = {0};
  char answer2_hash[SHA256_HEX_SIZE] = {0};
  char stored_hash1[SHA256_HEX_SIZE] = {0};
  char stored_hash2[SHA256_HEX_SIZE] = {0};

  if (!req) {
    return -2;
  }

  /* 验证确认文本 */
  if (strcmp(req->confirm, SECURITY_CONFIRM_TEXT) != 0) {
    printf("[Security] 验证失败：确认文本不匹配\n");
    return -2;
  }

  /* 查询存储的答案哈希 */
  const char *sql = "SELECT answer1_hash || '|' || answer2_hash "
                    "FROM security_questions WHERE id = 1;";

  if (db_query_string(sql, output, sizeof(output)) != 0 ||
      strlen(output) == 0) {
    printf("[Security] 验证失败：未设置密保\n");
    return -1;
  }

  /* 解析存储的数据: answer1_hash|answer2_hash */
  char *p1 = strchr(output, '|');
  if (!p1)
    return -1;
  *p1 = '\0';
  strncpy(stored_hash1, output, sizeof(stored_hash1) - 1);
  strncpy(stored_hash2, p1 + 1, sizeof(stored_hash2) - 1);

  /* 去除换行符 */
  char *nl = strchr(stored_hash2, '\n');
  if (nl)
    *nl = '\0';

  /* 检查存储的哈希是否有效 */
  if (strlen(stored_hash1) != 64 || strlen(stored_hash2) != 64) {
    printf("[Security] 验证失败：密保数据无效\n");
    return -1;
  }

  /* 计算输入答案的哈希 */
  compute_answer_hash(req->answer1, answer1_hash);
  compute_answer_hash(req->answer2, answer2_hash);

  /* 验证答案 */
  if (strcmp(answer1_hash, stored_hash1) != 0 ||
      strcmp(answer2_hash, stored_hash2) != 0) {
    printf("[Security] 验证失败：答案错误\n");
    return -1;
  }

  printf("[Security] 验证通过\n");
  return 0;
}

int security_reset_password(const SecurityVerifyRequest *req) {
  int ret;
  char default_hash[SHA256_HEX_SIZE] = {0};

  /* 先验证密保 */
  ret = security_verify(req);
  if (ret != 0) {
    return ret;
  }

  /* 重置密码为默认值 */
  printf("[Security] 正在重置密码为默认值...\n");

  sha256_hash_string(AUTH_DEFAULT_PASSWORD, default_hash);
  if (config_set("auth_password_hash", default_hash) != 0) {
    printf("[Security] 重置密码失败\n");
    return -4;
  }

  /* 清除所有登录Token */
  db_execute("DELETE FROM auth_tokens;");

  printf("[Security] 密码重置成功\n");
  return 0;
}

int security_factory_reset(const SecurityVerifyRequest *req) {
  int ret;
  char db_path[256] = {0};

  /* 先验证密保 */
  ret = security_verify(req);
  if (ret != 0) {
    return ret;
  }

  printf("[Security] ⚠️ 正在执行出厂重置...\n");

  /* 获取数据库路径 */
  const char *path = (const char *)(size_t)db_get_path();
  if (path) {
    strncpy(db_path, path, sizeof(db_path) - 1);
  }

  /* 删除所有表数据 */
  const char *tables[] = {"security_questions",
                          "auth_tokens",
                          "config",
                          "rathole_config",
                          "rathole_services",
                          "ipv6_proxy_config",
                          "ipv6_proxy_rules",
                          "ipv6_send_log",
                          "apn_config",
                          "apn_templates",
                          "sms",
                          "sent_sms",
                          "webhook_config",
                          "sms_config",
                          NULL};

  for (int i = 0; tables[i] != NULL; i++) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM %s;", tables[i]);
    db_execute(sql);
    printf("[Security] 已清除表: %s\n", tables[i]);
  }

  /* VACUUM压缩数据库 */
  db_execute("VACUUM;");

  printf("[Security] ✅ 出厂重置完成，正在重启系统...\n");

  /* Soft reboot: H9 marker via device_reboot (same as UI reboot) */
  device_reboot();

  return 0;
}
