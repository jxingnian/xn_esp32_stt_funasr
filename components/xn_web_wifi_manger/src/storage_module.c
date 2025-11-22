/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 18:20:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 18:38:54
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\storage_module.c
 * @Description: WiFi 存储模块实现
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "storage_module.h"

/* 日志 TAG */
static const char *TAG = "wifi_storage";

/* 保存存储模块配置及初始化状态 */
static wifi_storage_config_t s_storage_cfg;
static bool                  s_storage_inited = false;

/* 存储 WiFi 列表所用的 NVS key 名称 */
static const char *WIFI_LIST_KEY = "wifi_list";

/**
 * @brief 初始化 NVS，用于 WiFi 存储模块
 *
 * 该函数用于确保 NVS 子系统处于可用状态：
 *  - 如果 NVS 首次初始化或已正常初始化，则直接返回 ESP_OK；
 *  - 如果检测到 NVS 页空间不足或版本不兼容，则擦除整个 NVS 分区并重新初始化。
 *
 * @return esp_err_t
 *         - ESP_OK                      : 初始化成功
 *         - 其它 esp_err_t 错误码      : 初始化失败
 */
static esp_err_t wifi_storage_init_nvs(void)
{
    /* 调用 NVS 初始化接口 */
    esp_err_t ret = nvs_flash_init();

    /* 
     * 若返回值是空间不足或版本变更，说明需要擦除 NVS 分区重新初始化。
     * ESP_ERROR_CHECK 在调试版本下会在出错时中止程序，是 Espressif 推荐用法。
     */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

/**
 * @brief 比较两个 wifi_config_t 是否属于同一 SSID
 *
 * 这里只比较 STA 模式下的 ssid 字段，假定其以 '\0' 结尾或零填充。
 *
 * @param a 第一个 WiFi 配置
 * @param b 第二个 WiFi 配置
 *
 * @return true  : SSID 完全相同
 * @return false : SSID 不同
 */
static bool wifi_storage_is_same_ssid(const wifi_config_t *a, const wifi_config_t *b)
{
    /* 
     * 直接比较完整 ssid 缓冲区（固定长度），
     * 可以避免因为字符串未以 '\0' 结尾而导致的比较错误。
     */
    return (memcmp(a->sta.ssid, b->sta.ssid, sizeof(a->sta.ssid)) == 0);
}

/**
 * @brief 初始化 WiFi 存储模块
 *
 * 负责加载或设置存储模块的基本配置，并初始化 NVS。
 * 该函数可以多次调用，多次调用只在首次有效。
 *
 * @param config 存储模块配置指针；可为 NULL，NULL 时使用 WIFI_STORAGE_DEFAULT_CONFIG。
 *
 * @return esp_err_t
 *         - ESP_OK                : 初始化成功（或已初始化）
 *         - ESP_ERR_INVALID_STATE : NVS 初始化失败等
 *         - 其它错误码            : 来自 NVS 初始化的错误
 */
esp_err_t xn_wifi_storage_init(const wifi_storage_config_t *config)
{
    /* 若已经初始化过，直接返回成功，避免重复初始化 */
    if (s_storage_inited) {
        return ESP_OK;
    }

    /* 若用户未提供配置，则使用默认配置；否则拷贝用户配置 */
    if (config == NULL) {
        s_storage_cfg = WIFI_STORAGE_DEFAULT_CONFIG();
    } else {
        s_storage_cfg = *config;
    }

    /* 
     * max_wifi_num 不允许为 0，以避免后续申请零长度数组、除零等问题。
     * 如果用户配置为 0，则强制修正为 1。
     */
    if (s_storage_cfg.max_wifi_num == 0) {
        s_storage_cfg.max_wifi_num = 1;
    }

    /* 初始化 NVS，若失败则记录日志并返回错误码 */
    esp_err_t ret = wifi_storage_init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 标记存储模块已初始化 */
    s_storage_inited = true;

    return ESP_OK;
}

/**
 * @brief 读取所有已保存的 WiFi 配置
 *
 * @param configs    调用方提供的数组指针，长度至少为 max_wifi_num。
 * @param count_out  输出参数：实际读取到的 WiFi 数量。
 *
 * @return esp_err_t
 *         - ESP_OK                : 读取成功（包括“当前无配置”的情况）
 *         - ESP_ERR_INVALID_STATE : 存储模块未初始化
 *         - ESP_ERR_INVALID_ARG   : 参数为 NULL
 *         - ESP_FAIL              : NVS 中数据格式异常
 *         - 其它错误码            : 来自 NVS 的错误
 */
esp_err_t xn_wifi_storage_load_all(wifi_config_t *configs, uint8_t *count_out)
{
    /* 若存储模块尚未初始化，返回状态错误 */
    if (!s_storage_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 参数合法性检查：输出指针和存放数组都不能为 NULL */
    if (configs == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先将输出数量置 0，防止调用方误用未初始化的数据 */
    *count_out = 0;

    nvs_handle_t handle;
    /* 以只读方式打开指定命名空间 */
    esp_err_t    ret = nvs_open(s_storage_cfg.nvs_namespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间不存在，视为当前没有保存任何 WiFi 配置 */
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 首先查询 blob 的大小，以便计算存储的配置数量 */
    size_t blob_size = 0;
    ret              = nvs_get_blob(handle, WIFI_LIST_KEY, NULL, &blob_size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 尚未保存过任何列表，正常返回 */
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    /* 
     * blob_size 应当是 wifi_config_t 的整数倍，且大于 0，
     * 否则认为 NVS 中数据损坏或格式不兼容。
     */
    if (blob_size == 0 || (blob_size % sizeof(wifi_config_t)) != 0) {
        ESP_LOGE(TAG, "invalid blob size: %u", (unsigned int)blob_size);
        nvs_close(handle);
        return ESP_FAIL;
    }

    /* 存储的配置数量 */
    uint8_t max_num     = s_storage_cfg.max_wifi_num;
    uint8_t stored_num  = blob_size / sizeof(wifi_config_t);
    /* 实际读取数量为存储数量与最大数量中的较小值 */
    uint8_t read_num    = (stored_num > max_num) ? max_num : stored_num;
    size_t  read_size   = read_num * sizeof(wifi_config_t);

    /* 读取实际 WiFi 配置列表到调用者提供的数组中 */
    ret = nvs_get_blob(handle, WIFI_LIST_KEY, configs, &read_size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(data) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 输出实际读取到的配置个数 */
    *count_out = read_num;

    return ESP_OK;
}

/**
 * @brief 在 WiFi 成功连接后更新存储列表
 *
 * 管理模块在 STA 成功连接并获取 IP 后调用该接口。
 *
 * @param config 本次成功连接所使用的 WiFi 配置（一般由 esp_wifi_get_config 获取）
 *
 * @return esp_err_t
 *         - ESP_OK                : 更新并写入 NVS 成功
 *         - ESP_ERR_INVALID_STATE : 存储模块未初始化
 *         - ESP_ERR_INVALID_ARG   : 传入参数为 NULL
 *         - ESP_ERR_NO_MEM        : 申请临时列表内存失败
 *         - 其它错误码            : NVS 相关操作失败
 */
esp_err_t xn_wifi_storage_on_connected(const wifi_config_t *config)
{
    /* 若存储模块尚未初始化，返回状态错误 */
    if (!s_storage_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 参数合法性检查 */
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 保护性检查，确保 max_wifi_num 至少为 1 */
    uint8_t max_num = s_storage_cfg.max_wifi_num;
    if (max_num == 0) {
        max_num = 1;
    }

    /* 
     * 1. 将当前已保存的 WiFi 配置读入内存列表。
     * 为简单起见，直接申请 max_num 大小的数组作为工作缓冲。
     */
    wifi_config_t *list = (wifi_config_t *)calloc(max_num, sizeof(wifi_config_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = xn_wifi_storage_load_all(list, &count);
    if (ret != ESP_OK) {
        free(list);
        return ret;
    }

    /* 2. 检查该 WiFi 是否已经存在列表中（按 SSID 匹配） */
    int existing_index = -1;
    for (uint8_t i = 0; i < count; ++i) {
        if (wifi_storage_is_same_ssid(&list[i], config)) {
            existing_index = (int)i;
            break;
        }
    }

    if (existing_index >= 0) {
        /* 
         * 已存在于列表中：将其移动到首位。
         */
        if (existing_index > 0) {
            wifi_config_t tmp = list[existing_index];
            memmove(&list[1], &list[0], existing_index * sizeof(wifi_config_t));
            list[0] = tmp;
        }
    } else {
        /*
         * 不在列表中：需要插入到首位。
         */
        if (count < max_num) {
            /* 列表未满，整体后移一位腾出下标 0 */
            if (count > 0) {
                memmove(&list[1], &list[0], count * sizeof(wifi_config_t));
            }
            /* 将本次成功连接的配置放在首位 */
            list[0] = *config;
            count++;
        } else {
            /* 列表已满，丢弃最后一个元素 */
            if (max_num > 1) {
                memmove(&list[1], &list[0], (max_num - 1) * sizeof(wifi_config_t));
            }
            list[0] = *config;
            count   = max_num;
        }
    }

    /* 
     * 3. 将更新后的列表写回 NVS
     *  打开命名空间，写入 blob，并提交事务。
     */
    nvs_handle_t handle;
    ret = nvs_open(s_storage_cfg.nvs_namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write) failed: %s", esp_err_to_name(ret));
        free(list);
        return ret;
    }

    size_t blob_size = count * sizeof(wifi_config_t);
    ret              = nvs_set_blob(handle, WIFI_LIST_KEY, list, blob_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        free(list);
        return ret;
    }

    /* 提交写入操作，使其真正落盘 */
    ret = nvs_commit(handle);
    nvs_close(handle);
    free(list);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t xn_wifi_storage_delete_by_ssid(const char *ssid)
{
    /* 若存储模块尚未初始化，返回状态错误 */
    if (!s_storage_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_num = s_storage_cfg.max_wifi_num;
    if (max_num == 0) {
        max_num = 1;
    }

    wifi_config_t *list = (wifi_config_t *)calloc(max_num, sizeof(wifi_config_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = xn_wifi_storage_load_all(list, &count);
    if (ret != ESP_OK) {
        free(list);
        return ret;
    }

    if (count == 0) {
        free(list);
        return ESP_OK;
    }

    /* 构造一个目标配置，仅设置 SSID，用于复用 wifi_storage_is_same_ssid */
    wifi_config_t target = {0};
    strncpy((char *)target.sta.ssid, ssid, sizeof(target.sta.ssid) - 1);

    uint8_t write_idx = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (wifi_storage_is_same_ssid(&list[i], &target)) {
            /* 跳过要删除的条目 */
            continue;
        }
        if (write_idx != i) {
            list[write_idx] = list[i];
        }
        write_idx++;
    }

    nvs_handle_t handle;
    ret = nvs_open(s_storage_cfg.nvs_namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(delete) failed: %s", esp_err_to_name(ret));
        free(list);
        return ret;
    }

    if (write_idx == 0) {
        /* 已无任何配置，擦除 key */
        ret = nvs_erase_key(handle, WIFI_LIST_KEY);
        if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "nvs_erase_key failed: %s", esp_err_to_name(ret));
            nvs_close(handle);
            free(list);
            return ret;
        }
    } else {
        size_t blob_size = write_idx * sizeof(wifi_config_t);
        ret              = nvs_set_blob(handle, WIFI_LIST_KEY, list, blob_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_blob(delete) failed: %s", esp_err_to_name(ret));
            nvs_close(handle);
            free(list);
            return ret;
        }
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    free(list);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(delete) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
