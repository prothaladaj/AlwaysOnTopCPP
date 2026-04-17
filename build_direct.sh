#!/usr/bin/env bash
# build_direct.sh – Build AlwaysOnTop WITHOUT cmake
# Requires: python3, x86_64-w64-mingw32-g++, x86_64-w64-mingw32-windres
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CC="x86_64-w64-mingw32-g++"
WINDRES="x86_64-w64-mingw32-windres"

echo "=== Step 1: Generate icons ==="
python3 tools/gen_icons.py

echo ""
echo "=== Step 2: Compile resources ==="
mkdir -p build
$WINDRES \
    --input  res/app.rc \
    --output build/app_rc.o \
    --output-format=coff \
    -I res

echo ""
echo "=== Step 3: Compile & link ==="
$CC \
    -std=c++17 \
    -Os \
    -Wall \
    -fno-exceptions \
    -fno-rtti \
    -Ires \
    src/main.cpp \
    build/app_rc.o \
    -o build/AlwaysOnTop.exe \
    -mwindows \
    -s \
    -static-libgcc \
    -static-libstdc++ \
    -luser32 -lshell32 -ladvapi32 -lcomctl32

EXE="build/AlwaysOnTop.exe"
SIZE=$(du -sh "$EXE" | cut -f1)
echo ""
echo "=== Done! ==="
echo "  Binary : $EXE"
echo "  Size   : $SIZE"
