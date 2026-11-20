# HTTP 接口文档

固件 `ESPWROM32-4M-8xMOS` 对外的 HTTP 接口（端口 80）。网页、App、其他模块都以本文为准，不要靠变量名猜含义。
`mock/mock_device.py` 是这套接口的模拟器，字段和状态码保持一致；改接口时两边和本文一起改。

| 路径 | 作用 |
|---|---|
| `GET /` | 控制面板网页 |
| `GET /api/sensors` | 读取全部传感器、箱门、加热垫温控锁、8 路 MOS 状态 |
| `GET /change_relayN` | 开/关第 N 路 MOS（N = 1~8），返回 JSON 回执 |
| `GET /api/water/config` | 读取或修改自动浇水配置 |

## 约定

- 响应都是 UTF-8 JSON（`/` 除外）。
- **布尔量一律用整数 `0`/`1`**，不用 `true`/`false`。
- **传感器读取失败时，对应字段为 `null`**，不再用 -273、-99、-1 之类的特殊数值。客户端必须先判断 `null`。
- 字段名后缀说明单位：`_ts` = UTC 秒（`0` 表示从未发生），`_ms` = 毫秒，`_cnt` = 开机以来的累计次数（重启清零）。
- 传感器数据由固件每 2 秒采样一次并缓存，`/api/sensors` 只返回缓存，不会触碰总线，响应很快。轮询间隔 1 秒即可，更快拿到的还是同一份数据。

---

## GET /api/sensors

### 示例

```json
{
  "version": "1.5.0",
  "soil": {"moisture": 45.2, "raw": 2100, "temp": 23.50},
  "air": {"temp": 25.10, "humi": 60.2},
  "lux": 123.4,
  "door": {"closed": 1, "open_cnt": 3, "last_open_ts": 1767225600,
           "uvb_cut_ts": 0, "uvb_cut_lit_ms": 0},
  "heater_lock": 0,
  "strategy": 3,
  "db": {
    "ds":  {"streak": 0, "n": 1234, "fail": 2, "crc": 5, "nop": 0, "p85": 0},
    "bh":  {"n": 1234, "err": 0, "zero": 1},
    "aht": {"n": 1234, "fail": 0},
    "door": {"isr": 10}
  },
  "relays": [
    {"ch": 1, "device": "pump", "gpio": 19, "state": 0, "remaining_ms": 0,
     "max_ms": 180000, "last_on_ts": 0, "last_on_dur_ms": 0}
  ]
}
```

### 字段

| 字段 | 类型 | 含义 |
|---|---|---|
| `version` | string | 固件版本，如 `"1.5.0"` |
| `soil.moisture` | number | 土壤湿度，%，一位小数。由 `soil.raw` 按标定值（`raw_dry`/`raw_wet`，见 `/api/water/config`）换算，范围 0~100 |
| `soil.raw` | int | 土壤湿度传感器 ADC 原始值（GPIO34，0~4095）。值越大越干 |
| `soil.temp` | number \| null | 土壤温度，°C，DS18B20。读取失败为 `null`（偶发失败时固件会沿用上次有效值最多约 6 秒，之后才变 `null`） |
| `air.temp` | number \| null | 箱内空气温度，°C，AHT25。失败为 `null` |
| `air.humi` | number \| null | 箱内空气相对湿度，%，AHT25。失败为 `null` |
| `lux` | number \| null | 光照度，lx，BH1750。失败为 `null`。完全黑暗时是真实的 `0` |
| `door.closed` | int | 箱门状态，由干簧管（GPIO14）读出：`1` = 闭合（门关），`0` = 断开（门开）。这是**实时**电平，不是缓存 |
| `door.open_cnt` | int | 开门次数。经 50 ms 去抖，且 1.5 秒内重复开门只算一次。只统计"闭合→断开" |
| `door.last_open_ts` | int | 最近一次被计数的开门时间，UTC 秒，`0` = 从未 |
| `door.uvb_cut_ts` | int | 最近一次"开门导致 UVB 被断电"的时间，UTC 秒，`0` = 从未 |
| `door.uvb_cut_lit_ms` | int | 上述那次断电时，UVB 已经连续亮了多久，毫秒 |
| `heater_lock` | int | 加热垫温控锁：`1` = 此刻会拒绝开加热垫（土温 > 32 °C、箱温 > 35 °C，或土温和箱温都读不到），`0` = 允许 |
| `strategy` | int | 当前自动浇水策略：`0` 中午12点、`1` 浇表面、`2` 干了再浇透、`3` 关闭 |
| `relays` | array | 8 路 MOS，按通道 1~8 顺序，见下表 |
| `db` | object | 调试统计，见下文。**不保证稳定**，业务逻辑不要依赖 |

### `relays[]`

