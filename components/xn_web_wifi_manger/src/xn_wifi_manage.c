/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 19:24:41
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\xn_wifi_manage.c
 * @Description: WiFi 管理模块实现。
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"

#include "wifi_module.h"
#include "storage_module.h"
#include "web_module.h"
#include "xn_wifi_manage.h"

/* 日志 TAG，用于 ESP_LOGx 系列接口（如果后续需要打印日志，可直接使用此 TAG） */
static const char *TAG = "xn_wifi_manage";

/* WiFi 管理状态机当前状态。 */
static wifi_manage_state_t s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
static wifi_manage_config_t s_wifi_cfg;
static TaskHandle_t s_wifi_manage_task = NULL;   /* WiFi 管理任务句柄 */

/* 遍历已保存 WiFi 时的状态 */
static bool       s_wifi_connecting   = false;  /* 是否有连接正在进行 */
static uint8_t    s_wifi_try_index    = 0;      /* 当前尝试的 WiFi 下标 */
static TickType_t s_connect_failed_ts = 0;      /* 进入 CONNECT_FAILED 状态的时间戳 */

/* Web 模块回调实现：扫描附近 WiFi */
static esp_err_t web_cb_scan(web_scan_result_t *list, uint16_t *count_inout)
{
    if (list == NULL || count_inout == NULL || *count_inout == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t                     count = *count_inout;
    wifi_module_scan_result_t   *tmp   = (wifi_module_scan_result_t *)calloc(count, sizeof(wifi_module_scan_result_t));
    if (tmp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = xn_wifi_module_scan(tmp, &count);
    if (ret != ESP_OK) {
        free(tmp);
        return ret;
    }

    for (uint16_t i = 0; i < count; ++i) {
        memset(list[i].ssid, 0, sizeof(list[i].ssid));
        strncpy(list[i].ssid, tmp[i].ssid, sizeof(list[i].ssid) - 1);
        list[i].rssi = tmp[i].rssi;
    }

    *count_inout = count;
    free(tmp);
    return ESP_OK;
}

/* Web 模块回调：提交新的 WiFi 配置 */
static esp_err_t web_cb_configure(const char *ssid, const char *password)
{
    return xn_wifi_module_connect(ssid, password);
}

/* Web 模块回调：获取当前 WiFi 状态 */
static esp_err_t web_cb_get_status(web_wifi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));

    wifi_mode_t mode;
    esp_err_t   ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        return ret;
    }

    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        status->connected = false;
        return ESP_OK;
    }

    wifi_ap_record_t ap_info;
    ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        status->connected = false;
        return ESP_OK;
    }

    status->connected = true;
    strncpy(status->ssid, (const char *)ap_info.ssid, sizeof(status->ssid) - 1);
    status->rssi = ap_info.rssi;

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t        *netif   = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_get_ip_info(netif, &ip_info);
    }

    snprintf(status->ip,
             sizeof(status->ip),
             "%d.%d.%d.%d",
             IP2STR(&ip_info.ip));

    snprintf(status->bssid,
             sizeof(status->bssid),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             ap_info.bssid[0],
             ap_info.bssid[1],
             ap_info.bssid[2],
             ap_info.bssid[3],
             ap_info.bssid[4],
             ap_info.bssid[5]);

    return ESP_OK;
}

