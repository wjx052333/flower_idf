# OV3660 Sensor Profiles

这些 profile 文件通过修改 OV3660 寄存器参数来平衡低照度下的 **亮度、噪声、细节**。每个阴天分别测试，选画质最好的。

## 寄存器说明

| 寄存器 | 作用 | 调高效果 |
|--------|------|----------|
| `0x3a00` | bit0 night mode（帧积分） | 允许跨帧累积曝光 → 暗光更亮，但 AE 收敛慢 |
| `0x3a18/0x3a19` | AGC gain ceiling（增益上限） | 允许更高的模拟增益 → 更亮，但噪声指数级增加 |
| `0x3a0f/0x3a10` | AE target（目标亮度） | AE 算法追求更亮的输出 |
| `0x5306/0x5307` | denoise offset（空间降噪） | 噪声更少，但画面变软、细节丢失 |

## Profile 对比

| Profile | night mode | gain ceiling | AE target | denoise | 策略 |
|---------|-----------|-------------|-----------|---------|------|
| **X — 原版** | on (0x3b) | 15.5x (默认) | 高 (50/40) | 强 (30/40) | 之前调试好的版本，不可动 |
| **A — 亮度优先** | on (0x3b) | 15.5x | 高 (50/40) | 强 (30/40) | = X + 显式 gain ceiling，最亮 |
| **B — 平衡** | on (0x3b) | 8x | 中高 (40/30) | 中 (20/28) | 亮度和噪声折中 |
| **C — 干净优先** | on (0x3b) | 4x | 中 (30/28) | 轻 (14/18) | 压噪声保细节，画面偏暗 |
| **D — 细节优先** | off (0x3a) | 2x | 低 (28/20) | 极轻 (0c/10) | 白天专用，阴天大概率太暗 |

## 使用方法

```bash
# 默认 = A（亮度优先）
idf.py build

# 切到其他 profile（不需要 reconfigure，下次 build 自动生效）
SENSOR_PROFILE=x_original idf.py build
SENSOR_PROFILE=b_balanced idf.py build
SENSOR_PROFILE=c_clean    idf.py build
SENSOR_PROFILE=d_detail   idf.py build
```

## 测试建议

1. 阴天、同一场景、固定机位
2. 依次 `SENSOR_PROFILE=a_bright idf.py build flash monitor` 拍一张
3. 同样方式测 B 和 C
4. 对比三张图的 **噪声（雪花）** 和 **细节（叶片纹理）**
5. 如果 C 太暗 → 用 B；如果 B 噪声仍大 → 手动在这个文件基础上继续压低 gain ceiling 和 denoise

## 机制

`main/sensor_config/` 由 git 管理，`managed_components/` 被 gitignore 且会被 `fullclean` 删除。
编译时 `copy_sensor_config.cmake` 根据 `SENSOR_PROFILE` 环境变量将对应 profile 拷贝到 `managed_components` 中 OV3660 驱动目录。