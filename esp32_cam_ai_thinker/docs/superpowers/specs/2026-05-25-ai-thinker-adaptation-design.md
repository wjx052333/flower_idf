# AI Thinker ESP32-CAM Adaptation Design

**Date:** 2026-05-25  
**Scope:** Adapt `esp32_cam_ai_thinker` (currently a copy of `emakefun_esp32_s3r8_camera`) to run on the AI Thinker ESP32-CAM board (classic ESP32 + OV2640).

---

## 1. Background

The source project targets an **emakefun ESP32-S3-R8** board and uses:
- IDF `esp_video` + V4L2 DVP camera driver (ESP32-S3/P4 only)
- OV3660 sensor
- DS18B20 temperature, ADC soil-humidity, Modbus illuminance, relay

The target board (**AI Thinker ESP32-CAM**) is:
- Classic **ESP32** (Xtensa LX6 dual-core), 4 MB flash, 4 MB Quad-SPI PSRAM
- OV2640 sensor (2 MP)
- No external sensors or relay

`esp_video` does not support classic ESP32; the canonical driver is the `esp_camera` IDF component (`espressif/esp32-camera`).

---

## 2. Features Retained

- WiFi STA connection
- NTP time sync
- HMAC-SHA256 MQTT credentials (unchanged)
- MQTT 5.0 with User Property `{"project","flower"}` (unchanged)
- Nanopb: receive `snapshot` / `ota` commands; send `status_report` + `cmd_response` (unchanged)
- OV2640 DVP camera via `esp_camera`
- Snapshot: JPEG capture → HTTP PUT to pre-signed URL (flash LED on GPIO4 during capture)
- OTA: HTTPS firmware update via MQTT `OtaCommand`
- HTTP server: `/` (JSON status), `/view` (tuning UI), `/snapshot`, `/cam_ctrl` (mirror/flip/WB/contrast/saturation)
- Factory NVS partition for `device_id` / `device_secret`

## 3. Features Removed

- DS18B20 temperature sensor (GPIO46 invalid on classic ESP32)
- ADC soil-humidity sensor (GPIO numbering incompatible)
- Modbus-RTU illuminance (UART0 on GPIO43/44 invalid on classic ESP32)
- Relay control (GPIO48 invalid on classic ESP32)
- Breathing LED via LEDC (the app timer and LEDC wiring are removed; LED GPIO4 is used only as flash)
- Sensor config profile injection (OV3660-specific, in `CMakeLists.txt`)

`status_report` will still be published periodically but with no metrics (empty metrics array).

---

## 4. Architecture

```
main.c              WiFi / NTP / MQTT / HTTP / OTA / Snapshot
camera.c            esp_camera init + JPEG capture (OV2640)
camera.h            Public API — unchanged
build_info.c/h      Build timestamp — unchanged
flower.pb.c/h       Nanopb generated — unchanged
```

No new files are introduced. All removed features are deleted from `main.c` and `Kconfig.projbuild`.

---

## 5. Camera Driver

Replace `esp_video` / V4L2 with `espressif/esp32-camera` (IDF managed component).

**`camera.c` new implementation:**

```
camera_init()
  → esp_camera_init(camera_config_t)   // AI Thinker pins, OV2640
  → set JPEG quality to 12 (medium-high)
  → configure flash LED GPIO4 as output

camera_capture_jpeg(data, size)
  → turn on flash LED
  → esp_camera_fb_get()                // blocks until frame ready
  → *data = fb->buf, *size = fb->len
  → (caller must call camera_release_jpeg to free)

camera_release_jpeg()
  → esp_camera_fb_return(fb)
  → turn off flash LED (via off-delay timer)

camera_set_hmirror(enable)
camera_set_vflip(enable)
camera_set_contrast(level)
camera_set_saturation(level)
camera_set_wb_mode(mode)
  → sensor_t *s = esp_camera_sensor_get()
  → s->set_hmirror / set_vflip / set_contrast / set_saturation / set_whitebal + set_wb_mode
```

`camera.h` is unchanged — same five public functions.

---

## 6. AI Thinker Pin Assignment

From `CAMERA_MODEL_AI_THINKER` in `camera_pins.h`:

| Signal | GPIO |
|--------|------|
| PWDN   | 32   |
| RESET  | -1   |
| XCLK   | 0    |
| SIOD (SDA) | 26 |
| SIOC (SCL) | 27 |
| D0 (Y2) | 5  |
| D1 (Y3) | 18 |
| D2 (Y4) | 19 |
| D3 (Y5) | 21 |
| D4 (Y6) | 36 |
| D5 (Y7) | 39 |
| D6 (Y8) | 34 |
| D7 (Y9) | 35 |
| VSYNC  | 25   |
| HREF   | 23   |
| PCLK   | 22   |
| Flash LED | 4 |

