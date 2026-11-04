# AHT25 传感器集成与温湿度上报实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `ESPWROM32-4M-8xMOS` 固件中引入 AHT25 温湿度传感器驱动，复用 BH1750 I2C 总线，在 `s_metrics` 中扩充上报 `aht_temp` 与 `aht_humi`，更新版本号为 1.2.0，设备类型为 `HT_reed_AHT25_8Xmos`，并同步更新 Web/API 显示。

**Architecture:** 
复用 `ESPWROM32-4M-8xMOS-vivarium` 的 `aht25.c/.h` 驱动至目标工程，清理遗留的 `USE_BH1750` / Modbus 串口代码；在 I2C 初始流程挂载 AHT25（0x38）；所有传感器读取通过 `g_sensor_mutex` 互斥保护；将 `METRICS_COUNT` 从 6 扩展至 8，在 MQTT 上报、`/api/sensors` JSON 及 Web 仪表盘中展示。

**Tech Stack:** ESP-IDF v5.x / v6.x (C), FreeRTOS, nanopb (Protobuf), Node.js (离线测试环境)

---

### File Structure Map

- **Create:**
  - `ESPWROM32-4M-8xMOS/main/aht25.h` — AHT25 I2C 驱动头文件（从 vivarium 拷贝）
  - `ESPWROM32-4M-8xMOS/main/aht25.c` — AHT25 I2C 驱动源文件（从 vivarium 拷贝）
- **Modify:**
  - `ESPWROM32-4M-8xMOS/main/CMakeLists.txt` — 注册 `aht25.c`
  - `ESPWROM32-4M-8xMOS/main/main.c` — 移除 Modbus/USE_BH1750 宏，初始化 AHT25，更新版本/设备标识，扩展 metrics 与 API/Web
  - `ESPWROM32-4M-8xMOS/test/test_html.js` — 测试脚本更新新增 DOM、JSON 键及特征匹配
- **Docs:**
  - `docs/superpowers/specs/2026-11-04-aht25-sensor-integration-design.md` (已就绪)

---

### Task 1: 引入 AHT25 驱动文件并更新 CMakeLists.txt

**Files:**
- Create: `ESPWROM32-4M-8xMOS/main/aht25.h`
- Create: `ESPWROM32-4M-8xMOS/main/aht25.c`
- Modify: `ESPWROM32-4M-8xMOS/main/CMakeLists.txt:1-10`

- [ ] **Step 1: 复制 AHT25 驱动文件**

从 `ESPWROM32-4M-8xMOS-vivarium/main/` 复制 `aht25.h` 和 `aht25.c` 到 `ESPWROM32-4M-8xMOS/main/`：
```bash
cp ESPWROM32-4M-8xMOS-vivarium/main/aht25.h ESPWROM32-4M-8xMOS/main/aht25.h
cp ESPWROM32-4M-8xMOS-vivarium/main/aht25.c ESPWROM32-4M-8xMOS/main/aht25.c
```

- [ ] **Step 2: 验证新驱动文件已存在**

运行：
```bash
ls -l ESPWROM32-4M-8xMOS/main/aht25.*
```
预期输出：`aht25.c` 和 `aht25.h` 均存在且非空。

- [ ] **Step 3: 更新 CMakeLists.txt 注册 aht25.c**

在 `ESPWROM32-4M-8xMOS/main/CMakeLists.txt` 中将 `"aht25.c"` 加入 `SRCS`：
```cmake
idf_component_register(
    SRCS
        "main.c"
        "aht25.c"
        "ds18b20.c"
        "water_logic.c"
        "water_store.c"
        "../../nanopb_gen/proto/flower.pb.c"
    INCLUDE_DIRS
        "."
        "../../nanopb_gen/proto"
    REQUIRES
...
```

- [ ] **Step 4: 提交驱动文件与 CMakeLists**

```bash
git add ESPWROM32-4M-8xMOS/main/aht25.h ESPWROM32-4M-8xMOS/main/aht25.c ESPWROM32-4M-8xMOS/main/CMakeLists.txt
git commit -m "feat(sensor): import aht25 driver and register in cmake

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: 清理旧 Modbus/USE_BH1750 宏并在 I2C 总线初始化 AHT25

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/main/main.c`

- [ ] **Step 1: 包含 aht25.h 并移除 Modbus 宏与实现代码**

