# Vivarium 土壤湿度两点标定方案迁移与旧逻辑清理实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `ESPWROM32-4M-8xMOS` 的两点土壤湿度标定方案（0%空气干燥与100%水中湿润基准）迁移至 `ESPWROM32-4M-8xMOS-vivarium`，持久化至 NVS Flash，并在 Web 控制台上提供一键取当前 AD 标定能力，同时彻底清除 `9d9e3a9` 引入的复杂图表模式切换代码。

**Architecture:** 
- `vivarium_config_t` 结构体中扩展 `soil_raw_dry` 和 `soil_raw_wet`，开机自 NVS blob 加载并带范围回退；
- 提供纯计算函数 `vivarium_calc_soil_humidity()` 换算百分比并替代硬编码换算；
- `main.c` 中的 `/api/v1/config` 支持 GET 查询与 POST 动态修改标定参数并写入 Flash；
- 前端 `STATUS_HTML` 移除 `toggleSoilMode` 与模式切换相关 JS/DOM，在土壤折线图卡片顶部加入紧凑的标定输入与取当前 AD 按钮，图表固定绘制已标定的相对湿度百分比。

**Tech Stack:** ESP-IDF (C99), NVS Flash, FreeRTOS, ESP HTTP Server, Chart.js, Node.js (test runner)

---

### Task 1: 扩展 `vivarium_config.h` 和 `vivarium_config.c` 支持两点标定参数与持久化

**Files:**
- Modify: `ESPWROM32-4M-8xMOS-vivarium/main/vivarium_config.h`
- Modify: `ESPWROM32-4M-8xMOS-vivarium/main/vivarium_config.c`
- Test: `ESPWROM32-4M-8xMOS-vivarium/test/test_calibration.js`

- [ ] **Step 1: 编写 Node.js 校验脚本测试标定算法逻辑**

创建 `ESPWROM32-4M-8xMOS-vivarium/test/test_calibration.js`：
```javascript
const assert = require('assert');

function vivarium_calc_soil_humidity(raw_val, raw_dry, raw_wet) {
  if (raw_dry <= raw_wet) {
    raw_dry = 3110;
    raw_wet = 958;
  }
  let hum = (raw_dry - raw_val) * 100.0 / (raw_dry - raw_wet);
  if (hum < 0.0) hum = 0.0;
  if (hum > 100.0) hum = 100.0;
  return hum;
}

// 验证基本计算
assert.strictEqual(vivarium_calc_soil_humidity(3110, 3110, 958), 0.0);
assert.strictEqual(vivarium_calc_soil_humidity(958, 3110, 958), 100.0);
assert.strictEqual(vivarium_calc_soil_humidity(2034, 3110, 958), 50.0);
// 验证截断
assert.strictEqual(vivarium_calc_soil_humidity(3500, 3110, 958), 0.0);
assert.strictEqual(vivarium_calc_soil_humidity(500, 3110, 958), 100.0);
// 验证保护回退
assert.strictEqual(vivarium_calc_soil_humidity(3110, 1000, 2000), 0.0);

console.log('PASS: Soil calibration logic tests pass');
```

- [ ] **Step 2: 运行测试确保算法逻辑正确**

Run: `node ESPWROM32-4M-8xMOS-vivarium/test/test_calibration.js`
Expected: `PASS: Soil calibration logic tests pass`

- [ ] **Step 3: 修改 `vivarium_config.h`**

在 `ESPWROM32-4M-8xMOS-vivarium/main/vivarium_config.h` 的 `vivarium_config_t` 中增加宏与字段：
```c
#define DEF_SOIL_RAW_DRY  3110
#define DEF_SOIL_RAW_WET  958

typedef struct {
    /* Hotspot belly heat hysteresis */
    float    target_slab_temp_min;  /* Default: 31.0 C */
    float    target_slab_temp_max;  /* Default: 32.2 C */

    /* Bulk ambient heating hysteresis */
    float    target_bulk_temp_min;  /* Default: 24.0 C */
    float    target_bulk_temp_max;  /* Default: 26.5 C */

    /* Rainforest humidity threshold for venting */
    float    vent_humidity_thresh;  /* Default: 88.0 % */

    /* Routine fresh air purge interval */
    uint32_t routine_vent_interval_s; /* Default: 10800 s (3 hours) */

    /* Auto control enabled */
    bool     auto_control_enabled;  /* Default: true */

    /* Soil moisture 2-point ADC calibration (counts 0~4095) */
    uint16_t soil_raw_dry;          /* Default: 3110 (air / 0%) */
    uint16_t soil_raw_wet;          /* Default: 958  (water / 100%) */
} vivarium_config_t;

/* Calculate calibrated soil humidity (0.0% - 100.0%) from raw ADC reading */
float vivarium_calc_soil_humidity(int raw_val, uint16_t raw_dry, uint16_t raw_wet);
```

