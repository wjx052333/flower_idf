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
import wave
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(_REPO_ROOT))

from dotenv import load_dotenv
load_dotenv(str(_REPO_ROOT / "deploy" / ".env"))

import asyncio
import opuslib
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


# ── Audio helpers ────────────────────────────────────────────────────────────

def save_wav_16k(pcm_bytes: bytes, path: Path) -> None:
    """Save raw int16 PCM (16kHz mono) as WAV."""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm_bytes)


def packed_to_wav_48k(data: bytes, path: Path) -> None:
    """Decode packed Opus frames → WAV at 48kHz (Opus native rate, no resampling)."""
    dec = opuslib.Decoder(48000, 1)
    frames = []
    pos = 0
    while pos + 2 <= len(data):
        n = data[pos] | (data[pos + 1] << 8)
        pos += 2
        if n == 0 or pos + n > len(data):
            break
        frames.append(dec.decode(bytes(data[pos:pos + n]), 960, decode_fec=False))
        pos += n
    pcm = b"".join(frames)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(pcm)
    print(f"    decoded: {path.name}  {len(pcm)/2/48000:.1f}s  {len(frames)} frames")


# ── Opus packing helpers ─────────────────────────────────────────────────────

def pcm_to_opus_frames(pcm: bytes, encoder: OpusEncoder) -> list[bytes]:
    """Encode PCM to a list of Opus frames."""
    frame_bytes = encoder.frame_size * 2  # 320 samples * 2 bytes/sample = 640 bytes
    frames = []
    for i in range(0, len(pcm), frame_bytes):
        chunk = pcm[i : i + frame_bytes]
        if len(chunk) < frame_bytes:
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


async def generate_utterance(text: str, tts: iFlytekTTS,
                             encoder: OpusEncoder) -> tuple[bytes, bytes]:
    """Returns (packed_opus, raw_pcm_int16_16kHz)."""
    pcm = bytearray()
    async for chunk in tts.stream(text):
        pcm.extend(chunk.tobytes())
    frames = pcm_to_opus_frames(bytes(pcm), encoder)
    packed = opus_frames_to_flat(frames)
    return packed, bytes(pcm)


async def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    tts = iFlytekTTS(
        app_id=os.environ["XFYUN_APP_ID"],
        api_key=os.environ["XFYUN_API_KEY"],
        api_secret=os.environ["XFYUN_API_SECRET"],
    )
    encoder = OpusEncoder(SAMPLE_RATE, 1)

    # ── Generate tools inquiry ──────────────────────────────────────────────
    print(f"Generating tools inquiry: '{TOOLS_QUESTION}'...")
    tools_data, tools_pcm = await generate_utterance(TOOLS_QUESTION, tts, encoder)

    with open(OUTPUT_DIR / "test_tools_inquiry.opus", "wb") as f:
        f.write(tools_data)
    save_wav_16k(tools_pcm, OUTPUT_DIR / "test_tools_inquiry.wav")
    print(f"  opus: {len(tools_data)} bytes  wav: {len(tools_pcm)/2/SAMPLE_RATE:.1f}s")

    # ── Generate multiturn utterances ───────────────────────────────────────
    multiturn_data = []
    for i, text in enumerate(MULTITURN_DIALOG):
        print(f"Generating multiturn[{i}]: '{text}'...")
        data, pcm = await generate_utterance(text, tts, encoder)

        with open(OUTPUT_DIR / f"test_multiturn_{i}.opus", "wb") as f:
            f.write(data)
        save_wav_16k(pcm, OUTPUT_DIR / f"test_multiturn_{i}.wav")
        multiturn_data.append(data)
        print(f"  opus: {len(data)} bytes  wav: {len(pcm)/2/SAMPLE_RATE:.1f}s")

    # ── Generate test_opus_data.h ───────────────────────────────────────────
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

    # ── Generate test_opus_data.c ───────────────────────────────────────────
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

    # ── Decode C arrays back to WAV (round-trip verification) ───────────────
    print("Decoding C arrays → WAV (round-trip check, 48kHz)...")
    all_items = [("test_opus_tools", tools_data)] + [
        (f"test_opus_multiturn_{i}", multiturn_data[i]) for i in range(3)
    ]
    for name, data in all_items:
        packed_to_wav_48k(data, MAIN_DIR / f"{name}_decoded.wav")

    # ── Summary ─────────────────────────────────────────────────────────────
    total = len(tools_data) + sum(len(d) for d in multiturn_data)
    print(f"\nDone!  Opus total: {total} bytes ({total / 1024:.1f} KB)")
    print(f"WAV source  → {OUTPUT_DIR}/*.wav  (TTS PCM, 16kHz)")
    print(f"WAV decoded → {MAIN_DIR}/*_decoded.wav  (Opus round-trip, 48kHz)")


if __name__ == "__main__":
    asyncio.run(main())
