/*
 * @Author: 星年 jixingnian@gmail.com
 * @Date: 2025-12-10 14:37:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-12-10 14:37:00
 * @FilePath: \xn_esp32_stt_funasr\main\main.c
 * @Description: ESP32 WiFi配网 + FunASR 语音识别 By.星年
 */

#include <stdio.h>
#include "esp_log.h"
#include "xn_wifi_manage.h"
#include "xn_stt_funasr.h"

static const char *TAG = "main";

static void funasr_result_callback(const char *text, bool is_final, void *user_data)
{
    ESP_LOGI(TAG, "[%s] %s", is_final ? "最终" : "实时", text);
}

static void funasr_status_callback(bool connected, void *user_data)
{
    ESP_LOGI(TAG, "FunASR %s", connected ? "已连接" : "已断开");
}

static void wifi_event_callback(wifi_manage_state_t state)
{
    switch (state) {
    case WIFI_MANAGE_STATE_CONNECTED:
        ESP_LOGI(TAG, "WiFi 已连接，启动 FunASR");
        
        funasr_config_t cfg = {
            .server_url = "ws://win.xingnian.vip:10096",
            .sample_rate = 16000,
            .chunk_size = 6400,
            .hotwords = "阿里巴巴 20",
            .result_cb = funasr_result_callback,
            .status_cb = funasr_status_callback,
            .user_data = NULL,
        };
        
        if (funasr_init(&cfg) == ESP_OK) {
            funasr_connect();
        }
        break;
        
    case WIFI_MANAGE_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi 已断开");
        if (funasr_is_connected()) {
            funasr_disconnect();
            funasr_deinit();
        }
        break;
        
    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 WiFi配网 + FunASR 语音识别 By.星年");
    
    wifi_manage_config_t wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
    wifi_cfg.wifi_event_cb = wifi_event_callback;
    
    esp_err_t ret = wifi_manage_init(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败");
    }
}
