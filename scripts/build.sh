#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build"

FLASH_CLIENT_REPO="https://github.com/xenomit-do/xenomit-flash-client"
FLASH_CLIENT_TAG="v0.1.0"
FLASH_CLIENT_COMMIT="53f694fbbc41580ca50bc629bb4cba016e688b69"
FLASH_CLIENT_TMP=""
_cleanup_flash_client() {
  if [[ -n "$FLASH_CLIENT_TMP" ]]; then
    rm -rf "$FLASH_CLIENT_TMP"
  fi
}

if [[ -n "${XENOMIT_FLASH_CLIENT:-}" ]]; then
  FLASH_CLIENT="$XENOMIT_FLASH_CLIENT"
elif command -v git >/dev/null 2>&1; then
  FLASH_CLIENT_TMP="$(mktemp -d)"
  trap _cleanup_flash_client EXIT
  git -C "$FLASH_CLIENT_TMP" init -q
  git -C "$FLASH_CLIENT_TMP" remote add origin "$FLASH_CLIENT_REPO"
  git -C "$FLASH_CLIENT_TMP" fetch --depth 1 -q origin "refs/tags/${FLASH_CLIENT_TAG}^{}"
  checked_out="$(git -C "$FLASH_CLIENT_TMP" rev-parse FETCH_HEAD)"
  if [[ "$checked_out" != "$FLASH_CLIENT_COMMIT" ]]; then
    git -C "$FLASH_CLIENT_TMP" fetch --depth 1 -q origin "$FLASH_CLIENT_COMMIT"
    checked_out="$(git -C "$FLASH_CLIENT_TMP" rev-parse FETCH_HEAD)"
  fi
  if [[ "$checked_out" != "$FLASH_CLIENT_COMMIT" ]]; then
    echo "[xenomit-flash] expected xenomit-flash-client ${FLASH_CLIENT_TAG} (${FLASH_CLIENT_COMMIT}); got ${checked_out}" >&2
    exit 1
  fi
  git -C "$FLASH_CLIENT_TMP" checkout -q FETCH_HEAD
  FLASH_CLIENT="$FLASH_CLIENT_TMP"
else
  FLASH_CLIENT=""
fi

CMAKE_FLASH_CLIENT_ARGS=()
if [[ -n "$FLASH_CLIENT" ]]; then
  export XENOMIT_FLASH_CLIENT="$FLASH_CLIENT"
  CMAKE_FLASH_CLIENT_ARGS=(-DXENOMIT_FLASH_CLIENT="$FLASH_CLIENT")
fi

if command -v cmake >/dev/null 2>&1; then
  cmake -S "$ROOT/hook" -B "$ROOT/build/cmake" -DCMAKE_BUILD_TYPE=Release "${CMAKE_FLASH_CLIENT_ARGS[@]}"
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
