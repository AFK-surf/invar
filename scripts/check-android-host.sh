#!/usr/bin/env bash
# Verify the Android 6th-platform host layer:
#   1. Compile & run pure-JDK FramePacerTest (including IME frame coalescing cases)
#   2. Compile android_host.c (+ pion_mb_compat.c + wake_bridge.c) via Android NDK
#      aarch64-linux-android26-clang (or host clang -fsyntax-only with temp stubs)
#   3. Verify 100% 1-to-1 JNI method parity (Native.java <-> android_host.c) and
#      complete coverage of all host_native.pion / pion_mb_compat.c mb_host_* symbols
#   4. Brand hygiene gate (zero stale brand / internal identifiers)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "==> [1/4] Checking JDK & running FramePacerTest"
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
  PACER_OUT="$(mktemp -d /tmp/invar-pacer.XXXXXX)"
  trap 'rm -rf "$PACER_OUT" "${STUB_DIR:-}"' EXIT
  "$JAVAC_BIN" -d "$PACER_OUT" \
    android/app/src/main/java/dev/invar/FramePacer.java \
    android/pacer/FramePacerTest.java
  "$JAVA_BIN" -cp "$PACER_OUT" dev.invar.FramePacerTest
else
  echo "  SKIP: javac/java not found on this machine"
fi

