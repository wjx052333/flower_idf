#!/usr/bin/env bash
# 生成 proto 产物 + 下载前端 JS 依赖（部署时运行一次）
#   flower_pb2.py    — Python (sim_flr001.py 使用)
#   flower_proto.json — JSON descriptor (index.html 通过 protobufjs 使用)
#   protobuf.min.js  — protobufjs 7 runtime
#   mqtt.min.js      — MQTT.js browser bundle
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROTO="$SCRIPT_DIR/../../../../protocol/flower.proto"
PROTO_DIR="$(dirname "$PROTO")"

# ── Python ────────────────────────────────────────────────────────────────────
VENV_PYTHON="${VENV_PYTHON:-python3}"
$VENV_PYTHON -m grpc_tools.protoc \
    -I"$PROTO_DIR" \
    --python_out="$SCRIPT_DIR" \
    "$PROTO"
echo "Generated: $SCRIPT_DIR/flower_pb2.py"

# ── JavaScript JSON descriptor ────────────────────────────────────────────────
PBJS="$SCRIPT_DIR/node_modules/.bin/pbjs"
if [ ! -x "$PBJS" ]; then
    echo "Installing protobufjs-cli locally..."
    npm install --save-dev protobufjs-cli --prefix "$SCRIPT_DIR" 2>/dev/null
fi
"$PBJS" -t json -o "$SCRIPT_DIR/flower_proto.json" "$PROTO"
echo "Generated: $SCRIPT_DIR/flower_proto.json"

# ── JS vendor libs ────────────────────────────────────────────────────────────
echo "Downloading JS vendor libs..."
curl -fsSL "https://cdn.jsdelivr.net/npm/protobufjs@7/dist/protobuf.min.js" \
    -o "$SCRIPT_DIR/protobuf.min.js"
echo "Downloaded: protobuf.min.js"
curl -fsSL "https://cdn.jsdelivr.net/npm/mqtt/dist/mqtt.min.js" \
    -o "$SCRIPT_DIR/mqtt.min.js"
echo "Downloaded: mqtt.min.js"
