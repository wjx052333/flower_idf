#include "device_discovery.h"

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define TAG "Discovery"
#define DISCOVERY_PROBE_CMD "FLOWER_DISCOVER"
#define DISCOVERY_TASK_STACK_SIZE 3072
#define DISCOVERY_INTERVAL_US (5ULL * 1000ULL * 1000ULL) /* 5 seconds */
#define SOCKET_RECV_TIMEOUT_SEC 1

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
            if (s_running) vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 允许地址复用与广播
        int opt_enable = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable));
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt_enable, sizeof(opt_enable)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_BROADCAST: errno %d", errno);
            close(sock);
            if (s_running) vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 设置接收超时时间为 1 秒（解耦周期广播与接收超时，防止其他设备广播导致超时饿死）
        struct timeval timeout;
        timeout.tv_sec = SOCKET_RECV_TIMEOUT_SEC;
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
            if (s_running) vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        ESP_LOGI(TAG, "Listening on UDP %d and broadcasting every 5 s", DISCOVERY_DEFAULT_PORT);

        // 设置广播目标地址 255.255.255.255:3333
        struct sockaddr_in broadcast_dest;
        memset(&broadcast_dest, 0, sizeof(broadcast_dest));
        broadcast_dest.sin_family = AF_INET;
        broadcast_dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        broadcast_dest.sin_port = htons(DISCOVERY_DEFAULT_PORT);

        int64_t last_broadcast_us = 0; // 设为 0 确保启动后立即执行第一次广播公告

        // 3. 运行工作循环
        while (s_running) {
            int64_t now_us = esp_timer_get_time();

            // 检查 IP 是否仍然有效
            if (get_local_ip_str(ip_str, sizeof(ip_str)) != ESP_OK) {
                ESP_LOGW(TAG, "Local IP lost, closing socket to reconnect");
                break;
            }

            int tx_len = snprintf(tx_buffer, sizeof(tx_buffer), "FLOWER:%s:%s:%u",
                                  s_hostname, ip_str, s_http_port);

            // 检查定时广播周期（启动即刻发送，随后每 5 秒广播一次）
            if (now_us - last_broadcast_us >= DISCOVERY_INTERVAL_US) {
                sendto(sock, tx_buffer, tx_len, 0,
                       (struct sockaddr *)&broadcast_dest, sizeof(broadcast_dest));
                last_broadcast_us = now_us;
            }

            // 接收探测指令或局域网数据
            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                // 超时为正常现象，继续下一个循环
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
                    break; // 套接字严重异常，退出并重新建连
                }
            } else if (len > 0) {
                rx_buffer[len] = '\0';
                // 收到搜索请求
                if (strncmp(rx_buffer, DISCOVERY_PROBE_CMD, strlen(DISCOVERY_PROBE_CMD)) == 0) {
                    ESP_LOGI(TAG, "Received probe from %s:%d, replying",
                             inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port));
                    // 单播回复探测发送者
                    sendto(sock, tx_buffer, tx_len, 0,
                           (struct sockaddr *)&source_addr, sizeof(source_addr));
                }
            }
        }

        close(sock);
        if (s_running) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    s_discovery_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t device_discovery_start(const char *hostname, uint16_t http_port)
{
    if (s_running || s_discovery_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hostname || strlen(hostname) == 0 || strchr(hostname, ':') != NULL) {
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
        s_discovery_task_handle = NULL;
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
