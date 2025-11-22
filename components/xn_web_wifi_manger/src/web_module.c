/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 19:20:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 19:29:24
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\web_module.c
 * @Description: Web 配网模块实现
 */

#include "web_module.h"

#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"

static const char *TAG = "web_module";

static httpd_handle_t     s_httpd = NULL;
static web_module_config_t s_cfg;

/* 简单的 JSON 响应工具 */
static esp_err_t web_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* 读取整个 index.html 并返回 */
static esp_err_t web_handle_root(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "index.html not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char  buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* /scan: 扫描附近 WiFi，返回 JSON */
static esp_err_t web_handle_scan(httpd_req_t *req)
{
    if (s_cfg.scan_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"scan cb null\"}");
    }

    web_scan_result_t list[16];
    uint16_t          count = sizeof(list) / sizeof(list[0]);

    esp_err_t ret = s_cfg.scan_cb(list, &count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "scan cb failed: %s", esp_err_to_name(ret));
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"scan failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"status\":\"ok\",\"networks\":["); // Fix typo in JSON key

    for (uint16_t i = 0; i < count; ++i) {
        if (list[i].ssid[0] == '\0') {
            continue;
        }
        char item[256];
        snprintf(item,
                 sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 (i == 0) ? "" : ",",
                 list[i].ssid,
                 list[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* 读取请求体到缓冲区 */
static int web_read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = 0;
    int ret;
    while (total < (int)buf_size - 1) {
        ret = httpd_req_recv(req, buf + total, buf_size - 1 - total);
        if (ret <= 0) {
            break;
        }
        total += ret;
        if (ret < (int)(buf_size - 1 - total)) {
            break;
        }
    }
    buf[total] = '\0';
    return total;
}

/* 从简单 JSON 中提取字段值（非常简单的实现，只支持 {"key":"value"}） */
static bool web_extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char  pattern[64];
    char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* /configure: 提交新的 WiFi 配置 */
static esp_err_t web_handle_configure(httpd_req_t *req)
{
    char body[256];
    web_read_body(req, body, sizeof(body));

    char ssid[33]     = {0};
    char password[65] = {0};
    if (!web_extract_json_string(body, "ssid", ssid, sizeof(ssid))) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"ssid missing\"}");
    }
    (void)web_extract_json_string(body, "password", password, sizeof(password));

    if (s_cfg.configure_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"cfg cb null\"}");
    }

    esp_err_t ret = s_cfg.configure_cb(ssid, (password[0] == '\0') ? NULL : password);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"connect failed\"}");
    }

    return web_send_json(req, "{\"status\":\"ok\"}");
}

/* /api/status: 返回当前 WiFi 状态 */
static esp_err_t web_handle_status(httpd_req_t *req)
{
    if (s_cfg.get_status_cb == NULL) {
        return web_send_json(req, "{\"status\":\"disconnected\"}");
    }

    web_wifi_status_t status;
    memset(&status, 0, sizeof(status));

    esp_err_t ret = s_cfg.get_status_cb(&status);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    if (!status.connected) {
        return web_send_json(req, "{\"status\":\"disconnected\"}");
    }

    char json[256];
    snprintf(json,
             sizeof(json),
             "{\"status\":\"connected\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"bssid\":\"%s\"}",
             status.ssid,
             status.ip,
             status.rssi,
             status.bssid);

    return web_send_json(req, json);
}