| 字段 | 类型 | 含义 |
|---|---|---|
| `ch` | int | 通道号 1~8 |
| `device` | string | 设备名：`pump` 水泵、`uvb` 紫外灯、`strobe` 爆闪灯、`heater` 加热垫、`fan` 风扇、`mist` 喷雾、`spare7`、`spare8` |
| `gpio` | int | 控制该通道的 GPIO |
| `state` | int | `1` = 正在输出，`0` = 关 |
| `remaining_ms` | int | 开启状态下离自动关闭还剩多久，毫秒；关闭时为 `0` |
| `max_ms` | int | 该通道单次最长运行时间，毫秒。请求更长的时长会被裁剪到这个值 |
| `last_on_ts` | int | 最近一次开启时间，UTC 秒，`0` = 从未 |
| `last_on_dur_ms` | int | 最近一次开启的实际时长设定，毫秒 |

各通道 `max_ms`：水泵 180000、UVB 180000、爆闪灯 30000、加热垫 3600000、风扇 3600000、喷雾 60000、备用 60000。

### `db`（调试统计）

开机以来的累计值，用来判断传感器、接线、供电有没有问题。网页把它们显示在对应传感器卡片旁。

| 字段 | 含义 |
|---|---|
| `db.ds.n` / `fail` | DS18B20 读取次数 / 最终没读到的次数 |
| `db.ds.crc` | scratchpad 读取被 CRC 校验拒绝的次数（含后来重读成功的）。持续增长说明总线质量差：上拉、线长、干扰 |
| `db.ds.nop` | 总线复位没收到存在脉冲的次数。增长说明接线松或传感器掉线 |
| `db.ds.p85` | 读到 85 °C 上电默认值的次数。增长说明转换没跑完，常见于供电不足 |
| `db.ds.streak` | 当前连续失败的采样周期数。`> 0` 且 `soil.temp` 仍有值，表示现在显示的是沿用的旧值 |
| `db.bh.n` / `err` | BH1750 读取次数 / I2C 读取报错次数 |
| `db.bh.zero` | 被识别为误报的 0 lx、并靠重读纠正的次数。有规律增长时留意电源毛刺或总线干扰 |
| `db.aht.n` / `fail` | AHT25 读取次数 / 失败次数 |
| `db.door.isr` | 干簧管中断触发总次数。含两个边沿和抖动，通常比 `door.open_cnt` 大 |

---

## GET /change_relayN

N = 1~8，对应通道号。始终返回 JSON 回执。

| 参数 | 含义 |
|---|---|
| `duration=<毫秒整数>` | 开启并持续这么久。缺省时用该通道上次的时长，没有则用 10000 |
| `action=off` | 立即关闭 |

超过 `max_ms` 的时长会被裁剪，不会报错。

### 回执

| 状态码 | 回执 |
|---|---|
| 200 开启 | `{"ok":true,"ch":1,"device":"pump","requested_ms":10000,"actual_ms":10000,"clamped":false}` |
| 200 被裁剪 | 同上但 `"clamped":true`，并多一个 `message` |
| 200 关闭 | `{"ok":true,"ch":1,"device":"pump","action":"off"}` |
| 403 被拒绝 | `{"ok":false,"ch":4,"device":"heater","reason":"over_temp","message":"..."}`，加热垫被拒时多 `soil_c`、`air_c`（读不到为 `null`） |
| 400 参数错 | `{"ok":false,"reason":"bad_duration","message":"..."}` |

`reason` 取值：`door_open`（门开着，拒绝开 UVB）、`over_temp`（超温，拒绝开加热垫）、`no_temp`（土温和箱温都读不到，拒绝开加热垫）、`bad_duration`。

> 回执里的 `ok`、`clamped` 仍是 JSON 布尔值（`true`/`false`），尚未改成整数；网页和 App 按布尔值解析。

安全规则（均在固件里强制执行，与调用方无关）：

- 门开着时 UVB 开不起来；UVB 亮着时开门，会立即断电并让爆闪灯闪 1 秒，且不会自动恢复。
- 土温 > 32 °C 或箱温 > 35 °C，或两路温度都读不到时，加热垫开不起来；加热垫已开时会被切断。

---

## GET /api/water/config

不带参数：返回当前自动浇水配置。带参数：修改并保存到 Flash，再返回保存后的配置。

```json
{"strategy":3,"strategy_name":"<策略名>",
 "surface_thresh_pct":<int>,"surface_dur_s":<int>,
 "deep_thresh_pct":<int>,"deep_dur_s":<int>,
 "raw_dry":<int>,"raw_wet":<int>,
 "check_window":"HH:00-HH:00","cooldown_s":<int>}
```

可修改的查询参数（可同时带多个）：`strategy`、`surface_thresh_pct`、`surface_dur_s`、`deep_thresh_pct`、`deep_dur_s`、`raw_dry`、`raw_wet`。`raw_dry` 必须大于 `raw_wet`。`check_window` 和 `cooldown_s` 只读。
取值范围和校验见 `main/water_logic.c`（`water_config_apply_query`）。参数不合法返回 400，保存失败返回 500，正文为纯文本原因。
