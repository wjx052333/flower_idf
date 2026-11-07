# ESPWROM32-4M-8xMOS-web

ESPWROM32-4M-8xMOS 固件对外接口（HTTP + UDP 设备发现）的 Python 模拟器，用于没有硬件时开发/调试 App 和网页。不含 MQTT。

## 运行

只用 Python 标准库（Python 3.8+），无需安装依赖。

```
cd ESPWROM32-4M-8xMOS-web
python mock_device.py [--port 8080] [--device-id mock001] [--no-discovery]
```

- 设备页：`http://localhost:8080/`（固件自带控制面板）
- 调试页：`http://localhost:8080/debug`（注入传感器、开关箱门、模拟 BOOT 键）
- 其他参数：`--host`、`--advertise-ip`（发现包里的 IP，默认自动）、`--firmware-src`（main.c 路径）、`--state-file`（默认 `mock_state.json`）
- 设备真实端口是 80，模拟器默认 8080（避免要管理员权限）。
- 发现包会在局域网广播，App 会把模拟器当成一台真设备；不需要时加 `--no-discovery`。同机多开时只让一个实例开发现，其余加 `--no-discovery`。

## 文件

- `mock_device.py`：全部逻辑（设备状态、HTTP、UDP 发现、定时任务）
- `debug.html`：调试页，启动时读入；`/debug/api/*` 是模拟器专用接口，固件里不存在
- `mock_state.json`：运行时生成，保存注入的传感器值和浇水配置（相当于 NVS），重启保留；删除即恢复默认

## 与固件代码的关系

模拟器是对 `../ESPWROM32-4M-8xMOS/main/` 的手工移植，**不会自动同步**。对应关系：

| 模拟器 | 固件来源 |
|---|---|
| `/` 页面 | 运行时用正则从 `main.c` 的 `STATUS_HTML` 提取，自动跟随固件 |
| `/change_relayN`、`/api/sensors` | `main.c`：`handle_change_relay_ch`、`handle_api_sensors` |
| `/api/water/config`、自动浇水 | `water_logic.c/.h`、`check_auto_water` |
| UDP 3333 发现 | `components/device_discovery/device_discovery.c` |
| 继电器安全逻辑（UVB 门磁互锁、开门断电+爆闪、加热垫温控锁、最长时长裁剪） | `main.c`：`relay_set_unlocked`、`reed_*`、`sensor_task`、`app_timer_cb` |

固件改动后需要手工同步的常量（在 `mock_device.py` 顶部）：`FW_VERSION`、`RELAY_MAX_MS`、`RELAY_DEVICE`/`RELAY_NAME_CN`/`RELAY_VERB_CN`、`HEATER_*_MAX_C`、`WATER_*`、`DEF_RAW_*`、`STRATEGY_NAMES`、`DEFAULT_WATER_CFG`。
新增/修改 HTTP 接口或 JSON 字段时，同步改对应 handler，并保持 JSON 字段名、状态码、错误文案与固件一致。

## 与真机的差异

- 传感器值由调试页注入，不是真实采样；`/api/sensors` 立即反映注入值（固件有 2 秒缓存周期，加热垫温控锁按 2 秒周期检查）。
- 箱门状态由调试页直接设置，没有 50 ms 去抖；只对"闭合→断开"计数（对应固件的上升沿中断）。
- 板载 LED、ADC、I2C、NVS、OTA、MQTT 均未模拟。
- 浇水时间判断用本机时间换算到 UTC+8。
