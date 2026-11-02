// Android platform layer on android_native_app_glue.
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/thermal.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "platform/platform.h"

int app_main(const std::vector<std::string>& args);

static android_app* s_app = nullptr;
// TERM_WINDOW pre_exec done, post_exec deferred until the caller destroyed its surface.
static bool s_term_pending = false;

// APP_CMD_PAUSE .. APP_CMD_RESUME: in the background (platform_visible false).
static bool s_paused = false;

// Visibility from the lifecycle commands, from platform_pump and from glue processing (onAppCmd).
static void track_cmd(int8_t cmd) {
  if (cmd == APP_CMD_PAUSE) { s_paused = true; }
  if (cmd == APP_CMD_RESUME) { s_paused = false; }
}

static void on_app_cmd(android_app*, int32_t cmd) { track_cmd((int8_t)cmd); }

// Completes the deferred TERM_WINDOW (glue sets app->window = NULL and wakes the Java thread).
static void finish_term() {
  if (!s_term_pending) { return; }
  s_term_pending = false;
  android_app_post_exec_cmd(s_app, APP_CMD_TERM_WINDOW);
}

// Touch -> pointer queue (glue onInputEvent, app thread, from process_input in any poll). Follows the first finger
// down until it lifts; other fingers are ignored. Motion events are consumed (1). Back (key or gesture) is consumed and
// reported as PXB_EV_BACK on release (the app decides: stop the run, or leave the menu and finish); other keys keep the
// default handling (0).
static int32_t s_touch_id = -1; // pointer id of the tracked finger, -1 = none
static bool s_back = false;     // Back released since the last platform_pump

static void push_touch(PlatformPointerEvent::Kind kind, const AInputEvent* e, size_t i, bool down = false) {
  PlatformPointerEvent pe;
  pe.kind = kind;
  pe.touch = true;
  pe.down = down;
  pe.x = AMotionEvent_getX(e, i);
  pe.y = AMotionEvent_getY(e, i);
  platform_pointer_push(pe);
}

static int32_t on_input_event(android_app*, AInputEvent* e) {
  if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
    if (AKeyEvent_getKeyCode(e) != AKEYCODE_BACK) { return 0; }
    if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP && !(AKeyEvent_getFlags(e) & AKEY_EVENT_FLAG_CANCELED)) {
      s_back = true;
    }
    return 1;
  }
  if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) { return 0; }
  int32_t action = AMotionEvent_getAction(e);
  int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
  size_t index =
      (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  if (masked == AMOTION_EVENT_ACTION_DOWN) {
    s_touch_id = AMotionEvent_getPointerId(e, 0);
    push_touch(PlatformPointerEvent::pos, e, 0);
    push_touch(PlatformPointerEvent::button, e, 0, true);
    return 1;
  }
  size_t count = AMotionEvent_getPointerCount(e), tracked = count;
  for (size_t i = 0; i < count && s_touch_id >= 0; i++) {
    if (AMotionEvent_getPointerId(e, i) == s_touch_id) { tracked = i; }
  }
  if (tracked == count) { return 1; }
  if (masked == AMOTION_EVENT_ACTION_MOVE) {
    push_touch(PlatformPointerEvent::pos, e, tracked);
  } else if (masked == AMOTION_EVENT_ACTION_UP || (masked == AMOTION_EVENT_ACTION_POINTER_UP && index == tracked)) {
    push_touch(PlatformPointerEvent::pos, e, tracked);
    push_touch(PlatformPointerEvent::button, e, tracked, false);
    push_touch(PlatformPointerEvent::leave, e, tracked);
    s_touch_id = -1;
  } else if (masked == AMOTION_EVENT_ACTION_CANCEL) { // gesture taken away: leave first, so the release clicks nothing
    push_touch(PlatformPointerEvent::leave, e, tracked);
    push_touch(PlatformPointerEvent::button, e, tracked, false);
    s_touch_id = -1;
  }
  return 1;
}

// One poll step with normal glue processing.
static void poll_normal(int timeout) {
  android_poll_source* src = nullptr;
  int id = ALooper_pollOnce(timeout, nullptr, nullptr, (void**)&src);
  if (id >= 0 && src) { src->process(s_app, src); }
}

// stdout/stderr -> logcat: a pipe per stream, a detached thread forwards each line.
static void redirect_to_logcat(FILE* f, int fd, int prio) {
  setvbuf(f, nullptr, _IOLBF, 0);
  int p[2];
  if (pipe(p) != 0) { return; }
  dup2(p[1], fd);
  close(p[1]);
  std::thread([rd = p[0], prio] {
    std::string line;
    char buf[1024];
    ssize_t n;
    while ((n = read(rd, buf, sizeof(buf))) > 0) {
      for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') {
          __android_log_print(prio, "pxbenchy", "%s", line.c_str());
          line.clear();
        } else {
          line += buf[i];
        }
      }
    }
  }).detach();
}

