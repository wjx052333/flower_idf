# ESP32 局域网通用设备发现 (device_discovery) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 创建通用的 ESP-IDF 公共组件 `components/device_discovery`，使用 UDP 广播（3333 端口）实现局域网设备自发现与主动问答，并在 `relay`、`relay_v2`、`relay_four` 三个工程中无缝集成。

**Architecture:** 
- 通用组件 `device_discovery` 封装基于 FreeRTOS 任务和 lwIP BSD Socket 的 UDP 广播客户端与监听服务。
- 采用双向发现模式：每 5 秒向受限广播地址 `255.255.255.255:3333` 发送 `FLOWER:<hostname>:<ip>:<port>` 报文；同时阻塞监听接收，当收到上位机 `FLOWER_DISCOVER` 探测指令时，立刻回包应答。
- 三个工程共享 `components/device_discovery`，在各自 WiFi 连上并获取 IP 后调用 `device_discovery_start(hostname, 80)`。

**Tech Stack:** ESP-IDF v5.x / FreeRTOS, lwIP Socket (BSD sockets API), Node.js (离线报文与网络发现交互验证).

---

### Task 1: 编写设备发现协议的离线自动化测试工具

**Files:**
- Create: `test/test_device_discovery.js`

- [ ] **Step 1: 创建测试用例 `test/test_device_discovery.js`**

按照 `MEMORY.md` 规范，由于环境无宿主 C 编译器，我们编写 Node.js 测试脚本。该脚本既包含单元断言（验证报文格式解析、Magic Header 校验、字段提取），又具备真实 UDP 监听/探测能力，供上位机快速测试验证。

```javascript
/**
 * test_device_discovery.js
 * 设备发现报文格式校验与 UDP 监听/探测上位机离线测试工具
 */
const dgram = require('dgram');

// 1. 协议单元测试函数
function parseDiscoveryPacket(msg) {
    if (typeof msg !== 'string') return null;
    const parts = msg.trim().split(':');
    if (parts.length !== 4) return null;
    const [magic, hostname, ip, portStr] = parts;
    if (magic !== 'FLOWER') return null;
    const port = parseInt(portStr, 10);
    if (isNaN(port) || port <= 0 || port > 65535) return null;
    // 简易 IPv4 格式检查
    const ipRegex = /^(\d{1,3}\.){3}\d{1,3}$/;
    if (!ipRegex.test(ip)) return null;

    return { magic, hostname, ip, port };
}

// 2. 运行协议格式自测
function runUnitTests() {
    console.log("Running discovery protocol unit tests...");

    // 正确用例
    const validPacket = "FLOWER:rel_flr006:192.168.1.120:80";
    const res1 = parseDiscoveryPacket(validPacket);
    if (!res1 || res1.hostname !== 'rel_flr006' || res1.ip !== '192.168.1.120' || res1.port !== 80) {
        throw new Error("Failed valid packet test");
    }

    // 错误用例：非法 Header
    if (parseDiscoveryPacket("OTHER:rel_flr006:192.168.1.120:80") !== null) {
        throw new Error("Failed invalid header test");
    }

    // 错误用例：缺少字段
    if (parseDiscoveryPacket("FLOWER:rel_flr006:192.168.1.120") !== null) {
        throw new Error("Failed missing field test");
    }

    // 错误用例：非法端口
    if (parseDiscoveryPacket("FLOWER:rel_flr006:192.168.1.120:99999") !== null) {
        throw new Error("Failed invalid port test");
    }

    console.log("PASS: Discovery protocol unit tests passed successfully!");
}

runUnitTests();

// 3. 命令行参数驱动：若传入 --listen，则作为真实 UDP 监听器运行 3 秒测试
if (process.argv.includes('--listen')) {
    const PORT = 3333;
    const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });

    socket.on('message', (msg, rinfo) => {
        const text = msg.toString();
        console.log(`[UDP Recv] From ${rinfo.address}:${rinfo.port} -> ${text}`);
        const parsed = parseDiscoveryPacket(text);
        if (parsed) {
            console.log(`[Device Found] Hostname: ${parsed.hostname}, IP: ${parsed.ip}, Port: ${parsed.port}`);
        }
    });

    socket.bind(PORT, () => {
        socket.setBroadcast(true);
        console.log(`[Listener] Listening on 0.0.0.0:${PORT}... Sending FLOWER_DISCOVER probe...`);
        const probe = Buffer.from('FLOWER_DISCOVER');
        socket.send(probe, 0, probe.length, PORT, '255.255.255.255');
    });

    setTimeout(() => {
        socket.close();
        console.log("[Listener] Test run finished.");
        process.exit(0);
    }, 2000);
}
```

- [ ] **Step 2: 运行单元测试验证**

运行：`node test/test_device_discovery.js`  
预期输出：`PASS: Discovery protocol unit tests passed successfully!`

- [ ] **Step 3: 提交测试工具**

```bash
git add test/test_device_discovery.js
git commit -m "test: add offline verification and listener test script for device discovery"
```

---

### Task 2: 创建通用组件 `components/device_discovery`

**Files:**
- Create: `components/device_discovery/CMakeLists.txt`
- Create: `components/device_discovery/include/device_discovery.h`
- Create: `components/device_discovery/device_discovery.c`

- [ ] **Step 1: 创建 `components/device_discovery/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "device_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif
)
```

- [ ] **Step 2: 创建 `components/device_discovery/include/device_discovery.h`**

```c
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
```

- [ ] **Step 3: 创建 `components/device_discovery/device_discovery.c`**

