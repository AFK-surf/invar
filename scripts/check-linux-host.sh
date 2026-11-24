#!/usr/bin/env bash
# Compile all 10 Linux EGL/GLES3 + GTK4 + Pango/Cairo native host C sources,
# archive them into libinvar_host_linux.a, verify core exported symbols,
# and optionally run pionc2 check + test against packages/host.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CC="${CC:-cc}"
OUT_DIR="${INVAR_LINUX_OUT:-$ROOT/target/linux-host}"
OBJ_DIR="$OUT_DIR/obj"
LIB="$OUT_DIR/libinvar_host_linux.a"

PKG_LIBS="gtk4 epoxy pangocairo pangoft2 fontconfig gdk-pixbuf-2.0 libsoup-3.0 zlib"
if ! pkg-config --exists $PKG_LIBS; then
  echo "ERROR: Missing Linux dev libraries. Required pkg-config modules:" >&2
  echo "  $PKG_LIBS" >&2
  echo "On Debian/Ubuntu install: sudo apt-get install -y libgtk-4-dev libsoup-3.0-dev libepoxy-dev" >&2
  exit 1
fi

PKG_CFLAGS="$(pkg-config --cflags $PKG_LIBS)"

mkdir -p "$OBJ_DIR"
rm -f "$OBJ_DIR"/*.o "$LIB"

C_SOURCES=(
  "packages/hostapi/native/wake_bridge.c"
  "packages/host/compat/pion_mb_compat.c"
  "packages/host/native/linux_render.c"
  "packages/host/native/linux_text.c"
  "packages/host/native/linux_image.c"
  "packages/host/native/linux_listen.c"
  "packages/host/native/linux_browser_login.c"
  "packages/host/native/linux_views.c"
  "packages/host/native/linux_stream.c"
  "packages/host/native/linux_film.c"
  "packages/host/native/linux_app.c"
)

OBJS=()
echo "==> Linux host CC=$CC ($(uname -m))"
for src in "${C_SOURCES[@]}"; do
  base="$(basename "$src" .c)"
  obj="$OBJ_DIR/${base}.o"
  echo "  CC   $src"
  if [ "$base" = "pion_mb_compat" ] || [ "$base" = "wake_bridge" ]; then
    $CC -O2 -Wall -Wextra -Wno-unused-parameter -Ipackages/host/compat -Ipackages/host/native -c "$src" -o "$obj"
  else
    $CC -O2 -Ipackages/host/compat -Ipackages/host/native $PKG_CFLAGS -c "$src" -o "$obj"
  fi
  OBJS+=("$obj")
done

echo "==> Archiving ${#OBJS[@]} objects into $LIB"
ar rcs "$LIB" "${OBJS[@]}"
ranlib "$LIB"

echo "==> Verifying exported core symbols in $LIB"
NM_OUT="$(nm -g --defined-only "$LIB")"
REQUIRED_SYMS=(
  mb_host_run
  mb_host_present
  mb_host_shape
  mb_host_glass_light_angle
  mb_host_layer_supported
  mb_host_layer_render
  mb_host_layer_destroy
  mb_host_wake_now
  mb_host_wake_now_ptr
  invar_hostapi_set_native_wake
  invar_hostapi_call_native_wake
  mbf_supported
  mbf_snapshot
  mbf_open
  mb_render_init_offscreen
  mb_render_present
  mb_render_snapshot
  pion_host_run
  pion_host_present
  pion_host_shape
  pion_host_pump_main_loop
)

MISSING=0
for sym in "${REQUIRED_SYMS[@]}"; do
  if printf '%s\n' "$NM_OUT" | grep " T ${sym}$" >/dev/null; then
    echo "  OK   $sym"
  else
    echo "  MISS $sym" >&2
    MISSING=1
  fi
done

if [ "$MISSING" -ne 0 ]; then
  echo "ERROR: Missing required Linux host symbols!" >&2
  exit 1
fi

ls -lh "$LIB"
echo "==> Linux EGL/GLES3 host archive check PASSED (${#OBJS[@]} objects)"

echo "==> Kept groups (groups.h) unit test"
$CC -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Ipackages/host/native packages/host/native/groups_test.c -o "$OUT_DIR/groups_test" -lm
"$OUT_DIR/groups_test"

PIONC="${PIONC:-$(command -v pionc2 || true)}"
if [ -n "$PIONC" ]; then
  echo "==> Running $PIONC test packages/damage/parity (groups.h against damage::Groups)"
  "$PIONC" test packages/damage/parity
  echo "==> Running $PIONC check packages/host"
  "$PIONC" check packages/host
  echo "==> Running $PIONC test --package host packages/host (Mesa llvmpipe)"
  LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe "$PIONC" test --package host packages/host
  echo "==> Running $PIONC check packages/film"
  "$PIONC" check packages/film
  echo "==> Running $PIONC test --package film packages/film (Mesa llvmpipe)"
  LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe "$PIONC" test --package film packages/film
fi
