"""Generate flower_pb2.py from flower.proto using grpc_tools.protoc.

Usage:
    python gen_pb.py
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_FILE = os.path.join(HERE, "..", "..", "..", "..", "protocol", "ovo_iot_protocol", "iot", "protocol", "flower.proto")
PROTO_DIR = os.path.dirname(PROTO_FILE)
OUT_DIR = HERE


def main():
    proto = os.path.normpath(PROTO_FILE)
    out = os.path.normpath(OUT_DIR)
    proto_dir = os.path.normpath(PROTO_DIR)

    if not os.path.exists(proto):
        print(f"ERROR: proto not found: {proto}")
        return 1

    cmd = [
        sys.executable, "-m", "grpc_tools.protoc",
        f"-I{proto_dir}",
        f"--python_out={out}",
        proto,
    ]
    print(" ".join(cmd))
    result = subprocess.run(cmd)
    if result.returncode == 0:
        print(f"Generated: {os.path.join(out, 'flower_pb2.py')}")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())