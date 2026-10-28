#!/usr/bin/env bash
# 编译 mediasoup-worker（需要 meson / ninja / python3）
# 用法: ./scripts/build-worker.sh
set -e

WORKER_SRC_DIR="${1:-${HOME}/mediasoup-worker}"
OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)/sfu/mediasoup-worker/build"

if [ ! -d "$WORKER_SRC_DIR" ]; then
  echo "Usage: $0 <path-to-mediasoup-worker-source>"
  echo "  git clone https://github.com/versatica/mediasoup.git ${WORKER_SRC_DIR}"
  echo "  cd ${WORKER_SRC_DIR}/worker && meson setup build"
  exit 1
fi

cd "$WORKER_SRC_DIR/worker"
meson setup build --buildtype=release 2>/dev/null || meson compile -C build
meson compile -C build
mkdir -p "$OUT_DIR"
cp build/mediasoup-worker "$OUT_DIR/"
echo "worker binary at: $OUT_DIR/mediasoup-worker"
