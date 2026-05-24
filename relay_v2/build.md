# 编译与烧录指南

## 环境要求

- ESP-IDF v6.0.1，安装路径 `C:/esp/v6.0.1/esp-idf`
- 目标芯片：ESP32-C3（如需更换见下方说明）

---

## 1. 激活 IDF 环境

每次打开新终端都需要执行：

```bash
. /c/esp/v6.0.1/esp-idf/export.sh
```

Windows PowerShell 用：

```powershell
C:\esp\v6.0.1\esp-idf\export.ps1
```

---

## 2. 拉取依赖组件

`components/esp_mqtt` 不提交 git，需要手动拉取：

```bash
cd relay
git clone --depth 1 https://github.com/espressif/esp-mqtt.git components/esp_mqtt
```

> `components/nanopb` 已提交 git，无需单独拉取。

---

## 3. 首次拉取后配置

`sdkconfig` 不提交 git，首次需要从 `sdkconfig.defaults` 生成。
先确认 `sdkconfig.defaults` 里的配置正确：

```
CONFIG_WIFI_SSID="你的WiFi名"
CONFIG_WIFI_PASSWORD="你的WiFi密码"
CONFIG_DEVICE_ID="dev-005"
CONFIG_DEVICE_SECRET="..."
CONFIG_MQTT_BROKER_URI="mqtts://host:8883"
```

然后删除旧的 sdkconfig（如果存在）并设置目标芯片：

```bash
cd relay
rm -f sdkconfig
idf.py set-target esp32c3
```

> 如果是其他芯片，将 `esp32c3` 替换为 `esp32s3` / `esp32c6` 等。

---

## 4. 编译

```bash
idf.py build
```

编译成功后输出文件在 `build/` 目录，主要文件：

```
build/flower.bin       # 应用固件
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
```

---

## 5. 烧录

```bash
idf.py -p PORT flash
```

`PORT` 示例：
- Windows：`COM3`、`COM4`（设备管理器查看）
- Linux/Mac：`/dev/ttyACM0`、`/dev/ttyUSB0`

一次性完成编译+烧录+监控：

```bash
idf.py -p PORT flash monitor
```

---

## 6. 查看日志

```bash
idf.py -p PORT monitor
```

退出监控：`Ctrl + ]`

正常启动日志应包含：

```
I (xxx) Flower: Got IP: 192.168.x.x
I (xxx) Flower: MQTT connected, sub: flower/dev-005/down/cmd
I (xxx) Flower: Heartbeat published
```

---

## 7. 清理重编

```bash
idf.py fullclean
idf.py build
```

---

## 目录结构说明

```
relay/
├── main/
│   ├── main.c                  # 主程序
│   ├── Kconfig.projbuild       # 声明可配置项（WiFi、设备ID等）
│   └── proto/
│       ├── device.pb.c         # protobuf 生成文件（已提交，无需重新生成）
│       └── device.pb.h
├── components/
│   ├── esp_mqtt/               # MQTT 组件（需手动 git clone，见步骤2）
│   └── nanopb/                 # protobuf 编解码库
├── sdkconfig.defaults          # 配置初始值（含密码，不提交 git）
├── sdkconfig                   # 构建生成，不提交 git
└── CMakeLists.txt
```

---

## 注意事项

- `sdkconfig` 和 `sdkconfig.defaults` 包含 WiFi 密码和设备密钥，**不要提交到 git**
- `proto/device.pb.c` 和 `device.pb.h` 是由 `.proto` 文件生成的，已提交，无需安装 protoc
- 编译时出现 `mqtt does not contain a CMakeLists.txt` 是正常警告，可忽略

---

## Factory 分区（设备身份烧写）

设备 ID 和 Secret 存放在独立的 `fctry` NVS 分区（offset `0x10000`，大小 `0x1000`），
与应用固件分开烧写，OTA 升级不会覆盖它。

### 分区布局

```
0x9000   nvs       运行时配置（WiFi 连接信息等）
0xf000   phy_init  RF 校准数据
0x10000  fctry     设备身份（device_id + device_secret）← 本节关注
0x20000  factory   应用固件
```

### 生成 factory.bin

需要先激活 IDF 环境（保证 IDF_PATH 已设置）：

```bash
cd relay/factory
python gen_factory_nvs.py --id dev-001 --secret <device_secret> --out factory_dev-001.bin
```

参数说明：
- `--id`：设备 ID，建议用 MAC 地址或序列号
- `--secret`：该设备在后端数据库中对应的密钥
- `--out`：输出的 bin 文件名

### 烧写 factory 分区

**首次生产（固件 + factory 一起烧）：**

```bash
idf.py -p PORT flash                                          # 烧固件
esptool.py -p PORT write_flash 0x10000 factory_dev-001.bin   # 烧身份
```

**仅替换身份（不动固件）：**

```bash
esptool.py -p PORT write_flash 0x10000 factory_dev-001.bin
```

### 固件读取 factory 分区

代码从 `fctry` 分区的 `identity` namespace 读取：

```c
nvs_handle_t h;
nvs_open_from_partition("fctry", "identity", NVS_READONLY, &h);
size_t len = sizeof(g_device_id);
nvs_get_str(h, "device_id",     g_device_id,     &len);
len = sizeof(g_device_secret);
nvs_get_str(h, "device_secret", g_device_secret, &len);
nvs_close(h);
```
