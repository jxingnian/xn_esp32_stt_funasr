/**
 * 文件：web_provisioning.c
 * 职责：实现网页配网 HTTP 服务的启动、路由注册、表单解析与 WiFi 连接触发。
 * 注意：为保持组件通用性，不在此处直接写入 NVS；通过回调交由上层处理持久化。
 */

#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "web_provisioning.h"
#include "lwip/ip4_addr.h"

/**
 * HTTP 服务句柄与结果回调
 * - s_server：当前组件启动的 httpd 实例；为 NULL 表示未运行
 * - s_cb：用户注册的配网结果回调，用于上层处理（如保存到 NVS）
 */
static httpd_handle_t s_server;
static web_provisioning_result_cb s_cb;

/**
 * 发送 SPIFFS 中的静态文件到客户端
 * @param req  HTTP 请求
 * @param path SPIFFS 路径（例如 "/spiffs/index.html"）
 */
static esp_err_t serve_file(httpd_req_t *req, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        // 文件不存在或 SPIFFS 未挂载
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    char buf[1024];
    size_t n;
    // 简化处理：根据页面类型固定为 text/html；如需更多静态资源类型，可扩展 MIME 判断
    httpd_resp_set_type(req, "text/html");
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    // 结束分块响应
    return httpd_resp_send_chunk(req, NULL, 0);
}

/** 根路径处理：返回配网页面 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/index.html");
}

/**
 * 解析 application/x-www-form-urlencoded 的 key=value 对
 * 说明：仅做基本解析（+ 号转空格，不含百分号编码解码），足以满足简单表单提交场景。
 * @param body   请求体缓冲区（可就地扫描）
 * @param key    需要提取的键名（如 "ssid"）
 * @param out    输出缓冲区
 * @param outlen 输出缓冲区长度
 */
static void parse_kv(char *body, const char *key, char *out, size_t outlen) {
    size_t keylen = strlen(key);
    char *p = strstr(body, key);
    if (!p) { out[0] = '\0'; return; }
    p += keylen;
    if (*p == '=') p++;
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < outlen) {
        // 基础转换：将 '+' 视为空格
        if (*p == '+') out[i++] = ' ';
        else out[i++] = *p;
        p++;
    }
    out[i] = '\0';
}

/**
 * POST /wifi：接收配网信息并触发 WiFi 连接
 * 输入：application/x-www-form-urlencoded，字段 ssid/password
 * 输出：JSON {"ok": true}
 */
static esp_err_t wifi_post_handler(httpd_req_t *req) {
    int total = req->content_len;
    char *body = malloc(total + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    // 读取完整请求体
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) { free(body); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); }
        received += r;
    }
    body[total] = '\0';

    // 提取 ssid 与密码（长度受 WiFi 驱动结构体限制）
    char ssid[33];
    char pass[65];
    parse_kv(body, "ssid", ssid, sizeof(ssid));
    parse_kv(body, "password", pass, sizeof(pass));
    free(body);

    // 应用到 WiFi 配置并尝试连接
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    // 将配网信息通知上层（例如保存到 NVS、重启网络子系统等）
    if (s_cb) s_cb(ssid, pass);

    // 返回结果
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/**
 * POST /configure：接受 JSON {ssid,password}，与 /wifi 行为一致
 */
static esp_err_t configure_post_handler(httpd_req_t *req) {
    int total = req->content_len;
    char *body = malloc(total + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) { free(body); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); }
        received += r;
    }
    body[total] = '\0';
    // 朴素 JSON 解析（不打印敏感内容）
    char ssid[33] = {0};
    char pass[65] = {0};
    char *p1 = strstr(body, "\"ssid\"");
    if (p1) {
        p1 = strchr(p1, ':');
        if (p1) {
            p1++;
            while (*p1 && (*p1 == ' ')) p1++;
            if (*p1 == '"') {
                p1++;
                size_t i = 0;
                while (*p1 && *p1 != '"' && i + 1 < sizeof(ssid)) ssid[i++] = *p1++;
                ssid[i] = '\0';
            }
        }
    }
    char *p2 = strstr(body, "\"password\"");
    if (p2) {
        p2 = strchr(p2, ':');
        if (p2) {
            p2++;
            while (*p2 && (*p2 == ' ')) p2++;
            if (*p2 == '"') {
                p2++;
                size_t j = 0;
                while (*p2 && *p2 != '"' && j + 1 < sizeof(pass)) pass[j++] = *p2++;
                pass[j] = '\0';
            }
        }
    }
    free(body);
    wifi_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    if (s_cb) s_cb(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/**
 * GET /api/status：返回当前连接状态信息
 * 示例：{"status":"connected","ssid":"...","ip":"...","rssi":-50,"bssid":"xx:xx:xx:xx:xx:xx"}
 */
static esp_err_t status_get_handler(httpd_req_t *req) {
    wifi_ap_record_t ap; memset(&ap, 0, sizeof(ap));
    esp_err_t has_ap = esp_wifi_sta_get_ap_info(&ap);
    char ipstr[24] = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip; memset(&ip, 0, sizeof(ip));
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, ipstr, sizeof(ipstr));
        }
    }
    char bssid[24] = {0};
    if (has_ap == ESP_OK) {
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
    httpd_resp_set_type(req, "application/json");
    if (has_ap == ESP_OK) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"connected\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"bssid\":\"%s\"}",
                 (const char *)ap.ssid, ipstr, ap.rssi, bssid);
        return httpd_resp_send(req, resp, -1);
    } else {
        return httpd_resp_send(req, "{\"status\":\"disconnected\"}", -1);
    }
}

