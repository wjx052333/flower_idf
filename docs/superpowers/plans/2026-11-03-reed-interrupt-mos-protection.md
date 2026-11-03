# REED_PIN 中断与 MOS 2/3 联锁保护实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 REED_PIN (GPIO 14) 增加上升沿硬件中断与 FreeRTOS 事件处理任务，在闭合到断开跳变时强制关断 MOS 2 并触发 MOS 3 导通 1 秒；并在断开状态下请求开启 MOS 2 时，拒绝开启、触发 MOS 3 导通 1 秒并返回 HTTP 403 明确错误提示。

**Architecture:** 
1. 硬件中断采用经典 ISR + FreeRTOS 任务模式：ISR 发送任务通知即刻退出，专用 `reed_task` 执行软件延时消抖、冷却限频、关断 MOS 2 并闭合 MOS 3 1 秒。
2. MOS 2 实施双层防护：HTTP 接口层 (`handle_change_relay_ch`) 拦截开启请求并返回带特别提示的 403 响应，继电器核心驱动层 (`relay_set_unlocked`) 进行无条件兜底保护。
3. Web 前端 (`ctrl`) 捕获 HTTP 403 响应文本并向用户展示具体错误原因。

**Tech Stack:** ESP-IDF v5.x (C99), FreeRTOS (Task Notifications, Mutex), ESP-IDF GPIO Driver (`gpio_install_isr_service`, `gpio_isr_handler_add`), Node.js (离线静态语法与 HTML 验证).

---

### Task 1: 扩展离线语法与逻辑验证测试工具

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/test/test_html.js`

- [ ] **Step 1: 在 `test_html.js` 中增加静态语法与模式断言**

确保测试脚本能自动校验：
1. `STATUS_HTML` 中的 `ctrl` 函数正确读取并展示 `r.text()` 错误提示。
2. `main.c` 包含 `reed_isr_handler`、`reed_task`、`gpio_install_isr_service` 及 403 拦截逻辑。

在 `ESPWROM32-4M-8xMOS/test/test_html.js` 结尾追加断言检查：

```javascript
// 5. Check reed interrupt and MOS protection patterns
const requiredCodePatterns = [
  "reed_isr_handler",
  "reed_task",
  "gpio_install_isr_service",
  "gpio_isr_handler_add",
  "403 Forbidden"
];
for (const pattern of requiredCodePatterns) {
  if (!mainContent.includes(pattern)) {
    throw new Error(`FAIL: main.c missing required pattern: ${pattern}`);
  }
}
console.log('PASS: All reed interrupt and MOS protection patterns present in main.c.');
```

- [ ] **Step 2: 运行测试以验证初始状态失败**

Run: `node ESPWROM32-4M-8xMOS/test/test_html.js`
Expected: FAIL with "FAIL: main.c missing required pattern: reed_isr_handler"

- [ ] **Step 3: 暂时注释断言以确保测试脚本本身编写无语法错误，并恢复断言**

确认脚本在遇到未实现的模式时能正确终止并返回非 0 退出码。

- [ ] **Step 4: Commit**

```bash
git add ESPWROM32-4M-8xMOS/test/test_html.js
git commit -m "test: add code pattern checks for reed interrupt and mos protection"
```

---

### Task 2: Web 前端错误展示优化

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/main/main.c:932-944`

- [ ] **Step 1: 修改 `STATUS_HTML` 内嵌的 `ctrl(ch, act)` 函数**

将单纯抛出 `HTTP ${r.status}` 改为优先读取响应体文本 `await r.text()`，以便将服务端的拦截原因呈现给用户：

```javascript
    "async function ctrl(ch,act){\n"
    "  const inp=document.getElementById('d-'+ch);\n"
    "  const sec=Math.max(1,Math.min(3600,parseInt(inp?inp.value:'10',10)||10));\n"
    "  const url=urls[ch-1]+(act==='off'?'?action=off':('?duration='+(sec*1000)));\n"
    "  try{\n"
    "    const r=await fetch(url);\n"
    "    if(!r.ok){\n"
    "      const msg=await r.text();\n"
    "      throw new Error(msg||('HTTP '+r.status));\n"
    "    }\n"
    "    poll();\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent='控制失败: '+e.message;\n"
    "  }\n"
    "}\n"
```

