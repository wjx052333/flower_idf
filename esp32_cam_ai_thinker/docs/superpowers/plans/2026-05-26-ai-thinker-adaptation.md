# AI Thinker ESP32-CAM Adaptation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adapt the `esp32_cam_ai_thinker` IDF project (currently targeting emakefun ESP32-S3-R8 / OV3660) to build and run on the AI Thinker ESP32-CAM board (classic ESP32 / OV2640).

**Architecture:** Replace the `esp_video`/V4L2 camera driver (ESP32-S3 only) with the `espressif/esp32-camera` managed component; switch build target from `esp32s3` to `esp32`; remove all peripheral code that has no hardware on the AI Thinker (DS18B20, relay, ADC, Modbus, breathing LED); keep WiFi/MQTT/snapshot/OTA unchanged.

**Tech Stack:** ESP-IDF 5.x, `espressif/esp32-camera` (OV2640 driver), nanopb, MQTT 5.0, HTTPS OTA.

**Working directory:** `C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\esp32_cam_ai_thinker`

---

## File Map

| File | Action | What changes |
|------|--------|-------------|
| `main/idf_component.yml` | Modify | Swap `esp_video`+`esp_new_jpeg` → `espressif/esp32-camera` |
| `CMakeLists.txt` | Modify | Remove OV3660 sensor-config profile injection block |
| `main/CMakeLists.txt` | Modify | Remove `ds18b20.c` from SRCS; update REQUIRES |
| `main/Kconfig.projbuild` | Modify | Remove camera-pin and sensor GPIO menus; keep WiFi/MQTT only |
| `sdkconfig.defaults` | Modify | Switch to `esp32` target, 4 MB flash, Quad PSRAM, remove S3/OV3660 settings |
| `partitions.csv` | Replace | Resize OTA partitions to fit 4 MB flash |
| `main/ds18b20.c` | Delete | Not compiled, no hardware |
| `main/ds18b20.h` | Delete | Not compiled, no hardware |
| `main/camera.c` | Full rewrite | `esp_camera` API (was V4L2) |
| `main/camera.h` | No change | Public API stays identical |
| `main/main.c` | Modify | Remove DS18B20, relay, ADC, Modbus, breathing-LED code |

---

## Task 1: Update `idf_component.yml`

**Files:**
- Modify: `main/idf_component.yml`

- [ ] **Step 1: Replace the file contents**

```yaml
dependencies:
  espressif/esp32-camera: ">=2.0.0"
  idf: ">=5.3"
```

- [ ] **Step 2: Commit**

```bash
git add main/idf_component.yml
git commit -m "build: replace esp_video with esp32-camera managed component"
```

---

## Task 2: Update top-level `CMakeLists.txt`

Remove the OV3660 sensor-config profile injection (the `restore_sensor_config` custom target). Keep the `EXTRA_COMPONENT_DIRS` lines (nanopb lives there) and the `touch_build_info` target.

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Replace the file with the cleaned version**

```cmake
cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../ESP32-S3-CAM-OVxxxx/components")

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_thinker_cam_mqtt)

add_custom_target(touch_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/main/build_info.c
    COMMENT "Refreshing build timestamp"
)
```

- [ ] **Step 2: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: remove OV3660 sensor-config injection, rename project"
```

---

## Task 3: Update `main/CMakeLists.txt`

Remove `ds18b20.c` from SRCS. Remove `esp_driver_cam`, `esp_driver_i2c`, `esp_driver_uart`, `esp_adc`, `esp_new_jpeg` from REQUIRES. Add `esp32_camera` (the component name that `espressif/esp32-camera` registers as).

**Files:**
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Replace the file**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "camera.c"
        "build_info.c"
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
        esp_driver_gpio
        lwip
        nanopb
        esp_http_client
        esp_http_server
        esp_https_ota
        app_update
        spi_flash
        esp32_camera
)
```

- [ ] **Step 2: Commit**

```bash
git add main/CMakeLists.txt
git commit -m "build: remove ds18b20/S3-specific deps, add esp32_camera"
```

---

## Task 4: Update `main/Kconfig.projbuild`