- [ ] **Step 4: 修改 `vivarium_config.c`**

在 `vivarium_config.c` 中实现默认值、安全加载与换算函数：
```c
void vivarium_config_init_default(vivarium_config_t *cfg)
{
    if (!cfg) return;
    cfg->target_slab_temp_min     = 31.0f;
    cfg->target_slab_temp_max     = 32.2f;
    cfg->target_bulk_temp_min     = 24.0f;
    cfg->target_bulk_temp_max     = 26.5f;
    cfg->vent_humidity_thresh     = 88.0f;
    cfg->routine_vent_interval_s  = 10800; /* 3 hours */
    cfg->auto_control_enabled     = true;
    cfg->soil_raw_dry             = DEF_SOIL_RAW_DRY;
    cfg->soil_raw_wet             = DEF_SOIL_RAW_WET;
}
```
在 `vivarium_config_load()` 读取 blob 成功后增加安全防护检查：
```c
    if (cfg->soil_raw_dry <= cfg->soil_raw_wet || cfg->soil_raw_dry > 4095 || cfg->soil_raw_wet > 4095) {
        ESP_LOGW(TAG, "Invalid soil calibration (dry=%u, wet=%u), resetting to defaults",
                 (unsigned)cfg->soil_raw_dry, (unsigned)cfg->soil_raw_wet);
        cfg->soil_raw_dry = DEF_SOIL_RAW_DRY;
        cfg->soil_raw_wet = DEF_SOIL_RAW_WET;
    }
```
并在文件末尾实现 `vivarium_calc_soil_humidity()`：
```c
float vivarium_calc_soil_humidity(int raw_val, uint16_t raw_dry, uint16_t raw_wet)
{
    if (raw_dry <= raw_wet) {
        raw_dry = DEF_SOIL_RAW_DRY;
        raw_wet = DEF_SOIL_RAW_WET;
    }
    float hum = (float)(raw_dry - raw_val) * 100.0f / (float)(raw_dry - raw_wet);
    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;
    return hum;
}
```

- [ ] **Step 5: 提交 Task 1**

```bash
git add ESPWROM32-4M-8xMOS-vivarium/main/vivarium_config.h ESPWROM32-4M-8xMOS-vivarium/main/vivarium_config.c ESPWROM32-4M-8xMOS-vivarium/test/test_calibration.js
git commit -m "feat(vivarium): add soil moisture calibration parameters and calc function to vivarium_config

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: 更新后台传感器采样与 `/api/v1/config` 接口

**Files:**
- Modify: `ESPWROM32-4M-8xMOS-vivarium/main/main.c:640-675, 1298-1375`

- [ ] **Step 1: 修改后台采样任务，使用标定参数换算**

在 `ESPWROM32-4M-8xMOS-vivarium/main/main.c` 的 `vivarium_sample_sensors_to_cache()` 中：
替换旧的硬编码：
```c
    /* 1. Soil humidity from ADC1_CH6 (GPIO 34, optional/spare) */
    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    uint16_t dry = s_vivarium_cfg.soil_raw_dry;
    uint16_t wet = s_vivarium_cfg.soil_raw_wet;
    float soil_h = vivarium_calc_soil_humidity(raw_low, dry, wet);
```
移除：
```c
    float humidity_cap = raw_low * 100.0f / 4096.0f;
    const float humidity_max = 98.0f;
    const float humidity_min = 30.0f;
    float soil_h = 100.0f * (humidity_max - humidity_cap) / (humidity_max - humidity_min);
    if (soil_h < 0.0f) soil_h = 0.0f;
    if (soil_h > 100.0f) soil_h = 100.0f;