在 `ESPWROM32-4M-8xMOS/main/main.c`：
1. 包含 `#include "aht25.h"`
2. 移除：
```c
/* ── Lux sensor select: define USE_BH1750 for BH1750FVI I2C,
 *                       undefine for Modbus-RTU sensor ─────────────── */
#define USE_BH1750

#ifndef USE_BH1750
#define MODBUS_UART_PORT  UART_NUM_1
#define MODBUS_UART_TXD   GPIO_NUM_6
#define MODBUS_UART_RXD   GPIO_NUM_7
#define MODBUS_BAUD       9600
#endif /* !USE_BH1750 */
```
3. 移除 `#ifndef USE_BH1750` 中的 `modbus_crc16`、`modbus_init`、`modbus_reset`、`modbus_read_calc_enable`、`modbus_read_calc`、`modbus_read_illuminance` 等全部 Modbus 相关代码块。
4. 保留 BH1750 I2C 定义与实现，去掉围绕它的 `#else /* USE_BH1750 */` 和 `#endif /* USE_BH1750 */`。

- [ ] **Step 2: 在 I2C 初始化函数中挂载 AHT25**

修改 `bh1750_init(void)`（或者重命名为 `i2c_sensors_init(void)`），在创建 `s_i2c_bus` 并添加 BH1750 设备后，调用 `aht25_init(s_i2c_bus)`：
```c
    uint8_t cmd = BH1750_CONT_H;
    ESP_ERROR_CHECK(i2c_master_transmit(s_bh1750_dev, &cmd, 1, 100));
    vTaskDelay(pdMS_TO_TICKS(200)); /* wait for first measurement */

    /* Initialize AHT25 on shared I2C bus */
    esp_err_t aht_err = aht25_init(s_i2c_bus);
    if (aht_err != ESP_OK) {
        ESP_LOGW(TAG, "AHT25 init failed: %s", esp_err_to_name(aht_err));
    }
```
并在 `app_main` 中调用该初始化。

- [ ] **Step 3: 检查 main.c 中剩余的 USE_BH1750 引用并清理**

搜索 `USE_BH1750`，将采样处：
```c
#ifdef USE_BH1750
    float lux = bh1750_read_illuminance();
#else
    float lux = modbus_read_illuminance();
#endif
```
直接改为：
```c
    float lux = bh1750_read_illuminance();
```
共有两处（`publish_status_report` 和 `handle_api_sensors`），全部直接调用 `bh1750_read_illuminance()`。

- [ ] **Step 4: 提交 I2C 传感器初始化与冗余清理**

```bash
git add ESPWROM32-4M-8xMOS/main/main.c
git commit -m "refactor(i2c): remove modbus fallback and initialize aht25 on shared i2c bus

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: 更新版本号、设备类型及 s_metrics 上报扩容

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/main/main.c`

- [ ] **Step 1: 修改版本号宏定义**

在 `ESPWROM32-4M-8xMOS/main/main.c` 中：
```c
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 2
#define FW_VERSION_PATCH 0
```

- [ ] **Step 2: 修改 METRICS_COUNT 宏定义为 8**

```c
#define METRICS_COUNT 8
```

- [ ] **Step 3: 在 publish_status_report 中读取 AHT25 并填充 s_metrics**

在 `publish_status_report(void)` 中：
```c
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(SENSOR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Status report: sensor busy, skipping this tick");
        return;
    }
    float humidity = get_humidity();

    float temp = ds18b20_read_temperature();
    if (temp > -273.0f)
        ESP_LOGI(TAG, "Temperature(GPIO1)=%.2f°C", temp);

    float lux = bh1750_read_illuminance();
    if (lux >= 0)
        ESP_LOGI(TAG, "Illuminance=%.3f Lux", lux);

    float aht_temp = 0.0f, aht_humi = 0.0f;
    esp_err_t aht_err = aht25_read(&aht_temp, &aht_humi);
    if (aht_err == ESP_OK)
        ESP_LOGI(TAG, "AHT25 temp=%.2f°C, humi=%.1f%%", aht_temp, aht_humi);
    else
        ESP_LOGW(TAG, "AHT25 read failed: %s", esp_err_to_name(aht_err));

    xSemaphoreGive(g_sensor_mutex);
```
填充指标：
```c
    strcpy(s_metrics[6].key, "aht_temp");
    s_metrics[6].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[6].value.double_value = (aht_err == ESP_OK) ? (double)aht_temp : 0.0;

    strcpy(s_metrics[7].key, "aht_humi");
    s_metrics[7].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[7].value.double_value = (aht_err == ESP_OK) ? (double)aht_humi : 0.0;
```
设备类型更新：
```c
    strcpy(sr.device_type, "HT_reed_AHT25_8Xmos");
```
更新日志打印：
```c
    ESP_LOGI(TAG, "Status published (humidity=%.1f%%, temp=%.2f°C, lux=%.1f, reed=%d, aht_temp=%.2f°C, aht_humi=%.1f%%, mos=0x%02lx, prev_mos=0x%02lx)",
             humidity, (double)(temp > -273.0f ? temp : 0), (double)(lux >= 0 ? lux : 0),
             reed_closed, (double)(aht_err == ESP_OK ? aht_temp : 0), (double)(aht_err == ESP_OK ? aht_humi : 0),
             (unsigned long)mos_current, (unsigned long)mos_prev);
```

