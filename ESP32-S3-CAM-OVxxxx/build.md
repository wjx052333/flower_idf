# ESP32-S3-CAM Build & Test Guide

## Prerequisites

- ESP-IDF v5.5+ installed and activated (`C:\esp\esp-idf` or equivalent)
- ESP32-S3 board with OV5640 DVP camera
- 8 MB flash (adjust `partitions.csv` and `sdkconfig.defaults` if your module has 4 MB)
- Python 3 for the local OTA web server

## 1. First-time setup

### Set target and configure

```bash
cd client/esp32/flower_idf/ESP32-S3-CAM-OVxxxx

idf.py set-target esp32s3
```

Edit `sdkconfig.defaults` to set your real values before building:

```
CONFIG_WIFI_SSID="your_ssid"
CONFIG_WIFI_PASSWORD="your_password"
CONFIG_MQTT_BROKER_URI="mqtts://your-emqx-host:8883"
```

Or use menuconfig for interactive configuration:

```bash
idf.py menuconfig
# → Flower CAM Device Configuration
```

### Regenerate protobuf files (only needed when flower.proto changes)

```bash
cd client/esp32/flower_idf/relay
bash setup_proto.sh
```

The CAM project reuses the output from the relay's `setup_proto.sh` (see `CMakeLists.txt`).
If you made changes to `flower.proto` or `flower.options`, run this script and then copy
the generated `flower.pb.h` / `flower.pb.c` into `ESP32-S3-CAM-OVxxxx/main/proto/`.

`components/esp_mqtt` 不提交 git，需要手动拉取：

```bash
cd relay
git clone --depth 1 https://github.com/espressif/esp-mqtt.git components/esp_mqtt
```

## 2. Build

```bash
idf.py build
```

The build output is at `build/cam_mqtt.bin`.

## 3. Flash

### Full flash (first time — includes partition table + NVS)

```bash
idf.py -p COM<N> flash
```

Replace `COM<N>` with your serial port (Windows: `COM3`, Linux/Mac: `/dev/ttyUSB0`).

### Flash + monitor

```bash
idf.py -p COM<N> flash monitor
```

### Flash SR models (required for wake word + noise suppression)

```bash
esptool.py --chip esp32s3 write_flash 0x7E0000 srmodels.bin
```

Pack the models first with `scripts/pack_models.py` (see section 9).

Press `Ctrl+]` to exit the monitor.

## 4. Provision device identity

The firmware reads `device_id` and `device_secret` from the `fctry` NVS partition.
Use the relay project's provisioning script, or flash manually:

```bash
# From the relay project directory (same tool works for both boards)
python3 provision.py --port COM<N> --id cam001 --secret your_secret
```

If no identity is provisioned, the device will still connect using empty strings (useful for initial smoke-test).

## 5. OTA update

### Start the local firmware server

```bash
cd client/esp32/flower_idf/web
python3 server.py --no-tls --port 19100
```

The server automatically discovers `../relay/build/*.bin` and `../ESP32-S3-CAM-OVxxxx/build/*.bin`
if you set `BUILD_DIR` to point at the CAM build, or upload the bin manually via the web UI.

### Trigger OTA via MQTT

Send a `Command` protobuf with `OtaCommand.url` pointing to the firmware binary:

```
Topic:   flower/{device_id}/down/cmd
Payload: Command { ota { url: "http://192.168.0.x:19100/firmware/cam_mqtt.bin" } }
```

The device will respond with `OtaResponse` progress updates on:
```
Topic: flower/{device_id}/up/cmd_response
```

Sequence: `OTA_STARTED → OTA_PROGRESS (0–100%) → OTA_SUCCESS` then auto-restart.

## 6. Snapshot

### Trigger via MQTT

Send a `Command` protobuf with `SnapshotCommand`:

```
Topic:   flower/{device_id}/down/cmd
Payload: Command {
           snapshot {
             upload_url: "https://bucket.cos.region.myqcloud.com/key?<presigned-params>"
             width:  640   # optional, 0 = device default
             height: 480   # optional, 0 = device default
           }
         }
```

The device captures a JPEG frame, HTTP PUT to `upload_url`, then responds:

```
Topic: flower/{device_id}/up/cmd_response
Payload: CommandResponse {
           snapshot {
             result: SNAPSHOT_OK   # or error code
             size:   <bytes>
           }
         }
```

### SnapshotResultCode values

| Code | Meaning |
|------|---------|
| `SNAPSHOT_OK` | Upload succeeded |
| `SNAPSHOT_BUSY` | Previous snapshot still in progress |
| `SNAPSHOT_CAPTURE_FAILED` | V4L2 camera error |
| `SNAPSHOT_UPLOAD_FAILED` | HTTP PUT network/timeout error |
| `SNAPSHOT_UPLOAD_REJECTED` | HTTP PUT returned non-2xx (URL expired / auth) |
| `SNAPSHOT_INVALID_URL` | `upload_url` is empty |

## 7. Camera pin configuration

Default pins match the ESP32-S3-EYE / OV5640 DVP layout.
Override via menuconfig (`Flower CAM Device Configuration → Camera DVP Pin Configuration`)
or directly in `sdkconfig.defaults`:

```
CONFIG_CAM_SCCB_SCL_PIN=7
CONFIG_CAM_SCCB_SDA_PIN=8
CONFIG_CAM_XCLK_PIN=38
CONFIG_CAM_PCLK_PIN=41
CONFIG_CAM_VSYNC_PIN=17
CONFIG_CAM_DE_PIN=18
CONFIG_CAM_D0_PIN=45  # ... D7
```

## 8. Partition layout (16 MB flash)

| Partition | Offset     | Size     | Purpose |
|-----------|------------|----------|---------|
| nvs       | 0x009000   | 24 KB    | System NVS |
| phy_init  | 0x00F000   | 4 KB     | RF calibration |
| fctry     | 0x010000   | 4 KB     | Device identity (device_id, device_secret) |
| otadata   | 0x011000   | 8 KB     | OTA boot selection |
| ota_0     | 0x020000   | 3.875 MB | App slot 0 |
| ota_1     | 0x400000   | 3.875 MB | App slot 1 |
| model     | 0x7E0000   | 8.125 MB | SR models (WakeNet + NSNet, raw mmap) |

## 9. SR models

The audio pipeline requires two models:
- **WakeNet** (`wn9_hiesp`) — "Hi ESP" wake word detection in IDLE mode
- **NSNet** (`nsnet2`) — noise suppression in LISTENING mode

Both come bundled with the esp-sr component. To pack and flash:

```bash
cd C:\temp\esp32\test\livekit_mqtt_cpp

# Pack models into srmodels.bin
python client/esp32/flower_idf/ESP32-S3-CAM-OVxxxx/scripts/pack_models.py

# Flash to the model partition
esptool.py --chip esp32s3 write_flash 0x7E0000 srmodels.bin
```
