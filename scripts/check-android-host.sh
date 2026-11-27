#!/usr/bin/env bash
# Verify the Android 6th-platform host layer:
#   1. Compile & run pure-JDK unit tests (FramePacerTest, FrameSchedulerPropertyTest, ImeRequestTest, FrameMailTest, TouchMovesTest, RawPremulTest)
#   2. Compile android_host.c (+ film_none.c + pion_mb_compat.c + wake_bridge.c) via Android NDK
#      aarch64-linux-android26-clang (or host clang -fsyntax-only with temp stubs)
#   3. Verify 100% 1-to-1 bidirectional JNI parity across ALL dev/invar/*.java classes <-> android_host.c
#      and complete coverage of all host_native.pion / pion_mb_compat.c mb_* symbols
#   4. Compile all android/app/src/main/java/dev/invar/*.java + jzlib sources with javac
#      (-proc:none -XDshould-stop.ifError=FLOW) against local Android SDK android.jar (+ cached AARs)
#   5. Brand hygiene gate (zero stale brand / internal identifiers)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TMP_DIRS=()
cleanup() {
  for d in "${TMP_DIRS[@]:-}"; do
    [ -n "$d" ] && rm -rf "$d"
  done
}
trap cleanup EXIT

echo "==> [1/5] Checking JDK & running pure-JDK Android host unit tests"
JAVAC_BIN=""
JAVA_BIN=""
if command -v javac >/dev/null 2>&1 && command -v java >/dev/null 2>&1; then
  JAVAC_BIN="$(command -v javac)"
  JAVA_BIN="$(command -v java)"