/* Web 模块回调：获取已保存 WiFi 列表 */
static esp_err_t web_cb_get_saved(web_saved_wifi_t *list, uint8_t *count_inout)
{
    if (list == NULL || count_inout == NULL || *count_inout == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_out = *count_inout;

    uint8_t max_internal = (s_wifi_cfg.save_wifi_count <= 0)
                               ? 1
                               : (uint8_t)s_wifi_cfg.save_wifi_count;

    wifi_config_t *cfg_list = (wifi_config_t *)calloc(max_internal, sizeof(wifi_config_t));
    if (cfg_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = xn_wifi_storage_load_all(cfg_list, &count);
    if (ret != ESP_OK) {
        free(cfg_list);
        return ret;
    }

    if (count == 0) {
        *count_inout = 0;
        free(cfg_list);
        return ESP_OK;
    }

    if (count > max_out) {
        count = max_out;
    }

    for (uint8_t i = 0; i < count; ++i) {
        memset(list[i].ssid, 0, sizeof(list[i].ssid));
        strncpy(list[i].ssid, (const char *)cfg_list[i].sta.ssid, sizeof(list[i].ssid) - 1);
    }

    *count_inout = count;
    free(cfg_list);
    return ESP_OK;
}

/* Web 模块回调：根据保存的配置连接 WiFi */
static esp_err_t web_cb_connect_saved(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_internal = (s_wifi_cfg.save_wifi_count <= 0)
                               ? 1
                               : (uint8_t)s_wifi_cfg.save_wifi_count;

    wifi_config_t *cfg_list = (wifi_config_t *)calloc(max_internal, sizeof(wifi_config_t));
    if (cfg_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = xn_wifi_storage_load_all(cfg_list, &count);
    if (ret != ESP_OK) {
        free(cfg_list);
        return ret;
    }

    for (uint8_t i = 0; i < count; ++i) {
        if (strncmp((const char *)cfg_list[i].sta.ssid, ssid, sizeof(cfg_list[i].sta.ssid)) == 0) {
            const char *pwd = (cfg_list[i].sta.password[0] == '\0')
                                  ? NULL
                                  : (const char *)cfg_list[i].sta.password;
            ret            = xn_wifi_module_connect(ssid, pwd);
            free(cfg_list);
            return ret;
        }
    }

    free(cfg_list);
    return ESP_ERR_NOT_FOUND;
}

/* Web 模块回调：删除已保存 WiFi */
static esp_err_t web_cb_delete_saved(const char *ssid)
{
    return xn_wifi_storage_delete_by_ssid(ssid);
}

/* Web 模块回调：重置管理状态机重试计数 */
static esp_err_t web_cb_reset_retry(void)
{
    s_wifi_try_index    = 0;
    s_wifi_connecting   = false;
    s_connect_failed_ts = 0;
    s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
    return ESP_OK;
}

/**
 * @brief WiFi 模块事件回调
 * 
 */
static void wifi_manage_on_wifi_event(wifi_module_event_t event)
{
    switch (event) {
    case WIFI_MODULE_EVENT_STA_CONNECTED:
        /* WiFi 模块上报：STA 与 AP 建立物理连接（未必已经获取到 IP）
         */
        break;

    case WIFI_MODULE_EVENT_STA_GOT_IP:
        /* WiFi 模块上报：STA 已获取到 IP，认为 WiFi 连接完全成功 */
        s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECTED;
        s_wifi_connecting   = false;
        s_wifi_try_index    = 0;      /* 成功后下次从首选 WiFi 开始 */
        s_connect_failed_ts = 0;

        /* 获取当前 STA 配置，并通知存储模块将其提升为优先 WiFi。 */
        wifi_config_t current_cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) == ESP_OK) {
            (void)xn_wifi_storage_on_connected(&current_cfg);
        }
        break;

    case WIFI_MODULE_EVENT_STA_DISCONNECTED:
        /* WiFi 模块上报：STA 与 AP 连接断开 */
        s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        s_wifi_connecting   = false;
        s_wifi_try_index    = 0;
        break;

    case WIFI_MODULE_EVENT_STA_CONNECT_FAILED:
        /* WiFi 模块上报：本次 STA 连接 AP 过程失败，只标记本轮尝试结束 */
        s_wifi_connecting = false;
        s_wifi_try_index++;          /* 下次在 DISCONNECTED 状态中尝试下一个 WiFi */
        break;

    default:
        /* 未关心的事件类型，暂不做处理 */
        break;
    }
}

/**
 * @brief WiFi 管理状态机
 *
 */
static void xn_wifi_manage_step(void)
{
    /* 根据当前记录的状态，选择相应的处理分支 */
    switch (s_wifi_manage_state) {
    case WIFI_MANAGE_STATE_DISCONNECTED: {
        /* 在断开状态下：顺序遍历所有已保存 WiFi，逐个尝试连接 */

        if (s_wifi_connecting) {
            /* 正在等待本次尝试结果，不再发起新的连接 */
            break;
        }

        /* 读取全部已保存 WiFi 列表，数量由配置 save_wifi_count 控制 */
        uint8_t max_num = (s_wifi_cfg.save_wifi_count <= 0)
                              ? 1
                              : (uint8_t)s_wifi_cfg.save_wifi_count;

        wifi_config_t list[max_num];
        uint8_t       count = 0;

        if (xn_wifi_storage_load_all(list, &count) != ESP_OK || count == 0) {
            /* 当前没有任何可用配置，留给上层做 AP 配网等处理 */
            break;
        }

        if (s_wifi_try_index >= count) {
            /* 本轮已尝试完所有配置，仍未连接成功，进入失败状态 */
            s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECT_FAILED;
            s_connect_failed_ts = xTaskGetTickCount();
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            break;
        }

        wifi_config_t *cfg = &list[s_wifi_try_index];
        if (cfg->sta.ssid[0] == '\0') {
            /* 空 SSID，跳过到下一项 */
            s_wifi_try_index++;
            break;
        }

        const char *ssid     = (const char *)cfg->sta.ssid;
        const char *password = (cfg->sta.password[0] == '\0')
                                   ? NULL
                                   : (const char *)cfg->sta.password;

        if (xn_wifi_module_connect(ssid, password) == ESP_OK) {
            /* 已成功发起本次连接，等待事件回调给结果 */
            s_wifi_connecting = true;
        } else {
            /* 立即认为该配置失败，尝试下一个 */
            s_wifi_try_index++;
        }
        break;
    }

    case WIFI_MANAGE_STATE_CONNECTED:
        /* 已连接状态下暂不做周期性处理，具体业务逻辑留给上层 */
        break;

    case WIFI_MANAGE_STATE_CONNECT_FAILED: {
        /* 所有 WiFi 均已尝试且失败，根据重连间隔定时重试整轮遍历 */

        if (s_wifi_cfg.reconnect_interval_ms < 0) {
            /* 小于 0 表示不自动重连，保持在失败状态 */
            break;
        }

        TickType_t now   = xTaskGetTickCount();
        TickType_t delta = now - s_connect_failed_ts;
        TickType_t need  = pdMS_TO_TICKS((s_wifi_cfg.reconnect_interval_ms <= 0)
                                             ? 0
                                             : s_wifi_cfg.reconnect_interval_ms);

        if (delta >= need) {
            /* 到达重连间隔，重新从第一个 WiFi 开始整轮遍历 */
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        }
        break;
    }

    default:
        /* 理论上不应到达此分支，保留用于兜底保护或调试。 */
        break;
    }
}
 
/**
 * @brief WiFi 管理任务
 *
 * 周期性调用 xn_wifi_manage_step，驱动状态机运行。
 */
static void wifi_manage_task(void *arg)
{
    (void)arg;

    for (;;) {
        xn_wifi_manage_step();
        vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGE_STEP_INTERVAL_MS));
    }
}

