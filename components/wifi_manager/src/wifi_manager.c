/*
 * @Author: xingnian j_xingnian@163.com
 * @Date: 2025-07-29 14:55:50
 * @LastEditors: xingnian j_xingnian@163.com
 * @LastEditTime: 2025-11-14 11:30:00
 * @FilePath: components/wifi_manager/src/wifi_manager.c
 * @Description: WiFi 管理组件（独立于网页服务器）。
 */

#include "wifi_manager.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "WIFI MANAGER";

static int s_retry_num = 0;
// WiFi IP获取回调函数指针
static wifi_got_ip_callback_t s_got_ip_callback = NULL;
// 多WiFi配置管理
static multi_wifi_config_t s_multi_wifi_config = {0};
static bool s_multi_config_loaded = false;
// 添加循环检测变量
static int s_config_attempt_count = 0;  // 记录已尝试的配置数量

// 计算多WiFi配置的校验和
static uint32_t calculate_multi_config_checksum(const multi_wifi_config_t *config)
{
    uint32_t checksum = 0;
    const uint8_t *data = (const uint8_t *)config;
    size_t size = sizeof(multi_wifi_config_t) - sizeof(uint32_t); // 排除checksum字段本身

    for (size_t i = 0; i < size; i++) {
        checksum += data[i];
    }
    return checksum;
}

// 从NVS加载多WiFi配置
esp_err_t wifi_load_multi_configs(multi_wifi_config_t *multi_config)
{
    if (multi_config == NULL) {
        ESP_LOGE(TAG, "多WiFi配置参数为空");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("multi_wifi", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开多WiFi配置NVS失败: %s", esp_err_to_name(err));
        // 初始化默认配置
        memset(multi_config, 0, sizeof(multi_wifi_config_t));
        return ESP_ERR_NOT_FOUND;
    }

    size_t required_size = sizeof(multi_wifi_config_t);
    err = nvs_get_blob(nvs_handle, "config", multi_config, &required_size);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取多WiFi配置失败: %s", esp_err_to_name(err));
        memset(multi_config, 0, sizeof(multi_wifi_config_t));
        return err;
    }

    // 验证校验和
    uint32_t calculated_checksum = calculate_multi_config_checksum(multi_config);
    if (multi_config->checksum != calculated_checksum) {
        ESP_LOGW(TAG, "多WiFi配置校验和不匹配，可能数据损坏");
        memset(multi_config, 0, sizeof(multi_wifi_config_t));
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "成功加载 %d 个WiFi配置", multi_config->count);
    return ESP_OK;
}

// 保存多WiFi配置到NVS
esp_err_t wifi_save_multi_configs(const multi_wifi_config_t *multi_config)
{
    if (multi_config == NULL) {
        ESP_LOGE(TAG, "多WiFi配置参数为空");
        return ESP_ERR_INVALID_ARG;
    }

    // 创建副本并计算校验和
    multi_wifi_config_t config_copy = *multi_config;
    config_copy.checksum = calculate_multi_config_checksum(&config_copy);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("multi_wifi", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开多WiFi配置NVS失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, "config", &config_copy, sizeof(multi_wifi_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存多WiFi配置失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "成功保存 %d 个WiFi配置", config_copy.count);
    }

    return err;
}

// 按优先级排序WiFi配置
static void sort_wifi_configs_by_priority(multi_wifi_config_t *multi_config)
{
    ESP_LOGI(TAG, "WiFi配置排序（按成功连接计数器从大到小）");

    for (int i = 0; i < multi_config->count - 1; i++) {
        for (int j = 0; j < multi_config->count - i - 1; j++) {
            wifi_config_entry_t *a = &multi_config->configs[j];
            wifi_config_entry_t *b = &multi_config->configs[j + 1];

            // 按成功连接计数器排序（数值越大越优先）
            if (a->success_counter < b->success_counter) {
                wifi_config_entry_t temp = *a;
                *a = *b;
                *b = temp;
            }
            // 如果计数器相同，按优先级排序（数值越小优先级越高）
            else if (a->success_counter == b->success_counter && a->priority > b->priority) {
                wifi_config_entry_t temp = *a;
                *a = *b;
                *b = temp;
            }
        }
    }

    ESP_LOGI(TAG, "排序后的WiFi列表:");
    for (int i = 0; i < multi_config->count; i++) {
        ESP_LOGI(TAG, "  [%d] %s (计数器: %lu, 优先级: %lu)",
                 i, multi_config->configs[i].config.sta.ssid,
                 multi_config->configs[i].success_counter,
                 multi_config->configs[i].priority);
    }
}