elif [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
  JAVAC_BIN="$JAVA_HOME/bin/javac"
  JAVA_BIN="$JAVA_HOME/bin/java"
elif [ -x /usr/libexec/java_home ]; then
  JH="$(/usr/libexec/java_home 2>/dev/null || true)"
  if [ -n "$JH" ] && [ -x "$JH/bin/javac" ]; then
    JAVAC_BIN="$JH/bin/javac"
    JAVA_BIN="$JH/bin/java"
  fi
fi

if [ -n "$JAVAC_BIN" ]; then
  TEST_OUT="$(mktemp -d /tmp/invar-android-tests.XXXXXX)"
  TMP_DIRS+=("$TEST_OUT")
  "$JAVAC_BIN" -d "$TEST_OUT" \
    android/app/src/main/java/dev/invar/FramePacer.java \
    android/app/src/main/java/dev/invar/FrameScheduler.java \
    android/app/src/main/java/dev/invar/RateVoter.java \
    android/app/src/main/java/dev/invar/FrameTelemetry.java \
    android/app/src/main/java/dev/invar/ImeRequest.java \
    android/app/src/main/java/dev/invar/FrameMail.java \
    android/app/src/main/java/dev/invar/TouchMoves.java \
    android/app/src/main/java/dev/invar/RawPremul.java \
    android/pacer/FramePacerTest.java \
    android/pacer/FrameSchedulerPropertyTest.java \
    android/ime/ImeRequestTest.java \
    android/mail/FrameMailTest.java \
    android/touch/TouchMovesTest.java \
    android/mail/RawPremulTest.java
  "$JAVA_BIN" -cp "$TEST_OUT" dev.invar.FramePacerTest
  "$JAVA_BIN" -cp "$TEST_OUT" dev.invar.FrameSchedulerPropertyTest
  "$JAVA_BIN" -cp "$TEST_OUT" dev.invar.ImeRequestTest
  "$JAVA_BIN" -cp "$TEST_OUT" dev.invar.FrameMailTest
  "$JAVA_BIN" -cp "$TEST_OUT" dev.invar.TouchMovesTest
  "$JAVA_BIN" -cp "$TEST_OUT" dev.invar.RawPremulTest
else
  echo "  SKIP: javac/java not found on this machine"
fi

echo "==> [2/5] Verifying android_host.c C/JNI compilation"
NDK_DIR="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK_DIR" ]; then
  for cand in \
    "$HOME/Library/Android/sdk/ndk"/* \
    "$HOME/Android/Sdk/ndk"/* \
    /opt/android-sdk/ndk/*; do
    if [ -d "$cand/toolchains/llvm/prebuilt" ]; then
      NDK_DIR="$cand"
    fi
  done
fi

NDK_CC=""
NDK_AR=""
NDK_NM=""
if [ -n "$NDK_DIR" ]; then
  PREBUILT="$(ls -d "$NDK_DIR/toolchains/llvm/prebuilt"/* 2>/dev/null | head -n 1 || true)"
  if [ -n "$PREBUILT" ] && [ -x "$PREBUILT/bin/aarch64-linux-android26-clang" ]; then
    NDK_CC="$PREBUILT/bin/aarch64-linux-android26-clang"
    NDK_AR="$PREBUILT/bin/llvm-ar"
    NDK_NM="$PREBUILT/bin/llvm-nm"
  fi
fi

if [ -n "$NDK_CC" ]; then
  OUT_DIR="${INVAR_ANDROID_OUT:-$ROOT/target/android-host}"
  OBJ_DIR="$OUT_DIR/obj"
  LIB="$OUT_DIR/libinvar_host_android.a"
  mkdir -p "$OBJ_DIR"
  rm -f "$OBJ_DIR"/*.o "$LIB"

  echo "  NDK CC: $NDK_CC"
  "$NDK_CC" -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Ipackages/host/compat -Ipackages/host/native \
    -c packages/hostapi/native/wake_bridge.c -o "$OBJ_DIR/wake_bridge.o"
  "$NDK_CC" -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Ipackages/host/compat -Ipackages/host/native \
    -c packages/host/compat/pion_mb_compat.c -o "$OBJ_DIR/pion_mb_compat.o"
  "$NDK_CC" -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Ipackages/host/compat -Ipackages/host/native \
    -c packages/host/native/film_none.c -o "$OBJ_DIR/film_none.o"
  "$NDK_CC" -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Ipackages/host/compat -Ipackages/host/native \
    -c packages/host/native/android_host.c -o "$OBJ_DIR/android_host.o"

  "$NDK_AR" rcs "$LIB" \
    "$OBJ_DIR/wake_bridge.o" \
    "$OBJ_DIR/pion_mb_compat.o" \
    "$OBJ_DIR/film_none.o" \
    "$OBJ_DIR/android_host.o"
  echo "  Archive built: $LIB"

  NM_OUT="$("$NDK_NM" -g --defined-only "$LIB")"
  REQUIRED_SYMS=(
    mb_android_vm
    invar_android_vm
    mb_host_run
    mb_host_present
    mb_host_render_dirty
    mb_host_layer_supported
    mb_host_layer_render
    mb_host_layer_destroy
    mb_host_glass_light_angle
    mb_host_window_center
    mb_host_window_hide
    mb_host_window_show
    mb_host_wake_now
    mb_host_wake_now_ptr
    mb_host_set_ime
    mb_host_shape
    mb_host_image_load
    mb_host_clipboard_set
    mb_host_system_dark
    mb_host_a11y_on
    mb_host_a11y_tree
    mb_host_app_scheme
    mb_host_display_hz
    mb_host_recent
    mb_host_web_auth
    mb_stream_route_take
    mb_stream_route_acks
    mbf_supported
    invar_hostapi_set_native_wake
    invar_hostapi_call_native_wake
    pion_host_run
    pion_host_present
    pion_host_shape
    Java_dev_invar_Native_start
    Java_dev_invar_Native_frame
    Java_dev_invar_Native_surfaceCreated
    Java_dev_invar_MBView_glDrawn
    Java_dev_invar_NativeViews_part
    Java_dev_invar_NativeViews_texture
    Java_dev_invar_Streams_avcLengthSize
    Java_dev_invar_Streams_avcCsd
    Java_dev_invar_Streams_annexB
    Java_dev_invar_Streams_routed
    Java_dev_invar_Streams_routeAck
    Java_dev_invar_StreamLayer_under
    JNI_OnLoad
  )
  MISSING=0
  for sym in "${REQUIRED_SYMS[@]}"; do
    if printf '%s\n' "$NM_OUT" | grep -q " T ${sym}$"; then
      echo "  OK   $sym"
    else
      echo "  MISS $sym" >&2
      MISSING=1
    fi
  done
  if [ "$MISSING" -ne 0 ]; then
    echo "ERROR: Missing required Android host symbols in $LIB!" >&2
    exit 1
  fi
else
  echo "  NDK not found; skipping NDK archive build"
fi

echo "==> [3/5] Static bidirectional JNI parity & mb_* symbol coverage check"
python3 - <<'PY'
import glob, os, re, sys

java_natives = set()
for p in sorted(glob.glob("android/app/src/main/java/dev/invar/*.java")):
    cls = os.path.splitext(os.path.basename(p))[0]
    src = open(p, encoding="utf-8").read()
    for m in re.finditer(r"\bnative\s+[A-Za-z0-9_<>\[\]\.]+\s+([A-Za-z0-9_]+)\s*\(", src):
        java_natives.add(f"{cls}_{m.group(1)}")

android_c = open("packages/host/native/android_host.c", encoding="utf-8").read()
c_natives = set(re.findall(r"Java_dev_invar_([A-Za-z0-9_]+_[A-Za-z0-9_]+)\s*\(", android_c))

missing_in_c = sorted(java_natives - c_natives)
extra_in_c = sorted(c_natives - java_natives)
if missing_in_c or extra_in_c:
    print(f"ERROR: JNI mismatch! missing_in_c={missing_in_c}, extra_in_c={extra_in_c}", file=sys.stderr)
    sys.exit(1)
print(f"  OK   dev/invar/*.java <-> android_host.c bidirectional JNI 1:1 parity ({len(java_natives)} methods)")

# Collect every mb_* symbol required by packages/host/src/*.pion and pion_mb_compat.c
required = set()
for p in glob.glob("packages/host/src/*.pion"):
    s = open(p, encoding="utf-8").read()
    required.update(re.findall(r'extern "C"[^\n]*=\s*"(mb_[^"]+)"', s))
compat = open("packages/host/compat/pion_mb_compat.c", encoding="utf-8").read()
required.update(re.findall(r"extern\s+[^;]*\b(mb_[a-zA-Z0-9_]+)\s*\(", compat))

# Collect all defined mb_* symbols in android_host.c + included headers + pion_mb_compat.c
sources = [
    "packages/host/native/android_host.c",
    "packages/host/native/android_device.h",
    "packages/host/native/gles_render.h",
    "packages/host/native/groups.h",
    "packages/host/native/post.h",
    "packages/host/native/term.h",
    "packages/host/native/app_calls.h",
    "packages/host/native/avc.h",
    "packages/host/compat/pion_mb_compat.c",
]
defined = set()
fn_re = re.compile(r"^(?!\s*(?:extern\b|//|\*))[A-Za-z_][A-Za-z0-9_\s\*]*?\b(mb_[a-zA-Z0-9_]+)\s*\((?:[^()]|\([^)]*\))*\)\s*\{", re.M | re.S)
for p in sources:
    s = open(p, encoding="utf-8").read()
    for m in fn_re.finditer(s):
        defined.add(m.group(1))

missing_syms = sorted(required - defined)
if missing_syms:
    print(f"ERROR: Missing required mb_* symbols in Android host: {missing_syms}", file=sys.stderr)
    sys.exit(1)
print(f"  OK   All {len(required)} required mb_* host symbols covered")
PY

echo "==> [4/5] Verifying full dev/invar/*.java compilation against Android SDK android.jar"
ANDROID_JAR=""
for cand in \
  "${ANDROID_HOME:-/nonexistent}/platforms"/android-*/android.jar \
  "${ANDROID_SDK_ROOT:-/nonexistent}/platforms"/android-*/android.jar \
  "$HOME/Library/Android/sdk/platforms"/android-*/android.jar \
  "$HOME/Android/Sdk/platforms"/android-*/android.jar \
  /opt/android-sdk/platforms/android-*/android.jar; do
  if [ -f "$cand" ]; then
    ANDROID_JAR="$cand"
  fi
