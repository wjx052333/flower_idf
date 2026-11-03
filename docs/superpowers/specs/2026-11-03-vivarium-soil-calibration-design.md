# Vivarium 土壤湿度两点标定与配置迁移设计文档

## 1. 概述与背景
在 Commit `9d9e3a9` 中，`ESPWROM32-4M-8xMOS-vivarium` 引入了解耦采样与土壤湿度原始 AD 采样读数显示功能。但其前端和计算逻辑存在过度设计：
- 前端引入了 `toggleSoilMode()` 动态切换按钮，每次切换清空折线图数据、动态切换图表颜色与标签；
- 后端土壤湿度百分比换算硬编码了不可配置的 `humidity_max = 98.0f` 与 `humidity_min = 30.0f`，无法适应传感器硬件个体差异及供电微小压降；
- 缺少将实测空气干燥值与水中浸润值写入 Flash 的标定方案。

本项目旨在将 `ESPWROM32-4M-8xMOS` 中验证成熟的“两点标定方案”迁移至 `ESPWROM32-4M-8xMOS-vivarium`，彻底清理前端过度设计的切换逻辑，提供简单直接的 NVS Flash 持久化标定能力。

---

## 2. 标定算法与换算设计
电容式土壤湿度传感器输出与介质介电常数呈反比：
- 空气介电常数约 1，输出最高电压（对应 ESP32 12 位 ADC 典型值 3110，0% 干燥基准）；
- 纯水介电常数约 80，输出最低电压（对应典型值 958，100% 湿润基准）。

### 换算公式
$$\text{humidity}(\%) = \frac{\text{raw\_dry} - \text{raw\_val}}{\text{raw\_dry} - \text{raw\_wet}} \times 100\%$$
- 范围限制：严格截断在 `[0.0%, 100.0%]` 之间。
- 安全边界：若未标定或误配置 `raw_dry <= raw_wet`，自动安全回退为出厂默认值（3110 和 958）。

---

## 3. 架构与组件变更

### 3.1 结构体与存储扩展（`vivarium_config.h` & `vivarium_config.c`）
1. 在 `vivarium_config_t` 中追加：
   ```c
   #define DEF_SOIL_RAW_DRY  3110
   #define DEF_SOIL_RAW_WET  958

   typedef struct {
       /* 现有生态箱控制字段 */
       float    target_slab_temp_min;
       float    target_slab_temp_max;
       float    target_bulk_temp_min;
       float    target_bulk_temp_max;
       float    vent_humidity_thresh;
       uint32_t routine_vent_interval_s;
       bool     auto_control_enabled;

       /* 土壤湿度两点标定参数 (ADC1_CH6 counts, 0~4095) */
       uint16_t soil_raw_dry;   /* 默认 3110 */
       uint16_t soil_raw_wet;   /* 默认 958 */
   } vivarium_config_t;
   ```
2. 新增纯逻辑换算函数：
   ```c
   float vivarium_calc_soil_humidity(int raw_val, uint16_t raw_dry, uint16_t raw_wet);
   ```
3. 在 `vivarium_config_init_default()` 中设置默认值 `3110` / `958`。
4. 在 `vivarium_config_load()` 中进行范围及 `dry > wet` 安全检验；随整个配置结构体通过 NVS blob 持久化在 `"vivarium"` 空间中。

### 3.2 HTTP API 接口增强（`main.c`）
1. **`GET /api/v1/config`**：
   - 响应 JSON 增加 `"soil_raw_dry": %u` 与 `"soil_raw_wet": %u`。
2. **`POST /api/v1/config`**：
   - 支持 URL query 与 JSON body 解析 `soil_dry` 和 `soil_wet`；
   - 校验范围 `0 <= val <= 4095` 且 `soil_dry > soil_wet`；
   - 调用 `vivarium_config_save()` 立即写入 Flash 并动态重算。
3. **传感器后台采样更新**：
   - `vivarium_sample_sensors_to_cache()` 中调用 `vivarium_calc_soil_humidity(raw_low, s_vivarium_cfg.soil_raw_dry, s_vivarium_cfg.soil_raw_wet)` 更新 `g_soil_hum`。
   - 移除旧的 `humidity_max = 98.0f` / `humidity_min = 30.0f` 局部硬编码。

### 3.3 前端 Web 仪表盘重构（`STATUS_HTML`）
1. **移除 `9d9e3a9` 引入的复杂逻辑**：
   - 移除 HTML 中 `#btn-soil-mode`（“切换: 原始 AD 读数”）按钮；
   - 移除 JS 变量 `soilMode` 及函数 `toggleSoilMode()`；
   - 移除清空图表、动态切换数据集颜色与标题的逻辑；
   - 土壤折线图（`c-soil`）固定绘制已标定的土壤相对湿度百分比（`%`）。
2. **新增紧凑标定控制行**：
   - 位于土壤湿度曲线卡片顶部标题区域；
   - 包含：`干燥(0%): [输入框 id="cal-dry"] [取当前AD按钮] 湿润(100%): [输入框 id="cal-wet"] [取当前AD按钮] [保存标定按钮]`；
   - 点击“取当前AD”时读取轮询获取的最新 `d.soil_raw`；
   - 点击“保存标定”时向 `POST /api/v1/config?soil_dry=...&soil_wet=...` 发送请求并显示保存反馈。
3. **生境数据卡片保持**：
   - 展示 `土壤湿度 [IO34]`（如 `45.2 %`）及下方状态 `AD: 2150 / 4095`。

---

## 4. 验证计划
1. **HTML 与 JavaScript 静态校验**：
   - 复用测试工具提取 C 字符串反转义后的 HTML 与 JS；
   - 校验内联 `<script>` 语法无报错，所有引用的 DOM ID 在 HTML 中均存在。
2. **两点标定边界单元测试**：
   - `raw_dry` 读数返回 `0.0%`；
   - `raw_wet` 读数返回 `100.0%`；
   - 超出极值范围自动截断在 `[0.0%, 100.0%]`；
   - 非法参数 `dry <= wet` 被正确拦截拒绝。
