#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build"
if command -v cmake >/dev/null 2>&1; then
  cmake -S "$ROOT/hook" -B "$ROOT/build/cmake" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ROOT/build/cmake" -j
  ctest --test-dir "$ROOT/build/cmake" --output-on-failure
else
  OBJ="$ROOT/build/obj"
  mkdir -p "$OBJ"
  FLAGS=(-fPIC -DSUBHOOK_STATIC -DSUBHOOK_IMPLEMENTATION -DSUBHOOK_SEPARATE_SOURCE_FILES -I"$ROOT/third_party/subhook")
  gcc "${FLAGS[@]}" -c "$ROOT/third_party/subhook/subhook.c" -o "$OBJ/subhook.o"
  gcc "${FLAGS[@]}" -c "$ROOT/third_party/subhook/subhook_unix.c" -o "$OBJ/subhook_unix.o"
  gcc "${FLAGS[@]}" -c "$ROOT/third_party/subhook/subhook_x86.c" -o "$OBJ/subhook_x86.o"
  g++ -std=c++17 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE -I"$ROOT/hook/src" -I"$ROOT/third_party/subhook" \
    "$ROOT/hook/src/sha256.cpp" "$ROOT/hook/src/capture.cpp" "$ROOT/hook/src/preload.cpp" \
    "$OBJ/subhook.o" "$OBJ/subhook_unix.o" "$OBJ/subhook_x86.o" \
    -o "$ROOT/build/libxenomit_flash_hook.so"
  g++ -std=c++17 -D_GNU_SOURCE -I"$ROOT/hook/src" \
    "$ROOT/hook/src/sha256.cpp" "$ROOT/hook/src/sha256_test.cpp" \
    -o "$ROOT/build/xenomit_flash_sha_test"
  "$ROOT/build/xenomit_flash_sha_test"
fi

FLASH_CLIENT="$ROOT/../xenomit-flash-client"
if [[ -x "$ROOT/build/xenomit_flash_inject" ]]; then
  :
elif [[ -f "$FLASH_CLIENT/src/swf_bridge.cpp" ]]; then
  OBJ="$ROOT/build/obj"
  mkdir -p "$OBJ"
  CFLAGS=(-O2 -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -I"$FLASH_CLIENT/third_party/miniz" -I"$FLASH_CLIENT/third_party/lzma" -w)
  gcc "${CFLAGS[@]}" -c "$FLASH_CLIENT/third_party/miniz/miniz.c" -o "$OBJ/miniz.o"
  gcc "${CFLAGS[@]}" -c "$FLASH_CLIENT/third_party/miniz/miniz_tdef.c" -o "$OBJ/miniz_tdef.o"
  gcc "${CFLAGS[@]}" -c "$FLASH_CLIENT/third_party/miniz/miniz_tinfl.c" -o "$OBJ/miniz_tinfl.o"
  gcc "${CFLAGS[@]}" -c "$FLASH_CLIENT/third_party/lzma/LzmaDec.c" -o "$OBJ/LzmaDec.o"
  g++ -std=c++17 -O2 -D_GNU_SOURCE -D_LARGEFILE64_SOURCE \
    -I"$FLASH_CLIENT/src" -I"$FLASH_CLIENT/generated" \
    -I"$FLASH_CLIENT/third_party/miniz" -I"$FLASH_CLIENT/third_party/lzma" \
    "$ROOT/hook/src/inject_main.cpp" \
    "$FLASH_CLIENT/src/swf_bridge.cpp" \
    "$OBJ/miniz.o" "$OBJ/miniz_tdef.o" "$OBJ/miniz_tinfl.o" "$OBJ/LzmaDec.o" \
    -o "$ROOT/build/xenomit_flash_inject"
else
  echo "[xenomit-flash] xenomit-flash-client is missing; reflection bridge injector was not built" >&2
fi
echo "[xenomit-flash] hook built at $ROOT/build/libxenomit_flash_hook.so"