echo "==> [2/4] Verifying android_host.c C/JNI compilation"
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
  "$NDK_CC" -O2 -Wall -Wextra -Wno-unused-parameter -Ipackages/host/compat -Ipackages/host/native \
    -c packages/hostapi/native/wake_bridge.c -o "$OBJ_DIR/wake_bridge.o"
  "$NDK_CC" -O2 -Wall -Wextra -Wno-unused-parameter -Ipackages/host/compat -Ipackages/host/native \
    -c packages/host/compat/pion_mb_compat.c -o "$OBJ_DIR/pion_mb_compat.o"
  "$NDK_CC" -O2 -Wall -Wno-unused-parameter -Wno-unused-function -Ipackages/host/compat -Ipackages/host/native \
    -c packages/host/native/android_host.c -o "$OBJ_DIR/android_host.o"

  "$NDK_AR" rcs "$LIB" "$OBJ_DIR/wake_bridge.o" "$OBJ_DIR/pion_mb_compat.o" "$OBJ_DIR/android_host.o"
  echo "  Archive built: $LIB"

  NM_OUT="$("$NDK_NM" -g --defined-only "$LIB")"
  REQUIRED_SYMS=(
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
    invar_hostapi_set_native_wake
    invar_hostapi_call_native_wake
    pion_host_run
    pion_host_present
    pion_host_shape
    Java_dev_invar_Native_start
    Java_dev_invar_Native_frame
    Java_dev_invar_Native_surfaceCreated
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
  echo "  NDK not found; running host clang -fsyntax-only with temporary Android stubs"
  STUB_DIR="$(mktemp -d /tmp/invar-android-stubs.XXXXXX)"
  trap 'rm -rf "${PACER_OUT:-}" "$STUB_DIR"' EXIT
  mkdir -p "$STUB_DIR/GLES3" "$STUB_DIR/android" "$STUB_DIR/sys"
  cat > "$STUB_DIR/jni.h" <<'EOF'
#pragma once
#include <stdint.h>
#define JNIEXPORT
#define JNICALL
#define JNI_VERSION_1_6 0x00010006
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_TRUE 1
#define JNI_FALSE 0
#define JNI_ABORT 2
typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;
typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jarray;
typedef jarray jbyteArray;
typedef jarray jintArray;
typedef jarray jdoubleArray;
typedef void *jmethodID;
typedef struct { jint version; const char *name; jobject group; } JavaVMAttachArgs;
struct JNINativeInterface_;
typedef const struct JNINativeInterface_ *JNIEnv;
struct JNIInvokeInterface_;
typedef const struct JNIInvokeInterface_ *JavaVM;
struct JNINativeInterface_ {
  jclass (*FindClass)(JNIEnv *, const char *);
  jobject (*NewGlobalRef)(JNIEnv *, jobject);
  void (*DeleteLocalRef)(JNIEnv *, jobject);
  jmethodID (*GetStaticMethodID)(JNIEnv *, jclass, const char *, const char *);
  jobject (*CallStaticObjectMethod)(JNIEnv *, jclass, jmethodID, ...);
  jboolean (*CallStaticBooleanMethod)(JNIEnv *, jclass, jmethodID, ...);
  jint (*CallStaticIntMethod)(JNIEnv *, jclass, jmethodID, ...);
  jfloat (*CallStaticFloatMethod)(JNIEnv *, jclass, jmethodID, ...);
  jdouble (*CallStaticDoubleMethod)(JNIEnv *, jclass, jmethodID, ...);
  void (*CallStaticVoidMethod)(JNIEnv *, jclass, jmethodID, ...);
  jstring (*NewString)(JNIEnv *, const jchar *, jsize);
  jstring (*NewStringUTF)(JNIEnv *, const char *);
  jsize (*GetStringLength)(JNIEnv *, jstring);
  void (*GetStringRegion)(JNIEnv *, jstring, jsize, jsize, jchar *);
  const char *(*GetStringUTFChars)(JNIEnv *, jstring, jboolean *);
  void (*ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
  jsize (*GetArrayLength)(JNIEnv *, jarray);
  jbyteArray (*NewByteArray)(JNIEnv *, jsize);
  jintArray (*NewIntArray)(JNIEnv *, jsize);
  jdoubleArray (*NewDoubleArray)(JNIEnv *, jsize);
  jbyte *(*GetByteArrayElements)(JNIEnv *, jbyteArray, jboolean *);
  void (*ReleaseByteArrayElements)(JNIEnv *, jbyteArray, jbyte *, jint);
  void (*GetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, jbyte *);
  void (*SetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, const jbyte *);
  void (*SetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, const jint *);
  void (*GetDoubleArrayRegion)(JNIEnv *, jdoubleArray, jsize, jsize, jdouble *);
  jboolean (*ExceptionCheck)(JNIEnv *);
  void (*ExceptionClear)(JNIEnv *);
};
struct JNIInvokeInterface_ {
  jint (*GetEnv)(JavaVM *, void **, jint);
  jint (*AttachCurrentThread)(JavaVM *, JNIEnv **, void *);
  jint (*DetachCurrentThread)(JavaVM *);
};
EOF
  cat > "$STUB_DIR/android/log.h" <<'EOF'
#pragma once
#define ANDROID_LOG_INFO 4
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
EOF
  cat > "$STUB_DIR/sys/system_properties.h" <<'EOF'
#pragma once
#define PROP_VALUE_MAX 92
int __system_property_get(const char *name, char *value);
EOF
  cat > "$STUB_DIR/GLES3/gl3.h" <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef unsigned char GLubyte;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_SRC_ALPHA 0x0302
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_RGBA 0x1908
#define GL_R8 0x8229
#define GL_RGBA8 0x8058
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_TEXTURE4 0x84C4
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_EXTENSIONS 0x1F03
#define GL_QUERY_RESULT 0x8866
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_BOX 0x0C10
void glActiveTexture(GLenum);
void glAttachShader(GLuint, GLuint);
void glBindBuffer(GLenum, GLuint);
void glBindFramebuffer(GLenum, GLuint);
void glBindTexture(GLenum, GLuint);
void glBindVertexArray(GLuint);
void glBlendFunc(GLenum, GLenum);
void glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum);
void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void glBufferData(GLenum, GLsizeiptr, const void *, GLenum);
void glBufferSubData(GLenum, GLintptr, GLsizeiptr, const void *);
GLenum glCheckFramebufferStatus(GLenum);
void glClear(GLbitfield);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glCompileShader(GLuint);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum);
void glDeleteBuffers(GLsizei, const GLuint *);
void glDeleteFramebuffers(GLsizei, const GLuint *);
void glDeleteProgram(GLuint);
void glDeleteShader(GLuint);
void glDeleteTextures(GLsizei, const GLuint *);
void glDeleteVertexArrays(GLsizei, const GLuint *);
void glDisable(GLenum);
void glDrawArrays(GLenum, GLint, GLsizei);
void glDrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei);
void glEnable(GLenum);
void glEnableVertexAttribArray(GLuint);
void glEndQuery(GLenum);
void glBeginQuery(GLenum, GLuint);
void glFinish(void);
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void glGenBuffers(GLsizei, GLuint *);
void glGenFramebuffers(GLsizei, GLuint *);
void glGenQueries(GLsizei, GLuint *);
void glGenTextures(GLsizei, GLuint *);
void glGenVertexArrays(GLsizei, GLuint *);
void glGetIntegerv(GLenum, GLint *);
void glGetProgramInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
void glGetProgramiv(GLuint, GLenum, GLint *);
void glGetQueryObjectuiv(GLuint, GLenum, GLuint *);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
void glGetShaderiv(GLuint, GLenum, GLint *);
const GLubyte *glGetString(GLenum);
GLint glGetUniformLocation(GLuint, const GLchar *);
void glLinkProgram(GLuint);
void glPixelStorei(GLenum, GLint);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
void glScissor(GLint, GLint, GLsizei, GLsizei);
void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void glTexParameteri(GLenum, GLenum, GLint);
void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
void glUniform1f(GLint, GLfloat);
void glUniform1i(GLint, GLint);
void glUniform2f(GLint, GLfloat, GLfloat);
void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void glUseProgram(GLuint);
void glVertexAttribDivisor(GLuint, GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
void glViewport(GLint, GLint, GLsizei, GLsizei);
EOF
  clang -fsyntax-only -Wall -Wno-unused-parameter -Wno-unused-function \
    -I"$STUB_DIR" -Ipackages/host/compat -Ipackages/host/native \
    packages/host/native/android_host.c
  echo "  OK   clang -fsyntax-only passed"
fi

echo "==> [3/4] Static JNI parity & mb_host_* symbol coverage check"
python3 - <<'PY'
import glob, re, sys

native_java = open("android/app/src/main/java/dev/invar/Native.java", encoding="utf-8").read()
android_c = open("packages/host/native/android_host.c", encoding="utf-8").read()

java_natives = set(re.findall(r"static\s+native\s+\S+\s+([A-Za-z0-9_]+)\s*\(", native_java))
c_natives = set(re.findall(r"Java_dev_invar_Native_([A-Za-z0-9_]+)\s*\(", android_c))

missing_in_c = sorted(java_natives - c_natives)
extra_in_c = sorted(c_natives - java_natives)
if missing_in_c or extra_in_c:
    print(f"ERROR: JNI mismatch! missing_in_c={missing_in_c}, extra_in_c={extra_in_c}", file=sys.stderr)
    sys.exit(1)
print(f"  OK   Native.java <-> android_host.c JNI 1:1 parity ({len(java_natives)} methods)")

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

echo "==> [4/4] Brand hygiene gate"
BAD_PATTERN='still\.fail|ui\.monoid|dev\.mbui|libmbui'
if grep -rnE "$BAD_PATTERN" android/ packages/host/native/android_host.c packages/host/native/android_device.h; then
  echo "ERROR: Found forbidden brand/internal identifiers!" >&2
  exit 1
fi
echo "  OK   Zero forbidden brand identifiers found"
echo "==> Android host check PASSED"
