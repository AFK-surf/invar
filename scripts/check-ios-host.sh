#!/usr/bin/env bash
# Compile all 12 iOS Metal native host sources against the iOS Simulator SDK (arm64),
# archive them into libinvar_host_ios.a, and verify core exported symbols.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SDK="${INVAR_IOS_SDK:-iphonesimulator}"
ARCH="${INVAR_IOS_ARCH:-arm64}"
MIN_VER="${INVAR_IOS_MIN:-16.0}"
OUT_DIR="${INVAR_IOS_OUT:-$ROOT/target/ios-host}"
OBJ_DIR="$OUT_DIR/obj"
LIB="$OUT_DIR/libinvar_host_ios.a"

mkdir -p "$OBJ_DIR"
rm -f "$OBJ_DIR"/*.o "$LIB"

if [ "$SDK" = "iphoneos" ]; then
  MIN_FLAG="-miphoneos-version-min=$MIN_VER"
else
  MIN_FLAG="-mios-simulator-version-min=$MIN_VER"
fi

COMMON_FLAGS=(
  -arch "$ARCH"
  "$MIN_FLAG"
  -O2
  -Ipackages/host/compat
  -Ipackages/host/native
)

C_SOURCES=(
  "packages/hostapi/native/wake_bridge.c"
  "packages/host/compat/pion_mb_compat.c"
)

OBJC_SOURCES=(
  "packages/host/native/apple_render.m"
  "packages/host/native/apple_text.m"
  "packages/host/native/apple_image.m"
  "packages/host/native/apple_listen.m"
  "packages/host/native/apple_browser_login.m"
  "packages/host/native/apple_passwords.m"
  "packages/host/native/apple_views.m"
  "packages/host/native/apple_stream.m"
  "packages/host/native/apple_film.m"
  "packages/host/native/ios_app.m"
  "packages/host/native/ios_share.m"
)

OBJS=()

echo "==> iOS SDK: $SDK ($(xcrun --sdk "$SDK" --show-sdk-version))"
for src in "${C_SOURCES[@]}"; do
  base="$(basename "$src" .c)"
  obj="$OBJ_DIR/${base}.o"
  echo "  CC   $src"
  xcrun --sdk "$SDK" clang "${COMMON_FLAGS[@]}" -Wall -Wextra -Wno-unused-parameter -c "$src" -o "$obj"
  OBJS+=("$obj")
done

for src in "${OBJC_SOURCES[@]}"; do
  base="$(basename "$src" .m)"
  obj="$OBJ_DIR/${base}.o"
  echo "  OBJC $src"
  xcrun --sdk "$SDK" clang "${COMMON_FLAGS[@]}" -fobjc-arc -c "$src" -o "$obj"
  OBJS+=("$obj")
done

echo "==> Archiving ${#OBJS[@]} objects into $LIB"
xcrun --sdk "$SDK" ar rcs "$LIB" "${OBJS[@]}"
xcrun --sdk "$SDK" ranlib "$LIB"

echo "==> Verifying exported core symbols in $LIB"
NM_OUT="$(nm -gU "$LIB")"
REQUIRED_SYMS=(
  _mb_host_run
  _mb_host_present
  _mb_host_shape
  _mb_host_glass_light_angle
  _mb_host_layer_supported
  _mb_host_layer_render
  _mb_host_layer_destroy
  _mb_host_wake_now
  _mb_host_wake_now_ptr
  _invar_hostapi_set_native_wake
  _invar_hostapi_call_native_wake
  _pion_host_run
  _pion_host_present
  _pion_host_shape
  _pion_host_pump_main_loop
)

MISSING=0
for sym in "${REQUIRED_SYMS[@]}"; do
  if echo "$NM_OUT" | grep -q " T ${sym}$"; then
    echo "  OK   $sym"
  else
    echo "  MISS $sym" >&2
    MISSING=1
  fi
done

if [ "$MISSING" -ne 0 ]; then
  echo "ERROR: Missing required iOS host symbols!" >&2
  exit 1
fi

ls -lh "$LIB"
echo "==> iOS Metal host check PASSED (${#OBJS[@]} objects)"