XCLK frequency: 20 MHz (standard for OV2640).

---

## 7. File-by-File Changes

### `idf_component.yml` (`main/idf_component.yml`)
- Remove: `espressif/esp_video`, `espressif/esp_new_jpeg`
- Add: `espressif/esp32-camera: ">=2.0.0"`
- Lower IDF requirement to `>=5.3` (classic ESP32 is fully supported)

### `sdkconfig.defaults`
- `CONFIG_IDF_TARGET="esp32"` (was `esp32s3`)
- Flash: `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`, `"4MB"` (was 8MB)
- PSRAM: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_SPEED_80M=y`, `CONFIG_SPIRAM_USE_MALLOC=y`; remove `CONFIG_SPIRAM_MODE_OCT` (Quad is default for classic ESP32)
- Remove all `CONFIG_CAMERA_OV3660_*`, `CONFIG_ESP_VIDEO_*`, `CONFIG_CAMERA_XCLK_USE_LEDC`
- Remove `CONFIG_CAM_*` pin settings (now hardcoded in `camera.c` from `esp_camera` config struct)
- Keep: WiFi, MQTT, OTA, TLS, NVS, partition table settings

### `partitions.csv`
- Resize OTA partitions to fit 4 MB flash:
  - `ota_0`: offset `0x20000`, size `0x190000` (~1.6 MB)
  - `ota_1`: offset `0x1B0000`, size `0x190000` (~1.6 MB)
  - Remove or shrink to fit within 4 MB total

### `Kconfig.projbuild`
- Remove entire `Camera DVP Pin Configuration` submenu (pins are hardcoded)
- Remove `DS18B20_GPIO` config
- Keep: `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER_URI`

### `main/CMakeLists.txt`
- Remove `ds18b20.c` from SRCS
- Remove `esp_driver_cam`, `esp_driver_i2c`, `esp_driver_uart`, `esp_adc`, `esp_new_jpeg` from REQUIRES
- Add `esp32_camera` to REQUIRES (provided by the `espressif/esp32-camera` managed component)

### `CMakeLists.txt` (top-level)
- Remove `copy_sensor_config.cmake` custom target and `EXTRA_COMPONENT_DIRS` for `ESP32-S3-CAM-OVxxxx`
- Keep `EXTRA_COMPONENT_DIRS` for `relay/components` (nanopb lives there)

### `camera.c`
- Full rewrite (~120 lines) using `esp_camera.h` API
- No V4L2, no mmap, no software JPEG encoder, no I2C master bus setup
- Flash LED managed with same off-delay timer pattern

### `camera.h`
- No changes

### `main.c`
- Remove: `#include "ds18b20.h"`, all DS18B20 calls
- Remove: all ADC init and `adc_read_raw`
- Remove: all `modbus_*` functions and calls
- Remove: `RELAY_PIN`, `gpio_config` for relay, relay state variables, `send_relay_resp`, relay MQTT handler branch
- Remove: `LEDC_*` init and `app_timer_cb` breathing LED duty update; remove `s_app_timer`
- Remove: `HUMIDITY_ADC_CHANNEL`, `BREATH_*` constants
- Update `LED_PIN`: keep as breathing LED or remove entirely — since AI Thinker's GPIO2 is used for PSRAM on some variants, remove breathing LED
- Update `app_main`: remove all sensor/relay/LEDC init blocks
- Update `publish_status_report`: remove temp/humi/lux metrics, send empty metrics
- Keep: `MQTT_EVENT_RELAY_CONTROL_TAG` handler can be removed (no relay)

### `ds18b20.c` / `ds18b20.h`
- Remove files from project (not compiled, not needed)

### `sensor_config/` directory
- Remove or leave in place (no build references once CMakeLists.txt is cleaned)

---

## 8. `partitions.csv` for 4 MB Flash

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
fctry,    data, nvs,     0x10000,  0x1000,   readonly
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x190000,
ota_1,    app,  ota_1,   0x1B0000, 0x190000,
```

Total: 0x1B0000 + 0x190000 = 0x340000 = 3.25 MB < 4 MB ✓

---

## 9. Success Criteria

1. Project compiles with `idf.py build` targeting `esp32`
2. Camera initializes and produces JPEG frames from OV2640
3. MQTT connects and publishes `status_report` every 30 s
4. Snapshot command: JPEG captured, uploaded to pre-signed URL, response published
5. OTA command: firmware downloaded and applied, device reboots
6. HTTP `/snapshot` returns JPEG image
7. Flash LED (GPIO4) fires during each snapshot capture
