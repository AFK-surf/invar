// Device capabilities beyond the window on Android: dev.invar.Device handles
// key catching, soft keyboard inset, clipboard, keep-awake, haptics, and IME action.
// Included at the end of android_host.c (uses env_, jstr, jbytes, mstr).

static jclass device_cls;
static jmethodID m_dv_catch, m_dv_inset, m_dv_copy_shown, m_dv_read, m_dv_state, m_dv_picture, m_dv_text, m_dv_png,
    m_dv_end, m_dv_set, m_dv_awake, m_dv_haptic, m_dv_ime_action;

static void device_init(JNIEnv *env) {
  device_cls = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "dev/invar/Device"));
  m_dv_catch = (*env)->GetStaticMethodID(env, device_cls, "keyCatch", "(Z)V");
  m_dv_inset = (*env)->GetStaticMethodID(env, device_cls, "imeInset", "()D");
  m_dv_copy_shown = (*env)->GetStaticMethodID(env, device_cls, "copyShown", "()Z");
  m_dv_read = (*env)->GetStaticMethodID(env, device_cls, "clipRead", "()I");
  m_dv_state = (*env)->GetStaticMethodID(env, device_cls, "clipState", "(I)I");
  m_dv_picture = (*env)->GetStaticMethodID(env, device_cls, "clipPicture", "(I)I");
  m_dv_text = (*env)->GetStaticMethodID(env, device_cls, "clipText", "(I)Ljava/lang/String;");
  m_dv_png = (*env)->GetStaticMethodID(env, device_cls, "clipPng", "(I)[B");
  m_dv_end = (*env)->GetStaticMethodID(env, device_cls, "clipEnd", "(I)V");
  m_dv_set = (*env)->GetStaticMethodID(env, device_cls, "clipSet", "(Ljava/lang/String;[BLjava/lang/String;)V");
  m_dv_awake = (*env)->GetStaticMethodID(env, device_cls, "keepAwake", "(Z)V");
  m_dv_haptic = (*env)->GetStaticMethodID(env, device_cls, "haptic", "(I)V");
  m_dv_ime_action = (*env)->GetStaticMethodID(env, device_cls, "imeAction", "(I)V");
}

void mb_host_key_catch(int32_t on) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return;
  (*env)->CallStaticVoidMethod(env, device_cls, m_dv_catch, on ? JNI_TRUE : JNI_FALSE);
}

double mb_host_ime_inset(void) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return 0.0;
  return (*env)->CallStaticDoubleMethod(env, device_cls, m_dv_inset);
}

int32_t mb_host_copy_shown(void) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return 0;
  return (*env)->CallStaticBooleanMethod(env, device_cls, m_dv_copy_shown) ? 1 : 0;
}

int32_t mb_host_clip_read(void) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return 0;
  return (*env)->CallStaticIntMethod(env, device_cls, m_dv_read);
}

int32_t mb_host_clip_state(int32_t id) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return -1;
  return (*env)->CallStaticIntMethod(env, device_cls, m_dv_state, id);
}

int32_t mb_host_clip_picture(int32_t id) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return 0;
  return (*env)->CallStaticIntMethod(env, device_cls, m_dv_picture, id);
}

moonbit_string_t mb_host_clip_text(int32_t id) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return moonbit_make_string_raw(0);
  jstring s = (jstring)(*env)->CallStaticObjectMethod(env, device_cls, m_dv_text, id);
  moonbit_string_t out = s ? mstr(env, s) : moonbit_make_string_raw(0);
  if (s) (*env)->DeleteLocalRef(env, s);
  return out;
}

moonbit_bytes_t mb_host_clip_png(int32_t id) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return moonbit_make_bytes(0, 0);
  jbyteArray a = (jbyteArray)(*env)->CallStaticObjectMethod(env, device_cls, m_dv_png, id);
  jsize n = a ? (*env)->GetArrayLength(env, a) : 0;
  moonbit_bytes_t out = moonbit_make_bytes(n, 0);
  if (n) (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)out);
  if (a) (*env)->DeleteLocalRef(env, a);
  return out;
}

int32_t mb_host_clip_files(int32_t id) {
  (void)id;
  return 0;
}

moonbit_string_t mb_host_clip_file_name(int32_t id, int32_t i) {
  (void)id;
  (void)i;
  return moonbit_make_string_raw(0);
}

moonbit_bytes_t mb_host_clip_file(int32_t id, int32_t i) {
  (void)id;
  (void)i;
  return moonbit_make_bytes(0, 0);
}

void mb_host_clip_end(int32_t id) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return;
  (*env)->CallStaticVoidMethod(env, device_cls, m_dv_end, id);
}

void mb_host_clip_set(moonbit_string_t text, moonbit_bytes_t png, moonbit_string_t label) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return;
  jstring t = jstr(env, text), l = jstr(env, label);
  jbyteArray b = jbytes(env, png);
  (*env)->CallStaticVoidMethod(env, device_cls, m_dv_set, t, b, l);
  (*env)->DeleteLocalRef(env, t);
  (*env)->DeleteLocalRef(env, l);
  (*env)->DeleteLocalRef(env, b);
}

void mb_host_keep_awake(int32_t on) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return;
  (*env)->CallStaticVoidMethod(env, device_cls, m_dv_awake, on ? JNI_TRUE : JNI_FALSE);
}

void mb_host_haptic(int32_t kind) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return;
  (*env)->CallStaticVoidMethod(env, device_cls, m_dv_haptic, kind);
}

void mb_host_ime_action(int32_t kind) {
  JNIEnv *env = env_();
  if (!env || !device_cls) return;
  (*env)->CallStaticVoidMethod(env, device_cls, m_dv_ime_action, kind);
}

// Generic native call extension points (no app-specific native bridge in base host).
int32_t mb_host_native_call(moonbit_string_t name, moonbit_string_t params) {
  (void)name;
  (void)params;
  return 0;
}

int32_t mb_host_native_state(int32_t id) {
  (void)id;
  return -1;
}

moonbit_string_t mb_host_native_result(int32_t id) {
  (void)id;
  return moonbit_make_string_raw(0);
}

void mb_host_native_end(int32_t id) {
  (void)id;
}
