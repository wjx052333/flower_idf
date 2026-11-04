# AHT25 传感器集成与温湿度上报设计文档

## 1. 概述
在 `ESPWROM32-4M-8xMOS` 固件中增加 AHT25 数字温湿度传感器支持。AHT25 与现有的 BH1750 照度传感器复用同一组 I2C 总线（SCL=IO22, SDA=IO21）。
同时，在 MQTT 状态上报（`s_metrics`）中扩充两个字段（`aht_temp`、`aht_humi`），更新设备类型字符串为 `"HT_reed_AHT25_8Xmos"`，升级固件版本为 `1.2.0`（`FW_VERSION_MINOR=2`，`FW_VERSION_PATCH=0`），并在本地 `/api/sensors` JSON 接口与 Web 管理页面卡片中呈现 AHT25 读数。

## 2. 硬件与 I2C 架构设计

### 2.1 模块与文件引入
- 从 `ESPWROM32-4M-8xMOS-vivarium` 复用成熟的 `aht25.h` 与 `aht25.c`，复制至 `ESPWROM32-4M-8xMOS/main/` 目录。
- 在 `ESPWROM32-4M-8xMOS/main/CMakeLists.txt` 的 `SRCS` 中追加 `"aht25.c"`。

### 2.2 I2C 总线复用与无用宏清理
- 移除不再需要的 `USE_BH1750` 条件宏及旧的 Modbus 串口冗余分支，默认直接使用 I2C 架构。
- I2C 参数：
  - 端口：`I2C_NUM_0`
  - SCL：`GPIO_NUM_22`
  - SDA：`GPIO_NUM_21`
  - 内部上拉：使能（`flags.enable_internal_pullup = true`）
  - 总线挂载两个从机设备：
    - BH1750 照度计：I2C 地址 `0x23`
    - AHT25 温湿度：I2C 地址 `0x38`
- 初始化流程：
  - `i2c_new_master_bus` 创建总线。
  - 添加并初始化 BH1750 设备。
  - 调用 `aht25_init(s_i2c_bus)` 初始化 AHT25 并执行上电标定状态检测。

### 2.3 采样互斥保护
- 所有对 I2C 总线的读取（BH1750 与 AHT25）、ADC 及 DS18B20 采样均统一受 `g_sensor_mutex` 互斥保护，采样超时设为 3000ms。

## 3. 固件版本与 MQTT 状态上报设计

### 3.1 固件版本宏
```c
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 2
#define FW_VERSION_PATCH 0
```
`FW_VERSION_STR` 将自动计算并展开为 `"1.2.0"`。

### 3.2 设备类型标识
在 `publish_status_report()` 中：
```c
strcpy(sr.device_type, "HT_reed_AHT25_8Xmos");
```

### 3.3 `s_metrics` 数组与编码
`METRICS_COUNT` 由 6 扩展至 8：
```c
#define METRICS_COUNT 8
```
字段分配及映射：
1. `s_metrics[0]`：`"humidity"`（土壤湿度百分比，ADC1_CH6）
2. `s_metrics[1]`：`"temperature"`（DS18B20 测温，摄氏度）
3. `s_metrics[2]`：`"lux"`（BH1750 照度值，Lux）
4. `s_metrics[3]`：`"reed"`（干簧管开闭状态，0=打开，1=闭合）
5. `s_metrics[4]`：`"mosState"`（当前 8 路 MOS 输出位掩码）
6. `s_metrics[5]`：`"mosPrevState"`（上一次 8 路 MOS 输出位掩码）
7. `s_metrics[6]`：`"aht_temp"`（AHT25 温度，`FLOWER_METRIC_DOUBLE_VALUE_TAG`，异常时为 0.0）
8. `s_metrics[7]`：`"aht_humi"`（AHT25 湿度，`FLOWER_METRIC_DOUBLE_VALUE_TAG`，异常时为 0.0）

## 4. Web 管理界面与 `/api/sensors` 接口

### 4.1 `/api/sensors` JSON 响应
在输出 JSON 中追加：
- `"aht_temp": %.2f`
- `"aht_humi": %.1f`
（若读取失败则输出 -99.0 表示不可用）

### 4.2 Web 页面（`STATUS_HTML`）
- 在 `.vals` 容器中追加两张传感器展示卡片：
  - `<div class="val"><div class="num" id="v-aht-temp">--</div><div class="lbl">AHT25温度 (°C)</div></div>`
  - `<div class="val"><div class="num" id="v-aht-humi">--</div><div class="lbl">AHT25湿度 (%)</div></div>`
- 客户端 JavaScript 轮询时解析并刷新：
  - `v-aht-temp`：数值正常时保留一位小数，异常时显示 `ERR`。
  - `v-aht-humi`：数值正常时保留一位小数，异常时显示 `ERR`。

## 5. 测试与验证策略

1. **静态语法与代码结构校验**：
   - 检查 `CMakeLists.txt` 注册项完整性。
   - 检查 `aht25.c` / `aht25.h` 头文件引用与类型定义。
2. **HTML / JS 语法与 DOM 提取验证**：
   - 更新 `ESPWROM32-4M-8xMOS/test/test_html.js`。
   - 校验 HTML 标签平衡性（无未闭合标签）。
   - 校验内联 JS 的 V8 语法编译正确性。
   - 校验 JS 所引用的 `v-aht-temp` 和 `v-aht-humi` DOM 元素存在性。
   - 校验 `aht_temp`、`aht_humi`、`HT_reed_AHT25_8Xmos`、`FW_VERSION_MINOR 2` 等特征模式。
   - 执行 `node ESPWROM32-4M-8xMOS/test/test_html.js` 进行离线自测。