/* /api/saved: 返回已保存 WiFi 列表 */
static esp_err_t web_handle_saved(httpd_req_t *req)
{
    if (s_cfg.get_saved_cb == NULL) {
        return web_send_json(req, "[]");
    }

    web_saved_wifi_t list[16];
    uint8_t          count = sizeof(list) / sizeof(list[0]);

    esp_err_t ret = s_cfg.get_saved_cb(list, &count);
    if (ret != ESP_OK || count == 0) {
        return web_send_json(req, "[]");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    for (uint8_t i = 0; i < count; ++i) {
        if (list[i].ssid[0] == '\0') {
            continue;
        }
        char item[64];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\"}", (i == 0) ? "" : ",", list[i].ssid);
        httpd_resp_sendstr_chunk(req, item);
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* /api/connect: 根据保存的 SSID 重新连接（这里直接调用 wifi_module_connect 即可） */
static esp_err_t web_handle_connect(httpd_req_t *req)
{
    char body[128];
    web_read_body(req, body, sizeof(body));

    char ssid[33] = {0};
    if (!web_extract_json_string(body, "ssid", ssid, sizeof(ssid))) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"ssid missing\"}");
    }

    if (s_cfg.connect_saved_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    esp_err_t ret = s_cfg.connect_saved_cb(ssid);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }
    return web_send_json(req, "{\"status\":\"ok\"}");
}

/* /api/delete: 删除已保存 WiFi（简单实现：重写列表时跳过该 SSID） */
static esp_err_t web_handle_delete(httpd_req_t *req)
{
    char body[128];
    web_read_body(req, body, sizeof(body));

    char target_ssid[33] = {0};
    if (!web_extract_json_string(body, "ssid", target_ssid, sizeof(target_ssid))) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"ssid missing\"}");
    }

    if (s_cfg.delete_saved_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    esp_err_t ret = s_cfg.delete_saved_cb(target_ssid);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    return web_send_json(req, "{\"status\":\"ok\"}");
}

/* /api/reset_retry: 目前仅作为占位，具体逻辑由管理模块后续补充接口 */
static esp_err_t web_handle_reset_retry(httpd_req_t *req)
{
    if (s_cfg.reset_retry_cb != NULL) {
        (void)s_cfg.reset_retry_cb();
    }
    return web_send_json(req, "{\"status\":\"ok\"}");
}

static esp_err_t web_start_httpd(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = port;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return ESP_FAIL;
    }

    /* 注册 URI 处理函数 */
    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = web_handle_root,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &root_uri);

    httpd_uri_t scan_uri = {
        .uri      = "/scan",
        .method   = HTTP_GET,
        .handler  = web_handle_scan,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &scan_uri);

    httpd_uri_t cfg_uri = {
        .uri      = "/configure",
        .method   = HTTP_POST,
        .handler  = web_handle_configure,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &cfg_uri);

    httpd_uri_t status_uri = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = web_handle_status,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &status_uri);

    httpd_uri_t saved_uri = {
        .uri      = "/api/saved",
        .method   = HTTP_GET,
        .handler  = web_handle_saved,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &saved_uri);

    httpd_uri_t connect_uri = {
        .uri      = "/api/connect",
        .method   = HTTP_POST,
        .handler  = web_handle_connect,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &connect_uri);

    httpd_uri_t delete_uri = {
        .uri      = "/api/delete",
        .method   = HTTP_POST,
        .handler  = web_handle_delete,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &delete_uri);

    httpd_uri_t reset_uri = {
        .uri      = "/api/reset_retry",
        .method   = HTTP_POST,
        .handler  = web_handle_reset_retry,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &reset_uri);

    return ESP_OK;
}

static esp_err_t web_mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "wifi_spiffs",
        .max_files              = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t xn_web_module_start(const web_module_config_t *config)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    web_module_config_t cfg = WEB_MODULE_DEFAULT_CONFIG();
    if (config != NULL) {
        cfg = *config;
    }

    s_cfg = cfg;

    ESP_LOGI(TAG, "web module start, http_port=%u", (unsigned)cfg.http_port);

    esp_err_t ret = web_mount_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = web_start_httpd(cfg.http_port);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

esp_err_t xn_web_module_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    /* SPIFFS 在整个应用生命周期内仍可保持挂载状态，如需卸载可调用 esp_vfs_spiffs_unregister("wifi_spiffs") */

    return ESP_OK;
}