// Intent string extra "args" (am start --es args "..."), split on spaces/tabs. Missing extra → empty.
static std::vector<std::string> intent_args(ANativeActivity* act) {
  std::vector<std::string> out;
  JNIEnv* env = nullptr;
  if (act->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return out; }
  jclass act_cls = env->GetObjectClass(act->clazz);
  jobject intent =
      env->CallObjectMethod(act->clazz, env->GetMethodID(act_cls, "getIntent", "()Landroid/content/Intent;"));
  if (intent) {
    jclass intent_cls = env->GetObjectClass(intent);
    jmethodID get_extra = env->GetMethodID(intent_cls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring key = env->NewStringUTF("args");
    jstring val = (jstring)env->CallObjectMethod(intent, get_extra, key);
    if (val) {
      const char* c = env->GetStringUTFChars(val, nullptr);
      std::string cur;
      for (const char* p = c;; p++) {
        if (*p && *p != ' ' && *p != '\t') {
          cur += *p;
          continue;
        }
        if (!cur.empty()) { out.push_back(cur); }
        cur.clear();
        if (!*p) { break; }
      }
      env->ReleaseStringUTFChars(val, c);
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  act->vm->DetachCurrentThread();
  return out;
}

void android_main(android_app* app) {
  s_app = app;
  // a relaunch can reuse the cached process: drop input state left by the previous activity
  s_term_pending = false;
  s_touch_id = -1;
  s_back = false;
  s_paused = false;
  std::vector<PlatformPointerEvent> stale;
  platform_pointer_events(&stale);
  app->onInputEvent = on_input_event;
  app->onAppCmd = on_app_cmd;
  redirect_to_logcat(stdout, 1, ANDROID_LOG_INFO);
  redirect_to_logcat(stderr, 2, ANDROID_LOG_ERROR);
  int rc = app_main(intent_args(app->activity));
  finish_term();
  fflush(stderr);
  // last line of a run (the process stays cached after finish, so tools wait for this instead of the pid)
  printf("exit code: %d\n", rc);
  fflush(stdout);
  ANativeActivity_finish(app->activity);
  while (!app->destroyRequested) { poll_normal(-1); }
}

bool platform_init(int, int, const char*) {
  ANativeActivity_setWindowFlags(s_app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
  while (!s_app->window) {
    if (s_app->destroyRequested) { return false; }
    poll_normal(-1);
  }
  return true;
}

void platform_shutdown() { finish_term(); }

void* platform_surface_handle() { return s_term_pending ? nullptr : s_app->window; }

void platform_drawable_size(uint32_t* w, uint32_t* h) {
  ANativeWindow* win = s_term_pending ? nullptr : s_app->window;
  *w = win ? (uint32_t)ANativeWindow_getWidth(win) : 0;
  *h = win ? (uint32_t)ANativeWindow_getHeight(win) : 0;
}

uint32_t platform_pump() {
  finish_term();
  uint32_t ev = 0;
  for (;;) {
    android_poll_source* src = nullptr;
    int id = ALooper_pollOnce(0, nullptr, nullptr, (void**)&src);
    if (id < 0) { break; }
    if (!src) { continue; }
    if (id != LOOPER_ID_MAIN) {
      src->process(s_app, src);
      continue;
    }
    int8_t cmd = android_app_read_cmd(s_app);
    android_app_pre_exec_cmd(s_app, cmd);
    track_cmd(cmd);
    if (cmd == APP_CMD_TERM_WINDOW) {
      // post_exec deferred: the Java side stays blocked until the caller dropped its VkSurface
      s_term_pending = true;
      return ev | PXB_EV_SURFACE_LOST;
    }
    if (cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONFIG_CHANGED) { ev |= PXB_EV_RESIZED; }
    android_app_post_exec_cmd(s_app, cmd);
  }
  if (s_app->destroyRequested) { ev |= PXB_EV_QUIT; }
  if (s_back) { ev |= PXB_EV_BACK; }
  s_back = false;
  return ev;
}

bool platform_visible() { return !s_paused; }

bool platform_wait_surface() {
  finish_term();
  while (!s_app->window) {
    if (s_app->destroyRequested) { return false; }
    poll_normal(-1);
  }
  return true;
}

bool platform_read_file(const char* path, std::string* out) {
  AAsset* a = AAssetManager_open(s_app->activity->assetManager, path, AASSET_MODE_BUFFER);
  if (!a) { return false; }
  out->resize((size_t)AAsset_getLength64(a));
  bool ok = AAsset_read(a, out->data(), out->size()) == (int)out->size();
  AAsset_close(a);
  return ok;
}

std::vector<std::string> platform_list_scripts() {
  std::vector<std::string> out;
  AAssetDir* d = AAssetManager_openDir(s_app->activity->assetManager, "scripts"); // files only, no subdirectories
  if (!d) { return out; }
  while (const char* f = AAssetDir_getNextFileName(d)) {
    std::string name = f;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) { out.push_back("scripts/" + name); }
  }
  AAssetDir_close(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::pair<std::string, std::string>> platform_device_info() {
  static const char* const props[][2] = {
      {"manufacturer", "ro.product.manufacturer"},   {"model", "ro.product.model"},
      {"soc_manufacturer", "ro.soc.manufacturer"},   {"soc_model", "ro.soc.model"},
      {"build_fingerprint", "ro.build.fingerprint"}, {"sdk", "ro.build.version.sdk"},
      {"os_version", "ro.build.version.release"},
  };
  std::vector<std::pair<std::string, std::string>> out = {{"os", "android"}};
  for (const auto& p : props) {
    // read callback, not __system_property_get: ro.* values can exceed PROP_VALUE_MAX (the fingerprint does on the
    // SM-X520), and the getter then returns an error text instead of the value
    const prop_info* pi = __system_property_find(p[1]);
    if (!pi) { continue; }
    std::string value;
    __system_property_read_callback(
        pi, [](void* cookie, const char*, const char* v, uint32_t) { *(std::string*)cookie = v; }, &value);
    if (!value.empty()) { out.push_back({p[0], value}); }
  }
  return out;
}

// Pending Java exception (from the last JNI call): described to logcat, cleared, reported. True if there was one. JNI
// allows no other call while an exception is pending, so this runs after every call that can throw.
static bool jni_failed(JNIEnv* env, const char* what) {
  if (!env->ExceptionCheck()) { return false; }
  env->ExceptionDescribe();
  env->ExceptionClear();
  fprintf(stderr, "error: jni: %s threw (see logcat)\n", what);
  return true;
}

static jclass jni_class(JNIEnv* env, const char* name) {
  jclass c = env->FindClass(name);
  return jni_failed(env, name) ? nullptr : c;
}

static jmethodID jni_method(JNIEnv* env, jclass cls, const char* name, const char* sig) {
  jmethodID m = cls ? env->GetMethodID(cls, name, sig) : nullptr;
  return jni_failed(env, name) ? nullptr : m;
}

// Runs fn(env, activity) with the app thread attached (GetEnv first, attach / detach only when needed) inside a local
// frame, so every local reference fn creates is freed. False when attaching or the frame fails, else fn's result.
template <class F> static bool with_jni(F fn) {
  ANativeActivity* act = s_app->activity;
  JNIEnv* env = nullptr;
  bool attached = false; // attached here: detach at the end (the app thread is not attached otherwise)
  if (act->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
    if (act->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      fprintf(stderr, "error: jni: AttachCurrentThread failed\n");
      return false;
    }
    attached = true;
  }
  bool ok = false;
  if (env->PushLocalFrame(64) == 0) {
    ok = fn(env, act->clazz);
    env->PopLocalFrame(nullptr);
  } else {
    jni_failed(env, "PushLocalFrame");
  }
  if (attached) { act->vm->DetachCurrentThread(); }
  return ok;
}

// MediaStore insert + write, in the caller's local frame. Column names are the MediaStore.MediaColumns constant values
// (DISPLAY_NAME, MIME_TYPE, RELATIVE_PATH, IS_PENDING; API 29+), so no static field lookups are needed for them.
static bool save_results_jni(JNIEnv* env, jobject activity, const std::string& name, const std::string& json,
                             std::string* where) {
  jclass cv_cls = jni_class(env, "android/content/ContentValues");
  jclass int_cls = cv_cls ? jni_class(env, "java/lang/Integer") : nullptr;
  jclass dl_cls = int_cls ? jni_class(env, "android/provider/MediaStore$Downloads") : nullptr;
  jclass cr_cls = dl_cls ? jni_class(env, "android/content/ContentResolver") : nullptr;
  jclass os_cls = cr_cls ? jni_class(env, "java/io/OutputStream") : nullptr;
  jclass uri_cls = os_cls ? jni_class(env, "android/net/Uri") : nullptr;
  if (!uri_cls) { return false; }
  jmethodID cv_init = jni_method(env, cv_cls, "<init>", "()V");
  jmethodID cv_put_str = jni_method(env, cv_cls, "put", "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID cv_put_int = jni_method(env, cv_cls, "put", "(Ljava/lang/String;Ljava/lang/Integer;)V");
  jmethodID cv_clear = jni_method(env, cv_cls, "clear", "()V");
  jmethodID get_cr =
      jni_method(env, env->GetObjectClass(activity), "getContentResolver", "()Landroid/content/ContentResolver;");
  jmethodID cr_insert =
      jni_method(env, cr_cls, "insert", "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;");
  jmethodID cr_update =
      jni_method(env, cr_cls, "update",
                 "(Landroid/net/Uri;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I");
  jmethodID cr_delete = jni_method(env, cr_cls, "delete", "(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I");
  jmethodID cr_open = jni_method(env, cr_cls, "openOutputStream", "(Landroid/net/Uri;)Ljava/io/OutputStream;");
  jmethodID os_write = jni_method(env, os_cls, "write", "([B)V");
  jmethodID os_close = jni_method(env, os_cls, "close", "()V");
  jmethodID uri_str = jni_method(env, uri_cls, "toString", "()Ljava/lang/String;");
  jmethodID int_of = env->GetStaticMethodID(int_cls, "valueOf", "(I)Ljava/lang/Integer;");
  if (jni_failed(env, "valueOf")) { int_of = nullptr; }
  jfieldID dl_uri_f = env->GetStaticFieldID(dl_cls, "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;");
  if (jni_failed(env, "EXTERNAL_CONTENT_URI")) { dl_uri_f = nullptr; }
  if (!cv_init || !cv_put_str || !cv_put_int || !cv_clear || !get_cr || !cr_insert || !cr_update || !cr_delete ||
      !cr_open || !os_write || !os_close || !uri_str || !int_of || !dl_uri_f) {
    return false;
  }

  jobject downloads = env->GetStaticObjectField(dl_cls, dl_uri_f);
  jobject cr = env->CallObjectMethod(activity, get_cr);
  if (jni_failed(env, "getContentResolver") || !downloads || !cr) { return false; }
  jobject values = env->NewObject(cv_cls, cv_init);
  if (jni_failed(env, "ContentValues()") || !values) { return false; }
  jstring k_pending = env->NewStringUTF("is_pending");
  if (jni_failed(env, "NewStringUTF") || !k_pending) { return false; }
  const char* const strs[][2] = {
      {"_display_name", name.c_str()}, {"mime_type", "application/json"}, {"relative_path", "Download/pxBenchy"}};
  for (const auto& kv : strs) {
    jstring k = env->NewStringUTF(kv[0]);
    jstring v = k ? env->NewStringUTF(kv[1]) : nullptr;
    if (jni_failed(env, "NewStringUTF") || !v) { return false; }
    env->CallVoidMethod(values, cv_put_str, k, v);
    if (jni_failed(env, "ContentValues.put")) { return false; }
  }
  jobject one = env->CallStaticObjectMethod(int_cls, int_of, (jint)1);
  if (jni_failed(env, "Integer.valueOf")) { return false; }
  env->CallVoidMethod(values, cv_put_int, k_pending, one);
  if (jni_failed(env, "ContentValues.put")) { return false; }
  jobject uri = env->CallObjectMethod(cr, cr_insert, downloads, values);
  if (jni_failed(env, "ContentResolver.insert")) { return false; }
  if (!uri) {
    fprintf(stderr, "error: results: ContentResolver.insert returned null\n");
    return false;
  }

  // From here on a failure deletes the pending row.
  bool ok = false;
  jobject os = env->CallObjectMethod(cr, cr_open, uri);
  if (!jni_failed(env, "openOutputStream") && os) {
    jbyteArray bytes = env->NewByteArray((jsize)json.size());
    if (!jni_failed(env, "NewByteArray") && bytes) {
      env->SetByteArrayRegion(bytes, 0, (jsize)json.size(), (const jbyte*)json.data());
      if (!jni_failed(env, "SetByteArrayRegion")) {
        env->CallVoidMethod(os, os_write, bytes);
        ok = !jni_failed(env, "OutputStream.write");
      }
    }
    env->CallVoidMethod(os, os_close); // also after a failed write; a failed close fails the save (data may be lost)
    ok = !jni_failed(env, "OutputStream.close") && ok;
  } else if (!os) {
    fprintf(stderr, "error: results: openOutputStream returned null\n");
  }
  if (ok) {
    env->CallVoidMethod(values, cv_clear);
    ok = !jni_failed(env, "ContentValues.clear");
    jobject zero = ok ? env->CallStaticObjectMethod(int_cls, int_of, (jint)0) : nullptr;
    ok = ok && !jni_failed(env, "Integer.valueOf");
    if (ok) { env->CallVoidMethod(values, cv_put_int, k_pending, zero); }
    ok = ok && !jni_failed(env, "ContentValues.put");
    jint rows = ok ? env->CallIntMethod(cr, cr_update, uri, values, nullptr, nullptr) : 0;
    ok = ok && !jni_failed(env, "ContentResolver.update");
    if (ok && rows != 1) { fprintf(stderr, "warning: results: IS_PENDING update changed %d rows\n", (int)rows); }
  }
  jstring s = ok ? (jstring)env->CallObjectMethod(uri, uri_str) : nullptr;
  ok = ok && !jni_failed(env, "Uri.toString") && s;
  if (ok) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    if (c) {
      *where = c;
      env->ReleaseStringUTFChars(s, c);
    }
    ok = c != nullptr;
    jni_failed(env, "GetStringUTFChars");
  }
  if (!ok) {
    env->CallIntMethod(cr, cr_delete, uri, nullptr, nullptr);
    jni_failed(env, "ContentResolver.delete");
  }
  return ok;
}

bool platform_save_results(const std::string& name, const std::string& json, std::string* where) {
  return with_jni([&](JNIEnv* env, jobject act) { return save_results_jni(env, act, name, json, where); });
}

// java.lang.String from UTF-8 bytes (new String(byte[], "UTF-8")): NewStringUTF takes modified UTF-8 and aborts under
// CheckJNI on anything else (e.g. 4-byte sequences), and the JSON is arbitrary text from the reports.
static jstring jni_utf8(JNIEnv* env, const std::string& s) {
  jclass str_cls = jni_class(env, "java/lang/String");
  jmethodID init = jni_method(env, str_cls, "<init>", "([BLjava/lang/String;)V");
  jbyteArray bytes = init ? env->NewByteArray((jsize)s.size()) : nullptr;
  if (jni_failed(env, "NewByteArray") || !bytes) { return nullptr; }
  env->SetByteArrayRegion(bytes, 0, (jsize)s.size(), (const jbyte*)s.data());
  if (jni_failed(env, "SetByteArrayRegion")) { return nullptr; }
  jstring charset = env->NewStringUTF("UTF-8");
  if (jni_failed(env, "NewStringUTF") || !charset) { return nullptr; }
  jstring out = (jstring)env->NewObject(str_cls, init, bytes, charset);
  return jni_failed(env, "String(byte[], String)") ? nullptr : out;
}

// ACTION_SEND chooser on the activity: type, EXTRA_SUBJECT "pxBenchy results <model>", plus either EXTRA_STREAM = uri
// (with read grant and ClipData) or EXTRA_TEXT = text. Signatures checked with javap -s (android-34).
static bool share_jni(JNIEnv* env, jobject activity, const char* type, const std::string* uri,
                      const std::string* text) {
  jclass intent_cls = jni_class(env, "android/content/Intent");
  jclass uri_cls = intent_cls ? jni_class(env, "android/net/Uri") : nullptr;
  jclass clip_cls = uri_cls ? jni_class(env, "android/content/ClipData") : nullptr;
  if (!clip_cls) { return false; }
  jmethodID init = jni_method(env, intent_cls, "<init>", "(Ljava/lang/String;)V");
  jmethodID set_type = jni_method(env, intent_cls, "setType", "(Ljava/lang/String;)Landroid/content/Intent;");
  jmethodID put_str =
      jni_method(env, intent_cls, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
  jmethodID put_parcel =
      jni_method(env, intent_cls, "putExtra", "(Ljava/lang/String;Landroid/os/Parcelable;)Landroid/content/Intent;");
  jmethodID add_flags = jni_method(env, intent_cls, "addFlags", "(I)Landroid/content/Intent;");
  jmethodID set_clip = jni_method(env, intent_cls, "setClipData", "(Landroid/content/ClipData;)V");
  jmethodID start = jni_method(env, env->GetObjectClass(activity), "startActivity", "(Landroid/content/Intent;)V");
  jmethodID chooser = env->GetStaticMethodID(
      intent_cls, "createChooser", "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;");
  if (jni_failed(env, "createChooser")) { chooser = nullptr; }
  jmethodID parse = env->GetStaticMethodID(uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  if (jni_failed(env, "Uri.parse")) { parse = nullptr; }
  jmethodID raw_uri = env->GetStaticMethodID(clip_cls, "newRawUri",
                                             "(Ljava/lang/CharSequence;Landroid/net/Uri;)Landroid/content/ClipData;");
  if (jni_failed(env, "ClipData.newRawUri")) { raw_uri = nullptr; }
  if (!init || !set_type || !put_str || !put_parcel || !add_flags || !set_clip || !start || !chooser || !parse ||
      !raw_uri) {
    return false;
  }

  char model[PROP_VALUE_MAX] = {};
  __system_property_get("ro.product.model", model);
  std::string subject = std::string("pxBenchy results ") + model;
  // each call only when the previous one succeeded: no JNI call is allowed while an exception (OOM) is pending
  jstring j_action = env->NewStringUTF("android.intent.action.SEND"); // Intent.ACTION_SEND
  jstring j_type = j_action ? env->NewStringUTF(type) : nullptr;
  jstring j_subject_key = j_type ? env->NewStringUTF("android.intent.extra.SUBJECT") : nullptr;
  jstring j_title = j_subject_key ? env->NewStringUTF("Share results") : nullptr;
  if (jni_failed(env, "NewStringUTF") || !j_title) { return false; }
  jstring j_subject = jni_utf8(env, subject);
  if (!j_subject) { return false; }
  jobject intent = env->NewObject(intent_cls, init, j_action);
  if (jni_failed(env, "Intent()") || !intent) { return false; }
  // the builder methods return the intent itself: their local refs are freed with the frame
  env->CallObjectMethod(intent, set_type, j_type);
  if (jni_failed(env, "Intent.setType")) { return false; }
  env->CallObjectMethod(intent, put_str, j_subject_key, j_subject);
  if (jni_failed(env, "Intent.putExtra")) { return false; }
  if (uri) {
    jstring j_uri_str = env->NewStringUTF(uri->c_str()); // content:// URI: ASCII
    jstring j_stream_key = j_uri_str ? env->NewStringUTF("android.intent.extra.STREAM") : nullptr;
    jstring j_empty = j_stream_key ? env->NewStringUTF("") : nullptr;
    if (jni_failed(env, "NewStringUTF") || !j_empty) { return false; }
    jobject j_uri = env->CallStaticObjectMethod(uri_cls, parse, j_uri_str);
    if (jni_failed(env, "Uri.parse") || !j_uri) { return false; }
    env->CallObjectMethod(intent, put_parcel, j_stream_key, j_uri);
    if (jni_failed(env, "Intent.putExtra")) { return false; }
    env->CallObjectMethod(intent, add_flags, (jint)1); // Intent.FLAG_GRANT_READ_URI_PERMISSION
    if (jni_failed(env, "Intent.addFlags")) { return false; }
    // ClipData carries the grant (and the Sharesheet preview); startActivity would also migrate EXTRA_STREAM into it
    jobject clip = env->CallStaticObjectMethod(clip_cls, raw_uri, j_empty, j_uri);
    if (jni_failed(env, "ClipData.newRawUri") || !clip) { return false; }
    env->CallVoidMethod(intent, set_clip, clip);
    if (jni_failed(env, "Intent.setClipData")) { return false; }
  } else {
    jstring j_text_key = env->NewStringUTF("android.intent.extra.TEXT");
    if (jni_failed(env, "NewStringUTF") || !j_text_key) { return false; }
    jstring j_text = jni_utf8(env, *text);
    if (!j_text) { return false; }
    env->CallObjectMethod(intent, put_str, j_text_key, j_text);
    if (jni_failed(env, "Intent.putExtra")) { return false; }
  }
  jobject ch = env->CallStaticObjectMethod(intent_cls, chooser, intent, j_title);
  if (jni_failed(env, "Intent.createChooser") || !ch) { return false; }
  env->CallVoidMethod(activity, start, ch); // binder call; fine from the native app thread
  return !jni_failed(env, "startActivity");
}

bool platform_share_file(const std::string& where, std::string* msg) {
  if (where.compare(0, 10, "content://") != 0) {
    *msg = "not a content:// URI: " + where;
    return false;
  }
  bool ok =
      with_jni([&](JNIEnv* env, jobject act) { return share_jni(env, act, "application/json", &where, nullptr); });
  *msg = ok ? "share sheet opened" : "share failed (see log)";
  return ok;
}

bool platform_share_text(const std::string& text, std::string* msg) {
  if (text.size() > PXB_SHARE_TEXT_MAX) {
    *msg = "too large for a text share";
    return false;
  }
  bool ok = with_jni([&](JNIEnv* env, jobject act) { return share_jni(env, act, "text/plain", nullptr, &text); });
  *msg = ok ? "share sheet opened" : "share failed (see log)";
  return ok;
}

// ClipboardManager from the activity's getSystemService: the framework builds it with the main thread's Handler
// (SystemServiceRegistry), so no Looper is needed on this thread; setPrimaryClip is a binder call to the clipboard
// service. Android 13+ shows its own "copied" confirmation.
static bool copy_jni(JNIEnv* env, jobject activity, const std::string& text) {
  jclass clip_cls = jni_class(env, "android/content/ClipData");
  jclass cm_cls = clip_cls ? jni_class(env, "android/content/ClipboardManager") : nullptr;
  if (!cm_cls) { return false; }
  jmethodID get_service =
      jni_method(env, env->GetObjectClass(activity), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  jmethodID set_primary = jni_method(env, cm_cls, "setPrimaryClip", "(Landroid/content/ClipData;)V");
  jmethodID plain = env->GetStaticMethodID(
      clip_cls, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
  if (jni_failed(env, "ClipData.newPlainText")) { plain = nullptr; }
  if (!get_service || !set_primary || !plain) { return false; }
  jstring j_name = env->NewStringUTF("clipboard"); // Context.CLIPBOARD_SERVICE
  jstring j_label = j_name ? env->NewStringUTF("pxBenchy results") : nullptr;
  if (jni_failed(env, "NewStringUTF") || !j_label) { return false; }
  jobject cm = env->CallObjectMethod(activity, get_service, j_name);
  if (jni_failed(env, "getSystemService") || !cm) { return false; }
  if (!env->IsInstanceOf(cm, cm_cls)) {
    fprintf(stderr, "error: results: clipboard service is not a ClipboardManager\n");
    return false;
  }
  jstring j_text = jni_utf8(env, text);
  if (!j_text) { return false; }
  jobject clip = env->CallStaticObjectMethod(clip_cls, plain, j_label, j_text);
  if (jni_failed(env, "ClipData.newPlainText") || !clip) { return false; }
  env->CallVoidMethod(cm, set_primary, clip);
  return !jni_failed(env, "ClipboardManager.setPrimaryClip");
}

bool platform_copy_text(const std::string& text, std::string* msg) {
  if (text.size() > PXB_SHARE_TEXT_MAX) {
    *msg = "too large for the clipboard; use Share";
    return false;
  }
  bool ok = with_jni([&](JNIEnv* env, jobject act) { return copy_jni(env, act, text); });
  *msg = ok ? "copied" : "copy failed (see log)";
  return ok;
}

// new URL(url).openConnection() as HttpURLConnection, GET, body read in 16 KB chunks. Network errors are exceptions
// (described to logcat by jni_failed). Plain Java calls: fine on any attached thread, but not the Java main thread
// (NetworkOnMainThreadException), which this never runs on.
static bool http_get_jni(JNIEnv* env, const std::string& url, std::string* body, std::string* err) {
  *err = "jni failed (see log)";
  jclass url_cls = jni_class(env, "java/net/URL");
  jclass conn_cls = url_cls ? jni_class(env, "java/net/HttpURLConnection") : nullptr;
  jclass is_cls = conn_cls ? jni_class(env, "java/io/InputStream") : nullptr;
  if (!is_cls) { return false; }
  jmethodID url_init = jni_method(env, url_cls, "<init>", "(Ljava/lang/String;)V");
  jmethodID open = jni_method(env, url_cls, "openConnection", "()Ljava/net/URLConnection;");
  jmethodID set_connect = jni_method(env, conn_cls, "setConnectTimeout", "(I)V");
  jmethodID set_read = jni_method(env, conn_cls, "setReadTimeout", "(I)V");
  jmethodID set_prop = jni_method(env, conn_cls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID get_code = jni_method(env, conn_cls, "getResponseCode", "()I");
  jmethodID get_is = jni_method(env, conn_cls, "getInputStream", "()Ljava/io/InputStream;");
  jmethodID disconnect = jni_method(env, conn_cls, "disconnect", "()V");
  jmethodID read = jni_method(env, is_cls, "read", "([B)I");
  jmethodID close = jni_method(env, is_cls, "close", "()V");
  if (!url_init || !open || !set_connect || !set_read || !set_prop || !get_code || !get_is || !disconnect || !read ||
      !close) {
    return false;
  }
  jstring j_url = env->NewStringUTF(url.c_str()); // URL: ASCII
  jstring j_ua_key = j_url ? env->NewStringUTF("User-Agent") : nullptr;
  jstring j_ua = j_ua_key ? env->NewStringUTF("pxBenchy") : nullptr; // GitHub's API rejects requests without one
  if (jni_failed(env, "NewStringUTF") || !j_ua) { return false; }
  jobject u = env->NewObject(url_cls, url_init, j_url);
  if (jni_failed(env, "URL()") || !u) { return false; }
  jobject conn = env->CallObjectMethod(u, open);
  if (jni_failed(env, "URL.openConnection") || !conn) { return false; }
  if (!env->IsInstanceOf(conn, conn_cls)) {
    *err = "not an http(s) URL";
    return false;
  }

  // From here on every path disconnects.
  bool ok = false;
  env->CallVoidMethod(conn, set_connect, (jint)5000);
  bool setup = !jni_failed(env, "setConnectTimeout");
  if (setup) { env->CallVoidMethod(conn, set_read, (jint)5000); }
  setup = setup && !jni_failed(env, "setReadTimeout");
  if (setup) { env->CallVoidMethod(conn, set_prop, j_ua_key, j_ua); }
  setup = setup && !jni_failed(env, "setRequestProperty");
  jint code = setup ? env->CallIntMethod(conn, get_code) : 0;
  if (setup && jni_failed(env, "getResponseCode")) {
    *err = "network error (see log)";
  } else if (setup && code != 200) {
    *err = "HTTP " + std::to_string((int)code);
  } else if (setup) {
    jobject is = env->CallObjectMethod(conn, get_is);
    jbyteArray buf = !jni_failed(env, "getInputStream") && is ? env->NewByteArray(16 * 1024) : nullptr;
    if (!jni_failed(env, "NewByteArray") && buf) {
      body->clear();
      for (;;) {
        jint n = env->CallIntMethod(is, read, buf);
        if (jni_failed(env, "InputStream.read")) {
          *err = "network error (see log)";
          break;
        }
        if (n < 0) {
          ok = true;
          break;
        }
        if (body->size() + (size_t)n > PXB_HTTP_BODY_MAX) {
          *err = "response too large";
          break;
        }
        size_t at = body->size();
        body->resize(at + (size_t)n);
        env->GetByteArrayRegion(buf, 0, n, (jbyte*)body->data() + at);
      }
    }
    if (is) {
      env->CallVoidMethod(is, close);
      jni_failed(env, "InputStream.close");
    }
  }
  env->CallVoidMethod(conn, disconnect);
  jni_failed(env, "disconnect");
  return ok;
}

bool platform_http_get(const std::string& url, std::string* body, std::string* err) {
  return with_jni([&](JNIEnv* env, jobject) { return http_get_jni(env, url, body, err); });
}

// startActivity(new Intent(ACTION_VIEW, Uri.parse(url))): the browser (or the app that handles the URL).
static bool open_url_jni(JNIEnv* env, jobject activity, const std::string& url) {
  jclass intent_cls = jni_class(env, "android/content/Intent");
  jclass uri_cls = intent_cls ? jni_class(env, "android/net/Uri") : nullptr;
  if (!uri_cls) { return false; }
  jmethodID init = jni_method(env, intent_cls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
  jmethodID start = jni_method(env, env->GetObjectClass(activity), "startActivity", "(Landroid/content/Intent;)V");
  jmethodID parse = env->GetStaticMethodID(uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  if (jni_failed(env, "Uri.parse")) { parse = nullptr; }
  if (!init || !start || !parse) { return false; }
  jstring j_action = env->NewStringUTF("android.intent.action.VIEW"); // Intent.ACTION_VIEW
  jstring j_url = j_action ? env->NewStringUTF(url.c_str()) : nullptr;
  if (jni_failed(env, "NewStringUTF") || !j_url) { return false; }
  jobject j_uri = env->CallStaticObjectMethod(uri_cls, parse, j_url);
  if (jni_failed(env, "Uri.parse") || !j_uri) { return false; }
  jobject intent = env->NewObject(intent_cls, init, j_action, j_uri);
  if (jni_failed(env, "Intent()") || !intent) { return false; }
  env->CallVoidMethod(activity, start, intent);
  return !jni_failed(env, "startActivity");
}

bool platform_open_url(const std::string& url, std::string* msg) {
  bool ok = with_jni([&](JNIEnv* env, jobject act) { return open_url_jni(env, act, url); });
  *msg = ok ? "browser opened" : "open failed (see log)";
  return ok;
}

float platform_ui_scale() {
  int32_t dpi = s_app->config ? AConfiguration_getDensity(s_app->config) : 0; // glue refreshes config on changes
  return dpi > 0 && dpi != ACONFIGURATION_DENSITY_ANY && dpi != ACONFIGURATION_DENSITY_NONE ? dpi / 160.0f : 1.0f;
}

// activity.getWindow().getDecorView().getRootWindowInsets().getInsets(systemBars() | displayCutout()): plain getters,
// fine off the UI thread. The window is the view (rotated window space), so the insets need no rotation.
static bool safe_insets_jni(JNIEnv* env, jobject activity, PlatformInsets* out) {
  jclass act_cls = env->GetObjectClass(activity);
  jmethodID get_window = jni_method(env, act_cls, "getWindow", "()Landroid/view/Window;");
  jobject window = get_window ? env->CallObjectMethod(activity, get_window) : nullptr;
  if (jni_failed(env, "getWindow") || !window) { return false; }
  jmethodID get_decor = jni_method(env, env->GetObjectClass(window), "getDecorView", "()Landroid/view/View;");
  jobject decor = get_decor ? env->CallObjectMethod(window, get_decor) : nullptr;
  if (jni_failed(env, "getDecorView") || !decor) { return false; }
  jmethodID get_root =
      jni_method(env, env->GetObjectClass(decor), "getRootWindowInsets", "()Landroid/view/WindowInsets;");
  jobject wi = get_root ? env->CallObjectMethod(decor, get_root) : nullptr; // null until the view is attached
  if (jni_failed(env, "getRootWindowInsets") || !wi) { return false; }
  jclass type_cls = jni_class(env, "android/view/WindowInsets$Type");
  jmethodID bars = type_cls ? env->GetStaticMethodID(type_cls, "systemBars", "()I") : nullptr;
  jmethodID cutout = bars ? env->GetStaticMethodID(type_cls, "displayCutout", "()I") : nullptr;
  if (jni_failed(env, "WindowInsets$Type") || !cutout) { return false; }
  jint types = env->CallStaticIntMethod(type_cls, bars) | env->CallStaticIntMethod(type_cls, cutout);
  jmethodID get_insets = jni_method(env, env->GetObjectClass(wi), "getInsets", "(I)Landroid/graphics/Insets;");
  jobject in = get_insets ? env->CallObjectMethod(wi, get_insets, types) : nullptr;
  if (jni_failed(env, "getInsets") || !in) { return false; }
  jclass in_cls = env->GetObjectClass(in);
  const char* names[] = {"left", "top", "right", "bottom"};
  float* fields[] = {&out->left, &out->top, &out->right, &out->bottom};
  for (int i = 0; i < 4; i++) {
    jfieldID f = env->GetFieldID(in_cls, names[i], "I");
    if (jni_failed(env, names[i]) || !f) { return false; }
    *fields[i] = (float)env->GetIntField(in, f);
  }
  return true;
}

PlatformInsets platform_safe_insets() {
  PlatformInsets r;
  if (!with_jni([&](JNIEnv* env, jobject act) { return safe_insets_jni(env, act, &r); })) { r = PlatformInsets{}; }
  return r;
}

// Battery temperature from the sticky ACTION_BATTERY_CHANGED intent (registerReceiver(null, filter)), degrees C.
static float battery_temp_c(ANativeActivity* act) {
  JNIEnv* env = nullptr;
  if (act->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return 0.0f; }
  float out = 0.0f;
  jclass filter_cls = env->FindClass("android/content/IntentFilter");
  jobject filter = env->NewObject(filter_cls, env->GetMethodID(filter_cls, "<init>", "(Ljava/lang/String;)V"),
                                  env->NewStringUTF("android.intent.action.BATTERY_CHANGED"));
  jclass act_cls = env->GetObjectClass(act->clazz);
  jmethodID reg =
      env->GetMethodID(act_cls, "registerReceiver",
                       "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
  jobject intent = env->CallObjectMethod(act->clazz, reg, nullptr, filter);
  if (intent) {
    jclass intent_cls = env->GetObjectClass(intent);
    jmethodID get_int = env->GetMethodID(intent_cls, "getIntExtra", "(Ljava/lang/String;I)I");
    out = env->CallIntMethod(intent, get_int, env->NewStringUTF("temperature"), 0) / 10.0f; // tenths of a degree
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    out = 0.0f;
  }
  act->vm->DetachCurrentThread(); // frees the local refs
  return out;
}

#ifdef TRACY_ENABLE
// First integer in a sysfs file, -1 when missing or not readable.
static long read_sysfs(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) { return -1; }
  long v = -1;
  if (fscanf(f, "%ld", &v) != 1) { v = -1; }
  fclose(f);
  return v;
}

// GPU clock (MHz, 0 when unknown): Mali reports kHz, Adreno devfreq Hz.
static float gpu_mhz() {
  long khz = read_sysfs("/sys/class/misc/mali0/device/cur_freq");
  if (khz > 0) { return khz / 1e3f; }
  long hz = read_sysfs("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
  return hz > 0 ? hz / 1e6f : 0.0f;
}

// Up to 4 cpufreq policies (one per cluster, numbered by their first core) in order, MHz.
static void cpu_mhz(float out[4]) {
  int n = 0;
  char path[96];
  for (int cpu = 0; cpu < 32 && n < 4; cpu++) {
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", cpu);
    long khz = read_sysfs(path);
    if (khz > 0) { out[n++] = khz / 1e3f; }
  }
}
#endif

const char* platform_thermal_status_name(float status) {
  static const char* const names[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
  int i = (int)status;
  return i >= 0 && i < 7 ? names[i] : "?";
}

void platform_thermal(PlatformThermal* out) {
  static AThermalManager* mgr = AThermal_acquireManager();
  static float headroom = 0.0f; // NaN while warming up or rate-limited: keep the last value
  if (mgr) {
    float h = AThermal_getThermalHeadroom(mgr, 0);
    if (!std::isnan(h)) { headroom = h; }
    out->status = (float)AThermal_getCurrentThermalStatus(mgr);
  }
  out->headroom = headroom;
  out->battery_c = battery_temp_c(s_app->activity);
#ifdef TRACY_ENABLE
  out->gpu_mhz = gpu_mhz();
  cpu_mhz(out->cpu_mhz);
#endif
}
