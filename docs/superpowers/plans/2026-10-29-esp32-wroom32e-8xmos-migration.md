# ESP32-WROOM-32E + 8xMOS 代码迁移实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `relay_v2` 的全部工程代码（包括 MQTT、Web控制、OTA、设备自动发现、自动浇水策略及工厂烧写工具）迁移适配到新硬件 `ESPWROM32-4M-8xMOS`，并将继电器输出无缝切换为 MOS1 控制，保持分区表、OTA 和 factory 烧写机制完全一致。

**Architecture:** 
- 目标平台从 ESP32-C3 切换为经典 ESP32（双核 Xtensa LX6，4MB SPI Flash，`CONFIG_IDF_TARGET="esp32"`）。
- 硬件抽象层更新引脚映射：MOS1 (GPIO19) 替换原继电器；GPIO0 为按键；GPIO23 为呼吸灯 LED；GPIO34 (ADC1_CH6) 为土壤湿度模拟采样；GPIO4 为 DS18B20 水温单总线；GPIO21/22 为 BH1750 I2C 照度传感器。
- 启动保护：开机原子性初始化 8 路 MOS 管（GPIO 19, 18, 17, 16, 33, 25, 26, 27）为低电平（OFF）并使能下拉。
- 复用现有 `components/device_discovery`、`relay/components/nanopb` 以及 `relay_v2/components/esp_mqtt`。

**Tech Stack:** ESP-IDF v5/v6, C (C99/GNU99), Nanopb (Protobuf), FreeRTOS, ESP-MQTT (MQTT 5.0), mbedTLS, Python 3 (NVS 分区生成).

---

### Task 1: 创建工程骨架与构建配置

**Files:**
- Create: `ESPWROM32-4M-8xMOS/CMakeLists.txt`
- Create: `ESPWROM32-4M-8xMOS/partitions.csv`
- Create: `ESPWROM32-4M-8xMOS/sdkconfig.defaults`
- Create: `ESPWROM32-4M-8xMOS/.gitignore`
- Create: `ESPWROM32-4M-8xMOS/dependencies.lock`
- Create: `ESPWROM32-4M-8xMOS/ota.sh`
- Create: `ESPWROM32-4M-8xMOS/build.md`

- [ ] **Step 1: 创建工程顶层 CMakeLists.txt**

在 `ESPWROM32-4M-8xMOS/CMakeLists.txt` 中写入：
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

- [ ] **Step 2: 保持与 relay_v2 严格一致的 partitions.csv**

在 `ESPWROM32-4M-8xMOS/partitions.csv` 中写入：
```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
fctry,    data, nvs,     0x10000,  0x1000,   readonly
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x1E0000,
ota_1,    app,  ota_1,   0x200000, 0x1E0000,
```

- [ ] **Step 3: 创建适配 ESP32 的 sdkconfig.defaults**

