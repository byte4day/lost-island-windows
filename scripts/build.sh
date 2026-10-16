#!/bin/sh
# Build LostIsland.exe on Linux with llvm-mingw.
set -e
cd "$(dirname "$0")/.."
: "${LLVM_MINGW:=$(ls -d "$HOME"/.local/opt/llvm-mingw-* 2>/dev/null | head -1)}"
[ -d "$LLVM_MINGW" ] || { echo "Set LLVM_MINGW to an llvm-mingw directory" >&2; exit 1; }
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DLLVM_MINGW="$LLVM_MINGW" >/dev/null
cmake --build build -j"$(nproc)"
ls -la build/LostIsland.exe
