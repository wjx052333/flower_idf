"""Pack SR models into srmodels.bin for the ESP32-S3.

Packs WakeNet (wn9_hiesp: "Hi ESP") and NSNet2 models, then writes
srmodels.bin to the project root for flashing.

Usage from repo root:
    python client/esp32/flower_idf/ESP32-S3-CAM-OVxxxx/scripts/pack_models.py
    python client/esp32/flower_idf/ESP32-S3-CAM-OVxxxx/scripts/pack_models.py --flash
"""

import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent.parent.parent
_PROJ_DIR = _REPO_ROOT / "client" / "esp32" / "flower_idf" / "ESP32-S3-CAM-OVxxxx"
_SR_MODEL_DIR = _PROJ_DIR / "managed_components" / "espressif__esp-sr" / "model"
_PACK_MODEL_PY = _SR_MODEL_DIR / "pack_model.py"
_BUILD_DIR = Path(__file__).resolve().parent / "build" / "models"

# Models needed by audio_pipeline.c:
#   afe_sr_init() → WakeNet ("Hi ESP" wake word)
#   afe_vc_init() → NSNet (noise suppression)
MODELS = [
    _SR_MODEL_DIR / "wakenet_model" / "wn9_hiesp",
    _SR_MODEL_DIR / "nsnet_model" / "nsnet2",
]

# Partition from partitions.csv: model,data,spiffs,0x7E0000,0x820000
PARTITION_OFFSET = 0x7E0000
PARTITION_LABEL  = "model"


def main():
    flash = "--flash" in sys.argv

    if not _PACK_MODEL_PY.exists():
        print(f"ERROR: pack_model.py not found at {_PACK_MODEL_PY}")
        print("Run 'idf.py build' first to fetch esp-sr component.")
        sys.exit(1)

    for m in MODELS:
        if not m.is_dir():
            print(f"ERROR: model dir not found: {m}")
            sys.exit(1)

    work_dir = _BUILD_DIR
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    print(f"Copying models to {work_dir}...")
    for m in MODELS:
        dst = work_dir / m.name
        shutil.copytree(m, dst)
        print(f"  {m.name} → {dst}")

    print(f"Packing models with pack_model.py...")
    subprocess.run(
        [sys.executable, str(_PACK_MODEL_PY), "-m", str(work_dir), "-o", "srmodels.bin"],
        cwd=str(work_dir),
        check=True,
    )

    srmodels_bin = work_dir / "srmodels.bin"
    if not srmodels_bin.exists():
        print("ERROR: srmodels.bin was not generated")
        sys.exit(1)

    size = srmodels_bin.stat().st_size
    with open(srmodels_bin, "rb") as f:
        header = f.read(4)
    model_count = struct.unpack("<I", header)[0]
    print(f"Generated srmodels.bin: {size} bytes ({size / 1024:.1f} KB), model_count={model_count}")

    if model_count == 0:
        print("ERROR: srmodels.bin has 0 models — pack failed")
        print(f"Check that model dirs contain files: {work_dir}")
        sys.exit(1)

    out = _PROJ_DIR / "srmodels.bin"
    shutil.copy(srmodels_bin, out)
    print(f"Copied to {out}")

    if flash:
        print(f"\nFlashing srmodels.bin to partition '{PARTITION_LABEL}'...")
        subprocess.run([
            sys.executable, "-m", "esptool",
            "--chip", "esp32s3",
            "write_flash", f"0x{PARTITION_OFFSET:X}", str(out),
        ], check=True)
        print("Flash successful!")
    else:
        print(f"""
Done. To flash, run:

  esptool.py --chip esp32s3 write_flash 0x{PARTITION_OFFSET:X} {out}
""")


if __name__ == "__main__":
    main()
