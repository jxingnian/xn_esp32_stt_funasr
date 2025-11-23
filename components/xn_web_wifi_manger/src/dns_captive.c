#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"

#include "dns_captive.h"

#define DNS_PORT      53
#define DNS_BUF_SIZE  512

static const char *TAG = "dns_captive";

static TaskHandle_t s_dns_task   = NULL;
static uint32_t     s_dns_ap_ip  = 0;   /* 网络字节序 IPv4 地址 */

static void dns_captive_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[DNS_BUF_SIZE];

    for (;;) {
        struct sockaddr_in source_addr;
        socklen_t          addr_len = sizeof(source_addr);

        int len = recvfrom(sock,
                           buf,
                           sizeof(buf),
                           0,
                           (struct sockaddr *)&source_addr,
                           &addr_len);
        if (len < 12) {
            continue;
        }

        /* DNS 头部：前 12 字节 */
        uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qdcount == 0) {
            continue;
        }

        /* 跳过第一个问题的 QNAME */
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            uint8_t l = buf[pos];
            if (l == 0) {
                break;
            }
            pos += 1 + l;
        }
        if (pos + 5 >= len) {
            continue;
        }
        pos += 1; /* 跳过终止的 0 字节，pos 现在指向 QTYPE */

        /* 仅处理最常见的 A 记录查询，其它类型也统一返回 A 记录指向 AP IP */

        /* 构造响应：复用请求头 + Question 部分，追加一个 Answer */
        uint8_t resp[DNS_BUF_SIZE];
        if (len > (int)sizeof(resp)) {
            continue;
        }
        memcpy(resp, buf, len);
        int resp_len = len;

        /* 设置响应标志：标准响应，无错误 */
        resp[2] = 0x81; /* QR=1, OPCODE=0, AA=0, TC=0, RD=1 */
        resp[3] = 0x80; /* RA=1, Z=0, RCODE=0 */

        /* 保留原始 QDCOUNT，设置 ANCOUNT=1，NSCOUNT/ARCOUNT=0 */
        resp[6] = 0x00;
        resp[7] = 0x01;
        resp[8] = 0x00;
        resp[9] = 0x00;
        resp[10] = 0x00;
        resp[11] = 0x00;

        /* 追加 Answer：使用压缩名称指针指向第一个问题的 QNAME（偏移 12） */
        if (resp_len + 16 > (int)sizeof(resp)) {
            continue;
        }

        int ans = resp_len;

        /* NAME = 指针 0xC00C */
        resp[ans++] = 0xC0;
        resp[ans++] = 0x0C;

        /* TYPE = A (1) */
        resp[ans++] = 0x00;
        resp[ans++] = 0x01;

        /* CLASS = IN (1) */
        resp[ans++] = 0x00;
        resp[ans++] = 0x01;

        /* TTL = 60 秒 */
        resp[ans++] = 0x00;
        resp[ans++] = 0x00;
        resp[ans++] = 0x00;
        resp[ans++] = 0x3C;

        /* RDLENGTH = 4 字节（IPv4） */
        resp[ans++] = 0x00;
        resp[ans++] = 0x04;

        /* RDATA = AP IPv4 地址（网络字节序） */
        memcpy(&resp[ans], &s_dns_ap_ip, sizeof(s_dns_ap_ip));
        ans += (int)sizeof(s_dns_ap_ip);

        resp_len = ans;

        (void)sendto(sock,
                     resp,
                     resp_len,
                     0,
                     (struct sockaddr *)&source_addr,
                     addr_len);
    }

    /* 实际上不会到达这里 */
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_captive_start(const char *ap_ip)
{
    if (s_dns_task != NULL) {
        return ESP_OK;
    }

    if (ap_ip == NULL || ap_ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t ip = inet_addr(ap_ip);
    if (ip == IPADDR_NONE) {
        ESP_LOGE(TAG, "invalid ap_ip: %s", ap_ip);
        return ESP_ERR_INVALID_ARG;
    }

    s_dns_ap_ip = ip;

    BaseType_t ret = xTaskCreate(
        dns_captive_task,
        "dns_captive",
        3072,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_dns_task);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create task failed");
        s_dns_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started on %s", ap_ip);
    return ESP_OK;
}

void dns_captive_stop(void)
{
    if (s_dns_task != NULL) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
        ESP_LOGI(TAG, "stopped");
    }
}
