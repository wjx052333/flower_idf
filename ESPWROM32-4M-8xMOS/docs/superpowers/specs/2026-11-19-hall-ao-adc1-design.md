# 49E 线性霍尔传感器 AO 接入 GPIO 36 与折线图调试渲染设计规范

## 1. 需求与目标
- **硬件接入**：将 49E 线性霍尔传感器的模拟量输出（AO）接入 ESP32 的 `GPIO 36`（`SENSOR_VP` / 原生 `ADC1_CH0`）。
- **信号采集**：利用 ESP32 ADC1 在 `ADC_ATTEN_DB_12`（0~3.3V）量程下周期性采集 0~4095 原始计数值，独立于 Wi-Fi 射频无冲突。
- **数据流通**：将霍尔传感器原始计数值纳入 `sensor_cache_t` 并在 `/api/sensors` JSON 接口中增加 `hall_raw` 字段。
- **界面与图表呈现**：
  - 在 Web 面板的“传感器实时数据”卡片区增加“霍尔传感器 [IO36]”，实时显示原始 AD 计数值；
  - 仿照湿度、温度、照度折线图，在界面新增第 4 个折线图 `<canvas id="c-hall">`，绘制 0~4095 AD 计数曲线（点数与轮询间隔与整体一致），方便实时调试磁铁靠近远离过程中的零点和极性。
- **硬件文档同步**：同步更新 `docs/hardware_specification.md`。

---

## 2. 硬件引脚分配规范

| 引脚编号 | 硬件连接 | 功能与特性 | 说明 |
| :--- | :--- | :--- | :--- |
| **GPIO 14** | 49E 模块 **DO** 引脚 | 数字开关量输入（内部弱上拉，支持边沿中断） | 门禁/磁吸开合触发，保持原有干簧管安全联锁与毫秒级关断逻辑不变 |
| **GPIO 36 (VP)** | 49E 模块 **AO** 引脚 | **ADC1_CHANNEL_0**，0~3.3V 模拟采样 | 纯输入引脚（GPI），硬件独立于 Wi-Fi，用于获取连续磁场 AD 计数值 |

---

## 3. 固件架构与数据流实现 (`main/main.c`)

### 3.1 宏定义与 ADC 通道配置
- 增加宏定义：
  ```c
  #define ADC_PIN_HALL_AO       ADC_CHANNEL_0   /* GPIO36 (SENSOR_VP / ADC1_CH0) */
  ```
- 在 `adc_init()` 中：
  ```c
  static void adc_init(void)
  {
      adc_oneshot_unit_init_cfg_t unit_cfg = {
          .unit_id = ADC_UNIT_1,
      };
      ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

      adc_oneshot_chan_cfg_t chan_cfg = {
          .bitwidth = ADC_BITWIDTH_DEFAULT,
          .atten    = ADC_ATTEN_DB_12,   /* 0 ~ 3.3 V */
      };
      ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_PIN_LOW_HUMIDITY, &chan_cfg));
      ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_PIN_HALL_AO, &chan_cfg));
  }
  ```

### 3.2 传感器缓存扩展 (`sensor_cache_t`)
- 在 `sensor_cache_t` 结构体中新增字段：
  ```c
  typedef struct {
      bool  valid;        /* at least one sample taken */
      int   raw_low;      /* ADC counts, GPIO34 */
      float humidity;     /* calibrated ADC soil moisture, % */
      float soil_c;       /* DS18B20, -273 = read failed */
      float lux;          /* BH1750, -1 = read failed */
      bool  aht_ok;
      float air_c;        /* AHT25, only meaningful when aht_ok */
      float air_humi;
      int   hall_raw;     /* 49E Linear Hall AO raw counts, GPIO36 (ADC1_CH0) */
  } sensor_cache_t;
  ```
- 在 `sensor_task()` 周期采样中增加读取：
  ```c
  c.hall_raw = adc_read_raw(ADC_PIN_HALL_AO);
  ```

### 3.3 `/api/sensors` 响应扩展
- 在 JSON 格式化串中增加 `"hall_raw":%d`：
  ```c
  "\"humidity\":%.1f,\"raw_low\":%d,\"temperature\":%.2f,\"lux\":%.1f,\"reed\":%d,"
  "\"hall_raw\":%d,"
  ...
  (double)c.humidity, c.raw_low, soil_c, lux, reed_closed, c.hall_raw,
  ```

---

## 4. Web 界面与折线图渲染设计 (`STATUS_HTML`)

### 4.1 数值展示卡片
在 `.vals` 卡片容器中增加：
```html
<div class="val"><div class="num" id="v-hall">--</div><div class="lbl">霍尔AO [IO36]</div></div>
```

### 4.2 Chart.js 画布与实例化
- 画布 HTML：
  ```html
  <div class="chart-wrap"><canvas id="c-hall" height="100"></canvas></div>
  ```
- 实例化 Chart 对象（颜色选择高辨识度的紫色 `#b388ff`）：
  ```javascript
  const C = {
      h: mk('c-humi', '湿度', '%', '#4fc3f7'),
      t: mk('c-temp', '温度', '°C', '#ef9a9a'),
      l: mk('c-lux', '照度', 'lx', '#fff176'),
      ha: mk('c-hall', '霍尔AO', 'AD', '#b388ff')
  };
  ```
- 在 `poll()` 接收到数据后：
  ```javascript
  const hl = d.hall_raw !== undefined ? d.hall_raw : 0;
  const hlEl = document.getElementById('v-hall');
  if (hlEl) hlEl.textContent = hl;
  push(C.ha, t, hl);
  ```

---

## 5. 验证与测试规范
按照项目内存规范 [[verify-c-string-html-by-reconstruction]]：
1. 由于当前代理环境无 host C 编译器且 `idf.py` 不支持当前 MSys 环境，通过写 node 脚本提取 `STATUS_HTML` 内联的 HTML/CSS/JS。
2. 使用 `node --check` 语法检查提取的内嵌 JavaScript 脚本，确保语法 100% 正确且字符串转义无误。
3. 校验标签配对和 ID 匹配（`v-hall`, `c-hall` 等）。
4. 提示用户在物理机 IDF 环境中编译烧录并进行实际测试。
