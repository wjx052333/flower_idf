# xiaozhi-esp32 接入 livekit_mqtt_cpp 后端改造 TODO

## xiaozhi-esp32 有但 backend 没有的功能

### 1. 唤醒词检测 (Wake Word)
- xiaozhi: ESP-SR WakeNet 本地唤醒词识别，检测到唤醒词后自动开始对话
- backend: 无此概念，设备通过 MQTT 发送 `AgentRequest(CHAT)` 启动 AI 对话
- **改造方向**: 保留本地唤醒词检测，检测到后发送 MQTT `AgentRequest(CHAT)` 给 backend

### 2. 按键触发对讲 (Button-Triggered Chat)
- xiaozhi: GPIO 按键触发 ToggleChatState → 开始/停止对话
- backend: 无此概念，只能通过 MQTT 命令
- **改造方向**: 按键按下的动作改为发送 MQTT `AgentRequest(CHAT)` 启动对话，再按一次发送 `AgentRequest(STOP)` 停止

### 3. MCP Server
- xiaozhi: 完整的 MCP 协议服务端，支持设备端工具注册和云端工具调用
- backend: Agent Worker 通过 MQTT topic 和 HTTP API 执行工具
- 改造方向: 保留 MCP 框架，但消息路由改为通过 MQTT 发送/接收

### 4. 设备状态机 (8 States)
- xiaozhi: 8 种设备状态 (Starting/Idle/Connecting/Listening/Speaking/Upgrading/Activating/WifiConfiguring/AudioTesting/FatalError)
- backend: 仅有 online/offline 追踪
- 改造方向: 保留本地状态机，但不依赖服务端状态

### 5. 本地 VAD
- xiaozhi: 设备端 Silero VAD，检测说话/静音
- backend: Agent 服务端 VAD
- 改造方向: 保留本地 VAD，但 backend 的 mqtt_agent 在服务端也会做 VAD，双重保障

### 6. AEC 回声消除
- xiaozhi: 支持设备端 AEC 和服务端 AEC 两种模式
- backend: 无此概念
- 改造方向: 保留设备端 AEC 能力，作为 Kconfig 选项

### 7. 显示屏 UI
- xiaozhi: 完整的 LVGL LCD/OLED 显示系统，表情/状态/消息展示
- backend: Web dashboard
- 改造方向: 保留显示系统不变，将协议层替换后状态和消息仍然正常显示

### 8. OTA 固件升级 + 激活流程
- xiaozhi: 自有 OTA 服务 + 设备激活码流程
- backend: 通过 `OtaCommand` (flower 协议) 下发固件 URL
- 改造方向: 替换为 backend 的 OTA 流程

### 9. UDP 加密音频通道
- xiaozhi: 使用 AES-128-CTR 加密的 UDP 通道传输 Opus 音频
- backend: 直接使用 MQTT topic 传输 Opus 音频（Protobuf 编码）
- **改造方向**: 去掉 UDP 通道，Opus 帧通过 MQTT topic `{prefix}/{device_id}/up/opus` 发送，从 `{prefix}/{device_id}/down/opus` 接收

### 10. JSON 协议 vs Protobuf
- xiaozhi: 使用 JSON 格式的 MQTT 消息 (type/state/text 等字段)
- backend: 使用 Protobuf 二进制格式 (mqtt_agent.proto, flower.proto, device.proto)
- **改造方向**: 协议层改为 Protobuf 序列化 (nanopb)

### 11. WebSocket 协议支持
- xiaozhi: 同时支持 MQTT+UDP 和 WebSocket 两种协议
- backend: 仅 MQTT
- 改造方向: 去掉 WebSocket 支持，仅保留 MQTT

### 12. 资产管理 (Assets)
- xiaozhi: 从云端下载和管理 UI 资产包（表情、字体、音效）
- backend: 无此功能
- 改造方向: 可保留或移除

