/*
 * @Author: 星年 jixingnian@gmail.com
 * @Date: 2025-12-10 14:57:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-12-10 15:04:33
 * @FilePath: \xn_esp32_stt_funasr\main\main.c
 * @Description: ESP32 WiFi配网 + 音频采集 + FunASR 语音识别 By.星年
 */

#include <stdio.h>
#include "esp_log.h"
#include "xn_wifi_manage.h"
#include "xn_stt_funasr.h"
#include "audio_manager.h"
#include "audio_config_app.h"

static const char *TAG = "main";

static bool s_recording = false;

// ========== FunASR 回调 ==========

static void funasr_result_callback(const char *text, bool is_final, void *user_data)
{
    ESP_LOGI(TAG, "[%s] %s", is_final ? "最终" : "实时", text);
    
    if (is_final) {
        // 识别完成，停止录音
        audio_manager_stop_recording();
        s_recording = false;
        ESP_LOGI(TAG, "识别完成，停止录音");
    }
}

static void funasr_status_callback(bool connected, void *user_data)
{
    ESP_LOGI(TAG, "FunASR %s", connected ? "已连接" : "已断开");
    
}

// ========== 音频录音回调 ==========

static void audio_record_callback(const int16_t *pcm_data, size_t sample_count, void *user_ctx)
{
    // 始终消费数据,避免 AFE 缓冲区溢出
    // 只有在录音且连接时才发送到服务器
    if (s_recording && funasr_is_connected()) {
        size_t bytes = sample_count * sizeof(int16_t);
        funasr_send_audio((const uint8_t *)pcm_data, bytes);
    } else if (s_recording && !funasr_is_connected()) {
        // 调试:录音中但未连接
        static uint32_t warn_count = 0;
        if (warn_count++ % 100 == 0) {  // 每100帧打印一次,避免刷屏
            ESP_LOGW(TAG, "录音中但 FunASR 未连接,数据未发送");
        }
    }
    // 如果不录音,数据被丢弃,但缓冲区被清空
}

// ========== 音频事件回调 ==========

static void audio_event_callback(const audio_mgr_event_t *event, void *user_ctx)
{
    switch (event->type) {
    case AUDIO_MGR_EVENT_BUTTON_TRIGGER:
        ESP_LOGI(TAG, "按键触发，开始识别");
        if (!funasr_is_connected()) {
            ESP_LOGW(TAG, "⚠️ FunASR 未连接,无法开始识别");
        } else if (s_recording) {
            ESP_LOGW(TAG, "⚠️ 已在录音中");
        } else {
            // 开始 FunASR 识别会话
            if (funasr_start() == ESP_OK) {
                // 开始录音
                audio_manager_start_recording();
                s_recording = true;
                ESP_LOGI(TAG, "✅ 开始录音和识别");
            } else {
                ESP_LOGE(TAG, "❌ FunASR 启动失败");
            }
        }
        break;
        
    case AUDIO_MGR_EVENT_BUTTON_RELEASE:
        ESP_LOGI(TAG, "按键松开");
        if (s_recording) {
            // 先设置标志,停止发送数据
            s_recording = false;
            // 停止录音
            audio_manager_stop_recording();
            // 最后停止识别
            funasr_stop();
            ESP_LOGI(TAG, "停止录音和识别");
        }
        break;
        
    case AUDIO_MGR_EVENT_VAD_START:
        ESP_LOGI(TAG, "检测到人声");
        break;
        
    case AUDIO_MGR_EVENT_VAD_END:
        ESP_LOGI(TAG, "人声结束");
        break;
        
    default:
        break;
    }
}

// ========== WiFi 事件回调 ==========

static void wifi_event_callback(wifi_manage_state_t state)
{
    switch (state) {
    case WIFI_MANAGE_STATE_CONNECTED:
        ESP_LOGI(TAG, "WiFi 已连接，启动 FunASR");
        
        funasr_config_t cfg = {
            .server_url = "ws://win.xingnian.vip:10096",
            .sample_rate = 16000,
            .chunk_size = 6400,
            .hotwords = NULL,  // 暂时禁用热词,避免服务器崩溃
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
        if (s_recording) {
            audio_manager_stop_recording();
            s_recording = false;
        }
        if (funasr_is_connected()) {
            funasr_disconnect();
            funasr_deinit();
        }
        audio_manager_stop();
        break;
        
    default:
        break;
    }
}

// ========== 主函数 ==========

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32 WiFi配网 + 音频 + FunASR 语音识别");
    ESP_LOGI(TAG, "By.星年");
    ESP_LOGI(TAG, "========================================");
    
    // 初始化音频管理器
    audio_mgr_config_t audio_cfg;
    audio_config_app_build(&audio_cfg, audio_event_callback, NULL);
    
    esp_err_t ret = audio_manager_init(&audio_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频管理器初始化失败");
        return;
    }
    
    // 注册录音数据回调
    audio_manager_set_record_callback(audio_record_callback, NULL);
    
    audio_manager_start();

    ESP_LOGI(TAG, "音频管理器初始化成功");
    
    // 初始化 WiFi 管理器
    wifi_manage_config_t wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
    wifi_cfg.wifi_event_cb = wifi_event_callback;
    
    ret = wifi_manage_init(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败");
        return;
    }
    
    ESP_LOGI(TAG, "WiFi 管理器初始化成功");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "使用说明：");
    ESP_LOGI(TAG, "1. 连接 WiFi AP: XN-ESP32-AP (密码: 12345678)");
    ESP_LOGI(TAG, "2. 浏览器访问: http://192.168.4.1");
    ESP_LOGI(TAG, "3. 配置 WiFi 后自动连接 FunASR 服务器");
    ESP_LOGI(TAG, "4. 按下按键开始语音识别，松开按键结束");
    ESP_LOGI(TAG, "========================================");
}
