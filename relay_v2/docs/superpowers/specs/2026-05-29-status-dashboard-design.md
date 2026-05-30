# /status 传感器实时图表设计

## 概述

在 ESP32-C3 固件（relay_v2）的 HTTP 服务器上新增两条路由，实现浏览器直连设备查看传感器实时时序图。

## 新增路由

| 路由 | 方法 | 返回 | 说明 |
|---|---|---|---|
| `/status` | GET | `text/html` | 内嵌 HTML 页面，含 Chart.js 图表 |
| `/api/sensors` | GET | `application/json` | 当前传感器读数 |

## 数据流

```
GET /api/sensors
    └─ 取 g_sensor_mutex（timeout 2000ms）
    └─ 顺序读三个传感器（阻塞 ~800ms）
    └─ 释放 g_sensor_mutex
    └─ 返回 JSON

status_timer callback（30s MQTT 上报，已有）
    └─ 取 g_sensor_mutex（timeout 2000ms）
    └─ 读传感器（已有逻辑不变）
    └─ 释放 g_sensor_mutex
    └─ 发布 MQTT（已有逻辑不变）

GET /status
    └─ 返回内嵌 HTML 字符串（存 flash）
```

## 并发控制

新增全局 `SemaphoreHandle_t g_sensor_mutex`，在 `app_main()` 中 `xSemaphoreCreateMutex()` 初始化。

保护范围：DS18B20 GPIO bit-bang 和 Modbus UART 读取（非线程安全）。ADC 读取虽然安全，为一致性也纳入锁范围。

## JSON 格式

```json
{
  "humidity": 45.2,
  "temperature": 24.1,
  "lux": 1234.5,
  "relay_last_on_ts": 1748499000,
  "relay_last_on_dur_ms": 5000
}
```

- `humidity`：0–100，百分比
- `temperature`：摄氏度，DS18B20 错误时为 -273.0
- `lux`：勒克斯，Modbus 原始值 / 1000.0
- `relay_last_on_ts`：Unix 时间戳（秒），浏览器端本地化显示
- `relay_last_on_dur_ms`：毫秒，继电器最后一次持续时长

## 前端页面设计

### 依赖

- Chart.js 4.x，从 BootCDN 加载：
  `https://cdn.bootcdn.net/ajax/libs/Chart.js/4.4.1/chart.umd.min.js`

### URL 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `interval` | `2000` | 轮询间隔（ms） |
| `points` | `120` | 图表最大保留点数（滚动窗口） |

### 布局

1. 顶部文字区：继电器最后触发时间（浏览器本地时区）+ 持续时长
2. 三张折线图（各自独立）：
   - 湿度 (%)
   - 温度 (°C)
   - 照度 (lx)
3. 横轴：页面打开后的相对时间（HH:MM:SS）
4. 状态栏：轮询正常 / 错误提示

### 行为

- 页面加载后立即发起第一次轮询
- 每次成功拿到数据：append 到各图 dataset；超过 `points` 上限后移除最旧点
- 轮询出错：图表暂停，状态栏显示错误信息，继续重试
- 时间轴用浏览器端 `Date.now()` 记录（不依赖设备时钟）
- `relay_last_on_ts` 用 `new Date(ts * 1000).toLocaleString()` 渲染

## main.c 改动清单

1. 新增全局 `SemaphoreHandle_t g_sensor_mutex`
2. `app_main()` 中初始化 mutex
3. `status_timer` callback 读传感器部分加 take/give
4. 新增 `handle_api_sensors()` 函数
5. 新增 `handle_status()` 函数（返回 HTML 字符串常量）
6. `http_server_start()` 注册两条新 `httpd_uri_t`

HTML 字符串以 `static const char` 存在 flash，不占用 DRAM。