/**
 * @brief  WiFi 管理模块初始化
 *
 * @param config  用户传入的 WiFi 管理配置（可为 NULL，此时使用默认值）
 * @return esp_err_t
 *         - ESP_OK        : 初始化成功
 *         - ESP_ERR_NO_MEM: 创建重试定时器失败
 */
esp_err_t xn_wifi_manage_init(const wifi_manage_config_t *config)
{
    if (config == NULL) {
        s_wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
    } else {
        s_wifi_cfg = *config;
    }

    // 初始化 WiFi 模块配置，使用模块提供的默认配置作为基础
    wifi_module_config_t wifi_cfg = WIFI_MODULE_DEFAULT_CONFIG();

    // 根据当前 WiFi 管理配置，决定是否启用 STA / AP 模式
    wifi_cfg.enable_sta = true;  // 始终启用 STA，用于连接路由器上网
    wifi_cfg.enable_ap  = true;  // 始终启用 AP，用于配网或本地访问

    // 拷贝配网 AP 的 SSID 到 WiFi 模块配置
    // 使用 strncpy 避免缓冲区溢出，并手动添加字符串结束符
    strncpy(wifi_cfg.ap_ssid, s_wifi_cfg.ap_ssid, sizeof(wifi_cfg.ap_ssid));
    wifi_cfg.ap_ssid[sizeof(wifi_cfg.ap_ssid) - 1] = '\0';

    // 拷贝配网 AP 的密码到 WiFi 模块配置
    // 同样使用安全拷贝，并确保字符串以 '\0' 结尾
    strncpy(wifi_cfg.ap_password, s_wifi_cfg.ap_password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_password[sizeof(wifi_cfg.ap_password) - 1] = '\0';

    // 拷贝配网 AP 的 IP 地址到 WiFi 模块配置，便于在 AP 接口上设置固定 IP
    strncpy(wifi_cfg.ap_ip, s_wifi_cfg.ap_ip, sizeof(wifi_cfg.ap_ip));
    wifi_cfg.ap_ip[sizeof(wifi_cfg.ap_ip) - 1] = '\0';

    // 将 WiFi 模块事件回调指向当前管理模块，用于在获取到 IP/断开等事件时更新状态机
    wifi_cfg.event_cb = wifi_manage_on_wifi_event;

    // 调用底层 WiFi 模块初始化函数
    // 若初始化失败，则直接返回错误码给上层处理
    esp_err_t ret = xn_wifi_module_init(&wifi_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // 初始化WiFi存储模块
    wifi_storage_config_t storage_cfg = WIFI_STORAGE_DEFAULT_CONFIG();
    // 将管理配置中的 save_wifi_count 映射到存储模块的 max_wifi_num，确保至少为 1
    if (s_wifi_cfg.save_wifi_count <= 0) {
        storage_cfg.max_wifi_num = 1;
    } else {
        storage_cfg.max_wifi_num = (uint8_t)s_wifi_cfg.save_wifi_count;
    }

    ret = xn_wifi_storage_init(&storage_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // 初始化WEB配网模块
    web_module_config_t web_cfg = WEB_MODULE_DEFAULT_CONFIG();
    // 使用管理配置中的 web_port 作为 HTTP 端口，非法值时退回 80
    if (s_wifi_cfg.web_port > 0 && s_wifi_cfg.web_port <= 65535) {
        web_cfg.http_port = (uint16_t)s_wifi_cfg.web_port;
    }
    // 设置 Web 模块回调，所有实际逻辑由当前管理组件和底层模块实现
    web_cfg.scan_cb          = web_cb_scan;
    web_cfg.configure_cb     = web_cb_configure;
    web_cfg.get_status_cb    = web_cb_get_status;
    web_cfg.get_saved_cb     = web_cb_get_saved;
    web_cfg.connect_saved_cb = web_cb_connect_saved;
    web_cfg.delete_saved_cb  = web_cb_delete_saved;
    web_cfg.reset_retry_cb   = web_cb_reset_retry;

    ret = xn_web_module_start(&web_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // 创建WiFi管理任务
    if (s_wifi_manage_task == NULL) {
        BaseType_t ret_task = xTaskCreate(
            wifi_manage_task,
            "wifi_manage",
            4096,               // 任务栈大小，可根据实际需要调整
            NULL,
            tskIDLE_PRIORITY + 1,   // 任务优先级，可根据实际需要调整
            &s_wifi_manage_task);

        if (ret_task != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