// 添加WiFi配置
esp_err_t wifi_add_config(const wifi_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "WiFi配置参数为空");
        return ESP_ERR_INVALID_ARG;
    }

    // 加载现有配置
    if (!s_multi_config_loaded) {
        wifi_load_multi_configs(&s_multi_wifi_config);
        s_multi_config_loaded = true;
    }

    // 检查是否已存在相同SSID的配置
    for (int i = 0; i < s_multi_wifi_config.count; i++) {
        if (strcmp((char *)s_multi_wifi_config.configs[i].config.sta.ssid,
                   (char *)config->sta.ssid) == 0) {
            // 找到当前最大的计数器值
            uint32_t max_counter = 0;
            for (int j = 0; j < s_multi_wifi_config.count; j++) {
                if (s_multi_wifi_config.configs[j].success_counter > max_counter) {
                    max_counter = s_multi_wifi_config.configs[j].success_counter;
                }
            }

            // 更新现有配置，并提升优先级
            s_multi_wifi_config.configs[i].config = *config;
            s_multi_wifi_config.configs[i].success_counter = max_counter + 1;
            ESP_LOGI(TAG, "更新WiFi配置: %s (计数器: %lu，优先级提升)",
                     config->sta.ssid, s_multi_wifi_config.configs[i].success_counter);

            // 重新排序
            sort_wifi_configs_by_priority(&s_multi_wifi_config);

            return wifi_save_multi_configs(&s_multi_wifi_config);
        }
    }

    // 添加新配置
    if (s_multi_wifi_config.count >= MAX_WIFI_CONFIGS) {
        // 删除优先级最低的配置（最后一个）
        ESP_LOGW(TAG, "WiFi配置已满，删除优先级最低的配置: %s",
                 s_multi_wifi_config.configs[MAX_WIFI_CONFIGS - 1].config.sta.ssid);
        s_multi_wifi_config.count--;
    }

    // 找到当前最大的计数器值
    uint32_t max_counter = 0;
    for (int i = 0; i < s_multi_wifi_config.count; i++) {
        if (s_multi_wifi_config.configs[i].success_counter > max_counter) {
            max_counter = s_multi_wifi_config.configs[i].success_counter;
        }
    }

    // 添加新配置到末尾，给它最高优先级（最大计数器+1）
    wifi_config_entry_t *new_entry = &s_multi_wifi_config.configs[s_multi_wifi_config.count];
    new_entry->config = *config;
    new_entry->priority = s_multi_wifi_config.count;
    new_entry->success_counter = max_counter + 1;  // 新WiFi获得最高优先级
    new_entry->is_valid = true;
    s_multi_wifi_config.count++;

    ESP_LOGI(TAG, "添加新WiFi配置: %s (计数器: %lu，将获得最高优先级)",
             config->sta.ssid, new_entry->success_counter);

    // 重新排序
    sort_wifi_configs_by_priority(&s_multi_wifi_config);

    return wifi_save_multi_configs(&s_multi_wifi_config);
}

// 删除WiFi配置
esp_err_t wifi_remove_config(const char *ssid)
{
    if (ssid == NULL) {
        ESP_LOGE(TAG, "SSID参数为空");
        return ESP_ERR_INVALID_ARG;
    }

    // 加载现有配置
    if (!s_multi_config_loaded) {
        wifi_load_multi_configs(&s_multi_wifi_config);
        s_multi_config_loaded = true;
    }

    // 查找并删除配置
    for (int i = 0; i < s_multi_wifi_config.count; i++) {
        if (strcmp((char *)s_multi_wifi_config.configs[i].config.sta.ssid, ssid) == 0) {
            // 移动后续配置向前
            for (int j = i; j < s_multi_wifi_config.count - 1; j++) {
                s_multi_wifi_config.configs[j] = s_multi_wifi_config.configs[j + 1];
            }
            s_multi_wifi_config.count--;
            ESP_LOGI(TAG, "删除WiFi配置: %s", ssid);
            return wifi_save_multi_configs(&s_multi_wifi_config);
        }
    }

    ESP_LOGW(TAG, "未找到要删除的WiFi配置: %s", ssid);
    return ESP_ERR_NOT_FOUND;
}