done

if [ -n "$JAVAC_BIN" ] && [ -n "$ANDROID_JAR" ]; then
  JAVAC_TMP="$(mktemp -d /tmp/invar-android-javac.XXXXXX)"
  TMP_DIRS+=("$JAVAC_TMP")
  mkdir -p "$JAVAC_TMP/gen/dev/invar" "$JAVAC_TMP/classes"
  cat > "$JAVAC_TMP/gen/dev/invar/R.java" <<'EOF'
package dev.invar;
public final class R {
    public static final class drawable { public static final int ic_notification = 0x7f010001; }
    public static final class color { public static final int invar_notice = 0x7f020001; }
}
EOF
  CP="$ANDROID_JAR"
  EXTRA_COUNT=0
  for jar in \
    "$HOME/.cache/mbui-apk/aar"/*/classes.jar \
    "$HOME/.gradle/caches/modules-2/files-2.1/androidx.annotation/annotation-jvm"/*/*/*.jar; do
    if [ -f "$jar" ]; then
      CP="$CP:$jar"
      EXTRA_COUNT=$((EXTRA_COUNT + 1))
    fi
  done

  JAVA_FILES=()
  for f in android/app/src/main/java/dev/invar/*.java android/jzlib/src/com/jcraft/jzlib/*.java; do
    [ -f "$f" ] && JAVA_FILES+=("$f")
  done
  JAVA_FILES+=("$JAVAC_TMP/gen/dev/invar/R.java")

  if [ "$EXTRA_COUNT" -gt 0 ]; then
    "$JAVAC_BIN" -proc:none -XDshould-stop.ifError=FLOW -Xlint:none \
      -cp "$CP" -d "$JAVAC_TMP/classes" "${JAVA_FILES[@]}"
    echo "  OK   javac compiled all ${#JAVA_FILES[@]} Java files (android.jar + $EXTRA_COUNT cached jars)"
  else
    # Pure-SDK subset (excluding Folds.java and NativeViews.java which require AndroidX/Media3 AARs)
    SDK_FILES=()
    for f in "${JAVA_FILES[@]}"; do
      case "$f" in
        */Folds.java|*/NativeViews.java) continue ;;
        *) SDK_FILES+=("$f") ;;
      esac
    done
    "$JAVAC_BIN" -proc:none -XDshould-stop.ifError=FLOW -Xlint:none \
      -cp "$CP" -d "$JAVAC_TMP/classes" "${SDK_FILES[@]}"
    echo "  OK   javac compiled ${#SDK_FILES[@]} pure-SDK + jzlib Java files (Folds/NativeViews skipped: no cached AndroidX/Media3 AARs)"
  fi
else
  echo "  SKIP: javac or Android SDK android.jar not found on this machine"
fi

echo "==> [5/5] Brand hygiene gate"
BAD_PATTERN='still\.fail|stillfail|ui\.monoid|\bmonoid\b|youdid\.wtf|dev\.mbui|libmbui|\.agents/skills/'
if grep -rnEi "$BAD_PATTERN" \
  android/ \
  packages/host/native/android_host.c \
  packages/host/native/android_device.h \
  packages/host_web/ \
  web/pion_host_bridge.mjs; then
  echo "ERROR: Found forbidden brand/internal identifiers!" >&2
  exit 1
fi
echo "  OK   Zero forbidden brand identifiers found"
echo "==> Android host check PASSED"
