#!/usr/bin/env bash
# setup_proto.sh — fetch nanopb, set up IDF component, generate flower.pb.h/c
# Run once before `idf.py build`. Safe to re-run (idempotent).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

NANOPB_VERSION="0.4.9"
NANOPB_CACHE="$SCRIPT_DIR/.nanopb"
NANOPB_COMP="$SCRIPT_DIR/components/nanopb"
PROTO_OUT="$SCRIPT_DIR/main/proto"
PROTO_DIR="$REPO_ROOT/protocol"
PROTO_SRC="$PROTO_DIR/flower.proto"

# ── Dependency checks ────────────────────────────────────────────────────────
for cmd in git python3 protoc; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: $cmd not found"; exit 1; }
done

if [ ! -f "$PROTO_SRC" ]; then
    echo "ERROR: flower.proto not found at $PROTO_SRC"
    exit 1
fi

# ── 1. Fetch nanopb ──────────────────────────────────────────────────────────
if [ ! -f "$NANOPB_CACHE/pb.h" ]; then
    echo "[1/3] Cloning nanopb $NANOPB_VERSION..."
    git clone --depth 1 --branch "$NANOPB_VERSION" \
        https://github.com/nanopb/nanopb.git "$NANOPB_CACHE"
else
    echo "[1/3] nanopb already fetched, skipping."
fi

NANOPB_PLUGIN="$NANOPB_CACHE/generator/protoc-gen-nanopb"
chmod +x "$NANOPB_PLUGIN"

# ── 2. Set up IDF nanopb component ───────────────────────────────────────────
echo "[2/3] Setting up components/nanopb..."
mkdir -p "$NANOPB_COMP/include" "$NANOPB_COMP/src"

cp -f "$NANOPB_CACHE/pb.h" \
      "$NANOPB_CACHE/pb_encode.h" \
      "$NANOPB_CACHE/pb_decode.h" \
      "$NANOPB_CACHE/pb_common.h" \
      "$NANOPB_COMP/include/"

cp -f "$NANOPB_CACHE/pb_encode.c" \
      "$NANOPB_CACHE/pb_decode.c" \
      "$NANOPB_CACHE/pb_common.c" \
      "$NANOPB_COMP/src/"

cat > "$NANOPB_COMP/CMakeLists.txt" <<'EOF'
idf_component_register(SRC_DIRS ./src INCLUDE_DIRS ./include)
EOF

# ── 3. Generate flower.pb.h / flower.pb.c ────────────────────────────────────
echo "[3/3] Generating nanopb bindings for flower.proto..."
mkdir -p "$PROTO_OUT"

protoc \
    --plugin=protoc-gen-nanopb="$NANOPB_PLUGIN" \
    --nanopb_out="$PROTO_OUT" \
    --nanopb_opt=--c-style \
    "--nanopb_opt=--options-path=$PROTO_DIR" \
    -I "$PROTO_DIR" \
    "$PROTO_SRC"

echo "Done."
echo "  components/nanopb/   — IDF nanopb component"
echo "  main/proto/flower.pb.h"
echo "  main/proto/flower.pb.c"