// 获取保存的WiFi配置列表
esp_err_t wifi_get_saved_configs(wifi_config_entry_t **configs, uint8_t *count)
{
    if (configs == NULL || count == NULL) {
        ESP_LOGE(TAG, "参数为空");
        return ESP_ERR_INVALID_ARG;
    }

    // 加载现有配置
    if (!s_multi_config_loaded) {
        wifi_load_multi_configs(&s_multi_wifi_config);
        s_multi_config_loaded = true;
    }

    *configs = s_multi_wifi_config.configs;
    *count = s_multi_wifi_config.count;
    return ESP_OK;
}

// 更新WiFi配置的成功连接计数器
static void update_wifi_success_time(const char *ssid)
{
    // 找到当前最大的计数器值
    uint32_t max_counter = 0;
    for (int i = 0; i < s_multi_wifi_config.count; i++) {
        if (s_multi_wifi_config.configs[i].success_counter > max_counter) {
            max_counter = s_multi_wifi_config.configs[i].success_counter;
        }
    }

    // 更新成功连接的WiFi，设置为最高计数器+1
    for (int i = 0; i < s_multi_wifi_config.count; i++) {
        if (strcmp((char *)s_multi_wifi_config.configs[i].config.sta.ssid, ssid) == 0) {
            // 设置为最高计数器+1，确保下次开机优先连接这个WiFi
            s_multi_wifi_config.configs[i].success_counter = max_counter + 1;
            ESP_LOGI(TAG, "WiFi %s 成功连接，计数器: %lu (设置为最高优先级)",
                     ssid, s_multi_wifi_config.configs[i].success_counter);

            // 重新排序配置
            sort_wifi_configs_by_priority(&s_multi_wifi_config);
            // 保存到NVS
            wifi_save_multi_configs(&s_multi_wifi_config);
            break;
        }
    }
}

// 尝试连接下一个WiFi配置
esp_err_t wifi_connect_next_config(void)
{
    // 加载现有配置
    if (!s_multi_config_loaded) {
        wifi_load_multi_configs(&s_multi_wifi_config);
        s_multi_config_loaded = true;
    }

    if (s_multi_wifi_config.count == 0) {
        ESP_LOGW(TAG, "没有保存的WiFi配置");
        return ESP_ERR_NOT_FOUND;
    }

    // 尝试下一个配置
    s_multi_wifi_config.current_index++;
    if (s_multi_wifi_config.current_index >= s_multi_wifi_config.count) {
        s_multi_wifi_config.current_index = 0;
    }

    wifi_config_entry_t *current_config = &s_multi_wifi_config.configs[s_multi_wifi_config.current_index];
    ESP_LOGI(TAG, "尝试连接WiFi: %s (索引: %d)",
             current_config->config.sta.ssid, s_multi_wifi_config.current_index);

    esp_err_t err = esp_wifi_set_config(ESP_IF_WIFI_STA, &current_config->config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }

    return err;
}

