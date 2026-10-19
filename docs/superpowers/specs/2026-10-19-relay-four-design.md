# ESP32-C3 四路继电器控制器 (relay_four) 设计方案

日期：2026-10-19  
范围：`relay_four`（基于 ESP32-C3 的四路继电器控制器）  
状态：已与用户确认，待实施  

## 1. 背景与目标

为了支持多路外设独立供电与水阀/设备控制，在当前工程目录下新增独立的子工程 `relay_four`。

### 1.1 核心需求与对比
- **芯片平台**：ESP32-C3（4MB Flash）。
- **硬件功能**：
  - 4 路独立继电器输出：GPIO 3, 4, 5, 7。
  - 4 路物理按键输入：GPIO 10, 9, 6, 8，分别对应控制 Relay 1~4 的开关翻转。
  - 1 路状态指示灯：GPIO 2，使用 LEDC 实现呼吸灯效果。
- **与 `relay_v2` 的关系**：
  - 继承 `relay_v2` 的系统框架、分区表配置（双 OTA 分区）、MQTT 连接鉴权机制（HMAC-SHA256、MQTT 5.0、User Property）。
  - 继承 Web 服务基础架构，但移除全部温湿度（DS18B20/ADC）与照度（BH1750/Modbus）传感器读取逻辑及曲线图。
  - 目前所有通道均按“无自动策略”执行，仅响应外部指令与物理按键触发。

---

## 2. 硬件引脚映射与电气特性

| 模块 | 通道 | ESP32-C3 引脚 | 模式与电平 | 功能说明 |
| :--- | :--- | :--- | :--- | :--- |
| **Relay** | Relay 1 | GPIO 3 | GPIO_MODE_INPUT_OUTPUT | 默认输出低电平（常闭/吸合由电路决定，1=吸合） |
| | Relay 2 | GPIO 4 | GPIO_MODE_INPUT_OUTPUT | 默认输出低电平 |
| | Relay 3 | GPIO 5 | GPIO_MODE_INPUT_OUTPUT | 默认输出低电平 |
| | Relay 4 | GPIO 7 | GPIO_MODE_INPUT_OUTPUT | 默认输出低电平 |
| **Button**| Button 1 | GPIO 10 | GPIO_MODE_INPUT, 上拉 | 按下接地（低电平），边沿触发翻转 Relay 1 |
| | Button 2 | GPIO 9 | GPIO_MODE_INPUT, 上拉 | 按下接地（低电平），边沿触发翻转 Relay 2 |
| | Button 3 | GPIO 6 | GPIO_MODE_INPUT, 上拉 | 按下接地（低电平），边沿触发翻转 Relay 3 |
| | Button 4 | GPIO 8 | GPIO_MODE_INPUT, 上拉 | 按下接地（低电平），边沿触发翻转 Relay 4 |
| **LED** | 呼吸灯 | GPIO 2 | LEDC Channel 0 | 1000Hz PWM，正弦波呼吸指示灯 |

---

## 3. 软件架构设计

### 3.1 继电器通道统一管理
在 `main.c` 中定义通道抽象结构体并声明 4 个通道组成的数组：

```c
typedef struct {
    gpio_num_t       relay_gpio;        /* 继电器控制引脚 */
    gpio_num_t       button_gpio;       /* 对应物理按键引脚 */
    volatile bool    on;                /* 当前导通状态 */
    volatile int64_t on_us;             /* 吸合时刻 (esp_timer_get_time 微秒) */
    volatile uint32_t duration_ms;      /* 自动断开倒计时(ms)；0 为无倒计时(常开) */
    volatile int64_t last_on_utc_s;     /* 最后一次吸合的 UTC 时间戳 (秒) */
    volatile uint32_t last_on_dur_ms;   /* 最后一次设定的吸合持续时间 (ms) */
    bool             last_button_state; /* 上一次按键采样电平，用于去抖/边沿检测 */
} relay_channel_t;

static relay_channel_t s_relays[4];
```

### 3.2 控制 API 抽象
- `void relay_set(int ch, bool on, uint32_t duration_ms)`:
  - 设置指定通道的 GPIO 输出电平。
  - 更新 `on`、`on_us`、`duration_ms`。
  - 若 `on == true`，更新 `last_on_utc_s`（获取 NTP/系统时间）及 `last_on_dur_ms`。
- `void relay_toggle(int ch)`:
  - 翻转当前通道电平，默认采用前次设置的有效持续时间（或默认 10000ms）。
- `void relay_timer_tick(void)`:
  - 由 50ms 周期定时器（`app_timer_cb`）调用。
  - 检查 4 个通道的自动关闭逻辑：若 `on == true && duration_ms > 0`，计算经过时间，达到阈值时拉低 GPIO 并置 `on = false`。
  - 扫描 4 个按键引脚，检测下降沿并调用 `relay_toggle(ch)`。

