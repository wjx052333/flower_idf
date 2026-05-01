# Factory 分区烧写指南

设备 ID 和密钥存放在独立的 `fctry` NVS 分区（offset `0x10000`，大小 `0x1000` = 4KB），
与应用固件分开烧写，OTA 升级不会覆盖它。

---

## 文件说明

```
factory/
├── README.md                 # 本文件
├── factory_nvs_template.csv  # NVS 数据格式参考
└── gen_factory_nvs.py        # 生成 factory bin 的脚本
```

---

## 前置条件

- 已激活 IDF 环境（`export.sh` / `export.ps1`）
- Python 3

---

## 第一步：确定设备 ID 和密钥

| 字段 | 说明 | 示例 |
|------|------|------|
| `device_id` | 设备唯一标识，需与后端数据库一致 | `dev-001` |
| `device_secret` | HMAC-SHA256 密钥，需与后端记录一致 | `2c4d840b...`（64位hex） |

> **密钥生成建议**：每台设备使用不同的随机密钥。
> ```bash
> # Linux/Mac 生成一个随机密钥
> openssl rand -hex 32
> # 输出示例: 2c4d840b5f2c429c969bfde0337b2ef9e31810c596dfd0976898911cb7a91ea3
> ```

---

## 第二步：生成 factory.bin

```bash
cd relay

python factory/gen_factory_nvs.py --id     flr001  --secret 37e73a61317f6ca96f054ff7025c18ac67d0ccf636f376c78bb800fd9e8d2eb3   --out    factory/factory_flr001.bin
```

成功后输出：
```
Generating factory_dev-001.bin for device 'dev-001' ...
Done: /path/to/factory_dev-001.bin
Flash with: esptool.py write_flash 0x10000 factory_dev-001.bin
```

---

## 第三步：烧写

### 方式 A：首次生产（固件 + factory 一起烧）

```bash
# 1. 烧应用固件（包含 bootloader 和分区表）
idf.py -p COM3 flash

# 2. 烧设备身份（单独写 fctry 分区）
esptool.py -p COM7 write_flash 0x10000 factory/factory_flr001.bin
```

### 方式 B：批量烧写（生产线，只烧 factory，固件已在）

```bash
esptool.py -p COM3 --baud 921600 write_flash 0x10000 factory_dev-001.bin
```

### 方式 C：一条命令同时烧固件和 factory

```bash
esptool.py -p COM3 write_flash \
    0x0     build/bootloader/bootloader.bin \
    0x8000  build/partition_table/partition-table.bin \
    0x10000 factory_dev-001.bin \
    0x20000 build/flower.bin
```

---

## 验证

烧写完成后，开启串口监控，正常启动应看到：

```
I (xxx) Flower: Device identity loaded from fctry: id=dev-001
I (xxx) Flower: MQTT connected, sub: flower/dev-001/down/cmd
```

如果 `fctry` 分区未烧写或读取失败，会回落到 Kconfig 配置：

```
W (xxx) Flower: fctry partition not found (ESP_ERR_NVS_NOT_FOUND), using Kconfig fallback
```

---

## 批量生产脚本示例

```bash
#!/bin/bash
# batch_provision.sh — 批量烧写多台设备
# 使用方式: ./batch_provision.sh COM3 dev-001 <secret>

PORT=$1
DEVICE_ID=$2
SECRET=$3

python gen_factory_nvs.py --id "$DEVICE_ID" --secret "$SECRET" --out "factory_${DEVICE_ID}.bin"
esptool.py -p "$PORT" write_flash 0x10000 "factory_${DEVICE_ID}.bin"
echo "Done: $DEVICE_ID -> $PORT"
```
