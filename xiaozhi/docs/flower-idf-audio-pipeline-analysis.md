# flower_idf ESP32-S3-CAM 音频/KWS Pipeline 问题诊断

**日期:** 2026-05-14
**对比基准:** xiaozhi-esp32 `feat/mqtt_agent` 分支（已验证音频+KWS 正常工作）

## 概述

`flower_idf/ESP32-S3-CAM-OVxxxx/main` 的音频播放和 KWS 唤醒词均无法正常工作。通过对代码的逐行对比分析，识别出以下根因。

---

## 问题 #1 (CRITICAL)：MQTT Topic 缺少 user_id，服务端与设备 Topic 不匹配

### 现象
TTS 下行音频永远收不到，上行麦克风音频和 Agent 请求也到不了服务端。

### 根因

**xiaozhi-esp32（正常工作）的 Topic 结构：**
```
订阅:
  flower/{device_id}/down/cmd
  flower/{user_id}/{device_id}/down/opus          ← 含 user_id
  flower/{user_id}/{device_id}/down/agent_response ← 含 user_id

发布:
  flower/{device_id}/up/status
  flower/{user_id}/{device_id}/up/opus            ← 含 user_id
  flower/{user_id}/{device_id}/up/agent_request   ← 含 user_id
```

**flower_idf（不工作）的 Topic 结构：**
```
订阅:
  flower/{device_id}/down/cmd
  flower/{device_id}/down/opus          ← 缺少 user_id

发布:
  flower/{device_id}/up/status
  flower/{device_id}/up/cmd_response
  flower/{device_id}/up/opus            ← 缺少 user_id
  flower/{device_id}/up/agent_request   ← 缺少 user_id
```

**结论：** flower_idf 所有音频和 Agent 相关的 topic 都缺少 `{user_id}` 路径段。服务端发布 TTS 音频到 `flower/{user_id}/{device_id}/down/opus`，但设备订阅的是 `flower/{device_id}/down/opus` — topic 不匹配，消息永远路由不到设备。同理，设备上行音频和 Agent 请求也到不了服务端。

**代码位置：**
- `main.c:198-203` — `device_identity_init()` 中构建 topic 字符串
- 对比 `flower_mqtt_protocol.cc:137-142` — `BuildTopics()` 的正确实现

**修复方向：**
在 `flower_idf` 中引入 `user_id`（从 NVS `fctry` 分区或 Kconfig 读取），参考 xiaozhi 的 `BuildTopics()` 重建 topic 字符串。

---

## 问题 #2 (CRITICAL)：双 AFE 实例同时初始化，DRAM 不足导致管线创建失败

### 现象
KWS 唤醒词完全不触发。

### 根因

flower_idf 使用了**双 AFE 实例**架构：
- **AFE(SR)** — 用于唤醒词检测（LOW_COST 模式 + AEC）
- **AFE(VC)** — 用于语音通话（HIGH_PERF 模式 + AEC + NS）

两个 AFE 实例在 `audio_pipeline_init()` 中**同时创建**：
```
afe_sr_init()  → esp_afe_sr_iface->create_from_config()  // AFE(SR) 分配
afe_vc_init()  → esp_afe_sr_iface->create_from_config()  // AFE(VC) 分配
```

每个 AFE 实例内部使用 DL 推理引擎（Xtensa TIE 指令），其推理缓冲区必须在**内部 DRAM** 中分配。代码注释已承认 DRAM 压力：
```c
/* 4ch "RMNM" uses too much internal DRAM on this board (leaves lwip starved). */
```

即使降级为 2ch "MR" + LOW_COST，两个 AFE 实例的同时存在仍然竞争 ESP32-S3 有限的内置 DRAM（~320KB 可用，但 lwip/WiFi/蓝牙协议栈已占用大半）。

此外，mic_feed_task 和 detect_task 的栈也被**强制分配到内部 DRAM**（避免 Xtensa TIE LOOP 指令在 PSRAM 栈上崩溃）：
```c
s_mic_stack = heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // 16KB DRAM
s_detect_stack = heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); // 8KB DRAM
```

**如果 `afe_sr_init()` 或 `afe_vc_init()` 因内存不足失败**，`audio_pipeline_init()` 返回错误，`s_audio_pipeline_ok` 保持 `false`，全部音频管线静默关闭。

**代码位置：**
- `audio_pipeline.c:625-629` — 双 AFE 同时初始化
- `audio_pipeline.c:640-685` — task 栈的 DRAM 分配

**对比 xiaozhi-esp32（正常工作）：**
xiaozhi-esp32 只使用**一个** AFE 实例用于唤醒词检测（`AfeWakeWord`），音频处理（AEC/NS）在通话阶段可选启用（`AfeAudioProcessor`），不会同时运行两个完整 AFE。内存压力远小于 flower_idf。