Strip out the camera-pin submenu and DS18B20 GPIO config. Keep only WiFi SSID/password and MQTT broker URI.

**Files:**
- Modify: `main/Kconfig.projbuild`

- [ ] **Step 1: Replace the file**

```kconfig
menu "Flower CAM Device Configuration"

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

- [ ] **Step 2: Commit**

```bash
git add main/Kconfig.projbuild
git commit -m "config: remove camera-pin and sensor Kconfig options"
```

---

## Task 5: Update `sdkconfig.defaults`

Switch target to `esp32`, 4 MB flash, Quad PSRAM. Remove all S3-specific, OV3660, esp_video, and camera-pin settings.

**Files:**
- Modify: `sdkconfig.defaults`

- [ ] **Step 1: Replace the file**

```
# Flash / PSRAM (AI Thinker ESP32-CAM: 4MB flash + 4MB Quad PSRAM)
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"

CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y

# OTA: allow plain HTTP firmware URLs (for local dev server)
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y

# Partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# WiFi
CONFIG_WIFI_SSID="YOUR_SSID"
CONFIG_WIFI_PASSWORD="YOUR_PASSWORD"

# MQTT broker
CONFIG_MQTT_BROKER_URI="mqtts://your-broker:8883"

# TLS: skip cert check (dev only)
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y

# Enable MQTT 5.0
CONFIG_MQTT_PROTOCOL_5=y

# MQTT task stack
CONFIG_MQTT_TASK_STACK_SIZE=10240

# MQTT out-buffer large enough for a full JPEG snapshot
CONFIG_MQTT_BUFFER_SIZE=65536

# Logging
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# Target
CONFIG_IDF_TARGET="esp32"

# PSRAM alloc thresholds
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
```

- [ ] **Step 2: Commit**

```bash
git add sdkconfig.defaults
git commit -m "config: switch to esp32 target, 4MB flash, Quad PSRAM"
```

---

## Task 6: Update `partitions.csv`

Resize both OTA partitions to 1.6 MB each so they fit within 4 MB flash.

**Files:**
- Modify: `partitions.csv`

- [ ] **Step 1: Replace the file**

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
fctry,    data, nvs,     0x10000,  0x1000,   readonly
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x190000,
ota_1,    app,  ota_1,   0x1B0000, 0x190000,
```

Total used: 0x340000 = 3.25 MB < 4 MB ✓

- [ ] **Step 2: Commit**

```bash
git add partitions.csv
git commit -m "config: resize partitions for 4MB flash"
```

---

## Task 7: Delete `ds18b20.c` and `ds18b20.h`

**Files:**
- Delete: `main/ds18b20.c`
- Delete: `main/ds18b20.h`

- [ ] **Step 1: Delete the files**

```bash
rm main/ds18b20.c main/ds18b20.h
```

- [ ] **Step 2: Commit**

```bash
git add -u main/ds18b20.c main/ds18b20.h
git commit -m "remove: ds18b20 driver (no hardware on AI Thinker)"
```

---

## Task 8: Rewrite `camera.c`

Full replacement using the `esp_camera` API. The public API declared in `camera.h` is unchanged.

**Files:**
- Modify: `main/camera.c`

- [ ] **Step 1: Replace the entire file**

