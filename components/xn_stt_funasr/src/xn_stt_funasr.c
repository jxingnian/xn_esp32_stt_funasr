/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-12-10 14:37:00
 * @FilePath: \xn_esp32_stt_funasr\components\xn_stt_funasr\src\xn_stt_funasr.c
 * @Description: FunASR 语音识别客户端实现
 */

#include "xn_stt_funasr.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "funasr";

typedef struct {
    esp_websocket_client_handle_t ws_client;
    funasr_config_t config;
    bool connected;
    bool started;
} funasr_ctx_t;

static funasr_ctx_t *s_ctx = NULL;

static void ws_event_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ctx->connected = true;
        if (s_ctx->config.status_cb) {
            s_ctx->config.status_cb(true, s_ctx->config.user_data);
        }
        break;
        
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        s_ctx->connected = false;
        s_ctx->started = false;
        if (s_ctx->config.status_cb) {
            s_ctx->config.status_cb(false, s_ctx->config.user_data);
        }
        break;
        
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 && data->data_len > 0) {
            char *json_str = strndup((char *)data->data_ptr, data->data_len);
            if (json_str) {
                cJSON *root = cJSON_Parse(json_str);
                if (root) {
                    cJSON *text = cJSON_GetObjectItem(root, "text");
                    cJSON *is_final = cJSON_GetObjectItem(root, "is_final");
                    
                    if (text && cJSON_IsString(text) && s_ctx->config.result_cb) {
                        bool final = (is_final && cJSON_IsTrue(is_final));
                        s_ctx->config.result_cb(text->valuestring, final, s_ctx->config.user_data);
                    }
                    cJSON_Delete(root);
                }
                free(json_str);
            }
        }
        break;
        
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;
        
    default:
        break;
    }
}

esp_err_t funasr_init(const funasr_config_t *config)
{
    if (!config || !config->server_url) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_ctx) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_ctx = calloc(1, sizeof(funasr_ctx_t));
    if (!s_ctx) {
        ESP_LOGE(TAG, "No memory");
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(&s_ctx->config, config, sizeof(funasr_config_t));
    
    esp_websocket_client_config_t ws_cfg = {
        .uri = config->server_url,
        .buffer_size = 4096,
    };
    
    s_ctx->ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ctx->ws_client) {
        free(s_ctx);
        s_ctx = NULL;
        ESP_LOGE(TAG, "Failed to init websocket client");
        return ESP_FAIL;
    }
    
    esp_websocket_register_events(s_ctx->ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    
    ESP_LOGI(TAG, "FunASR initialized");
    return ESP_OK;
}

esp_err_t funasr_deinit(void)
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx->connected) {
        funasr_disconnect();
    }
    
    if (s_ctx->ws_client) {
        esp_websocket_client_destroy(s_ctx->ws_client);
    }
    
    free(s_ctx);
    s_ctx = NULL;
    
    ESP_LOGI(TAG, "FunASR deinitialized");
    return ESP_OK;
}

esp_err_t funasr_connect(void)
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx->connected) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }
    
    esp_err_t ret = esp_websocket_client_start(s_ctx->ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start websocket client");
        return ret;
    }
    
    ESP_LOGI(TAG, "Connecting to %s", s_ctx->config.server_url);
    return ESP_OK;
}

esp_err_t funasr_disconnect(void)
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx->started) {
        funasr_stop();
    }
    
    if (s_ctx->ws_client) {
        esp_websocket_client_stop(s_ctx->ws_client);
    }
    
    s_ctx->connected = false;
    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

esp_err_t funasr_start(void)
{
    if (!s_ctx || !s_ctx->connected) {
        ESP_LOGE(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx->started) {
        ESP_LOGW(TAG, "Already started");
        return ESP_OK;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(root, "mode", "2pass");
    cJSON_AddNumberToObject(root, "chunk_size", s_ctx->config.chunk_size ? s_ctx->config.chunk_size : 6400);
    cJSON_AddNumberToObject(root, "chunk_interval", 200);
    cJSON_AddStringToObject(root, "wav_name", "esp32");
    cJSON_AddBoolToObject(root, "is_speaking", true);
    cJSON_AddStringToObject(root, "wav_format", "pcm");
    cJSON_AddNumberToObject(root, "audio_fs", s_ctx->config.sample_rate ? s_ctx->config.sample_rate : 16000);
    cJSON_AddBoolToObject(root, "itn", true);
    
    if (s_ctx->config.hotwords) {
        cJSON_AddStringToObject(root, "hotwords", s_ctx->config.hotwords);
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    
    int ret = esp_websocket_client_send_text(s_ctx->ws_client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send start message");
        return ESP_FAIL;
    }
    
    s_ctx->started = true;
    ESP_LOGI(TAG, "Recognition started");
    return ESP_OK;
}

esp_err_t funasr_send_audio(const uint8_t *data, size_t len)
{
    if (!s_ctx || !s_ctx->connected || !s_ctx->started) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int ret = esp_websocket_client_send_bin(s_ctx->ws_client, (const char *)data, len, portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send audio");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t funasr_stop(void)
{
    if (!s_ctx || !s_ctx->started) {
        return ESP_ERR_INVALID_STATE;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddBoolToObject(root, "is_speaking", false);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    
    int ret = esp_websocket_client_send_text(s_ctx->ws_client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send stop message");
        return ESP_FAIL;
    }
    
    s_ctx->started = false;
    ESP_LOGI(TAG, "Recognition stopped");
    return ESP_OK;
}

bool funasr_is_connected(void)
{
    return (s_ctx && s_ctx->connected);
}
