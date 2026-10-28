#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISCOVERY_DEFAULT_PORT 3333

/**
 * @brief 启动局域网 UDP 设备发现服务（创建独立任务，主动定时广播并监听搜索请求）
 *
 * @param hostname   设备的名称/主机名（如 "rel_flr006" 或 "flr001"）
 * @param http_port  设备的 Web 访问端口（通常为 80）
 * @return esp_err_t ESP_OK 成功启动；ESP_ERR_INVALID_STATE 已经启动；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t device_discovery_start(const char *hostname, uint16_t http_port);

/**
 * @brief 停止设备发现服务并释放资源
 *
 * @return esp_err_t ESP_OK 成功停止；ESP_ERR_INVALID_STATE 未启动
 */
esp_err_t device_discovery_stop(void);

#ifdef __cplusplus
}
#endif