**修复方向：**
方案 A（推荐）：移除此文件的整个 audio_pipeline 子系统，参考 xiaozhi-esp32 架构重写（单独的 AfeWakeWord + AudioService + AudioProcessor）。
方案 B：改为单 AFE 实例，在模式切换时用 `reset_buffer()` 切换 AFE 类型（SR ↔ VC），而非同时持有两个实例。

---

## 问题 #3 (IMPORTANT)：PA（功放）使能时机不确定

### 现象
即使解决了 topic 问题，开机启动提示音也可能无声或延迟。

### 根因

PA 通过 CH32V003 IO expander 的 pin 4 控制。初始化分为两步：

1. `audio_pa_init()` 在 `app_main()` 中调用 — 设置 pin 方向，**初始状态 PA=OFF**（out=0x67, pin4=LOW）
2. `audio_pa_enable(true)` 在 `playback_task` 的任务函数体中调用 — **PA=ON**（out=0x77, pin4=HIGH）

playback_task 是 FreeRTOS 任务，其调度时机不确定。在 `audio_pipeline_init()` 返回后、playback_task 实际执行到 `audio_pa_enable(true)` 之前，PA 处于关闭状态。

**对比 xiaozhi-esp32（正常工作）：**
`Initialize_Expander()` 在构造函数中同步执行，PA 引脚（pin4+pin6）直接置 HIGH，不依赖异步任务。

**代码位置：**
- `audio.c:287-315` — `audio_pa_init()` PA 初始化为 OFF
- `audio_pipeline.c:559-560` — `audio_pa_enable(true)` 在 playback_task 中调用
- `main.c:893` — `audio_pa_init()` 调用点

**修复方向：**
在 `audio_pa_init()` 末尾直接调用 `audio_pa_enable(true)`，或把 PA 使能移到 `audio_hw_init()` 中同步完成。

---

## 问题 #4 (IMPORTANT)：采样率 16kHz vs 24kHz — xiaozhi 已验证 24kHz 可用

### 现象
如果音频硬件初始化本身有微妙问题，16kHz 可能不是最优配置。

### 根因

| 参数 | xiaozhi-esp32（正常） | flower_idf（异常） |
|------|----------------------|---------------------|
| 采样率 | 24000 Hz | 16000 Hz |
| MCLK | 6.144 MHz (256×24k) | 4.096 MHz (256×16k) |
| Opus 输入 | 16000 Hz (resample from 24k) | 16000 Hz (直通) |

xiaozhi-esp32 使用 24kHz 采样率已经过实际验证（语音唤醒+对话均正常）。flower_idf 降为 16kHz 可能引入以下问题：

1. **ES7210 TDM 时序差异** — 16kHz × 4ch × 16bit = 1.024 Mbps，24kHz 下为 1.536 Mbps。BCLK 分频参数需精确匹配
2. **AEC 对齐** — AFE 的 AEC 模块对采样率敏感，16kHz 下的回声消除效果可能与 24kHz 不同
3. **esp-sr WakeNet 模型** — 大部分预训练 WakeNet 模型针对 16kHz 设计，这点 16kHz 应无问题；但 AFE 内部的 resampling 路径可能与 24kHz 不同

**代码位置：**
- `audio.h:9` — `#define AUDIO_SAMPLE_RATE 16000`
- `audio_pipeline.c:50` — `#define SAMPLE_RATE 16000`

**修复方向：**
建议统一为 24kHz（与已验证的 xiaozhi-esp32 保持一致），降低调试变量数量。

---

## 问题 #5 (IMPORTANT)：缺少 `agent_response` topic 订阅 + 按键 PTT 未发送 AgentRequest

### 现象
按键对讲（PTT）切换为 listening 模式后，服务端无响应。

### 根因

**agent_response 未订阅：**
xiaozhi-esp32 订阅 `flower/{user_id}/{device_id}/down/agent_response`，flower_idf 根本没有这个 topic。虽然 xiaozhi 目前也没真正等待 AgentResponse（已知 bug），但至少订阅着，服务端的响应能收到并打印日志。flower_idf 完全缺失此订阅。

**按键 PTT 不发送 AgentRequest：**
`button_task()` 中按下按钮的代码为：
```c
if (stable_level == 0) {
    if (!audio_pipeline_is_listening()) {
        audio_pipeline_start_listening();
        // AgentRequest CHAT 代码被 #if 0 注释掉了！
    }
}
```

AgentRequest(CHAT) 的发送被 `#if 0` 禁用。注释说明是 "diagnostic — isolate VFS heap corruption"。这意味着按键触发 listening 模式后，只启动了本地 AFE(VC) 上行链路，但从未通知服务端"我要开始对话"，服务端不会下发 TTS。

**代码位置：**
- `main.c:577` — MQTT 订阅列表缺少 agent_response topic
- `main.c:776-788` — AgentRequest(CHAT) 被 `#if 0` 禁用

**修复方向：**
1. 添加 `flower/{user_id}/{device_id}/down/agent_response` 订阅
2. 恢复按键 PTT 的 AgentRequest 发送（使用 `esp_mqtt_client_enqueue`）