// WiFi事件处理函数
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            {
                wifi_event_ap_staconnected_t *ap_event = (wifi_event_ap_staconnected_t *) event_data;
                ESP_LOGI(TAG, "设备 "MACSTR" 已连接, AID=%d",
                         MAC2STR(ap_event->mac), ap_event->aid);
            }
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            {
                wifi_event_ap_stadisconnected_t *ap_disc_event = (wifi_event_ap_stadisconnected_t *) event_data;
                ESP_LOGI(TAG, "设备 "MACSTR" 已断开连接, AID=%d",
                         MAC2STR(ap_disc_event->mac), ap_disc_event->aid);
            }
            break;
        case WIFI_EVENT_STA_START:
            // 尝试连接到AP
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED,已连接到AP");
            s_retry_num = 0; // 重置重试计数
            // 注意：s_config_attempt_count 在获取到IP后才重置，这里不重置
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *) event_data;
                ESP_LOGW(TAG, "WiFi断开连接,原因:%d", event->reason);
                if (s_retry_num < MAX_RETRY_COUNT) {
                    ESP_LOGI(TAG, "重试连接到当前AP... (%d/%d)", s_retry_num + 1, MAX_RETRY_COUNT);
                    esp_wifi_connect();
                    s_retry_num++;
                } else {
                    // 当前WiFi重试次数用完，尝试下一个WiFi配置
                    s_retry_num = 0; // 重置重试计数

                    // 加载多WiFi配置
                    if (!s_multi_config_loaded) {
                        wifi_load_multi_configs(&s_multi_wifi_config);
                        s_multi_config_loaded = true;
                    }

                    // 检查是否已经尝试过所有配置
                    s_config_attempt_count++;
                    ESP_LOGI(TAG, "WiFi连接失败 (已尝试: %d/%d)",
                             s_config_attempt_count, s_multi_wifi_config.count);

                    // 如果还有未尝试的配置，尝试下一个
                    if (s_config_attempt_count < s_multi_wifi_config.count) {
                        esp_err_t err = wifi_connect_next_config();
                        if (err == ESP_OK) {
                            ESP_LOGI(TAG, "切换到下一个WiFi配置");
                            break; // 继续尝试连接，不启动AP模式
                        }
                    }

                    // 所有WiFi配置都连接失败，保存失败状态
                    ESP_LOGW(TAG, "所有WiFi配置都尝试失败，保持AP模式运行");
                    ESP_LOGI(TAG, "可以连接到 %s 进行配网", ESP_AP_SSID);

                    // 保存连接失败状态到NVS
                    nvs_handle_t nvs_handle;
                    esp_err_t err = nvs_open("wifi_state", NVS_READWRITE, &nvs_handle);
                    if (err == ESP_OK) {
                        nvs_set_u8(nvs_handle, "connection_failed", 1);
                        nvs_commit(nvs_handle);
                        nvs_close(nvs_handle);
                    }
                }
            }
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
            ESP_LOGI(TAG, "获取到IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_num = 0; // 重置重试计数
            s_config_attempt_count = 0; // 重置配置尝试计数（在获取IP后才重置）

            // 获取当前连接的WiFi SSID
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                // 更新成功连接的WiFi优先级
                update_wifi_success_time((char *)ap_info.ssid);
                ESP_LOGI(TAG, "成功连接到WiFi: %s", ap_info.ssid);
            }

            // 保存成功状态到NVS
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open("wifi_state", NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK) {
                nvs_set_u8(nvs_handle, "connection_failed", 0);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
            }

            // 调用注册的回调函数
            if (s_got_ip_callback != NULL) {
                s_got_ip_callback(&event->ip_info);
            }
        }
    }
}

