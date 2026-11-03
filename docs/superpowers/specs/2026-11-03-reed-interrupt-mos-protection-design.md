# REED_PIN 中断与 MOS 2/3 联锁保护设计规范

## 1. 概述与背景

在 ESPWROM32-4M-8xMOS 固件中，`REED_PIN` (GPIO 14) 连接了干簧管传感器，用于感应位置/箱门/磁开关状态。
目前代码中通过内部上拉将 GPIO 14 配置为普通输入口，在 `/api/sensors` 中周期性轮询读取。

本设计旨在引入**硬件中断响应**与**继电器联锁控制逻辑**：
1. 当干簧管出现由“闭合”向“断开”的跳变时（低电平变为高电平），触发中断，若此时 MOS 2（通道 1）处于打开状态，立即强制关闭 MOS 2，并触发 MOS 3（通道 2）闭合（导通）1 秒。
2. 当干簧管处于断开状态下，若外部或内部请求打开 MOS 2，系统拒绝打开 MOS 2，并联动触发 MOS 3 闭合 1 秒；若该请求来自 HTTP 接口，则返回 HTTP 403 Forbidden 并输出明确提示信息。

---

## 2. 硬件映射与电气定义

- **干簧管引脚**: `REED_PIN` = `GPIO_NUM_14`，内部上拉开启 (`GPIO_PULLUP_ENABLE`)。
  - 闭合状态：引脚接 GND，读取电平为 `0` (`gpio_get_level == 0`)。
  - 断开状态：引脚被内部上拉拉高，读取电平为 `1` (`gpio_get_level == 1`)。
  - **闭合到断开的跳变**：对应**低电平向高电平跳变的上升沿**（`GPIO_INTR_POSEDGE`）。
- **MOS 继电器通道映射**:
  - MOS 1：通道索引 0 (`MOS1_PIN` / GPIO 19)
  - MOS 2：通道索引 1 (`MOS2_PIN` / GPIO 18)，HTTP 端点 `/change_relay2`
  - MOS 3：通道索引 2 (`MOS3_PIN` / GPIO 17)，HTTP 端点 `/change_relay3`

---

## 3. 架构设计与详细实现

### 3.1 硬件中断与 FreeRTOS 任务设计

为了避免在硬件中断服务程序（ISR）中直接调用耗时或持有 FreeRTOS 互斥锁的操作，采用标准的 **ISR + 专用事件处理任务** 模式：

```
[干簧管断开跳变 0 -> 1]
         │ (硬件上升沿中断 GPIO_INTR_POSEDGE)
         ▼
[reed_isr_handler] (IRAM)
         │ (vTaskNotifyGiveFromISR)
         ▼
   [reed_task] (FreeRTOS Task, 优先级 5, 栈 2048 字节)
         │
         ├─ 1. 软件消抖 (vTaskDelay 30ms) & 确认电平仍为 1
         ├─ 2. 冷却时间判断 (防机械触点高频抖动)
         ├─ 3. 获取 s_relay_mutex
         │      ├─ 检查 s_relays[1].on: 若为 true，立即 relay_set_unlocked(1, false, 0)
         │      └─ 触发 relay_set_unlocked(2, true, 1000) (MOS 3 闭合 1s)
         └─ 4. 释放 s_relay_mutex
```

#### 关键实现细节：
- **GPIO 中断初始化**：
  在 `app_main` 中配置 `REED_PIN` 的 `intr_type = GPIO_INTR_POSEDGE`；
  调用 `gpio_install_isr_service(0)`；
  注册 `gpio_isr_handler_add(REED_PIN, reed_isr_handler, NULL)`。
- **任务通知与防抖**：
  `reed_task` 使用 `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` 阻塞等待；
  收到中断后延迟 30ms 重新检测 `gpio_get_level(REED_PIN) == 1`；
  设置 200ms 防抖冷却时间，防止单次机械振荡导致重复触发。
- **状态联动**：
  在 `s_relay_mutex` 互斥保护下：
  若 MOS 2 (`s_relays[1].on`) 处于打开状态，立即关断 (`relay_set_unlocked(1, false, 0)`)；
  调用 `relay_set_unlocked(2, true, 1000)` 打开 MOS 3 并启动 1000ms 倒计时关断。

---

### 3.2 MOS 2 请求拦截与安全防护（双层防护机制）

