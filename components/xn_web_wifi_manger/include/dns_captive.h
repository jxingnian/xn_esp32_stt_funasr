#ifndef DNS_CAPTIVE_H
#define DNS_CAPTIVE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动简易 DNS 服务器，将所有域名解析到指定 AP IP。
 *
 * @param ap_ip  AP 网口 IPv4 地址字符串，例如 "192.168.4.1"
 */
esp_err_t dns_captive_start(const char *ap_ip);

/**
 * @brief 停止 DNS 服务器（当前实现为简单删除任务）。
 */
void dns_captive_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DNS_CAPTIVE_H */
