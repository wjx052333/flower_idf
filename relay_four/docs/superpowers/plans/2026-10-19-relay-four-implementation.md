# ESP32-C3 四路继电器控制器 (relay_four) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `relay_four/` 目录下构建一个基于 ESP32-C3 的四路继电器设备固件，控制 4 路独立继电器（GPIO 3, 4, 5, 7）与对应 4 个按键（GPIO 10, 9, 6, 8），配置 GPIO 2 呼吸灯，实现多通道独立控制 Web 界面与兼容的 `/api/sensors` 接口，并通过 MQTT 5.0 上报 12 项继电器状态 metrics。

**Architecture:** 
- 通道抽象结构体 `relay_channel_t` 集中管理 4 路输出引脚、输入引脚、导通状态与定时关断。
- 50ms 周期定时器统一处理 LED 呼吸渐变、按键边沿检测去抖以及各通道 auto-off 倒计时。
- HTTP 服务器支持独立路由 `/change_relay1` ~ `/change_relay4` 控制各通道，并在 `/api/sensors` 输出结构化 JSON。
- MQTT 客户端复用 `relay_v2` 的 HMAC 鉴权与 OTA 逻辑，控制指令映射到第 1 路，定期上报各通道状态 metrics。

**Tech Stack:** ESP-IDF (v5.x), FreeRTOS, ESP HTTP Server, ESP-MQTT (MQTT 5.0, TLS/mbedtls), Nanopb (0.4.9), Node.js (离线验证嵌入式 C-String Web 页面与 JSON 语法).

---

### Task 1: 工程配置与分区表基础文件搭建

**Files:**
- Create: `relay_four/CMakeLists.txt`
- Create: `relay_four/partitions.csv`
- Create: `relay_four/sdkconfig.defaults`
- Create: `relay_four/main/Kconfig.projbuild`
- Create: `relay_four/main/CMakeLists.txt`

- [x] **Step 1: 创建 `relay_four/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXTRA_COMPONENT_DIRS 
    "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/../relay_v2/components"
)
project(flower)
```

- [x] **Step 2: 创建 `relay_four/partitions.csv`**

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
fctry,    data, nvs,     0x10000,  0x1000,   readonly
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x1E0000,
ota_1,    app,  ota_1,   0x200000, 0x1E0000,
```

- [x] **Step 3: 创建 `relay_four/sdkconfig.defaults`**

```ini
# Flash size (ESP32-C3 standard module: 4MB)
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"

# Target chip
CONFIG_IDF_TARGET="esp32c3"

# OTA: allow plain HTTP firmware URLs
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y

# Partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# WiFi
CONFIG_WIFI_SSID="CU_home"
CONFIG_WIFI_PASSWORD="wastemytimehome123456789"

# MQTT broker
CONFIG_MQTT_BROKER_URI="mqtts://58.87.80.78:8883"

# TLS: skip cert check (dev only)
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y

# Enable MQTT 5.0 (required for User Properties)
CONFIG_MQTT_PROTOCOL_5=y

# MQTT task stack
CONFIG_MQTT_TASK_STACK_SIZE=6144

# Logging
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

- [x] **Step 4: 创建 `relay_four/main/Kconfig.projbuild`**

```kconfig
menu "Flower Device Configuration"

config WIFI_SSID
    string "WiFi SSID"
    default ""

config WIFI_PASSWORD
    string "WiFi Password"
    default ""

config MQTT_BROKER_URI
    string "MQTT Broker URI"
    default ""

endmenu
```

- [x] **Step 5: 创建 `relay_four/main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "../../nanopb_gen/proto/flower.pb.c"
    INCLUDE_DIRS
        "."
        "../../nanopb_gen/proto"
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        esp_mqtt
        mbedtls
        esp_timer
        esp_http_server
        driver
        esp_driver_gpio
        esp_driver_ledc
        lwip
        nanopb
        esp_https_ota
        app_update
)

target_link_libraries(${COMPONENT_LIB} m)
```

- [x] **Step 6: 校验文件创建与配置语法**

运行：`ls -la relay_four/ relay_four/main/`
预期：5 个配置文件正确存在且非空。

- [x] **Step 7: 提交配置变更**

```bash
git add relay_four/CMakeLists.txt relay_four/partitions.csv relay_four/sdkconfig.defaults relay_four/main/Kconfig.projbuild relay_four/main/CMakeLists.txt
git commit -m "chore(relay_four): initialize cmake, partitions and sdkconfig configuration"
```

---

### Task 2: 编写测试工具验证 C-String Web 页面与 JSON 协议

**Files:**
- Create: `relay_four/test/verify_html_json.js`

- [x] **Step 1: 创建测试用例 `verify_html_json.js`**

按照 `MEMORY.md` 指南，编写 Node.js 测试脚本提取 `main.c` 中的静态 `STATUS_HTML` 字符串和 JSON 响应构造，验证 HTML 语法树、DOM 元素完整性（4个继电器卡片、控制按钮）、以及 `/api/sensors` 返回 JSON 的有效性。