```c
/**
 * camera.c — OV2640 camera via esp_camera (AI Thinker ESP32-CAM)
 */

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camera.h"

/* AI Thinker ESP32-CAM pin definitions */
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22
#define CAM_FLASH_LED    4

#define LED_OFF_DELAY_US  (1000 * 1000)
#define TAG "Camera"

static camera_fb_t       *s_fb        = NULL;
static SemaphoreHandle_t  s_cam_sem;
static esp_timer_handle_t s_led_timer = NULL;
static bool               s_cam_ok    = false;

static void led_off_timer_cb(void *arg)
{
    gpio_set_level(CAM_FLASH_LED, 0);
}

esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location  = CAMERA_FB_IN_PSRAM,
    };

    esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_cam_ok = true;
    ESP_LOGI(TAG, "Camera init OK (OV2640, VGA JPEG)");

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << CAM_FLASH_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(CAM_FLASH_LED, 0);

    esp_timer_create_args_t targs = {
        .callback              = led_off_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "led_off",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&targs, &s_led_timer);

    s_cam_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(s_cam_sem);
    return ESP_OK;
}

esp_err_t camera_init_with_i2c(i2c_master_bus_handle_t i2c_handle)
{
    /* esp_camera manages I2C internally; external handle not used */
    (void)i2c_handle;
    return camera_init();
}

bool camera_is_ready(void)
{
    return s_cam_ok;
}

esp_err_t camera_capture_jpeg(const uint8_t **data, uint32_t *size)
{
    if (!s_cam_ok) {
        ESP_LOGE(TAG, "camera_capture_jpeg: camera not ready");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_cam_sem, portMAX_DELAY);

    gpio_set_level(CAM_FLASH_LED, 1);

    s_fb = esp_camera_fb_get();
    if (!s_fb) {
        gpio_set_level(CAM_FLASH_LED, 0);
        xSemaphoreGive(s_cam_sem);
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        return ESP_FAIL;
    }

    esp_timer_stop(s_led_timer);
    esp_timer_start_once(s_led_timer, LED_OFF_DELAY_US);

    *data = s_fb->buf;
    *size = s_fb->len;
    return ESP_OK;
}

void camera_release_jpeg(void)
{
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
    xSemaphoreGive(s_cam_sem);
}

static sensor_t *get_sensor(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) ESP_LOGE(TAG, "esp_camera_sensor_get returned NULL");
    return s;
}

esp_err_t camera_set_hmirror(int enable)
{
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_hmirror(s, enable ? 1 : 0);
    return ESP_OK;
}

esp_err_t camera_set_vflip(int enable)
{
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_vflip(s, enable ? 1 : 0);
    return ESP_OK;
}

esp_err_t camera_set_contrast(int level)
{
    if (level < -2 || level > 2) return ESP_ERR_INVALID_ARG;
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_contrast(s, level);
    return ESP_OK;
}

esp_err_t camera_set_saturation(int level)
{
    if (level < -2 || level > 2) return ESP_ERR_INVALID_ARG;
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_saturation(s, level);
    return ESP_OK;
}

esp_err_t camera_set_wb_mode(int mode)
{
    if (mode < 0 || mode > 4) return ESP_ERR_INVALID_ARG;
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    if (mode == 0) {
        s->set_whitebal(s, 1);      /* AWB on */
    } else {
        s->set_whitebal(s, 0);      /* AWB off */
        s->set_wb_mode(s, mode);    /* 1=Sunny 2=Cloudy 3=Office 4=Home */
    }
    return ESP_OK;
}
```

- [ ] **Step 2: Commit**

```bash
git add main/camera.c
git commit -m "camera: rewrite for esp_camera/OV2640 (AI Thinker pins)"
```

---

## Task 9: Simplify `main.c`

Remove DS18B20, ADC, Modbus, relay, and breathing-LED code. Simplify `publish_status_report` to send an empty metrics list. Remove the relay MQTT handler branch.

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Remove unused `#include` lines**

Remove these four lines (they reference removed peripherals):

```c
#include "ds18b20.h"           // remove
#include "driver/ledc.h"       // remove
#include "driver/uart.h"       // remove
#include "esp_adc/adc_oneshot.h" // remove
```

- [ ] **Step 2: Remove hardware macros**

Remove this block entirely (lines 79–93 in the original):

```c
#define LED_PIN              2
#define RELAY_PIN            48
#define HUMIDITY_ADC_CHANNEL ADC_CHANNEL_0  /* GPIO1 */
#define BREATH_PERIOD_MS     3000
#define APP_TIMER_INTERVAL_MS 50
#define BREATH_STEPS         (BREATH_PERIOD_MS / APP_TIMER_INTERVAL_MS)
#define RELAY_DEFAULT_DURATION_MS 10000

#define MODBUS_UART_PORT  UART_NUM_0
#define MODBUS_UART_TXD   GPIO_NUM_43
#define MODBUS_UART_RXD   GPIO_NUM_44
#define MODBUS_BAUD       9600

#ifndef CONFIG_DS18B20_GPIO
#define CONFIG_DS18B20_GPIO  46
#endif
```