```

- [ ] **Step 2: 修改 `GET /api/v1/config` 输出 `soil_raw_dry` 与 `soil_raw_wet`**

在 `handle_agent_config_get` 中：
```c
    snprintf(json, sizeof(json),
        "{\n"
        "  \"target_slab_temp_min\": %.1f,\n"
        "  \"target_slab_temp_max\": %.1f,\n"
        "  \"target_bulk_temp_min\": %.1f,\n"
        "  \"target_bulk_temp_max\": %.1f,\n"
        "  \"vent_humidity_thresh\": %.1f,\n"
        "  \"routine_vent_interval_s\": %lu,\n"
        "  \"auto_control_enabled\": %s,\n"
        "  \"soil_raw_dry\": %u,\n"
        "  \"soil_raw_wet\": %u\n"
        "}",
        s_vivarium_cfg.target_slab_temp_min,
        s_vivarium_cfg.target_slab_temp_max,
        s_vivarium_cfg.target_bulk_temp_min,
        s_vivarium_cfg.target_bulk_temp_max,
        s_vivarium_cfg.vent_humidity_thresh,
        (unsigned long)s_vivarium_cfg.routine_vent_interval_s,
        s_vivarium_cfg.auto_control_enabled ? "true" : "false",
        (unsigned)s_vivarium_cfg.soil_raw_dry,
        (unsigned)s_vivarium_cfg.soil_raw_wet
    );
```

- [ ] **Step 3: 修改 `POST /api/v1/config` 支持解析与保存 `soil_dry`、`soil_wet`**

在 `handle_agent_config_post` 中加入 query 参数和 JSON 解析：
```c
    if (httpd_query_key_value(query, "soil_dry", val, sizeof(val)) == ESP_OK) {
        int d = atoi(val);
        if (d >= 0 && d <= 4095) s_vivarium_cfg.soil_raw_dry = (uint16_t)d;
    }
    if (httpd_query_key_value(query, "soil_wet", val, sizeof(val)) == ESP_OK) {
        int w = atoi(val);
        if (w >= 0 && w <= 4095) s_vivarium_cfg.soil_raw_wet = (uint16_t)w;
    }
```
如果从 JSON 体解析，同样支持 `"soil_dry"` / `"soil_raw_dry"` 和 `"soil_wet"` / `"soil_raw_wet"`。
保存前确保 `soil_raw_dry > soil_raw_wet`，若不满足则不更新：
```c
    if (s_vivarium_cfg.soil_raw_dry <= s_vivarium_cfg.soil_raw_wet) {
        ESP_LOGW(TAG, "Invalid soil calibration dry <= wet, reverting");
        s_vivarium_cfg.soil_raw_dry = DEF_SOIL_RAW_DRY;
        s_vivarium_cfg.soil_raw_wet = DEF_SOIL_RAW_WET;
    }
```

- [ ] **Step 4: 提交 Task 2**

```bash
git add ESPWROM32-4M-8xMOS-vivarium/main/main.c
git commit -m "feat(vivarium): integrate soil calibration into sensor sampling and /api/v1/config

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: 重构前端 Web 仪表盘并清理 `9d9e3a9` 过度设计的代码

**Files:**
- Modify: `ESPWROM32-4M-8xMOS-vivarium/main/main.c:880-1110`
- Create: `ESPWROM32-4M-8xMOS-vivarium/test/test_html.js`

- [ ] **Step 1: 创建 vivarium 的 HTML/JS 静态语法检查测试工具**

创建 `ESPWROM32-4M-8xMOS-vivarium/test/test_html.js`（根据 MEMORY.md 提取并验证 C-string HTML/JS 与 DOM ID 映射）。

- [ ] **Step 2: 清理 HTML 中的过度设计元素并加入标定栏**