---

## 问题 #6 (MINOR)：`on_wake_word_detected` 中也未处理 AgentRequest 响应

唤醒词触发后调用了 `audio_pipeline_start_listening()` 并发送了 AgentRequest(CHAT)（`main.c:723-732`），但没有等待 AgentResponse 就直接开始上行音频。这与 xiaozhi 的已知 bug C2 相同。此处也与 topic 问题叠加（AgentRequest 发到了错误的 topic），导致服务端根本收不到。

---

## 问题 #7 (MINOR)：`esp_srmodel_init("model")` 被调用两次

`afe_sr_init()` 和 `afe_vc_init()` 各自调用了 `esp_srmodel_init("model")`。该函数扫描 model 分区并分配 srmodel_list_t。第二次调用创建了一个独立的 model list，仅用于查找 NS 模型。虽然功能上可行，但增加了内存碎片化和启动时间。

**代码位置：**
- `audio_pipeline.c:219` — afe_sr_init() 中第一次
- `audio_pipeline.c:277` — afe_vc_init() 中第二次

---

## Pipeline 全链路对比

```
┌─────────────────────────────────────────────────────────────────┐
│                    xiaozhi-esp32 (正常工作)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  [Board Init] → Initialize_Expander() → PA=ON (同步)             │
│       ↓                                                          │
│  [Audio HW]  → BoxAudioCodec → I2S duplex (24kHz)                │
│       ↓                                                          │
│  [Audio Svc] → AudioService::Initialize()                        │
│       │         - AfeWakeWord (1 AFE, SR only)                   │
│       │         - Opus enc/dec @ 16kHz (resample 24k→16k)       │
│       │         - AudioProcessor (optional AEC/NS)               │
│       ↓                                                          │
│  [MQTT]      → BuildTopics() with user_id                       │
│       │         - sub: flower/{user_id}/{device_id}/down/opus    │
│       │         - pub: flower/{user_id}/{device_id}/up/opus      │
│       ↓                                                          │
│  ✅ TTS 播放正常  ✅ KWS 唤醒正常  ✅ 双向对讲正常                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                   flower_idf (不工作)                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  [Board Init] → audio_pa_init() → PA=OFF (异步使能)              │
│       ↓                                                          │
│  [Audio HW]  → audio_hw_init() → I2S duplex (16kHz)              │
│       ↓                                                          │
│  [Audio Pipe]→ audio_pipeline_init()                             │
│       │         - AFE(SR) ← 可能 DRAM 不足创建失败               │
│       │         - AFE(VC) ← 同时创建, 加剧 DRAM 压力             │
│       │         - Opus enc/dec @ 16kHz                           │
│       ↓                                                          │
│  [MQTT]      → device_identity_init() WITHOUT user_id            │
│       │         - sub: flower/{device_id}/down/opus  ❌          │
│       │         - pub: flower/{device_id}/up/opus    ❌          │
│       ↓                                                          │
│  ❌ Topic 不匹配：TTS 音频永远收不到                               │
│  ❌ DRAM 不足：AFE 创建可能静默失败，KWS 不工作                    │
│  ❌ PA 延迟使能：启动提示音可能无声                                │
│  ❌ 按键 PTT: AgentRequest 被 #if 0 禁用                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## 修复优先级

| 优先级 | 问题 | 影响 | 修复复杂度 |
|--------|------|------|-----------|
| P0 | MQTT Topic 缺少 user_id | TTS 下行/音频上行全部不通 | 低（改字符串构建） |
| P0 | 双 AFE DRAM 不足 | KWS 不触发 | 高（需重写管线架构） |
| P1 | PA 异步使能 | 提示音无声 | 低（移一行代码） |
| P1 | 按键 PTT AgentRequest 被禁用 | 按键对讲不工作 | 低（取消 #if 0） |
| P2 | 16kHz vs 24kHz | 可能影响音质/AEC | 中（改配置+验证） |
| P2 | 缺少 agent_response 订阅 | 服务端 ACK 丢失 | 低（加 topic+订阅） |

## 建议方案

**短期（最小改动验证）：**
1. 修复 MQTT Topic（加 user_id）
2. `audio_pa_init()` 末尾直接使能 PA
3. 取消按键 PTT 的 `#if 0`
4. 测试：此时 TTS 播放应能工作（topic 匹配后下行数据可达）

**中期（修复 KWS）：**
5. 评估单 AFE vs 双 AFE — 如果 DRAM 不足，改为单 AFE 实例，通过 mode 切换和 `reset_buffer()` 复用
6. 或将 AFE(VC) 降级为与 AFE(SR) 相同的 LOW_COST 模式

**长期（对齐 xiaozhi）：**
7. 用 xiaozhi-esp32 的 AudioService + AfeWakeWord 架构替换整个 audio_pipeline.c
8. 统一采样率为 24kHz