实现完整逻辑：
1. 校验输入参数并缓存 `hostname` 和 `http_port`；
2. 创建 `discovery_task`，堆栈深度为 3072 字节，优先级为 4；
3. 循环等待 WiFi STA 接口获取有效非 0 IP 地址；
4. 创建 UDP socket，开启 `SO_BROADCAST`，配置 `SO_RCVTIMEO` 为 5 秒接收超时；
5. 在主循环中：
   - 超时（返回负数且 errno 为 EAGAIN/EWOULDBLOCK）或成功接收后，触发组包 `"FLOWER:<hostname>:<ip>:<port>"` 并发送广播至 `255.255.255.255:3333`；
   - 若接收到数据且匹配 `"FLOWER_DISCOVER"`，立即向来源套接字地址单播回包；
   - 若遇到套接字致命错误，安全调用 `close()`，休眠 3 秒后重连重建套接字。

```c
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
```

- [ ] **Step 4: 提交组件代码**

```bash
git add components/device_discovery/
git commit -m "feat(components): implement universal udp device discovery component"
```

---

### Task 3: 在 `relay_four` 中集成设备发现组件

**Files:**
- Modify: `relay_four/CMakeLists.txt`
- Modify: `relay_four/main/CMakeLists.txt`
- Modify: `relay_four/main/main.c`

- [ ] **Step 1: 修改 `relay_four/CMakeLists.txt` 添加公共组件搜索目录**

确保 `EXTRA_COMPONENT_DIRS` 包含 `${CMAKE_CURRENT_SOURCE_DIR}/../components`：

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components"
    "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/../relay_v2/components"
)
project(flower)
```

- [ ] **Step 2: 修改 `relay_four/main/CMakeLists.txt`**

在 `REQUIRES` 列表中追加 `device_discovery`。

- [ ] **Step 3: 修改 `relay_four/main/main.c`**

1. 引入头文件 `#include "device_discovery.h"`；
2. 在 `app_main()` 中，`wifi_init_sta()` 与 `http_server_start()` 之后调用：
```c
/* UDP Device Discovery */
device_discovery_start(s_host_name, 80);
```

- [ ] **Step 4: 运行回归验证**

运行：`node relay_four/test/verify_html_json.js`
预期：验证通过。

- [ ] **Step 5: 提交 `relay_four` 集成变更**

```bash
git add relay_four/CMakeLists.txt relay_four/main/CMakeLists.txt relay_four/main/main.c
git commit -m "feat(relay_four): integrate udp device discovery service"
```

---

### Task 4: 在 `relay_v2` 中集成设备发现组件

**Files:**
- Modify: `relay_v2/CMakeLists.txt`
- Modify: `relay_v2/main/CMakeLists.txt`
- Modify: `relay_v2/main/main.c`

- [ ] **Step 1: 修改 `relay_v2/CMakeLists.txt`**

将 `${CMAKE_CURRENT_SOURCE_DIR}/../components` 加入 `EXTRA_COMPONENT_DIRS`：

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components"
    "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components"
)
project(flower)
```

- [ ] **Step 2: 修改 `relay_v2/main/CMakeLists.txt`**

在 `REQUIRES` 列表中追加 `device_discovery`。

- [ ] **Step 3: 修改 `relay_v2/main/main.c`**

1. 引入头文件 `#include "device_discovery.h"`；
2. 在 `app_main()` 中，`wifi_init_sta()` 与 `http_server_start()` 之后调用：
```c
/* UDP Device Discovery */
device_discovery_start(g_device_id, 80);
```

- [ ] **Step 4: 提交 `relay_v2` 集成变更**

```bash
git add relay_v2/CMakeLists.txt relay_v2/main/CMakeLists.txt relay_v2/main/main.c
git commit -m "feat(relay_v2): integrate udp device discovery service"
```

---

### Task 5: 在 `relay` (V1) 中集成设备发现组件

**Files:**
- Modify: `relay/CMakeLists.txt`
- Modify: `relay/main/CMakeLists.txt`
- Modify: `relay/main/main.c`

- [ ] **Step 1: 修改 `relay/CMakeLists.txt`**

增加 `EXTRA_COMPONENT_DIRS` 引入 `${CMAKE_CURRENT_SOURCE_DIR}/../components`：

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../components")
project(flower)
```

- [ ] **Step 2: 修改 `relay/main/CMakeLists.txt`**

在 `REQUIRES` 列表中追加 `device_discovery`。

- [ ] **Step 3: 修改 `relay/main/main.c`**

1. 引入头文件 `#include "device_discovery.h"`；
2. 在 `app_main()` 中，`wifi_init_sta()` 与 `http_server_start()` 之后调用：
```c
/* UDP Device Discovery */
device_discovery_start(g_device_id, 80);
```

- [ ] **Step 4: 提交 `relay` 集成变更**

```bash
git add relay/CMakeLists.txt relay/main/CMakeLists.txt relay/main/main.c
git commit -m "feat(relay): integrate udp device discovery service"
```

---

### Task 6: 整体联调校验与交付

**Files:**
- Test: `test/test_device_discovery.js`
- Test: `relay_four/test/verify_html_json.js`

- [ ] **Step 1: 运行全套离线测试**

运行：`node test/test_device_discovery.js`  
运行：`node relay_four/test/verify_html_json.js`  
预期：全部 PASS 输出。

- [ ] **Step 2: 检查 Git 分支与改动完整性**

运行：`git status` 与 `git log -n 6 --oneline`  
预期：提交历史清晰，三个项目全部规范集成。
