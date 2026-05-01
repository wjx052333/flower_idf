#!/usr/bin/env bash
# 生成 proto 产物 + 下载前端 JS 依赖（部署时运行一次）
#   device_pb2.py    — Python (sim_flr001.py 使用)
#   device_proto.json — JSON descriptor (index.html 通过 protobufjs 使用)
#   protobuf.min.js  — protobufjs 7 runtime
#   mqtt.min.js      — MQTT.js browser bundle
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROTO="$SCRIPT_DIR/../../../../protocol/ovo_iot_protocol/iot/protocol/device.proto"
PROTO_DIR="$(dirname "$PROTO")"

# ── Python ────────────────────────────────────────────────────────────────────
VENV_PYTHON="${VENV_PYTHON:-python3}"
$VENV_PYTHON -m grpc_tools.protoc \
    -I"$PROTO_DIR" \
    --python_out="$SCRIPT_DIR" \
    "$PROTO"
echo "Generated: $SCRIPT_DIR/device_pb2.py"

# ── JavaScript JSON descriptor ────────────────────────────────────────────────
if command -v npx >/dev/null 2>&1; then
    if ! npx --no-install pbjs --version >/dev/null 2>&1; then
        echo "Installing protobufjs-cli locally..."
        npm install --save-dev protobufjs-cli 2>/dev/null
    fi
    npx pbjs -t json "$PROTO" -o "$SCRIPT_DIR/device_proto.json"
    echo "Generated: $SCRIPT_DIR/device_proto.json"
else
    echo "npx not found, skipping device_proto.json"
fi

# ── JS vendor libs ────────────────────────────────────────────────────────────
echo "Downloading JS vendor libs..."
curl -fsSL "https://cdn.jsdelivr.net/npm/protobufjs@7/dist/protobuf.min.js" \
    -o "$SCRIPT_DIR/protobuf.min.js"
echo "Downloaded: protobuf.min.js"
curl -fsSL "https://cdn.jsdelivr.net/npm/mqtt/dist/mqtt.min.js" \
    -o "$SCRIPT_DIR/mqtt.min.js"
echo "Downloaded: mqtt.min.js"
