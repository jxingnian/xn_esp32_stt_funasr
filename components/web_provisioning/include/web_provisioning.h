#ifndef WEB_PROVISIONING_H
#define WEB_PROVISIONING_H

/**
 * 组件名称：web_provisioning（网页配网）
 *
 * 组件作用：
 * - 提供一个基于 esp_http_server 的轻量级 HTTP 服务，用于在本地网络中展示设备的配网页面（SPIFFS 中的 /spiffs/index.html）。
 * - 接收来自网页的配网表单（POST /wifi），解析 SSID 与密码，并触发设备连接到指定 AP。
 * - 通过回调接口将配网结果（ssid/password）通知到上层业务（例如保存至 NVS 或继续后续流程）。
 *
 * 设计要点：
 * - 封装启动/停止/运行状态查询 API，避免与其它 HTTP 服务直接耦合。
 * - 使用 SPIFFS 作为静态资源存储，页面由构建脚本打包到分区镜像（参见 main/CMakeLists.txt:66-69）。
 * - 为避免端口冲突，可在启动时传入非 80 端口（例如 8080），或在启用该组件前停止旧的 HTTP 服务。
 *
 * 使用示例：
 *   #include "web_provisioning.h"
 *   static void on_provision(const char *ssid, const char *password) {
 *       // 例如：写入 NVS，或更新全局 WiFi 配置
 *   }
 *   void app_init() {
 *       web_provisioning_set_callback(on_provision);
 *       web_provisioning_start(80);
 *   }
 */

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 回调类型：网页提交配网信息后通知上层
 * @param ssid     提交的 WiFi SSID（最长 32 字节）
 * @param password 提交的 WiFi 密码（最长 64 字节）
 */
typedef void (*web_provisioning_result_cb)(const char *ssid, const char *password);

/**
 * 启动网页配网 HTTP 服务
 * @param port 指定监听端口；传 0 或未设置时默认使用 80
 * @return ESP_OK 启动成功；其它为错误码
 */
esp_err_t web_provisioning_start(uint16_t port);

/**
 * 停止网页配网 HTTP 服务
 * @return ESP_OK 停止成功；其它为错误码
 */
esp_err_t web_provisioning_stop(void);

/**
 * 查询网页配网服务是否正在运行
 * @return true 正在运行；false 未运行
 */
bool web_provisioning_running(void);

/**
 * 设置配网结果回调
 * @param cb 回调函数指针；传 NULL 可取消回调
 */
void web_provisioning_set_callback(web_provisioning_result_cb cb);

#ifdef __cplusplus
}
#endif

#endif
