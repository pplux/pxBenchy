#include <charconv>
#include <cstdio>
#include <string_view>
#include <string>
#include <vector>
#include "platform/platform.h"
#include "git_version.h"
#include "lua/gpu_api.h"
#include "menu.h"
#include "session.h"

struct Options {
  bool validation = false;
  GpuApi vulkan = GpuApi::vk11;
  bool vulkan_auto = true; // resolve `vulkan` with the 1.3 probe (D7)
  bool json = false;
  bool menu = false;        // no script: launcher menu (menu.h)
  bool vulkan_info = false; // print gpu_vulkan_info_json and exit
  RunOptions run;
};

#ifdef PXB_VALIDATION
#define VALIDATION_USAGE "[--validation] "
#else
#define VALIDATION_USAGE ""
#endif

static void usage() {
  fprintf(stderr, "usage: pxbenchy " VALIDATION_USAGE "[--vulkan auto|1.1|1.3] [--frames N] [--frames-warmup N] "
                  "[--no-hud] [--offscreen] [--json] [--screenshot file.png] [--screenshot-frame N] "
                  "[--vulkan-info] [<script>]\n"
                  "  no script: menu (the options set its initial state)\n");
}

// Parses v (the value of option opt) as a non-negative integer.
static bool parse_count(std::string_view opt, std::string_view v, long long* out) {
  auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), *out);
  if (ec != std::errc() || end != v.data() + v.size() || *out < 0) {
    fprintf(stderr, "error: %.*s needs a non-negative integer ('%.*s')\n", (int)opt.size(), opt.data(), (int)v.size(),
            v.data());
    return false;
  }
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
#ifdef PXB_VALIDATION
    if (a == "--validation") {
      opt->validation = true;
      continue;
    }
#endif
    if (a == "--vulkan") {
      const char* v = i + 1 < args.size() ? args[++i].c_str() : "";
      opt->vulkan_auto = false;
      if (std::string(v) == "auto") {
        opt->vulkan_auto = true;
      } else if (std::string(v) == gpu_api_name(GpuApi::vk11)) {
        opt->vulkan = GpuApi::vk11;
      } else if (std::string(v) == gpu_api_name(GpuApi::vk13)) {
        opt->vulkan = GpuApi::vk13;
      } else {
        fprintf(stderr, "error: --vulkan needs auto, 1.1 or 1.3 ('%s')\n", v);
        return false;
      }
    } else if (a == "--frames") {
      if (!parse_count(a, i + 1 < args.size() ? args[++i] : "", &opt->run.frames)) { return false; }
    } else if (a == "--frames-warmup") {
      if (!parse_count(a, i + 1 < args.size() ? args[++i] : "", &opt->run.frames_warmup)) { return false; }
    } else if (a == "--no-hud") {
      opt->run.hud = false;
    } else if (a == "--offscreen") {
      opt->run.offscreen = true;
    } else if (a == "--json") {
      opt->json = true;
    } else if (a == "--vulkan-info") {
      opt->vulkan_info = true;
    } else if (a == "--screenshot") {
      if (i + 1 >= args.size()) {
        fprintf(stderr, "error: --screenshot needs a file path\n");
        return false;
      }
      opt->run.screenshot = args[++i];
    } else if (a == "--screenshot-frame") {
      if (!parse_count(a, i + 1 < args.size() ? args[++i] : "", &opt->run.screenshot_frame)) { return false; }
      if (opt->run.screenshot_frame == 0) {
        fprintf(stderr, "error: --screenshot-frame starts at 1\n");
        return false;
      }
    } else if (a.rfind("--", 0) == 0) {
      fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
      return false;
    } else {
      opt->run.script = a;
      have_script = true;
    }
  }
  opt->menu = !have_script;
  if (opt->menu) { opt->run.offscreen = true; }
  if (opt->menu && (opt->json || !opt->run.screenshot.empty())) {
    fprintf(stderr, "error: --json and --screenshot need a script\n");
    return false;
  }
  if (opt->run.screenshot_frame != 1 && opt->run.screenshot.empty()) {
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
  if (opt.vulkan_info) {
    std::string info = gpu_vulkan_info_json() + "\n";
    fwrite(info.data(), 1, info.size(), stdout);
    fflush(stdout);
    return 0;
  }
  if (opt.vulkan_auto) { opt.vulkan = gpu_vk13_supported() ? GpuApi::vk13 : GpuApi::vk11; }
  const RunOptions& ro = opt.run;
  std::string script = opt.menu ? "(menu)" : ro.script;
  printf("git=%s validation=%d vulkan=%s%s frames=%lld frames_warmup=%lld hud=%d offscreen=%d json=%d script=%s\n",
         PXB_GIT_HASH, opt.validation ? 1 : 0, gpu_api_name(opt.vulkan), opt.vulkan_auto ? " (auto)" : "", ro.frames,
         ro.frames_warmup, ro.hud ? 1 : 0, ro.offscreen ? 1 : 0, opt.json ? 1 : 0, script.c_str());
  fflush(stdout);

  Session s;
  s.vulkan_info = gpu_vulkan_info_json();
  if (!session_init(&s, opt.vulkan, opt.validation)) { return 1; }
  int rc;
  if (opt.menu) {
    rc = menu_run(&s, ro);
  } else {
    RunResult r = run_script(&s, ro);
    const std::string& report = opt.json ? r.json : r.text;
    fwrite(report.data(), 1, report.size(), stdout);
    fflush(stdout);
    rc = r.ok ? 0 : 1;
  }
  session_shutdown(&s);
  return rc;
}

#if !defined(__ANDROID__)
int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return app_main(args);
}
#endif