// 初始化WiFi软AP（AP+STA），不启动网页服务器
esp_err_t wifi_init_softap(void)
{
    // 允许在已初始化情况下重复调用（返回 ESP_ERR_INVALID_STATE 时忽略）
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 同时创建STA和AP接口
    esp_netif_create_default_wifi_sta(); // 创建默认WIFI STA
    esp_netif_create_default_wifi_ap();  // 创建默认WIFI AP

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // 使用默认WiFi初始化配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));  // 初始化WiFi

    // 注册WiFi事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // 加载多WiFi配置
    esp_err_t load_err = wifi_load_multi_configs(&s_multi_wifi_config);
    wifi_config_t sta_config;
    bool config_valid = false;
    bool need_reload = false;

    if (load_err == ESP_OK && s_multi_wifi_config.count > 0) {
        // 按优先级排序配置（最近成功连接的在前面）
        sort_wifi_configs_by_priority(&s_multi_wifi_config);

        // 使用优先级最高的配置（索引0）
        sta_config = s_multi_wifi_config.configs[0].config;
        s_multi_wifi_config.current_index = 0;
        config_valid = true;
        s_multi_config_loaded = true;

        ESP_LOGI(TAG, "使用优先级最高的WiFi配置: %s", sta_config.sta.ssid);
    } else {
        // 尝试从旧的单WiFi配置迁移
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
        if (err == ESP_OK) {
            uint32_t checksum = 0;
            err = nvs_get_u32(nvs_handle, "checksum", &checksum);
            if (err == ESP_OK && checksum == 0x12345678) {
                size_t size = sizeof(wifi_config_t);
                err = nvs_get_blob(nvs_handle, "sta_config", &sta_config, &size);
                if (err == ESP_OK && size == sizeof(wifi_config_t)) {
                    // 迁移旧配置到新的多WiFi系统
                    ESP_LOGI(TAG, "发现旧WiFi配置，迁移到多WiFi系统: %s", sta_config.sta.ssid);
                    wifi_add_config(&sta_config);
                    need_reload = true;  // 标记需要重新加载
                }
            }
            nvs_close(nvs_handle);
        }

        if (!need_reload) {
            // 初始化默认配置
            ESP_LOGI(TAG, "使用默认WiFi配置");
            memset(&sta_config, 0, sizeof(wifi_config_t));
            strcpy((char *)sta_config.sta.ssid, "xingnian");
            strcpy((char *)sta_config.sta.password, "12345678");

            // 添加到多WiFi配置系统
            wifi_add_config(&sta_config);
            need_reload = true;  // 标记需要重新加载
        }

        // 重新加载配置以获取正确排序后的最高优先级WiFi
        if (need_reload) {
            load_err = wifi_load_multi_configs(&s_multi_wifi_config);
            if (load_err == ESP_OK && s_multi_wifi_config.count > 0) {
                sort_wifi_configs_by_priority(&s_multi_wifi_config);
                sta_config = s_multi_wifi_config.configs[0].config;
                s_multi_wifi_config.current_index = 0;
                s_multi_config_loaded = true;
                config_valid = true;
                ESP_LOGI(TAG, "重新加载后使用优先级最高的WiFi: %s", sta_config.sta.ssid);
            } else {
                ESP_LOGE(TAG, "重新加载WiFi配置失败");
            }
        }
    }

    // 配置AP参数
    wifi_config_t ap_config = {
        .ap = {
            .ssid = ESP_AP_SSID,
            .ssid_len = strlen(ESP_AP_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_AP_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = true
            },
        },
    };

    // 如果没有设置密码，使用开放认证
    if (strlen(ESP_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // 设置为AP+STA模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 配置AP和STA
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    if (config_valid) {
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    }

    // 启动WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成(AP+STA模式)");
    ESP_LOGI(TAG, "  AP SSID: %s, 密码: %s, 信道: %d", ESP_AP_SSID, ESP_AP_PASS, ESP_WIFI_CHANNEL);
    if (config_valid) {
        ESP_LOGI(TAG, "  STA SSID: %s", sta_config.sta.ssid);
    }

    return ESP_OK;
}

esp_err_t wifi_reset_connection_retry(void)
{
    // 重置重试计数
    s_retry_num = 0;
    // 重置配置尝试计数
    s_config_attempt_count = 0;

    // 重置NVS中的连接失败标志
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_state", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, "connection_failed", 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    return ESP_OK;
}

// 扫描周围WiFi网络
esp_err_t wifi_scan_networks(wifi_ap_record_t **ap_records, uint16_t *ap_count)
{
    esp_err_t ret;
    uint16_t number = DEFAULT_SCAN_LIST_SIZE;

    // 分配内存用于存储扫描结果
    *ap_records = malloc(DEFAULT_SCAN_LIST_SIZE * sizeof(wifi_ap_record_t));
    if (*ap_records == NULL) {
        ESP_LOGE(TAG, "为扫描结果分配内存失败");
        return ESP_ERR_NO_MEM;
    }

    // 配置扫描参数
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 150,
    };

    // 开始扫描
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "开始扫描失败");
        free(*ap_records);
        *ap_records = NULL;
        return ret;
    }

    // 获取扫描结果
    ret = esp_wifi_scan_get_ap_records(&number, *ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取扫描结果失败");
        free(*ap_records);
        *ap_records = NULL;
        return ret;
    }

    // 获取找到的AP数量
    ret = esp_wifi_scan_get_ap_num(ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取扫描到的AP数量失败");
        free(*ap_records);
        *ap_records = NULL;
        return ret;
    }

    // 限制AP数量不超过默认扫描列表大小
    if (*ap_count > DEFAULT_SCAN_LIST_SIZE) {
        *ap_count = DEFAULT_SCAN_LIST_SIZE;
    }

    // 打印扫描结果
    ESP_LOGI(TAG, "发现 %d 个接入点:", *ap_count);
    for (int i = 0; i < *ap_count; i++) {
        ESP_LOGI(TAG, "SSID: %s, 信号强度: %d", (*ap_records)[i].ssid, (*ap_records)[i].rssi);
    }

    return ESP_OK;
}

// 保存WiFi配置到NVS（旧接口，兼容性保留，内部使用新的多WiFi系统）
esp_err_t wifi_save_config_to_nvs(const wifi_config_t *sta_config)
{
    if (sta_config == NULL) {
        ESP_LOGE(TAG, "WiFi配置参数为空");
        return ESP_ERR_INVALID_ARG;
    }

    // 使用新的多WiFi配置系统
    esp_err_t err = wifi_add_config(sta_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi配置已保存到多WiFi系统，SSID: %s", sta_config->sta.ssid);
    }

    return err;
}

// 注册WiFi IP获取回调函数
esp_err_t wifi_register_got_ip_callback(wifi_got_ip_callback_t callback)
{
    if (callback == NULL) {
        ESP_LOGE(TAG, "回调函数指针不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    s_got_ip_callback = callback;
    ESP_LOGI(TAG, "WiFi IP获取回调函数注册成功");
    return ESP_OK;
}