- [ ] **Step 2: 运行 HTML/JS 验证测试**

Run: `node ESPWROM32-4M-8xMOS/test/test_html.js`
Expected: 前 4 项 HTML 和 JS 语法 PASS，停在第 5 项缺失模式（`reed_isr_handler`）。

- [ ] **Step 3: Commit**

```bash
git add ESPWROM32-4M-8xMOS/main/main.c
git commit -m "feat(web): display server error message on relay control failure"
```

---

### Task 3: 继电器核心保护与 HTTP 拦截逻辑实现

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/main/main.c` (在 `relay_set_unlocked` 和 `handle_change_relay_ch` 中增加断开判定)

- [ ] **Step 1: 在 `relay_set_unlocked` 中加入底层联锁防御**

在 `ESPWROM32-4M-8xMOS/main/main.c` 的 `relay_set_unlocked`（约 178 行）添加：
当目标通道为 MOS 2 (`ch == 1`) 且试图打开 (`on == true`) 时，检查干簧管状态：若 `gpio_get_level(REED_PIN) != 0`（断开），则记录警告日志，联动开启 MOS 3 导通 1 秒 (`relay_set_unlocked(2, true, 1000)`)，并直接退出，拒绝打开 MOS 2。

```c
static void relay_set_unlocked(int ch, bool on, uint32_t duration_ms)
{
    if (ch < 0 || ch >= NUM_RELAYS) return;

    /* Interlock: if reed switch is open (level != 0), forbid turning on MOS 2 and trigger MOS 3 for 1s */
    if (ch == 1 && on) {
        if (gpio_get_level(REED_PIN) != 0) {
            ESP_LOGW(TAG, "Reed switch open: rejected turning on MOS 2, triggering MOS 3 for 1s");
            relay_set_unlocked(2, true, 1000);
            return;
        }
    }

    if (on) {
        s_relays[ch].on_us = esp_timer_get_time();
        s_relays[ch].duration_ms = duration_ms;
        time_t _now; time(&_now);
        s_relays[ch].last_on_utc_s = (int64_t)_now;
        s_relays[ch].last_on_dur_ms = duration_ms;
    }
    s_relays[ch].on = on;
    gpio_set_level(s_relays[ch].relay_gpio, on ? 1 : 0);
}
```

- [ ] **Step 2: 在 HTTP 接口 `handle_change_relay_ch` 中加入 403 拦截**

在 `ESPWROM32-4M-8xMOS/main/main.c` 的 `handle_change_relay_ch`（约 1160 行）中：
当 `ch == 1` 且 `!is_off` 时，若 `gpio_get_level(REED_PIN) != 0`，调用 `relay_set(2, true, 1000)`，并返回 HTTP 403 Forbidden 以及中文特别提示：

```c
    if (ch == 1 && !is_off && gpio_get_level(REED_PIN) != 0) {
        ESP_LOGW(TAG, "HTTP request to open MOS 2 rejected: reed switch is open");
        relay_set(2, true, 1000);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "干簧管处于断开状态，拒绝开启MOS 2（已触发MOS 3闭合1秒）");
        return ESP_OK;
    }
```

- [ ] **Step 3: 运行验证**

Run: `node ESPWROM32-4M-8xMOS/test/test_html.js`
Expected: 确认 `403 Forbidden` 模式通过，等待 Task 4 的 ISR 与任务实现。

- [ ] **Step 4: Commit**

```bash
git add ESPWROM32-4M-8xMOS/main/main.c
git commit -m "feat(relay): reject MOS 2 activation and trigger MOS 3 when reed switch is open"
```

---

### Task 4: REED_PIN 硬件中断、消抖处理任务与强制关断 MOS 2

**Files:**
- Modify: `ESPWROM32-4M-8xMOS/main/main.c`

- [ ] **Step 1: 定义 ISR 服务例程与后台任务 `reed_task`**

在 `ESPWROM32-4M-8xMOS/main/main.c` 中添加全局句柄、ISR 以及事件处理任务：
1. `static TaskHandle_t s_reed_task_handle = NULL;`
2. `reed_isr_handler`：在 `IRAM_ATTR` 中通过 `vTaskNotifyGiveFromISR` 唤醒任务并切换上下文。
3. `reed_task`：
   - 等待通知；
   - 延时 30ms 重新检测 `gpio_get_level(REED_PIN) != 0` 实现防抖；
   - 检查冷却时间戳（例如 200ms），防止机械触点连跳；
   - 在 `s_relay_mutex` 保护下：
     - 若 `s_relays[1].on == true`（MOS 2 开启），调用 `relay_set_unlocked(1, false, 0)` 立即关断；
     - 调用 `relay_set_unlocked(2, true, 1000)` 触发 MOS 3 导通 1 秒。

```c
/* ── Reed switch interrupt & event task ─────────────────────────────────── */
static TaskHandle_t s_reed_task_handle = NULL;

