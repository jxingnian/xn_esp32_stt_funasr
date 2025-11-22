/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 19:10:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 19:24:08
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\web_module.h
 * @Description: Web 配网模块
 * 
 * 该模块负责：
 *  - 启动一个 HTTP 服务器；
 *  - 从 SPIFFS 中挂载并返回 index.html；
 *  - 提供 index.html 中使用的接口：
 *      /scan           : 扫描周围 WiFi，返回 JSON 列表；
 *      /configure      : 接收前端提交的 SSID/密码，发起配置；
 *      /api/status     : 返回当前连接状态（connected/未连接 等）；
 *      /api/saved      : 返回已保存的 WiFi 列表；
 *      /api/connect    : 根据 SSID 连接已保存 WiFi；
 *      /api/delete     : 删除已保存 WiFi；
 *      /api/reset_retry: 重置 WiFi 管理重试计数（由管理模块实现）。
 */

#ifndef WEB_MODULE_H
#define WEB_MODULE_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Web 扫描结果
 */
typedef struct {
    char   ssid[32];  ///< AP 名称
    int8_t rssi;      ///< 信号强度
} web_scan_result_t;

/**
 * @brief Web 已保存 WiFi 条目
 */
typedef struct {
    char ssid[32];    ///< 已保存的 WiFi 名称
} web_saved_wifi_t;

/**
 * @brief Web 展示用的 WiFi 状态
 */
typedef struct {
    bool  connected;      ///< 是否已连接
    char  ssid[32];       ///< 当前连接的 SSID
    char  ip[16];         ///< 当前 IP 地址
    int8_t rssi;          ///< 信号强度
    char  bssid[18];      ///< BSSID 字符串表示
} web_wifi_status_t;

typedef esp_err_t (*web_scan_cb_t)(web_scan_result_t *list, uint16_t *count_inout);
typedef esp_err_t (*web_configure_cb_t)(const char *ssid, const char *password);
typedef esp_err_t (*web_get_status_cb_t)(web_wifi_status_t *status);
typedef esp_err_t (*web_get_saved_cb_t)(web_saved_wifi_t *list, uint8_t *count_inout);
typedef esp_err_t (*web_connect_saved_cb_t)(const char *ssid);
typedef esp_err_t (*web_delete_saved_cb_t)(const char *ssid);
typedef esp_err_t (*web_reset_retry_cb_t)(void);

/**
 * @brief Web 配网模块配置
 */
typedef struct {
    uint16_t http_port;               ///< HTTP 服务器监听端口，一般为 80
    web_scan_cb_t          scan_cb;   ///< 扫描附近 WiFi 的回调
    web_configure_cb_t     configure_cb;    ///< 提交新配置的回调
    web_get_status_cb_t    get_status_cb;   ///< 获取当前状态的回调
    web_get_saved_cb_t     get_saved_cb;    ///< 获取已保存 WiFi 列表的回调
    web_connect_saved_cb_t connect_saved_cb;///< 连接到已保存 WiFi 的回调
    web_delete_saved_cb_t  delete_saved_cb; ///< 删除已保存 WiFi 的回调
    web_reset_retry_cb_t   reset_retry_cb;  ///< 重置重试计数的回调
} web_module_config_t;

/**
 * @brief Web 模块默认配置
 */
#define WEB_MODULE_DEFAULT_CONFIG()            \
    (web_module_config_t){                     \
        .http_port        = 80,                \
        .scan_cb          = NULL,              \
        .configure_cb     = NULL,              \
        .get_status_cb    = NULL,              \
        .get_saved_cb     = NULL,              \
        .connect_saved_cb = NULL,              \
        .delete_saved_cb  = NULL,              \
        .reset_retry_cb   = NULL,              \
    }

/**
 * @brief 启动 Web 配网模块
 *
 * 内部会：
 *  - 初始化/启动 HTTP 服务器；
 *  - 注册与 index.html 对应的 URI 处理函数；
 *  - 挂载 SPIFFS 中的网页资源。
 *
 * @param config Web 模块配置指针；可为 NULL，为 NULL 时使用 WEB_MODULE_DEFAULT_CONFIG。
 * @return esp_err_t
 *         - ESP_OK           : 启动成功
 *         - 其它错误码       : 启动失败
 */
esp_err_t xn_web_module_start(const web_module_config_t *config);

/**
 * @brief 停止 Web 配网模块
 *
 * 关闭 HTTP 服务器并释放相关资源。
 *
 * @return esp_err_t
 */
esp_err_t xn_web_module_stop(void);

#endif /* WEB_MODULE_H */

