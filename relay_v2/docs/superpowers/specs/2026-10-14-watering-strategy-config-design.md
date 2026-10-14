# 可配置浇水策略设计

日期：2026-10-14
范围：`relay_v2`（ESP32-S3 花卉继电器控制器）
状态：已与用户确认，待实施

## 背景

`relay_v2/main/main.c` 目前的自动浇水逻辑（commit `76e9127` 引入）是硬编码的：

- 每天 12:00 检查一次湿度
- 按阶梯表决定浇水时长：`>80%→6min`、`70-80%→8min`、`60-70%→10min`、`50-60%→15min`、`<50%→20min`
- 所有阈值和时长都是 `#define` / 函数内字面量，改一次要重新编译烧录
- web 页面（`STATUS_HTML`）里只有一段说明文字，无法修改任何参数

需要把它变成三种可在 web 上切换、参数可改的策略。

## 目标

1. 三种浇水策略，web 上可切换，web 上能看到当前生效的策略
2. 策略 2、3 的阈值与时长可在 web 上修改，改完立即生效
3. 配置断电重启后保留
4. 策略 1（中午阶梯浇水）行为不变，参数保持只读

## 非目标

- 不修改策略 1 的阶梯表与 12:00 时间点（用户明确要求保持只读）
- 不改动 MQTT 继电器控制、OTA、状态上报协议
- 不改动手动按钮 / `/change_relay1` 的行为

## 三种策略定义

| ID | 名称 | 触发条件 | 时长 |
|----|------|----------|------|
| 0 | 中午12点（NOON） | 每天 12:00 检查一次，湿度落在阶梯区间 | 阶梯表（只读） |
| 1 | 浇表面（SURFACE） | 湿度 < 阈值（默认 50%） | 固定时长（默认 60 秒） |
| 2 | 干了再浇透（DEEP） | 湿度 < 阈值（默认 10%） | 固定时长（默认 1200 秒） |

策略 1、2 为持续监测型，检测窗口与冷却见下节。

## 架构

### 数据模型

```c
typedef enum {
    WATER_STRATEGY_NOON    = 0,
    WATER_STRATEGY_SURFACE = 1,
    WATER_STRATEGY_DEEP    = 2,
} water_strategy_t;

typedef struct {
    uint8_t  strategy;            /* water_strategy_t，默认 0 */
    uint16_t surface_thresh_pct;  /* 默认 50   */
    uint16_t surface_dur_s;       /* 默认 60   */
    uint16_t deep_thresh_pct;     /* 默认 10   */
    uint16_t deep_dur_s;          /* 默认 1200 */
} water_config_t;
```

时长用秒（`uint16_t` 上限 65535 秒 ≈ 18.2 小时，足够）。阈值用整数百分比（湿度读数精度不需要小数）。

### 持久化

NVS 默认分区（`app_main` 已调用 `nvs_flash_init()`），namespace `"water"`：

| 键 | 类型 | 对应字段 |
|----|------|----------|
| `strategy` | u8  | `strategy` |
| `s_thresh` | u16 | `surface_thresh_pct` |
| `s_dur`    | u16 | `surface_dur_s` |
| `d_thresh` | u16 | `deep_thresh_pct` |
| `d_dur`    | u16 | `deep_dur_s` |

开机 `water_config_load()`：任一键读取失败则该字段保持默认值（逐字段读，不做全有全无）。web 保存时 `water_config_save()` 写入全部 5 个键并 `nvs_commit`。

### 文件划分

新增：

- `relay_v2/main/water_logic.c` / `water_logic.h` —— 纯逻辑，**无 ESP-IDF 依赖**（只用 libc 的 `time.h` / `stdio.h` / `string.h` / `stdlib.h`），因此可在 PC 上用 gcc 编译测试。内容：
  - `water_config_t` 与 `water_strategy_t`
  - `water_config_t *water_config(void)` —— 返回当前配置指针
  - `void water_config_reset(void)` —— 恢复默认值
  - `bool water_config_apply_query(const char *query, char *err, size_t errlen)` —— 解析 `k=v&k=v` 形式的 query，逐项校验并更新；任一项非法则整体不生效并写 `err`
  - `int water_config_to_json(char *buf, size_t n)` —— 序列化为 JSON（含 `strategy_name`、`check_window`、`cooldown_s`）
  - `water_decision_t water_strategy_eval(time_t now, float humidity)` —— 纯决策函数，返回 `{bool water; uint32_t duration_ms;}`，内部维护「当天已触发」与「上次自动浇水时间戳」两个静态状态
  - `bool water_humidity_valid(float humidity)` —— 读数是否落在 [0,100]，用于识别故障传感器

- `relay_v2/main/water_store.c` / `water_store.h` —— 仅 NVS 读写：
  - `void water_store_load(void)`
  - `void water_store_save(void)`