- [ ] **Step 3: Remove state variables for removed peripherals**

Remove these declarations:

```c
static esp_timer_handle_t s_app_timer = NULL;
static int64_t            s_app_tick  = 0;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

static volatile bool            s_relay_on       = false;
static volatile int64_t         s_relay_on_us    = 0;
static volatile uint32_t        s_relay_duration = RELAY_DEFAULT_DURATION_MS;
```

- [ ] **Step 4: Remove the `app_timer_cb` function**

Remove the entire function (it handled breathing LED duty update and relay auto-off):

```c
static void app_timer_cb(void *arg)
{
    // ... entire function body ...
}
```

- [ ] **Step 5: Remove the `adc_init` and `adc_read_raw` functions**

Remove both functions entirely:

```c
static void adc_init(void) { ... }
static int adc_read_raw(adc_channel_t channel) { ... }
```

- [ ] **Step 6: Remove the `send_relay_resp` function**

Remove the entire function:

```c
static void send_relay_resp(bool success) { ... }
```

- [ ] **Step 7: Remove all Modbus functions**

Remove these three functions entirely:

```c
static uint16_t modbus_crc16(const uint8_t *data, int len) { ... }
static void modbus_init(void) { ... }
static float modbus_read_illuminance(void) { ... }
```

- [ ] **Step 8: Remove the `s_metrics` array and `encode_metrics` callback**

Remove these two declarations:

```c
static flower_metric_t s_metrics[3];

static bool encode_metrics(pb_ostream_t *stream, const pb_field_t *field,
                           void *const *arg) { ... }
```

- [ ] **Step 9: Replace `publish_status_report`**

Replace the entire function with a version that sends no metrics:

```c
static void publish_status_report(void)
{
    if (!s_mqtt_connected) return;
    time_t now; time(&now);

    flower_status_report_t sr = FLOWER_STATUS_REPORT_INIT_ZERO;
    sr.timestamp     = (int64_t)now * 1000;
    sr.has_version   = true;
    sr.version.major = 1;
    sr.version.minor = 0;
    sr.version.patch = 0;
    strcpy(sr.device_type, "camera");

    uint8_t buf[STATUS_BUF_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_status_report_t_msg, &sr)) {
        ESP_LOGE(TAG, "Status encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
    ESP_LOGI(TAG, "Status published");
}
```

- [ ] **Step 10: Remove the relay handler from `handle_mqtt_data`**

In the `switch (s_cmd.which_payload)` block, remove the entire `FLOWER_COMMAND_RELAY_CONTROL_TAG` case:

```c
case FLOWER_COMMAND_RELAY_CONTROL_TAG: {
    // ... entire case body ...
    break;
}
```

- [ ] **Step 11: Also remove the HTTP relay handler and its registration**

Remove the `http_relay_handler` function:

```c
static esp_err_t http_relay_handler(httpd_req_t *req) { ... }
```

And remove these two lines from `http_server_start`:

```c
httpd_uri_t relay_uri    = { .uri = "/change_relay1", .method = HTTP_GET, .handler = http_relay_handler };
httpd_register_uri_handler(s_httpd, &relay_uri);
```

- [ ] **Step 12: Clean up `app_main` — remove sensor/relay/LED init blocks**

Remove these blocks from `app_main` (keep everything else):

```c
/* ADC: humidity sensor on IO1 */
adc_init();

/* Modbus-RTU: illuminance sensor (IO43=TXD, IO44=RXD) via UART0 */
modbus_init();

/* DS18B20 temperature sensor */
if (CONFIG_DS18B20_GPIO >= 0) {
    ds18b20_init((gpio_num_t)CONFIG_DS18B20_GPIO);
}

/* LEDC: breathing LED on IO2 */
ledc_timer_config_t ledc_timer = { ... };
ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
ledc_channel_config_t ledc_ch = { ... };
ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch));

/* GPIO: relay (output, default off, active-low) */
gpio_config_t io_conf = {};
io_conf.pin_bit_mask = (1ULL << RELAY_PIN);
io_conf.mode         = GPIO_MODE_OUTPUT;
ESP_ERROR_CHECK(gpio_config(&io_conf));
gpio_set_level(RELAY_PIN, 1);

/* App timer: 50ms breathing LED */
esp_timer_create_args_t app_args = { ... };
ESP_ERROR_CHECK(esp_timer_create(&app_args, &s_app_timer));
ESP_ERROR_CHECK(esp_timer_start_periodic(s_app_timer,
                    (uint64_t)APP_TIMER_INTERVAL_MS * 1000ULL));
```