---

## 4. Web 服务与前端交互

### 4.1 HTTP 端点设计
1. **控制端点**（独立路由）：
   - `GET /change_relay1`
   - `GET /change_relay2`
   - `GET /change_relay3`
   - `GET /change_relay4`
   - **查询参数**：
     - `duration`（可选）：指定开启动作持续时长（毫秒），如 `?duration=15000`。
     - `action`（可选）：`off` 表示强制关闭；未提供或为 `on` 时表示开启。
   - **返回**：HTTP 200，纯文本 `"0"` 表示成功。

2. **状态端点**：
   - `GET /api/sensors`（沿用既有路径以保持兼容）：
   - 返回 `application/json`：
     ```json
     {
       "relays": [
         {"ch": 1, "gpio": 3, "state": 0, "last_on_ts": 1713500000, "last_on_dur_ms": 10000},
         {"ch": 2, "gpio": 4, "state": 0, "last_on_ts": 0, "last_on_dur_ms": 0},
         {"ch": 3, "gpio": 5, "state": 0, "last_on_ts": 0, "last_on_dur_ms": 0},
         {"ch": 4, "gpio": 7, "state": 0, "last_on_ts": 0, "last_on_dur_ms": 0}
       ],
       "relay_last_on_ts": 1713500000,
       "relay_last_on_dur_ms": 10000
     }
     ```

3. **页面端点**：
   - `GET /`：内置 HTML 静态页面。
   - 布局：包含 4 个深色卡片（Relay 1 ~ 4），各卡片展示：
     - 状态徽章（在线/离线、开启/关闭）；
     - 最后触发时间与持续时长；
     - 开启操作输入框（设定秒数）与【开启】、【关闭】操作按钮。
   - 前端定时（每 2s）通过 `fetch('/api/sensors')` 无刷新更新状态。

---

## 5. MQTT 通信与数据上报

### 5.1 连接鉴权与 Topic
- **NVS 凭据加载**：从 `fctry` 分区读取 `device_id` 和 `device_secret`。
- **HMAC 鉴权**：连接前生成 `clientId{id}timestamp{ms}` 的 HMAC-SHA256 签名作为密码。
- **MQTT 5.0**：启用 User Property `{"project", "flower"}`。
- **Topic**：
  - 下行指令：`flower/{DEVICE_ID}/down/cmd`
  - 上行状态：`flower/{DEVICE_ID}/up/status`
  - 指令响应：`flower/{DEVICE_ID}/up/cmd_response`

### 5.2 下行控制响应
- 收到 `Command` 且载荷为 `flower_relay_control_t` 时：
  - 因 Proto 中无 channel 字段，**默认控制第 1 路继电器（GPIO 3）**。
  - 按指定 `on` 与 `duration_ms` 控制 Relay 1。
  - 返回 `CommandResponse`（载荷 `relay_control`），结果置为 `FLOWER_CMD_RESULT_OK`。

### 5.3 上行 StatusReport 结构 (`metrics` 方案 C)
- `device_type` 设定为 `"C3_relay_four"`。
- 上报周期：30 秒。
- `metrics` 回调包含 12 项标量度量指标（类型均为 `FLOWER_METRIC_INT64_VALUE_TAG`）：
  - 实时状态：`relay1State`, `relay2State`, `relay3State`, `relay4State` (1=吸合, 0=断开)
  - 触发时间戳：`relay1LastOnTs`, `relay2LastOnTs`, `relay3LastOnTs`, `relay4LastOnTs` (UTC 秒)
  - 设定时长：`relay1LastOnDur`, `relay2LastOnDur`, `relay3LastOnDur`, `relay4LastOnDur` (毫秒)

---

## 6. 工程结构与配置

```
relay_four/
├── CMakeLists.txt              # 项目级 CMake 文件 (project(flower))
├── partitions.csv              # 分区表配置 (nvs, phy_init, fctry, otadata, ota_0, ota_1)
├── sdkconfig.defaults          # 基础配置项 (Flash 4MB, custom partition, MQTT 5.0, TLS)
└── main/
    ├── CMakeLists.txt          # 组件注册 (移除 sensor 依赖)
    ├── Kconfig.projbuild       # 默认 WiFi/MQTT 配置项
    └── main.c                  # 主程序
```

- **依赖项清理**：
  - 剔除 `ds18b20.c` / `ds18b20.h`。
  - 剔除 `water_logic.c` / `water_logic.h` 及 `water_store.c` / `water_store.h`。
  - 移除对 ADC 驱动、I2C 驱动、Modbus UART 的硬件初始化代码。
