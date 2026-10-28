// Android platform layer on android_native_app_glue.
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/thermal.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

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

// Completes the deferred TERM_WINDOW (glue sets app->window = NULL and wakes the Java thread).
static void finish_term() {
  if (!s_term_pending) return;
  s_term_pending = false;
  android_app_post_exec_cmd(s_app, APP_CMD_TERM_WINDOW);
}

// One poll step with normal glue processing.
static void poll_normal(int timeout) {
  android_poll_source* src = nullptr;
  int id = ALooper_pollOnce(timeout, nullptr, nullptr, (void**)&src);
  if (id >= 0 && src) src->process(s_app, src);
}

// stdout/stderr -> logcat: a pipe per stream, a detached thread forwards each line.
static void redirect_to_logcat(FILE* f, int fd, int prio) {
  setvbuf(f, nullptr, _IOLBF, 0);
  int p[2];
  if (pipe(p) != 0) return;
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
  if (act->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return out;
  jclass act_cls = env->GetObjectClass(act->clazz);
  jobject intent = env->CallObjectMethod(act->clazz, env->GetMethodID(act_cls, "getIntent", "()Landroid/content/Intent;"));
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
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
        if (!*p) break;
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
  redirect_to_logcat(stdout, 1, ANDROID_LOG_INFO);
  redirect_to_logcat(stderr, 2, ANDROID_LOG_ERROR);
  int rc = app_main(intent_args(app->activity));
  finish_term();
  fflush(stderr);
  // last line of a run (the process stays cached after finish, so tools wait for this instead of the pid)
  printf("exit code: %d\n", rc);
  fflush(stdout);
  ANativeActivity_finish(app->activity);
  while (!app->destroyRequested) poll_normal(-1);
}

bool platform_init(int, int, const char*) {
  ANativeActivity_setWindowFlags(s_app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
  while (!s_app->window) {
    if (s_app->destroyRequested) return false;
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
    if (id < 0) break;
    if (!src) continue;
    if (id != LOOPER_ID_MAIN) {
      src->process(s_app, src);
      continue;
    }
    int8_t cmd = android_app_read_cmd(s_app);
    android_app_pre_exec_cmd(s_app, cmd);
    if (cmd == APP_CMD_TERM_WINDOW) {
      // post_exec deferred: the Java side stays blocked until the caller dropped its VkSurface
      s_term_pending = true;
      return ev | PXB_EV_SURFACE_LOST;
    }
    if (cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONFIG_CHANGED) ev |= PXB_EV_RESIZED;
    android_app_post_exec_cmd(s_app, cmd);
  }
  if (s_app->destroyRequested) ev |= PXB_EV_QUIT;
  return ev;
}

bool platform_wait_surface() {
  finish_term();
  while (!s_app->window) {
    if (s_app->destroyRequested) return false;
    poll_normal(-1);
  }
  return true;
}

bool platform_read_file(const char* path, std::string* out) {
  AAsset* a = AAssetManager_open(s_app->activity->assetManager, path, AASSET_MODE_BUFFER);
  if (!a) return false;
  out->resize((size_t)AAsset_getLength64(a));
  bool ok = AAsset_read(a, out->data(), out->size()) == (int)out->size();
  AAsset_close(a);
  return ok;
}

// Battery temperature from the sticky ACTION_BATTERY_CHANGED intent (registerReceiver(null, filter)), degrees C.
static float battery_temp_c(ANativeActivity* act) {
  JNIEnv* env = nullptr;
  if (act->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 0.0f;
  float out = 0.0f;
  jclass filter_cls = env->FindClass("android/content/IntentFilter");
  jobject filter = env->NewObject(filter_cls, env->GetMethodID(filter_cls, "<init>", "(Ljava/lang/String;)V"),
                                  env->NewStringUTF("android.intent.action.BATTERY_CHANGED"));
  jclass act_cls = env->GetObjectClass(act->clazz);
  jmethodID reg = env->GetMethodID(act_cls, "registerReceiver",
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

void platform_thermal(PlatformThermal* out) {
  static AThermalManager* mgr = AThermal_acquireManager();
  static float headroom = 0.0f; // NaN while warming up or rate-limited: keep the last value
  if (mgr) {
    float h = AThermal_getThermalHeadroom(mgr, 0);
    if (!std::isnan(h)) headroom = h;
    out->status = (float)AThermal_getCurrentThermalStatus(mgr);
  }
  out->headroom = headroom;
  out->battery_c = battery_temp_c(s_app->activity);
}
