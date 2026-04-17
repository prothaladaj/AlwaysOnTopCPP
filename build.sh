#!/usr/bin/env bash
# build.sh – One-shot build script for AlwaysOnTop
# Requires: python3, cmake, x86_64-w64-mingw32-g++ (MinGW-w64)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Step 1: Generate icons ==="
python3 tools/gen_icons.py

echo ""
echo "=== Step 2: Configure (MinGW cross-compile) ==="
cmake -B build \
      -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      .

echo ""
echo "=== Step 3: Build ==="
cmake --build build --config Release -j"$(nproc)"

EXE="$(find build -name 'AlwaysOnTop.exe' | head -1)"
if [[ -n "$EXE" ]]; then
    SIZE=$(du -sh "$EXE" | cut -f1)
    echo ""
    echo "=== Done! ==="
    echo "  Binary : $EXE"
    echo "  Size   : $SIZE"
else
    echo "ERROR: AlwaysOnTop.exe not found in build/" >&2
    exit 1
fi