static void IRAM_ATTR reed_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_reed_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_reed_task_handle, &high_task_wakeup);
        portYIELD_FROM_ISR(high_task_wakeup);
    }
}

static void reed_task(void *arg)
{
    (void)arg;
    int64_t last_trigger_us = 0;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Debounce delay: 30 ms */
        vTaskDelay(pdMS_TO_TICKS(30));

        /* Verify that REED_PIN is still open (level == 1) */
        if (gpio_get_level(REED_PIN) == 0) {
            continue; /* False trigger / bounce */
        }

        /* Cooldown: ignore triggers within 200 ms */
        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_trigger_us) < 200000LL) {
            continue;
        }
        last_trigger_us = now_us;

        ESP_LOGI(TAG, "Reed switch opened (closed->open transition detected)");

        if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
        /* If MOS 2 is currently ON, turn it OFF immediately */
        if (s_relays[1].on) {
            ESP_LOGW(TAG, "Reed opened while MOS 2 was ON: forcing MOS 2 OFF immediately");
            relay_set_unlocked(1, false, 0);
        }
        /* Trigger MOS 3 for 1 second */
        relay_set_unlocked(2, true, 1000);
        if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
    }
}
```

- [ ] **Step 2: 在 `app_main` 中配置中断与启动任务**

在 `app_main` 的 GPIO 初始化部分：
1. 将 `REED_PIN` 的配置增加中断类型：`io_conf.intr_type = GPIO_INTR_POSEDGE;`
2. 创建任务：`xTaskCreate(reed_task, "reed_task", 2048, NULL, 5, &s_reed_task_handle);`
3. 注册 ISR 服务：
```c
    /* Install GPIO ISR service and attach handler for REED_PIN */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(REED_PIN, reed_isr_handler, NULL));
```

- [ ] **Step 3: 运行完整静态检验测试**

Run: `node ESPWROM32-4M-8xMOS/test/test_html.js`
Expected: ALL CHECKS PASSED!

- [ ] **Step 4: Commit**

```bash
git add ESPWROM32-4M-8xMOS/main/main.c
git commit -m "feat(gpio): add REED_PIN rising-edge interrupt and auto-cutoff logic"
```

---

### Task 5: 最终综合验证与代码审查

**Files:**
- Check: `ESPWROM32-4M-8xMOS/main/main.c`
- Check: `ESPWROM32-4M-8xMOS/test/test_html.js`

- [ ] **Step 1: 运行所有可用测试用例**

Run: `node ESPWROM32-4M-8xMOS/test/test_html.js`
Expected: 0 exit code, "ALL CHECKS PASSED!"

- [ ] **Step 2: 静态代码审查与死锁风险检查**

检查：
1. `relay_set_unlocked` 内递归调用 `relay_set_unlocked(2, true, 1000)`，无重入死锁问题（因为都是 `_unlocked` 版本）。
2. `handle_change_relay_ch` 中调用 `relay_set(2, true, 1000)`，此时未持有 `s_relay_mutex`，互斥锁正常获取释放。
3. `reed_task` 中获取 `s_relay_mutex` 后统一调用 `relay_set_unlocked`，无死锁问题。
4. `gpio_install_isr_service` 兼容 `ESP_ERR_INVALID_STATE`（若已被其它驱动安装）。

- [ ] **Step 3: Git 状态确认与清洁度检查**

Run: `git status`
Expected: 工作区 clean，无遗留未跟踪临时文件。
