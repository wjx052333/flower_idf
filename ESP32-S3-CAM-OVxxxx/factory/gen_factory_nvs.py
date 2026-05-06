#!/usr/bin/env python3
"""
gen_factory_nvs.py — 为单台设备生成 factory NVS bin
用法:
    python gen_factory_nvs.py --id <device_id> --secret <device_secret> --out <output.bin>
示例:
    python gen_factory_nvs.py --id dev-001 --secret abc123 --out factory_dev-001.bin
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile

NVS_GEN = os.path.join(
    os.environ.get("IDF_PATH", ""),
    "components", "nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py"
)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--id",     required=True, help="Device ID")
    parser.add_argument("--secret", required=True, help="Device Secret")
    parser.add_argument("--out",    default="factory.bin", help="Output bin file")
    parser.add_argument("--size",   default="0x1000", help="Partition size (default 0x1000)")
    args = parser.parse_args()

    if not os.environ.get("IDF_PATH"):
        print("ERROR: IDF_PATH not set. Run export.sh first.", file=sys.stderr)
        sys.exit(1)

    # 写临时 CSV
    with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False, newline="") as f:
        tmp_csv = f.name
        writer = csv.writer(f)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow(["identity", "namespace", "", ""])
        writer.writerow(["device_id",     "data", "string", args.id])
        writer.writerow(["device_secret", "data", "string", args.secret])

    try:
        cmd = [
            sys.executable, NVS_GEN,
            "generate", tmp_csv, args.out, args.size
        ]
        print(f"Generating {args.out} for device '{args.id}' ...")
        subprocess.run(cmd, check=True)
        print(f"Done: {os.path.abspath(args.out)}")
        print(f"Flash with: esptool.py write_flash 0x10000 {args.out}")
    finally:
        os.unlink(tmp_csv)

if __name__ == "__main__":
    main()