在 `STATUS_HTML` 中：
1. 移除旧的切换按钮：
```html
<button id="btn-soil-mode" ...>切换: 原始 AD 读数</button>
```
2. 替换为紧凑标定控制行：
```html
<div class="chart-wrap">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;flex-wrap:wrap;gap:4px">
    <span style="font-size:12px;color:#81c784">🌱 土壤湿度 (相对湿度 %)</span>
    <div style="font-size:11px;color:#888;display:flex;align-items:center;gap:4px">
      干燥(0%): <input type="number" id="cal-dry" min="0" max="4095" style="width:48px;padding:2px;background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:4px;text-align:center;font-size:11px">
      <button class="btn on" style="margin-left:0;padding:2px 6px;font-size:11px" onclick="setCal('dry')">取当前AD</button>
      湿润(100%): <input type="number" id="cal-wet" min="0" max="4095" style="width:48px;padding:2px;background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:4px;text-align:center;font-size:11px">
      <button class="btn on" style="margin-left:0;padding:2px 6px;font-size:11px" onclick="setCal('wet')">取当前AD</button>
      <button class="btn on" style="margin-left:4px;padding:2px 8px;font-size:11px" onclick="saveCal()">保存标定</button>
      <span id="cal-msg" style="color:#81c784;font-size:11px"></span>
    </div>
  </div>
  <canvas id="c-soil" height="80"></canvas>
</div>
```

- [ ] **Step 3: 清理 JS 代码并添加标定读写逻辑**

1. 彻底删除：
- `let soilMode = 'hum';`
- `function toggleSoilMode() { ... }`
- `poll()` 中针对 `soilMode === 'raw'` 的分支处理
2. 增加：
```javascript
let latestSoilRaw = null;

function setCal(which) {
  if (latestSoilRaw === null) { alert('尚未获取到传感器读数'); return; }
  if (which === 'dry') document.getElementById('cal-dry').value = latestSoilRaw;
  if (which === 'wet') document.getElementById('cal-wet').value = latestSoilRaw;
  document.getElementById('cal-msg').textContent = '';
}

async function loadCal() {
  try {
    const r = await fetch('/api/v1/config');
    if (!r.ok) return;
    const d = await r.json();
    if (d.soil_raw_dry !== undefined) document.getElementById('cal-dry').value = d.soil_raw_dry;
    if (d.soil_raw_wet !== undefined) document.getElementById('cal-wet').value = d.soil_raw_wet;
  } catch (e) {}
}

async function saveCal() {
  const dry = document.getElementById('cal-dry').value;
  const wet = document.getElementById('cal-wet').value;
  const msg = document.getElementById('cal-msg');
  msg.textContent = '保存中...';
  try {
    const r = await fetch('/api/v1/config?soil_dry=' + dry + '&soil_wet=' + wet, { method: 'POST' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    msg.textContent = '已保存';
    setTimeout(() => { msg.textContent = ''; }, 3000);
  } catch (e) {
    msg.textContent = '保存失败: ' + e.message;
  }
}
```
3. 在 `poll()` 中：
```javascript
    if (d.soil_raw !== undefined) {
      latestSoilRaw = d.soil_raw;
    }
    pushMulti(C_soil, t, [parseFloat(sh)]);
```
4. 页面初始化时调用：`loadCal();`

- [ ] **Step 4: 运行 `test_html.js` 验证静态语法和 DOM ID 完整性**

Run: `node ESPWROM32-4M-8xMOS-vivarium/test/test_html.js`
Expected: `ALL CHECKS PASSED!`

- [ ] **Step 5: 提交 Task 3**

```bash
git add ESPWROM32-4M-8xMOS-vivarium/main/main.c ESPWROM32-4M-8xMOS-vivarium/test/test_html.js
git commit -m "refactor(vivarium): clean up toggleSoilMode and add calibration controls in web UI

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: 端到端检查与综合测试验证

**Files:**
- Modify: `ESPWROM32-4M-8xMOS-vivarium/test/run_tests.sh`
- Verify: `git diff` across `ESPWROM32-4M-8xMOS-vivarium`

- [ ] **Step 1: 将校验脚本加入 `run_tests.sh`**

在 `ESPWROM32-4M-8xMOS-vivarium/test/run_tests.sh` 顶部增加 node 静态测试执行。

- [ ] **Step 2: 运行测试脚本**

Run: `node ESPWROM32-4M-8xMOS-vivarium/test/test_calibration.js && node ESPWROM32-4M-8xMOS-vivarium/test/test_html.js`
Expected: 全部测试通过 (ALL CHECKS PASSED)

- [ ] **Step 3: 检查代码改动并提交**

```bash
git add ESPWROM32-4M-8xMOS-vivarium/test/run_tests.sh
git commit -m "test(vivarium): integrate calibration and html verification into run_tests.sh

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```