- `relay_v2/test/test_water.c` —— PC 侧断言测试，只编译 `water_logic.c`

修改：

- `relay_v2/main/main.c`
  - 删除 `auto_water_duration_ms()`，`check_auto_water()` 改为调用 `water_strategy_eval()` 并执行继电器动作
  - 新增 httpd handler `handle_api_water_config_get` / `handle_api_water_config_set`，注册 `/api/water/config`
  - `/api/sensors` JSON 增加 `"strategy"` 字段
  - `STATUS_HTML` 增加策略配置卡片
  - `app_main` 中调用 `water_store_load()`
- `relay_v2/main/CMakeLists.txt` —— `SRCS` 增加 `water_logic.c`、`water_store.c`

这样 `main.c` 只保留 GPIO 动作、HTTP 注册和 HTML，逻辑与持久化各自独立、可单独理解。

### 决策流程

`check_auto_water()` 由 `status_timer_cb` 每 30 秒调用一次。改造后：

```
time(&now)
humidity = 读 ADC（持 g_sensor_mutex）
if (!water_humidity_valid(humidity)) 仅记日志跳过      // 传感器故障
d = water_strategy_eval(now, humidity)                 // 内部含 NTP 保护
if (!d.water) return
if (s_relay_on) 仅记日志跳过
开继电器(d.duration_ms)，记录 s_last_on_utc_s / s_last_on_dur_ms
```

**NTP 保护（新增）**：`status_timer` 在 `app_main` 等待 NTP 同步之前就已启动，同步完成前 `time()` 返回 1970-01-01 附近的值。该时刻 UTC 00:00 在 `CST-8` 下正好是本地 08:00，会落进策略 2/3 的检测窗口，导致用未同步的时间去触发浇水。加 `now < 1700000000`（2023-11-14）门槛即可避免。策略 1 因为要求 `tm_hour==12` 不受影响，但一并受保护。

这个门槛放在 `water_strategy_eval()` 内部而不是 `check_auto_water()` 里，这样它能被主机端测试覆盖（见测试第 1 条）。

**时区**：`app_main` 已执行 `setenv("TZ", "CST-8", 1); tzset();`，`localtime_r()` 返回北京时间，窗口判断直接用本地时间，无需额外换算。

`water_strategy_eval()` 内部规则：

- **NOON**：`t.tm_hour == 12 && t.tm_min == 0` 且当天未触发 → 按阶梯表算时长返回。当天标记在 `t.tm_hour == 0 && t.tm_min == 0` 时复位（沿用现有做法）。
- **SURFACE / DEEP**：仅当 `8 <= t.tm_hour && t.tm_hour < 16`（本地时间）**且** `now - s_last_auto_water_s >= 1800`（30 分钟冷却）→ 若 `humidity < 阈值` 则返回 `{true, 配置时长}`，同时更新 `s_last_auto_water_s`。冷却时间戳跨策略共享，切换策略不清空。
- 冷却时间戳仅存内存，不持久化：重启后冷却清零。

**冷却在「决定浇水」时就消费，而不是在实际开继电器时**：如果 `water_strategy_eval()` 判定要浇但 `main.c` 因为继电器已被手动/MQTT 打开而跳过动作，冷却计时仍然从判定时刻起算。这样手动浇水与自动浇水重叠时不会在手动浇水结束后立刻补浇一次。策略 1 的「当天已触发」标记同理（现有代码已是此行为）。

检测窗口（08:00–16:00）与冷却（30 分钟）写死为 `#define`，不作为可配置项。

### HTTP API

**`GET /api/water/config`** —— 返回当前配置：

```json
{"strategy":1,"strategy_name":"浇表面","surface_thresh_pct":50,"surface_dur_s":60,
 "deep_thresh_pct":10,"deep_dur_s":1200,"check_window":"08:00-16:00","cooldown_s":1800}
```

**`GET /api/water/config?strategy=2&surface_thresh_pct=45&surface_dur_s=90`** —— 更新配置：

- 只更新 query 中出现的字段，未出现的保持不变
- 校验：`strategy` ∈ [0,2]；阈值 ∈ [0,100]；时长 ∈ [1,65535]。任一项非法 → 整体不生效，返回 HTTP 400 + 错误说明文本
- 合法 → 写 NVS → 返回保存后的完整 JSON（与 GET 同格式）

沿用现有 `/change_relay1?duration=` 的 GET + query 风格，与既有代码一致，设备在局域网内使用。

**`GET /api/sensors`** —— 现有 JSON 增加 `"strategy"` 字段（当前策略 ID），供页面刷新「当前策略」显示。

### Web UI

在现有页面（`STATUS_HTML`）继电器卡片下方新增「自动浇水策略」卡片：