### 13. Hello/Goodbye 会话模型
- xiaozhi: MQTT hello 握手 → 获取 UDP 密钥 → UDP 音频传输 → goodbye 结束
- backend: AgentRequest(CHAT) → AgentResponse(ok) → MQTT opus 收发 → AgentRequest(STOP)
- **改造方向**: 替换为 backend 的 Agent 会话模型

---

## 核心改造任务：按键 + 唤醒词触发 MQTT 对讲

### Phase 1: 协议层替换（最优先）

- [ ] 1.1 集成 nanopb 和 protobuf 定义 (mqtt_agent.proto, flower.proto, device.proto)
- [ ] 1.2 实现新的 `MqttAgentProtocol` 类，替换现有的 `MqttProtocol` + UDP
- [ ] 1.3 实现 MQTT 直连 backend（HMAC-SHA256 认证，MQTT 5.0）
- [ ] 1.4 实现 Protobuf 消息编解码代替 JSON

### Phase 2: 按键触发 MQTT 对讲

- [ ] 2.1 修改 `ToggleChatState()` → 发送 `AgentRequest(CHAT)` 到 `{prefix}/{device_id}/up/agent_request`
- [ ] 2.2 处理 `AgentResponse` → 根据 ok 字段开始/拒绝对话
- [ ] 2.3 Opus 音频上行：从 audio_service 的 send_queue 取出 → 编码为 `AudioFrame` protobuf → publish 到 `up/opus`
- [ ] 2.4 Opus 音频下行：订阅 `down/opus` → 解码 `AudioFrame` protobuf → 推入 decode_queue
- [ ] 2.5 再次按键 → 发送 `AgentRequest(STOP)` 停止对话

### Phase 3: 唤醒词触发 MQTT 对讲

- [ ] 3.1 保留 ESP-SR WakeNet 唤醒词检测
- [ ] 3.2 唤醒词检测到后 → 自动发送 `AgentRequest(CHAT)` 启动对话
- [ ] 3.3 对话中再次检测到唤醒词 → 发送 `AgentRequest(INTERRUPT)` 打断当前 TTS
- [ ] 3.4 对话结束 → 重新启用唤醒词检测

### Phase 4: 状态和显示适配

- [ ] 4.1 调整设备状态机，适配 backend 的会话流程
- [ ] 4.2 LCD 显示状态同步（Listening/Speaking/Idle）
- [ ] 4.3 处理 `AudioStats` 中的 transcript/llm_response 字段用于显示

### Phase 5: 设备身份和认证

- [ ] 5.1 实现 factory NVS 分区，存储 device_id 和 device_secret
- [ ] 5.2 实现 HMAC-SHA256 MQTT 认证
- [ ] 5.3 实现 StatusReport 定期上报（30s 间隔）
- [ ] 5.4 处理 Command 消息（OTA、snapshot、relay）

---

## 消息流对比

### 原有 xiaozhi 流程:
```
按键/唤醒词 → ToggleChat → MQTT hello → 等 UDP 密钥 → UDP 音频收发 → goodbye
```

### 改造后流程:
```
按键/唤醒词 → AgentRequest(CHAT) → AgentResponse(ok) → MQTT opus 收发 → AgentRequest(STOP)
```

## 关键 Topic 映射

| 方向 | xiaozhi 原有 | 改造后 |
|------|-------------|--------|
| 设备→服务端 音频 | UDP (AES加密) | `{prefix}/{id}/up/opus` (protobuf) |
| 服务端→设备 音频 | UDP (AES加密) | `{prefix}/{id}/down/opus` (protobuf) |
| 设备→服务端 控制 | MQTT publish_topic (JSON) | `{prefix}/{id}/up/agent_request` (protobuf) |
| 服务端→设备 控制 | MQTT subscribe_topic (JSON) | `{prefix}/{id}/down/agent_response` (protobuf) |
| 设备→服务端 状态 | 无 | `{prefix}/{id}/up/status` (protobuf) |
| 服务端→设备 命令 | 无 | `{prefix}/{id}/down/cmd` (protobuf) |

# wjx052333/xiaozhi-esp32
https://github.com/wjx052333/xiaozhi-esp32