#### 第一层：HTTP 接口拦截 (`handle_change_relay_ch`)
在处理 `/change_relay2` 请求时：
- 判断请求是否为开启操作（`!is_off`）且目标通道为 MOS 2（`ch == 1`）。
- 读取当前干簧管状态：`gpio_get_level(REED_PIN) != 0`（即断开状态）。
- 若断开：
  1. 调用 `relay_set(2, true, 1000)` 触发 MOS 3 导通 1 秒；
  2. 拒绝打开 MOS 2；
  3. 设置 HTTP 响应状态码为 `403 Forbidden`：
     `httpd_resp_set_status(req, "403 Forbidden");`
     `httpd_resp_set_type(req, "text/plain; charset=utf-8");`
  4. 响应体输出提示信息：
     `"干簧管处于断开状态，拒绝开启MOS 2（已触发MOS 3闭合1秒）"`；
  5. 记录警告日志并返回 `ESP_OK`。
- 若请求为关闭操作（`is_off == true`），依然允许正常关闭。

#### 第二层：底层继电器核心拦截 (`relay_set_unlocked`)
作为底层的安全兜底保护：
- 在 `relay_set_unlocked(int ch, bool on, uint32_t duration_ms)` 中：
  若 `ch == 1` 且 `on == true`，检查 `gpio_get_level(REED_PIN) != 0`。
  若处于断开状态：
  1. 触发 `relay_set_unlocked(2, true, 1000)`；
  2. 记录警告日志；
  3. 直接返回，不将 MOS 2 引脚置高。
- 这样能阻断来自任何未知路径或未来扩展模块对 MOS 2 的误开启。

---

### 3.3 Web 前端错误捕获与提示展示

- 在 `STATUS_HTML` 内嵌的前端脚本中，优化 `ctrl(ch, act)` 函数的错误处理：
  ```javascript
  async function ctrl(ch, act) {
    const inp = document.getElementById('d-' + ch);
    const sec = Math.max(1, Math.min(3600, parseInt(inp ? inp.value : '10', 10) || 10));
    const url = urls[ch - 1] + (act === 'off' ? '?action=off' : ('?duration=' + (sec * 1000)));
    try {
      const r = await fetch(url);
      if (!r.ok) {
        const msg = await r.text();
        throw new Error(msg || ('HTTP ' + r.status));
      }
      poll();
    } catch (e) {
      document.getElementById('status').textContent = '控制失败: ' + e.message;
    }
  }
  ```
- **用户交互表现**：
  当用户在干簧管断开时点击 MOS 2 的“开”按钮，底部状态栏直接显示服务端返回的详细文字错误，卡片状态保持“关”，同时 MOS 3 卡片在下次轮询或即时刷新中显示“开”（持续 1 秒后自动变为关）。

---

## 4. 边界条件与容错处理

1. **机械触点跳动 (Contact Bounce)**：
   干簧管在断开瞬间可能发生毫秒级反复通断，通过 FreeRTOS 任务延时 30ms 重新采样电平 + 记录上次触发时间戳（`esp_timer_get_time()`）实现软件双重消抖，消抖窗口内忽略重复中断。
2. **锁安全性**：
   ISR 中仅发送 `vTaskNotifyGiveFromISR`，严禁 `xSemaphoreTake`。
   在后台任务中安全获取 `s_relay_mutex` 后操作 `s_relays[1]` 与 `s_relays[2]`。
3. **断开时请求关闭 MOS 2**：
   允许在断开状态下发送关闭 MOS 2 的请求（`action=off`），以便系统状态恢复或界面复位。
4. **中断唤醒时的 MOS 2 状态**：
   若 MOS 2 已经被开启（例如在干簧管闭合时打开了 MOS 2，随后干簧管被移开断开），中断任务必须原子地立即关断 MOS 2，并导通 MOS 3 1 秒。

---

## 5. 验证与测试方案

由于环境限制（无本地 ESP-IDF 编译链），采用以下严密的离线测试与验证步骤：
1. **HTML 字符串重构测试**：提取内嵌的 C 语言转义字符串，通过 Node.js 解析验证 HTML/JS 语法正确性与完整性。
2. **静态代码审查**：
   - 检查 `gpio_config`、`gpio_install_isr_service`、`gpio_isr_handler_add` 的参数及返回值处理。
   - 检查 `vTaskNotifyGiveFromISR` 与 `portYIELD_FROM_ISR` 上下文切换。
   - 检查 `s_relay_mutex` 互斥保护的完整性与死锁风险（确保在持有互斥锁期间不发生二次重入）。
