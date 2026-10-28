#include "device_discovery.h"

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define TAG "Discovery"
#define DISCOVERY_PROBE_CMD "FLOWER_DISCOVER"
#define DISCOVERY_TASK_STACK_SIZE 3072
#define DISCOVERY_INTERVAL_SEC 5

static TaskHandle_t s_discovery_task_handle = NULL;
static volatile bool s_running = false;
static char s_hostname[64] = {0};
static uint16_t s_http_port = 80;

static esp_err_t get_local_ip_str(char *ip_buf, size_t buf_len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_default_netif();
    }
    if (!netif) {
        return ESP_FAIL;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return ESP_FAIL;
    }
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(ip_buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

static void discovery_task(void *pvParameters)
{
    char rx_buffer[128];
    char tx_buffer[128];
    char ip_str[32];

    ESP_LOGI(TAG, "Device discovery task started (hostname=%s, port=%u)", s_hostname, s_http_port);

    while (s_running) {
        // 1. 等待 IP 有效
        while (s_running && get_local_ip_str(ip_str, sizeof(ip_str)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!s_running) break;

        // 2. 创建 UDP Socket
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 允许广播
        int broadcast_enable = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_BROADCAST: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 设置接收超时时间为 5 秒
        struct timeval timeout;
        timeout.tv_sec = DISCOVERY_INTERVAL_SEC;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // 绑定到 INADDR_ANY:3333
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(DISCOVERY_DEFAULT_PORT);

        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        ESP_LOGI(TAG, "Listening on UDP %d and broadcasting every %d s",
                 DISCOVERY_DEFAULT_PORT, DISCOVERY_INTERVAL_SEC);

        // 设置广播目标地址 255.255.255.255:3333
        struct sockaddr_in broadcast_dest;
        memset(&broadcast_dest, 0, sizeof(broadcast_dest));
        broadcast_dest.sin_family = AF_INET;
        broadcast_dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        broadcast_dest.sin_port = htons(DISCOVERY_DEFAULT_PORT);

        // 3. 运行工作循环
        while (s_running) {
            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            // 获取最新 IP
            if (get_local_ip_str(ip_str, sizeof(ip_str)) == ESP_OK) {
                int tx_len = snprintf(tx_buffer, sizeof(tx_buffer), "FLOWER:%s:%s:%u",
                                      s_hostname, ip_str, s_http_port);

                if (len < 0) {
                    // 超时：执行主动定时广播
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        sendto(sock, tx_buffer, tx_len, 0,
                               (struct sockaddr *)&broadcast_dest, sizeof(broadcast_dest));
                    } else {
                        ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
                        break; // 退出并重新建连
                    }
                } else if (len > 0) {
                    rx_buffer[len] = '\0';
                    // 收到搜索请求
                    if (strncmp(rx_buffer, DISCOVERY_PROBE_CMD, strlen(DISCOVERY_PROBE_CMD)) == 0) {
                        ESP_LOGI(TAG, "Received probe from %s:%d, replying",
                                 inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port));
                        // 回复给探测发送者
                        sendto(sock, tx_buffer, tx_len, 0,
                               (struct sockaddr *)&source_addr, sizeof(source_addr));
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    s_discovery_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t device_discovery_start(const char *hostname, uint16_t http_port)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hostname || strlen(hostname) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';
    s_http_port = (http_port > 0) ? http_port : 80;
    s_running = true;

    BaseType_t ret = xTaskCreate(discovery_task, "dev_discovery",
                                 DISCOVERY_TASK_STACK_SIZE, NULL, 4,
                                 &s_discovery_task_handle);
    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create discovery task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t device_discovery_stop(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = false;
    return ESP_OK;
}