在 `ESPWROM32-4M-8xMOS/sdkconfig.defaults` 中写入（设定 `CONFIG_IDF_TARGET="esp32"`）：
```ini
# Target: ESP32 (Xtensa Dual Core)
CONFIG_IDF_TARGET="esp32"

# Flash size: 4MB
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"

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

- [ ] **Step 4: 创建 .gitignore, dependencies.lock, ota.sh 和 build.md**

创建 `ESPWROM32-4M-8xMOS/.gitignore`：
```gitignore
build/
sdkconfig
sdkconfig.old
*.bin
factory/*.bin
```

复制 `relay_v2/dependencies.lock` 到 `ESPWROM32-4M-8xMOS/dependencies.lock`。

创建 `ESPWROM32-4M-8xMOS/ota.sh`：
```bash
python ../web/ota_cli.py flr004 ./build/flower.bin --backend https://58.87.80.78:8080 --insecure
```

创建 `ESPWROM32-4M-8xMOS/build.md`（说明 `idf.py set-target esp32` 及烧录流程）。

- [ ] **Step 5: 检查并提交工程配置文件**

```bash
git status
```

---

### Task 2: 迁移 factory 出厂设备烧写目录与工具

**Files:**
- Create: `ESPWROM32-4M-8xMOS/factory/factory_nvs_template.csv`
- Create: `ESPWROM32-4M-8xMOS/factory/gen_factory_nvs.py`
- Create: `ESPWROM32-4M-8xMOS/factory/README.md`

- [ ] **Step 1: 复制并验证 factory_nvs_template.csv**

将 `relay_v2/factory/factory_nvs_template.csv` 复制至 `ESPWROM32-4M-8xMOS/factory/factory_nvs_template.csv`。

- [ ] **Step 2: 复制并验证 gen_factory_nvs.py**

将 `relay_v2/factory/gen_factory_nvs.py` 复制至 `ESPWROM32-4M-8xMOS/factory/gen_factory_nvs.py`。

- [ ] **Step 3: 复制并更新 factory/README.md 指南**

将 `relay_v2/factory/README.md` 复制至 `ESPWROM32-4M-8xMOS/factory/README.md`，确认 offset `0x10000` 与 size `0x1000` 保持一致。

---

### Task 3: 迁移主程序依赖组件与业务逻辑代码

**Files:**
- Create: `ESPWROM32-4M-8xMOS/main/CMakeLists.txt`
- Create: `ESPWROM32-4M-8xMOS/main/Kconfig.projbuild`
- Create: `ESPWROM32-4M-8xMOS/main/ds18b20.h`
- Create: `ESPWROM32-4M-8xMOS/main/ds18b20.c`
- Create: `ESPWROM32-4M-8xMOS/main/water_logic.h`
- Create: `ESPWROM32-4M-8xMOS/main/water_logic.c`
- Create: `ESPWROM32-4M-8xMOS/main/water_store.h`
- Create: `ESPWROM32-4M-8xMOS/main/water_store.c`
- Create: `ESPWROM32-4M-8xMOS/test/run_tests.sh`
- Create: `ESPWROM32-4M-8xMOS/test/test_water.c`

- [ ] **Step 1: 创建 main/CMakeLists.txt 和 main/Kconfig.projbuild**

创建 `ESPWROM32-4M-8xMOS/main/CMakeLists.txt`：
```cmake
idf_component_register(
    SRCS
        "main.c"
        "ds18b20.c"
        "water_logic.c"
        "water_store.c"
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
        esp_adc
        esp_driver_uart
        esp_driver_i2c
        lwip
        nanopb
        esp_https_ota
        app_update
        device_discovery
)

target_link_libraries(${COMPONENT_LIB} m)
```

复制 `relay_v2/main/Kconfig.projbuild` 至 `ESPWROM32-4M-8xMOS/main/Kconfig.projbuild`。

- [ ] **Step 2: 迁移 DS18B20 驱动代码**

复制 `relay_v2/main/ds18b20.h` 和 `ds18b20.c` 至 `ESPWROM32-4M-8xMOS/main/`。

- [ ] **Step 3: 迁移自动浇水策略和存储逻辑**

复制 `relay_v2/main/water_logic.h`、`water_logic.c`、`water_store.h`、`water_store.c` 至 `ESPWROM32-4M-8xMOS/main/`。

- [ ] **Step 4: 迁移单元测试脚本与用例**

复制 `relay_v2/test/run_tests.sh` 和 `test_water.c` 至 `ESPWROM32-4M-8xMOS/test/`。

---

### Task 4: 适配 main.c 硬件引脚映射与 8 路 MOS 初始化

**Files:**
- Create: `ESPWROM32-4M-8xMOS/main/main.c`

- [ ] **Step 1: 修改硬件宏定义映射**

在 `ESPWROM32-4M-8xMOS/main/main.c` 中：
1. 8路 MOS管引脚定义与继电器映射：
   ```c
   /* ── 8-Channel MOS Pins ────────────────────────────────────────────────── */
   #define MOS1_PIN  19
   #define MOS2_PIN  18
   #define MOS3_PIN  17
   #define MOS4_PIN  16
   #define MOS5_PIN  33
   #define MOS6_PIN  25
   #define MOS7_PIN  26
   #define MOS8_PIN  27

   /* Relay logic replaced by MOS1 */
   #define RELAY_PIN MOS1_PIN
   ```
2. 按键与 LED：
   ```c
   #define BUTTON_PIN  0   /* Board Key / Boot button */
   #define LED_PIN     23  /* Board User LED */
   ```
3. 传感器接口：
   ```c
   #define ADC_PIN_LOW_HUMIDITY  ADC_CHANNEL_6   /* GPIO34 (ADC1_CH6) */
   #define DS18B20_PIN           GPIO_NUM_4      /* GPIO4 */
   #define BH1750_SCL_IO         GPIO_NUM_22     /* Standard I2C SCL on ESP32 */
   #define BH1750_SDA_IO         GPIO_NUM_21     /* Standard I2C SDA on ESP32 */
   ```

- [ ] **Step 2: 修改 GPIO 初始化以保护 8 路 MOS 管上电安全**

在 `app_main()` 中，将原单一继电器配置改为配置全部 8 路 MOS 引脚，并在方向配置前原子性拉低并使能内部下拉：
```c
    /* GPIO: 8x MOS outputs (default off, pull-down enabled) */
    static const gpio_num_t s_mos_pins[8] = {
        MOS1_PIN, MOS2_PIN, MOS3_PIN, MOS4_PIN,
        MOS5_PIN, MOS6_PIN, MOS7_PIN, MOS8_PIN
    };
    uint64_t mos_mask = 0;
    for (int i = 0; i < 8; i++) {
        mos_mask |= (1ULL << s_mos_pins[i]);
        gpio_set_level(s_mos_pins[i], 0);
    }
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = mos_mask;
    io_conf.mode         = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
```

- [ ] **Step 3: 检查 Web 控制界面与状态上报**

确认 Web 页面中继电器开关控制继续操作 `RELAY_PIN` (即 MOS1)，状态上报字段、MQTT 消息结构与 Protobuf 协议保持完全向后兼容。

---

### Task 5: 语法与代码结构静态检查及验证

**Files:**
- Verify: `ESPWROM32-4M-8xMOS/main/main.c`
- Verify: `ESPWROM32-4M-8xMOS/main/CMakeLists.txt`
- Verify: `ESPWROM32-4M-8xMOS/CMakeLists.txt`

- [ ] **Step 1: 代码结构与宏完整性核验**

验证 `ESPWROM32-4M-8xMOS/main/main.c` 中没有遗漏任何旧芯片的引脚引用，宏定义与 `hardware_specification_v1.0.md` 规范完全对齐。

- [ ] **Step 2: 验证分区表与 Factory 工具一致性**

比对 `ESPWROM32-4M-8xMOS/partitions.csv` 与 `relay_v2/partitions.csv` 校验哈希，比对 `factory/gen_factory_nvs.py` 逻辑。

- [ ] **Step 3: 提取 Web C 字符串 HTML 语法验证**

利用 Node.js 对内嵌网页模板的 HTML / JS 进行语法解析校验，确保移植无乱码或截断。