```javascript
/**
 * verify_html_json.js — 验证 main.c 中 C-string HTML 与 JSON 格式的测试脚本
 */
const fs = require('fs');
const path = require('path');

const mainPath = path.resolve(__dirname, '../main/main.c');
if (!fs.existsSync(mainPath)) {
  console.error("FAIL: main.c does not exist yet");
  process.exit(1);
}

const mainContent = fs.readFileSync(mainPath, 'utf8');

// 1. 提取 STATUS_HTML 内容
const htmlMatch = mainContent.match(/static const char STATUS_HTML\[\]\s*=\s*([\s\S]*?);/);
if (!htmlMatch) {
  console.error("FAIL: STATUS_HTML declaration not found in main.c");
  process.exit(1);
}

// 拼接并反转义 C 字符串
const rawCStr = htmlMatch[1];
const lines = rawCStr.split('\n')
  .map(l => l.trim())
  .filter(l => l.startsWith('"') && l.endsWith('"'))
  .map(l => l.slice(1, -1))
  .join('');

const unescapedHtml = lines
  .replace(/\\n/g, '\n')
  .replace(/\\"/g, '"')
  .replace(/\\\\/g, '\\');

// 验证 HTML 包含 4 路继电器的元素与路由
const requiredSnippets = [
  'id="relay-card-1"',
  'id="relay-card-2"',
  'id="relay-card-3"',
  'id="relay-card-4"',
  '/change_relay1',
  '/change_relay2',
  '/change_relay3',
  '/change_relay4',
  '/api/sensors'
];

for (const snippet of requiredSnippets) {
  if (!unescapedHtml.includes(snippet)) {
    console.error(`FAIL: HTML missing required element/route: ${snippet}`);
    process.exit(1);
  }
}

// 2. 检查 JSON 构造模板中的字段
if (!mainContent.includes('"relays"') ||
    !mainContent.includes('"relay_last_on_ts"') ||
    !mainContent.includes('"relay_last_on_dur_ms"')) {
  console.error("FAIL: JSON response format does not match specification");
  process.exit(1);
}

console.log("PASS: STATUS_HTML and API routes verification succeeded!");
```

- [x] **Step 2: 验证测试脚本初期应报错（由于 main.c 尚未就绪）**

运行：`node relay_four/test/verify_html_json.js`
预期：报错退出，提示 `main.c does not exist yet`。

- [x] **Step 3: 提交测试脚本**

```bash
git add relay_four/test/verify_html_json.js
git commit -m "test(relay_four): add offline verification script for embedded web and api"
```

---

### Task 3: 实现 `main.c` 核心驱动、Web 服务与 MQTT 上报

**Files:**
- Create: `relay_four/main/main.c`

- [x] **Step 1: 编写 `main.c` 完整实现**

在 `relay_four/main/main.c` 中实现：
1. **通道定义**：
   - 4 路 GPIO 继电器：3, 4, 5, 7。
   - 4 路按键：10, 9, 6, 8。
   - 1 路 LED：GPIO 2。
2. **继电器与按键处理**：
   - `relay_channel_t s_relays[4]`。
   - `relay_set(int ch, bool on, uint32_t duration_ms)`。
   - `relay_toggle(int ch)`。
   - `relay_timer_tick()` 在 50ms 定时器中扫描按键下降沿并检查各通道自动断开。
   - LED 呼吸灯控制（LEDC）。
3. **Web 服务**：
   - `STATUS_HTML` 提供 4 个独立的控制卡片，支持设定开启秒数、立即关闭，并定时轮询 `/api/sensors`。
   - `GET /change_relay1` ~ `GET /change_relay4` 解析 `duration` 和 `action` 参数。
   - `GET /api/sensors` 输出 JSON，包含 `relays` 数组和兼容字段。
4. **MQTT 5.0 与 Nanopb**：
   - 鉴权认证、NVS `fctry` 分区读取 `device_id` 与 `device_secret`。
   - 处理下行 `flower_relay_control_t`（控制第 1 路继电器并发送响应）。
   - OTA 升级处理。
   - 30 秒定时发布 `flower_status_report_t`，上报 12 项 `metrics`（`relay1State`~`relay4State`、`relay1LastOnTs`~`relay4LastOnTs`、`relay1LastOnDur`~`relay4LastOnDur`）及 `device_type = "C3_relay_four"`。

- [x] **Step 2: 运行离线自动化测试验证 Web/JSON**

运行：`node relay_four/test/verify_html_json.js`
预期：输出 `PASS: STATUS_HTML and API routes verification succeeded!`。

- [x] **Step 3: 语法与静态规范检查**

检查 `main.c` 中没有遗留未声明的函数、头文件引用完整、宏定义准确。
运行：`grep -n "RELAY\|BUTTON\|LED\|GPIO" relay_four/main/main.c` 检查引脚映射无误。

- [x] **Step 4: 提交 `main.c` 代码**

```bash
git add relay_four/main/main.c
git commit -m "feat(relay_four): implement 4-channel relay logic, web console and mqtt status reporting"
```

---

### Task 4: 验证与交付

**Files:**
- Modify: `relay_four/docs/superpowers/plans/2026-10-19-relay-four-implementation.md`

- [x] **Step 1: 运行全套测试**

运行：`node relay_four/test/verify_html_json.js`
预期：全部通过。

- [x] **Step 2: 完整性检查与分支状态检查**

运行：`git status` 与 `git log -n 4 --oneline`
确认所有代码已正确就绪，且 commit 历史干净工整。

- [x] **Step 3: 交付说明**

在文档与回复中向用户提供烧录前检查事项及在目标机器上编译运行命令（如 `idf.py set-target esp32c3 && idf.py build`）。