/**
 * GET /scan：同步扫描周边 WiFi 并返回列表
 * 输出：{"networks":[{"ssid":"...","rssi":-60},...]}
 */
static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_scan_config_t sc; memset(&sc, 0, sizeof(sc));
    sc.show_hidden = false; sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_start();
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"scan_failed\"}", -1);
    }
    uint16_t ap_count = 0; esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!aps) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"networks\":[]}", -1);
    }
    esp_wifi_scan_get_ap_records(&ap_count, aps);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");
    for (uint16_t i = 0; i < ap_count; i++) {
        char item[160];
        snprintf(item, sizeof(item),
                 "{\"ssid\":\"%s\",\"rssi\":%d}%s",
                 (const char *)aps[i].ssid, aps[i].rssi,
                 (i + 1 < ap_count) ? "," : "");
        httpd_resp_sendstr_chunk(req, item);
    }
    free(aps);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/**
 * GET /api/saved：返回当前已配置的 STA SSID（简化为单项）
 */
static esp_err_t saved_get_handler(httpd_req_t *req) {
    wifi_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    if (cfg.sta.ssid[0]) {
        snprintf(resp, sizeof(resp), "[{\"ssid\":\"%s\"}]", (const char *)cfg.sta.ssid);
    } else {
        snprintf(resp, sizeof(resp), "[]");
    }
    return httpd_resp_send(req, resp, -1);
}

/**
 * POST /api/reset_retry：占位接口（成功返回），便于旧页面流程使用
 */
static esp_err_t reset_retry_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/**
 * POST /api/connect：按当前配置触发连接；传入 {ssid} 不更改密码
 */
static esp_err_t connect_post_handler(httpd_req_t *req) {
    esp_wifi_connect();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/**
 * POST /api/delete：清空当前 STA 配置并断开连接
 */
static esp_err_t delete_post_handler(httpd_req_t *req) {
    wifi_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_disconnect();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/**
 * 启动网页配网服务
 * - 初始化 NVS 与网络接口（确保 httpd 可工作）
 * - 挂载 SPIFFS 分区（基于 main/CMakeLists.txt 创建的 spiffs_data）
 * - 注册两个路由：GET /（配网页面）与 POST /wifi（提交表单）
 */
esp_err_t web_provisioning_start(uint16_t port) {
    if (s_server) return ESP_OK;

    // 基础初始化：NVS 与网络栈
    nvs_flash_init();
    esp_netif_init();

    // 配置 HTTP 服务器
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port ? port : 80;

    // 挂载 SPIFFS：用于提供 /spiffs/index.html
    esp_vfs_spiffs_conf_t spiffs_conf = { .base_path = "/spiffs", .partition_label = "spiffs_data", .max_files = 5, .format_if_mount_failed = true };
    esp_vfs_spiffs_register(&spiffs_conf);

    // 启动 HTTP 服务
    if (httpd_start(&s_server, &cfg) != ESP_OK) return ESP_FAIL;

    // 注册路由：根路径与配网提交
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &root);
    httpd_uri_t wifi_post = { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &wifi_post);
    httpd_uri_t cfg_post = { .uri = "/configure", .method = HTTP_POST, .handler = configure_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &cfg_post);
    httpd_uri_t status_get = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &status_get);
    httpd_uri_t scan_get = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &scan_get);
    httpd_uri_t saved_get = { .uri = "/api/saved", .method = HTTP_GET, .handler = saved_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &saved_get);
    httpd_uri_t reset_post = { .uri = "/api/reset_retry", .method = HTTP_POST, .handler = reset_retry_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &reset_post);
    httpd_uri_t connect_post = { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &connect_post);
    httpd_uri_t delete_post = { .uri = "/api/delete", .method = HTTP_POST, .handler = delete_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &delete_post);
    return ESP_OK;
}

/** 停止网页配网服务并清理句柄 */
esp_err_t web_provisioning_stop(void) {
    if (!s_server) return ESP_OK;
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_OK;
}

/** 查询服务运行状态 */
bool web_provisioning_running(void) {
    return s_server != NULL;
}

/** 设置配网结果回调 */
void web_provisioning_set_callback(web_provisioning_result_cb cb) {
    s_cb = cb;
}
