# ESPWROM32-4M-8xMOS 编译与烧录指南

## 硬件与环境要求

- **主控硬件**：ESP32-WROOM-32E (4MB Flash，无 PSRAM) + 8 路 MOS 板
- **目标芯片**：ESP32 (Xtensa 双核 LX6)
- **编译工具**：ESP-IDF v6.0.1（或 v5.x+）

---

## 1. 激活 IDF 环境

每次打开新终端都需要执行：

```bash
. /c/esp/v6.0.1/esp-idf/export.sh
```

Windows PowerShell：

```powershell
C:\esp\v6.0.1\esp-idf\export.ps1
```

---

## 2. 首次编译配置

`sdkconfig` 不提交 git，首次编译需要从 `sdkconfig.defaults` 生成。

进入工程目录，设置目标芯片为 `esp32`：

```bash
cd ESPWROM32-4M-8xMOS
rm -f sdkconfig
idf.py set-target esp32
```

---

## 3. 编译

```bash
idf.py build
```

编译成功后输出文件在 `build/` 目录：
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/flower.bin`

---

## 4. 首次出厂烧录（固件 + 设备身份）

### 第一步：生成该设备的身份分区 bin

```bash
python factory/gen_factory_nvs.py --id <device_id> --secret <device_secret> --out factory/factory_<device_id>.bin
```

### 第二步：烧写应用固件与 factory 分区

```bash
# 烧录应用固件（包含 bootloader 与分区表）
idf.py -p COMx flash

# 烧写设备唯一身份到 fctry 分区（offset 0x10000）
esptool.py -p COMx write_flash 0x10000 factory/factory_<device_id>.bin
```

或单条命令一键全烧：

```bash
esptool.py -p COMx write_flash \
    0x1000  build/bootloader/bootloader.bin \
    0x8000  build/partition_table/partition-table.bin \
    0x10000 factory/factory_<device_id>.bin \
    0x20000 build/flower.bin
```
*(注：经典 ESP32 的 bootloader 烧录地址通常为 0x1000，具体以 idf.py build 输出提示为准)*

---

## 5. OTA 升级

固件编译成功后，使用 OTA 命令行工具推送：

```bash
./ota.sh
```
