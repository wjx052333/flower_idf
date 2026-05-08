"""Generate test Opus data files and C source for ESP32 auto-test.

Uses iFlytek TTS → PCM → Opus encode, then embeds pre-encoded Opus frames
so the ESP32 doesn't need an encoder during test — it just wraps frames in
AudioFrame protobuf and publishes.

Usage:
    cd <repo_root>
    python client/esp32/flower_idf/ESP32-S3-CAM-OVxxxx/scripts/generate_test_opus.py
"""

import os
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(_REPO_ROOT))

from dotenv import load_dotenv
load_dotenv(str(_REPO_ROOT / "deploy" / ".env"))

import asyncio
from mqtt_agent.plugins.tts import iFlytekTTS
from mqtt_agent.pipeline import OpusEncoder

TOOLS_QUESTION = "你有哪些工具可以用"
MULTITURN_DIALOG = [
    "你好，我是测试设备五号",
    "今天天气怎么样",
    "帮我开灯",
]

OUTPUT_DIR = Path(__file__).resolve().parent
MAIN_DIR = OUTPUT_DIR.parent / "main"
SAMPLE_RATE = 16000


def pcm_to_opus_frames(pcm: bytes, encoder: OpusEncoder) -> list[bytes]:
    """Encode PCM to a list of Opus frames."""
    fs = encoder.frame_size  # 320 samples = 640 bytes for 20ms @ 16kHz
    frames = []
    for i in range(0, len(pcm), fs):
        chunk = pcm[i : i + fs]
        if len(chunk) < fs:
            break
        opus = encoder.encode(chunk)
        if opus:
            frames.append(opus)
    return frames


def opus_frames_to_flat(frames: list[bytes]) -> bytes:
    """Pack frames as [u16_le_len][data]... into a flat byte sequence."""
    data = bytearray()
    for f in frames:
        data.extend(len(f).to_bytes(2, "little"))
        data.extend(f)
    return bytes(data)


def flat_to_c_array(name: str, data: bytes, columns: int = 16) -> str:
    """Convert a flat byte sequence to a C const uint8_t array."""
    lines = [f"const uint8_t {name}[] = {{"]
    for i in range(0, len(data), columns):
        chunk = data[i : i + columns]
        hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_vals},")
    lines.append("};")
    lines.append(f"const size_t {name}_size = {len(data)};")
    return "\n".join(lines)


async def generate_utterance(text: str, tts: iFlytekTTS, encoder: OpusEncoder) -> bytes:
    """Generate packed Opus stream for a text utterance."""
    pcm = bytearray()
    async for chunk in tts.stream(text):
        pcm.extend(chunk.tobytes())
    frames = pcm_to_opus_frames(bytes(pcm), encoder)
    packed = opus_frames_to_flat(frames)
    return packed


async def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    tts = iFlytekTTS(
        app_id=os.environ["XFYUN_APP_ID"],
        api_key=os.environ["XFYUN_API_KEY"],
        api_secret=os.environ["XFYUN_API_SECRET"],
    )
    encoder = OpusEncoder(SAMPLE_RATE, 1)

    # ── Generate tools inquiry ──
    print(f"Generating tools inquiry: '{TOOLS_QUESTION}'...")
    tools_data = await generate_utterance(TOOLS_QUESTION, tts, encoder)
    tools_path = OUTPUT_DIR / "test_tools_inquiry.opus"
    with open(tools_path, "wb") as f:
        f.write(tools_data)
    print(f"  → {tools_path} ({len(tools_data)} bytes packed Opus)")

    # ── Generate multiturn utterances ──
    multiturn_data = []
    for i, text in enumerate(MULTITURN_DIALOG):
        print(f"Generating multiturn[{i}]: '{text}'...")
        data = await generate_utterance(text, tts, encoder)
        path = OUTPUT_DIR / f"test_multiturn_{i}.opus"
        with open(path, "wb") as f:
            f.write(data)
        multiturn_data.append(data)
        print(f"  → {path} ({len(data)} bytes packed Opus)")

    # ── Generate test_opus_data.h ──
    print("Generating test_opus_data.h...")
    header = """#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Pre-encoded Opus test utterances.
 *
 * Format: flat sequence of [u16_le frame_len][opus_payload]...
 * Read with: len = data[pos] | (data[pos+1] << 8); frame = &data[pos+2]; pos += 2 + len;
 */

extern const uint8_t test_opus_tools[];
extern const size_t  test_opus_tools_size;

extern const uint8_t test_opus_multiturn_0[];
extern const size_t  test_opus_multiturn_0_size;
extern const uint8_t test_opus_multiturn_1[];
extern const size_t  test_opus_multiturn_1_size;
extern const uint8_t test_opus_multiturn_2[];
extern const size_t  test_opus_multiturn_2_size;
"""
    with open(MAIN_DIR / "test_opus_data.h", "w", encoding="utf-8") as f:
        f.write(header)
    print(f"  → {MAIN_DIR / 'test_opus_data.h'}")

    # ── Generate test_opus_data.c ──
    print("Generating test_opus_data.c...")
    parts = [
        '#include "test_opus_data.h"',
        "",
        "/* Tools inquiry: 你有哪些工具可以用 */",
        flat_to_c_array("test_opus_tools", tools_data),
        "",
        "/* Multiturn turn 0: 你好，我是测试设备五号 */",
        flat_to_c_array("test_opus_multiturn_0", multiturn_data[0]),
        "",
        "/* Multiturn turn 1: 今天天气怎么样 */",
        flat_to_c_array("test_opus_multiturn_1", multiturn_data[1]),
        "",
        "/* Multiturn turn 2: 帮我开灯 */",
        flat_to_c_array("test_opus_multiturn_2", multiturn_data[2]),
        "",
    ]
    with open(MAIN_DIR / "test_opus_data.c", "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print(f"  → {MAIN_DIR / 'test_opus_data.c'}")

    # ── Summary ──
    total = len(tools_data) + sum(len(d) for d in multiturn_data)
    print(f"\nDone! Total Opus data: {total} bytes ({total / 1024:.1f} KB)")


if __name__ == "__main__":
    asyncio.run(main())
