# emakefun ESP32-S3-R8 Camera Node

基于 ESP-IDF 的 MQTT 摄像头节点，运行在 emakefun ESP32-S3-R8 开发板上。

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-S3-R8（双核 240MHz，8MB PSRAM） |
| Flash | 8MB |
| 摄像头 | OV3660 DVP，640×480 YUYV → 软件 JPEG 编码 |
| 闪光灯 | GPIO 3（拍照时自动亮起） |
| 接口 | WiFi 802.11 b/g/n |

## 功能

- **MQTT 5.0**：HMAC-SHA256 鉴权，接收 snapshot / OTA 指令
- **HTTP 预览**：内置 HTTP server，浏览器直接查看实时抓图
- **Snapshot 上传**：收到 MQTT 指令后抓 JPEG → HTTP PUT 到预签名 URL
- **OTA**：通过 MQTT 指令触发 HTTPS 固件升级
- **LED flash**：抓图期间自动点亮 GPIO 3 补光

## 快速开始

### 1. 配置凭据

新建 `sdkconfig.defaults.private`（已 gitignore）：

```ini
CONFIG_WIFI_SSID="your_ssid"
CONFIG_WIFI_PASSWORD="your_password"
CONFIG_MQTT_BROKER_URI="mqtts://your-broker:8883"
```

### 2. 编译烧录

```bash
idf.py build flash monitor
```

如果之前编译过其他配置，先清理：

```bash
rm -rf build sdkconfig
idf.py build flash monitor
```

### 3. 烧录设备身份（可选）

设备身份（device_id / device_secret）存在 `fctry` NVS 分区，参考 `../relay/factory/README.md`：

```bash
python ../relay/factory/gen_factory_nvs.py --id <device_id> --secret <secret>
esptool.py write_flash 0x10000 factory.bin
```

未烧录时设备仍可运行，但 MQTT topic 中的 device_id 为空。

## HTTP 预览

设备联网后串口会打印 IP 地址，例如 `Got IP: 192.168.1.123`。

| 端点 | 方法 | 说明 |
|---|---|---|
| `http://<IP>/` | GET | 设备状态 JSON |
| `http://<IP>/snapshot` | GET | 抓一帧 JPEG 并返回 |

### 浏览器预览

直接在浏览器打开：

```
http://192.168.1.123/snapshot
```

浏览器会显示一张 JPEG 静态图。**刷新页面**即可重新抓取一帧。

### curl 保存图片

```bash
curl http://192.168.1.123/snapshot -o snap.jpg
```

### 连续预览（每秒刷新）

用 HTML 轮询即可实现类视频预览：

```html
<!DOCTYPE html>
<html>
<body>
  <img id="cam" src="http://192.168.1.123/snapshot" width="640">
  <script>
    setInterval(() => {
      document.getElementById('cam').src =
        'http://192.168.1.123/snapshot?' + Date.now();
    }, 1000);
  </script>
</body>
</html>
```

将 IP 替换为实际地址，保存为 `.html` 文件用浏览器打开即可。

## MQTT Topics

| Topic | 方向 | 说明 |
|---|---|---|
| `flower/{id}/down/cmd` | ↓ 下行 | 接收 snapshot / OTA 指令（protobuf） |
| `flower/{id}/up/status` | ↑ 上行 | 每 30s 上报状态 |
| `flower/{id}/up/cmd_response` | ↑ 上行 | 指令执行结果 |

## 引脚定义

| 信号 | GPIO |
|---|---|
| XCLK | 15 |
| PCLK | 13 |
| VSYNC | 6 |
| HREF | 7 |
| D0–D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| SCCB SCL | 5 |
| SCCB SDA | 4 |
| Flash LED | 3 |

## 分区表

| 分区 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| nvs | 0x9000 | 24KB | WiFi/系统 NVS |
| fctry | 0x10000 | 4KB | 设备身份（只读） |
| ota_0 | 0x20000 | 3.875MB | OTA 槽 0 |
| ota_1 | 0x400000 | 3.875MB | OTA 槽 1 |
