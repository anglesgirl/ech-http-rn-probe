/*
 * 最小 C 宿主：证明去 Dart 化后的 ech_http 引擎可由任意运行时驱动
 * （这里是最朴素的 C，JNI / Objective-C++ 同理），并验证 ECH 的
 * fail-closed 语义在真实网络上成立。
 *
 * 用法: eh_smoke <ech_config_b64|-> <connect_ip> <host> [path]
 *   第二个参数传 "-" 表示关闭 ECH（普通验证 TLS，用作对照）。
 *   直连 IP 只改变 TCP 物理端点，TLS 内层 SNI 与 Host 仍为目标域名。
 *
 * 退出码: 0 通过 / 2 参数错 / 3 引擎创建失败 / 4 请求句柄创建失败
 *         5 请求成功但 ECH 未被接受 / 6 请求未成功
 */
#define _POSIX_C_SOURCE 200809L /* -std=c11 下 nanosleep 需要显式 POSIX 声明 */

#include "ech_http.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEADER_KEEP 8192
#define MESSAGE_KEEP 512

static EhRequest *g_request = NULL;
static char g_headers[HEADER_KEEP];
static size_t g_headers_len = 0;
static size_t g_body_bytes = 0;
static int g_status = 0;
static int g_ech_accepted = -1;
static int g_ech_retries = -1;
static int g_final = -1; /* -1 = 未结束; 0 = 成功; 其余为 curl 错误码 */
static char g_message[MESSAGE_KEEP];

/* 事件类型: 1=响应头 2=响应体 3=完成 4=错误 */
static bool on_event(void *user_data, int32_t type, int32_t code,
                     int32_t ech_accepted, int32_t ech_retries,
                     const uint8_t *data, size_t length) {
  (void)user_data;
  switch (type) {
    case 1:
      g_status = code;
      g_ech_accepted = ech_accepted;
      g_ech_retries = ech_retries;
      g_headers_len = length < HEADER_KEEP - 1 ? length : HEADER_KEEP - 1;
      memcpy(g_headers, data, g_headers_len);
      g_headers[g_headers_len] = '\0';
      break;
    case 2:
      g_body_bytes += length;
      /* 归还接收额度，避免撞上原生侧 256 KiB 背压上限 */
      if (g_request) eh_request_acknowledge(g_request, length);
      break;
    case 3:
      g_final = code;
      break;
    case 4:
      g_final = code != 0 ? code : -2;
      snprintf(g_message, sizeof g_message, "%.*s",
               (int)(length < MESSAGE_KEEP - 1 ? length : MESSAGE_KEEP - 1),
               (const char *)data);
      break;
    default:
      break;
  }
  return true;
}

static void msleep(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "用法: %s <ech_config_b64|-> <connect_ip> <host> [path]\n", argv[0]);
    return 2;
  }
  const int use_ech = strcmp(argv[1], "-") != 0;
  const char *path = argc > 4 ? argv[4] : "/";

  char url[1024];
  char headers[512];
  snprintf(url, sizeof url, "https://%s%s", argv[3], path);
  snprintf(headers, sizeof headers, "User-Agent: ech-http-rn-probe/1.0\r\nAccept: */*\r\n");

  printf("== ech_http 去 Dart 化引擎冒烟 ==\n");
  printf("目标       : %s\n", url);
  printf("直连 IP    : %s（TLS SNI / Host 仍为 %s）\n", argv[2], argv[3]);
  printf("ECH 模式   : %s\n", use_ech ? "要求 ECH（fail-closed）" : "关闭（对照）");

  EhClient *client = eh_client_create();
  if (!client) {
    fprintf(stderr, "!! eh_client_create 失败\n");
    return 3;
  }
  printf("引擎版本   : %s\n\n", eh_version());

  EhOptions options;
  memset(&options, 0, sizeof options);
  options.url = url;
  options.method = "GET";
  options.headers = headers;
  options.ech_config = use_ech ? argv[1] : NULL;
  options.connect_ip = argv[2];
  options.timeout_ms = 25000;
  options.connect_timeout_ms = 12000;
  options.max_response_bytes = 8 * 1024 * 1024;
  options.auto_uncompress = true;

  g_request = eh_request_start(client, &options, on_event, NULL);
  if (!g_request) {
    fprintf(stderr, "!! eh_request_start 返回空\n");
    eh_client_destroy(client);
    return 4;
  }

  for (int waited = 0; waited < 30000 && g_final < 0; waited += 50) msleep(50);
  eh_request_destroy(g_request);
  g_request = NULL;
  eh_client_destroy(client);

  printf("--- 结果 ---\n");
  printf("curl 终态  : %d\n", g_final);
  printf("HTTP 状态  : %d\n", g_status);
  printf("响应体     : %zu 字节\n", g_body_bytes);
  printf("ECH 接受   : %d\n", g_ech_accepted);
  printf("ECH 重试   : %d\n", g_ech_retries);
  if (g_message[0]) printf("错误信息   : %s\n", g_message);
  if (g_headers_len) printf("响应首行   : %.*s\n", (int)strcspn(g_headers, "\r\n"), g_headers);

  if (g_final == 0) {
    if (!use_ech) {
      printf("\n[PASS] 对照链路成功（未启用 ECH）\n");
      return 0;
    }
    if (g_ech_accepted == 1) {
      printf("\n[PASS] 真实 ECH 握手成功，echAccepted=1，重试 %d 次\n", g_ech_retries);
      return 0;
    }
    printf("\n[FAIL] 请求成功但 ECH 未被接受（echAccepted=%d）\n", g_ech_accepted);
    return 5;
  }
  printf("\n[FAIL] 请求未成功，终态 %d\n", g_final);
  return 6;
}
