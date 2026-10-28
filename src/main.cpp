#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "platform/platform.h"
#include "platform/thermal.h"
#include "git_version.h"
#include "lua/gpu_api.h"
#include "lua/vm.h"
#include "lua/bind_gpu.h"
#include "lua/bind_pxb.h"
#include "lua/timers_report.h"
#include <tracy/Tracy.hpp>

struct Options {
  bool validation = false;
  GpuApi vulkan = GpuApi::vk11;
  long long frames = 6000;  // 0 = run indefinitely
  long long frames_warmup = 60;
  bool hud = true;
  bool offscreen = false;
  bool json = false;
  std::string screenshot;        // "" = off
  long long screenshot_frame = 1;
  std::string script;
};

static void usage() {
  fprintf(stderr, "usage: pxbenchy [--validation] [--vulkan 1.1|1.3] [--frames N] [--frames-warmup N] [--no-hud] "
                  "[--offscreen] [--json] [--screenshot file.png] [--screenshot-frame N] <script>\n");
}

// Parses the value after option args[*i] as a non-negative integer (advances *i).
static bool parse_count(const std::vector<std::string>& args, size_t* i, long long* out) {
  const char* opt = args[*i].c_str();
  const char* v = *i + 1 < args.size() ? args[++*i].c_str() : "";
  char* end = nullptr;
  errno = 0;
  long long n = (*v >= '0' && *v <= '9') ? strtoll(v, &end, 10) : -1;
  if (n < 0 || *end != '\0' || errno == ERANGE) {
    fprintf(stderr, "error: %s needs a non-negative integer ('%s')\n", opt, v);
    return false;
  }
  *out = n;
  return true;
}

static bool parse_args(const std::vector<std::string>& args, Options* opt) {
  bool have_script = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (have_script) {
      fprintf(stderr, "error: script must be the last argument ('%s')\n", a.c_str());
      return false;
    }
    if (a == "--validation") {
      opt->validation = true;
    } else if (a == "--vulkan") {
      const char* v = i + 1 < args.size() ? args[++i].c_str() : "";
      if (std::string(v) == gpu_api_name(GpuApi::vk11)) {
        opt->vulkan = GpuApi::vk11;
      } else if (std::string(v) == gpu_api_name(GpuApi::vk13)) {
        opt->vulkan = GpuApi::vk13;
      } else {
        fprintf(stderr, "error: --vulkan needs 1.1 or 1.3 ('%s')\n", v);
        return false;
      }
    } else if (a == "--frames") {
      if (!parse_count(args, &i, &opt->frames)) return false;
    } else if (a == "--frames-warmup") {
      if (!parse_count(args, &i, &opt->frames_warmup)) return false;
    } else if (a == "--no-hud") {
      opt->hud = false;
    } else if (a == "--offscreen") {
      opt->offscreen = true;
    } else if (a == "--json") {
      opt->json = true;
    } else if (a == "--screenshot") {
      if (i + 1 >= args.size()) {
        fprintf(stderr, "error: --screenshot needs a file path\n");
        return false;
      }
      opt->screenshot = args[++i];
    } else if (a == "--screenshot-frame") {
      if (!parse_count(args, &i, &opt->screenshot_frame)) return false;
      if (opt->screenshot_frame == 0) {
        fprintf(stderr, "error: --screenshot-frame starts at 1\n");
        return false;
      }
    } else if (a.rfind("--", 0) == 0) {
      fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
      return false;
    } else {
      opt->script = a;
      have_script = true;
    }
  }
  if (!have_script) {
    fprintf(stderr, "error: missing script\n");
    return false;
  }
  if (opt->screenshot_frame != 1 && opt->screenshot.empty()) {
    fprintf(stderr, "error: --screenshot-frame needs --screenshot\n");
    return false;
  }
  return true;
}

int app_main(const std::vector<std::string>& args) {
  Options opt;
  if (!parse_args(args, &opt)) {
    usage();
    return 1;
  }
  printf("git=%s validation=%d vulkan=%s frames=%lld frames_warmup=%lld hud=%d offscreen=%d json=%d script=%s\n",
         PXB_GIT_HASH, opt.validation ? 1 : 0, gpu_api_name(opt.vulkan), opt.frames, opt.frames_warmup, opt.hud ? 1 : 0,
         opt.offscreen ? 1 : 0, opt.json ? 1 : 0, opt.script.c_str());
  fflush(stdout);

  if (!platform_init(1280, 720, "pxBenchy")) {
    fprintf(stderr, "error: platform_init failed\n");
    return 1;
  }
  GpuApi gpu_api = opt.vulkan;
  const GpuApiTable* gpu = gpu_api_table(gpu_api);
  GpuDevice dev;
  if (!gpu->device_init(&dev, opt.validation, platform_surface_handle())) {
    fprintf(stderr, "error: gpu_device_init failed\n");
    platform_shutdown();
    return 1;
  }
  if (!gpu->shader_init()) {
    fprintf(stderr, "error: gpu_shader_init failed\n");
    gpu->device_shutdown(&dev);
    platform_shutdown();
    return 1;
  }
  uint32_t api = dev.props.apiVersion;
  printf("device: %s api=%u.%u.%u queue_family=%u validation=%s display_timing=%s\n", dev.props.deviceName,
         VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api), dev.queue_family,
         dev.validation ? "on" : "off", dev.display_timing ? "yes" : "no");
  fflush(stdout);
  thermal_start();
  GpuSwapchain sc;
  sc.offscreen = opt.offscreen;
  sc.readable = !opt.screenshot.empty();
  GpuLua gl;
  TimerSet ts;
  bool ok = gpu->swapchain_create(&dev, &sc);
  if (ok && bind_gpu_init(&gl, gpu_api, &dev, &sc, &ts, opt.frames, opt.frames_warmup, opt.hud)) {
    lua_State* L = vm_create();
    if (L) {
      bind_gpu_register(L, &gl);
      bind_pxb_register(L, &ts, opt.frames, opt.frames_warmup);
    }
    gl.screenshot = opt.screenshot;
    gl.screenshot_frame = opt.screenshot_frame;
    ok = L && vm_run(L, opt.script);
    bind_gpu_finish(&gl);
    if (ok && !opt.screenshot.empty() && !gl.screenshot_taken) {
      fprintf(stderr, "error: --screenshot: script ended before frame %lld\n", opt.screenshot_frame);
      ok = false;
    }
    if (gl.screenshot_taken) {
      // screenshot runs end without a report
    } else if (opt.json) {
      bind_gpu_report_json(&gl, opt.script.c_str());
    } else {
      timers_report(&ts, &gl.timers, gpu);
      bind_gpu_report(&gl);
    }
    vm_destroy(L);
    bind_gpu_shutdown(&gl);
  } else {
    ok = false;
    fprintf(stderr, "error: swapchain setup failed\n");
  }
  {
    ZoneScopedN("wait_idle");
    vkDeviceWaitIdle(dev.device);
  }
  thermal_stop();
  gpu->swapchain_destroy(&dev, &sc);
  gpu->shader_shutdown();
  gpu->device_shutdown(&dev);
  platform_shutdown();
  return ok ? 0 : 1;
}

#if !defined(__ANDROID__)
int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return app_main(args);
}
#endif