- [ ] **Step 4: 提交版本号、设备类型与 metrics 变更**

```bash
git add ESPWROM32-4M-8xMOS/main/main.c
git commit -m "feat(metrics): expand s_metrics with aht_temp/aht_humi, bump fw to 1.2.0 and set device_type

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: 更新 `/api/sensors` 响应与 Web 管理页面卡片

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/main/main.c`

- [ ] **Step 1: 在 handle_api_sensors 中读取 AHT25 并追加 JSON 字段**

在 `handle_api_sensors(httpd_req_t *req)` 中：
```c
    float aht_temp = 0.0f, aht_humi = 0.0f;
    esp_err_t aht_err = aht25_read(&aht_temp, &aht_humi);

    xSemaphoreGive(g_sensor_mutex);
```
将 json 格式化字符串扩大（增大 buffer 大小至 1024）：
```c
    char json[1024];
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json),
        "{"
        "\"version\":\"" FW_VERSION_STR "\","
        "\"humidity\":%.1f,\"raw_low\":%d,\"temperature\":%.2f,\"lux\":%.1f,\"reed\":%d,"
        "\"aht_temp\":%.2f,\"aht_humi\":%.1f,"
        "\"relay_last_on_ts\":%lld,\"relay_last_on_dur_ms\":%lu,\"strategy\":%u,"
        "\"relays\":["
...
        humidity,
        raw_low,
        (double)(temp > -273.0f ? temp : -273.0f),
        (double)(lux >= 0.0f ? lux : -1.0f),
        reed_closed,
        (double)(aht_err == ESP_OK ? aht_temp : -99.0f),
        (double)(aht_err == ESP_OK ? aht_humi : -99.0f),
        (long long)s_relays[0].last_on_utc_s,
...
```

- [ ] **Step 2: 在 STATUS_HTML 中添加 AHT25 卡片与 JS 渲染**

在 `STATUS_HTML` 的 `.vals` 容器中追加：
```html
    "  <div class=\"val\"><div class=\"num\" id=\"v-aht-temp\">--</div><div class=\"lbl\">AHT25温度 (°C)</div></div>\n"
    "  <div class=\"val\"><div class=\"num\" id=\"v-aht-humi\">--</div><div class=\"lbl\">AHT25湿度 (%)</div></div>\n"
```
在 JS 轮询解析 `d` 时加入：
```javascript
    "    if(d.aht_temp!==undefined){\n"
    "      document.getElementById('v-aht-temp').textContent=d.aht_temp>-40?d.aht_temp.toFixed(1):'ERR';\n"
    "    }\n"
    "    if(d.aht_humi!==undefined){\n"
    "      document.getElementById('v-aht-humi').textContent=d.aht_humi>=0?d.aht_humi.toFixed(1):'ERR';\n"
    "    }\n"
```

- [ ] **Step 3: 提交 Web 界面与 API 增强**

```bash
git add ESPWROM32-4M-8xMOS/main/main.c
git commit -m "feat(web): display aht25 temperature and humidity in web dashboard and api

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: 更新并运行测试套件验证离线代码质量

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/test/test_html.js`

- [ ] **Step 1: 更新 test_html.js 包含新增字段和模式**

在 `ESPWROM32-4M-8xMOS/test/test_html.js` 中：
1. `requiredJsonKeys` 增加 `"aht_temp"` 和 `"aht_humi"`。
2. `requiredCodePatterns`：
   - 移除 `"HT_reed_8Xmos"`
   - 增加 `"HT_reed_AHT25_8Xmos"`
   - 增加 `"aht25_init"`
   - 增加 `"aht25_read"`
   - 增加 `"aht_temp"`
   - 增加 `"aht_humi"`
   - 确保包含 `"v-aht-temp"` 和 `"v-aht-humi"` 的 DOM 元素检查

- [ ] **Step 2: 运行测试**

运行：
```bash
node ESPWROM32-4M-8xMOS/test/test_html.js
```
预期输出：
```
PASS: All HTML tags balanced.
PASS: Script #1 syntax is valid!
PASS: Script #2 syntax is valid!
PASS: All required JSON keys present in main.c.
PASS: All required code and version patterns present in main.c.
ALL CHECKS PASSED!
```

- [ ] **Step 3: 提交测试脚本**

```bash
git add ESPWROM32-4M-8xMOS/test/test_html.js
git commit -m "test(html): verify aht25 dom cards, json keys and code patterns

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```
