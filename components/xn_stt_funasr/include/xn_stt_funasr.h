/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-12-10 14:37:00
 * @FilePath: \xn_esp32_stt_funasr\components\xn_stt_funasr\include\xn_stt_funasr.h
 * @Description: FunASR 语音识别客户端组件对外接口
 */

#ifndef XN_STT_FUNASR_H
#define XN_STT_FUNASR_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 识别结果回调函数
 * @param text 识别文本
 * @param is_final 是否为最终结果
 * @param user_data 用户数据
 */
typedef void (*funasr_result_cb_t)(const char *text, bool is_final, void *user_data);

/**
 * @brief 连接状态回调函数
 * @param connected true=已连接, false=已断开
 * @param user_data 用户数据
 */
typedef void (*funasr_status_cb_t)(bool connected, void *user_data);

/**
 * @brief FunASR 客户端配置
 */
typedef struct {
    const char *server_url;         ///< 服务器地址，如 "ws://192.168.1.100:10096"
    int sample_rate;                ///< 采样率，默认 16000
    int chunk_size;                 ///< 音频块大小（字节），默认 6400
    const char *hotwords;           ///< 热词，如 "阿里巴巴 20"
    funasr_result_cb_t result_cb;   ///< 识别结果回调
    funasr_status_cb_t status_cb;   ///< 连接状态回调
    void *user_data;                ///< 用户数据指针
} funasr_config_t;

/**
 * @brief 初始化 FunASR 客户端
 * @param config 配置参数
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_init(const funasr_config_t *config);

/**
 * @brief 反初始化 FunASR 客户端
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_deinit(void);

/**
 * @brief 连接到 FunASR 服务器
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_connect(void);

/**
 * @brief 断开连接
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_disconnect(void);

/**
 * @brief 开始识别会话
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_start(void);

/**
 * @brief 发送音频数据
 * @param data 音频数据
 * @param len 数据长度
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_send_audio(const uint8_t *data, size_t len);

/**
 * @brief 停止识别会话
 * @return ESP_OK 成功，其他失败
 */
esp_err_t funasr_stop(void);

/**
 * @brief 检查是否已连接
 * @return true 已连接，false 未连接
 */
bool funasr_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* XN_STT_FUNASR_H */