- [ ] **Step 13: Update the comment in `app_main` and the log header**

Change the file-top comment block to reflect the new board:

```c
/**
 * main.c — Flower AI Thinker ESP32-CAM node (ESP32 + OV2640)
 *
 * Features:
 *   1. WiFi STA + NTP time sync
 *   2. HMAC-SHA256 MQTT credentials (clientId{id}timestamp{ms})
 *   3. MQTT 5.0 with User Property {"project","flower"} on CONNECT
 *   4. nanopb: receives snapshot/ota commands; sends status_report + cmd_response
 *   5. OV2640 DVP camera via esp_camera
 *   6. Snapshot: JPEG capture + HTTP PUT to pre-signed URL (flash LED GPIO4)
 *   7. OTA: HTTPS firmware update triggered via MQTT OtaCommand
 */
```

- [ ] **Step 14: Commit**

```bash
git add main/main.c
git commit -m "main: remove sensors/relay/LEDC, simplify for AI Thinker"
```

---

## Task 10: Build Verification

Run a full clean build to confirm everything compiles.

**Files:** (none modified — verification only)

- [ ] **Step 1: Delete stale sdkconfig and managed_components cache**

```bash
idf.py fullclean
```

Expected: cleans build directory without errors.

- [ ] **Step 2: Run the build**

```bash
idf.py build
```

Expected: build completes with output like:
```
Project build complete. To flash, run:
 idf.py flash
```

No errors. Warnings about unused Kconfig symbols are acceptable.

- [ ] **Step 3: If build fails — common fixes**

**"No such component esp32_camera"** → Check that `idf_component.yml` was saved correctly and run `idf.py update-dependencies` before `idf.py build`.

**"CONFIG_CAM_LED_GPIO undeclared"** → Make sure the old `#if CONFIG_CAM_LED_GPIO >= 0` guard was fully removed from the new `camera.c` (the rewrite in Task 8 does not use it).

**"identifier 'RELAY_PIN' undeclared"** → A macro removal in Task 9 was missed. Search `main.c` for remaining `RELAY_PIN`, `LED_PIN`, `HUMIDITY_ADC_CHANNEL` references and remove them.

**Partition overflow** → Run `idf.py size` to see binary size; if it exceeds 0x190000 (1.6 MB), reduce `CONFIG_LOG_DEFAULT_LEVEL` or disable unused IDF components.

- [ ] **Step 4: Commit build artifacts note**

```bash
git add sdkconfig   # generated sdkconfig (new esp32 target)
git commit -m "build: verified clean build for AI Thinker (esp32 + OV2640)"
```

---

## Self-Review Checklist (spec coverage)

| Spec requirement | Task |
|-----------------|------|
| Switch IDF target to esp32 | Task 5 |
| Switch camera driver to esp_camera | Tasks 1, 3, 8 |
| AI Thinker pin assignment | Task 8 |
| 4 MB flash config | Task 5 |
| Quad PSRAM config | Task 5 |
| Resize partitions for 4 MB | Task 6 |
| Remove DS18B20 | Tasks 7, 9 |
| Remove relay | Task 9 |
| Remove ADC humidity | Task 9 |
| Remove Modbus illuminance | Task 9 |
| Remove breathing LED / LEDC | Task 9 |
| Keep WiFi / NTP / MQTT | Task 9 (unchanged) |
| Keep OTA | Task 9 (unchanged) |
| Keep snapshot (JPEG capture + HTTP PUT) | Tasks 8, 9 |
| Flash LED on GPIO4 during snapshot | Task 8 |
| Camera sensor controls (mirror/flip/WB/contrast/sat) | Task 8 |
| status_report with empty metrics | Task 9 |
| Remove Kconfig camera-pin submenu | Task 4 |
| Remove sensor_config profile injection | Task 2 |