```
┌ 自动浇水策略 ─────────────────────────────┐
│ 当前：浇表面（湿度 < 50% → 浇 60 秒）      │
│ ( ) 中午12点  (•) 浇表面  ( ) 干了再浇透   │
│ 浇表面：     湿度 < [ 50] % → 浇 [  60] 秒 │
│ 干了再浇透： 湿度 < [ 10] % → 浇 [1200] 秒 │
│ [保存]  已保存                              │
└────────────────────────────────────────────┘
```

- 三个单选按钮切换策略，选中项高亮
- 策略 1 的参数以只读文字展示阶梯表说明，不做输入框
- 「保存」调用 `/api/water/config?` 拼接当前表单值，成功后回读 `/api/water/config` 回填并显示「已保存」；失败显示设备返回的错误
- 「当前策略」文字由 `poll()` 中 `/api/sensors` 返回的 `strategy` 字段驱动，保证显示的是设备真实状态
- 页面加载时先 `GET /api/water/config` 回填表单

## 错误处理

| 情况 | 行为 |
|------|------|
| NVS 读取失败 / 首次开机 | 该字段用默认值，记 `ESP_LOGW`，不阻塞启动 |
| NVS 写入失败 | 内存中配置仍生效，返回 HTTP 500 + 错误文本 |
| query 参数非法 | 整体不生效，HTTP 400 + 具体哪个字段越界 |
| NTP 未同步 | 跳过本轮自动浇水判断 |
| 湿度读数落在 [0,100] 之外 | 视为传感器故障，跳过本轮自动浇水，记 `ESP_LOGW` |
| 自动浇水触发时继电器已 ON | 跳过本轮，记 `ESP_LOGW`（沿用现有行为） |
| 取传感器互斥锁超时 | 跳过本轮（HTTP 接口返回 503），记 `ESP_LOGW` |

### 湿度读数合理性检查

`get_humidity()` 把 ADC 原始值线性映射到 30%~98% 的窗口，窗口之外会产生不合理的结果：

| 传感器状态 | 算出湿度 | 若不检查的后果 |
|-----------|---------|---------------|
| 悬空 / 读数偏高 | 约 -2.9% | 永远「很干」，每 30 分钟浇一次，无限持续 |
| 短路 / 读数偏低 | 约 +144% | 安全，不浇 |

旧逻辑只在 12:00 判断一次，影响有限；新逻辑每个 tick 都判断，若用户选了「干了再浇透」（阈值 10%、时长 20 分钟、冷却 30 分钟），卡死的传感器会让设备约 2/3 的时间都在浇水。

因此在 `check_auto_water()` 里、调用 `water_strategy_eval()` 之前，先用 `water_humidity_valid()`（定义在 `water_logic.c`，可被主机端测试覆盖）过滤读数；超范围则跳过本轮并记日志。

`publish_status_report()` 与 `/api/sensors` **不**过滤，照原样上报原始值——那正是用户发现传感器坏掉的途径。

### 互斥锁获取失败的处理

`check_auto_water()`、`publish_status_report()`、`handle_api_sensors()` 三处取 `g_sensor_mutex` 时必须检查返回值：超时（3 秒）就直接返回，不能继续读传感器、更不能 `xSemaphoreGive()` 一个自己没持有的锁——FreeRTOS 下那会释放别的任务仍持有的互斥锁，导致两个任务同时持锁，而这条路径上的 DS18B20 读取不可重入。超时时间用共享常量 `SENSOR_MUTEX_TIMEOUT_MS`。

## 测试

新增 `relay_v2/test/test_water.c`，用 gcc 在 PC 上编译 `water_logic.c` 运行断言，覆盖：

1. 时间未同步（`now = 0`）→ 不浇水
2. 策略 2/3 窗口边界：本地 07:59 不浇、08:00 浇、15:59 浇、16:00 不浇
3. 冷却：触发后 29 分钟不浇、30 分钟后可再浇
4. 冷却跨策略共享：策略 2 触发后切到策略 3，冷却期内不浇
5. 策略 1：12:00 触发一次，当天 12:00 再次调用不重复触发；次日 00:00 后复位
6. 判定要浇时冷却立即消费：返回 `water=true` 后立刻再次调用返回 `water=false`
7. 湿度等于阈值 → 不浇（严格小于）
8. query 解析：合法值生效；越界值（strategy=3、阈值=101、时长=0）整体不生效并返回错误
9. JSON 序列化字段完整
10. 湿度合理性：0/50/100 有效；-0.1/100.1 越界无效；-2.9（悬空传感器）与 144.1（短路传感器）无效

NVS 持久化与 HTTP 接口在设备上手工验证：改配置 → 重启 → 配置仍在。

## 实施顺序

1. `water_logic.h/c` + PC 测试（先写测试）
2. `water_store.c/h`
3. 接入 `main.c`：`check_auto_water()` 改造 + NTP 保护
4. HTTP handler + `/api/sensors` 字段
5. `STATUS_HTML` 策略卡片
6. `CMakeLists.txt` 更新，编译验证
