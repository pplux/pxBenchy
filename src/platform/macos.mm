#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "platform/platform.h"
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>

static NSWindow* g_window = nil;
static NSView* g_view = nil;
static id g_delegate = nil;
static uint32_t g_events = 0;
static volatile sig_atomic_t g_sigint = 0;

static void on_sigint(int) { g_sigint = 1; }

@interface PxbView : NSView
@end

// Mouse -> pointer queue (platform_pump dispatches the events). Positions: view points, flipped to top-left origin,
// times backingScaleFactor = drawable pixels.
static void push_mouse(NSView* v, NSEvent* e, PlatformPointerEvent::Kind kind, int button = 0, bool down = false) {
  NSPoint p = [v convertPoint:e.locationInWindow fromView:nil];
  CGFloat scale = v.window.backingScaleFactor;
  PlatformPointerEvent pe;
  pe.kind = kind;
  pe.button_index = button;
  pe.down = down;
  if (kind == PlatformPointerEvent::wheel) {
    float k = e.hasPreciseScrollingDeltas ? 0.1f : 1.0f; // trackpad: pixel deltas | wheel: lines
    pe.x = (float)e.scrollingDeltaX * k;
    pe.y = (float)e.scrollingDeltaY * k;
  } else {
    pe.x = (float)(p.x * scale);
    pe.y = (float)((v.bounds.size.height - p.y) * scale);
  }
  platform_pointer_push(pe);
}

// NSEvent buttonNumber (0 left, 1 right, 2 middle) = ImGui button index; others are ignored.
static void push_button(NSView* v, NSEvent* e, bool down) {
  if (e.buttonNumber > 2) { return; }
  push_mouse(v, e, PlatformPointerEvent::pos);
  push_mouse(v, e, PlatformPointerEvent::button, (int)e.buttonNumber, down);
}

@implementation PxbView
- (BOOL)wantsUpdateLayer {
  return YES;
}
- (CALayer*)makeBackingLayer {
  return [CAMetalLayer layer];
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e {
  return YES;
} // first click on an inactive window reaches the UI
- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  for (NSTrackingArea* a in self.trackingAreas) { [self removeTrackingArea:a]; }
  [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                     options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                             NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                       owner:self
                                                    userInfo:nil]];
}
- (void)mouseMoved:(NSEvent*)e {
  push_mouse(self, e, PlatformPointerEvent::pos);
}
- (void)mouseDragged:(NSEvent*)e {
  push_mouse(self, e, PlatformPointerEvent::pos);
}
- (void)rightMouseDragged:(NSEvent*)e {
  push_mouse(self, e, PlatformPointerEvent::pos);
}
- (void)otherMouseDragged:(NSEvent*)e {
  push_mouse(self, e, PlatformPointerEvent::pos);
}
- (void)mouseExited:(NSEvent*)e {
  push_mouse(self, e, PlatformPointerEvent::leave);
}
- (void)mouseDown:(NSEvent*)e {
  push_button(self, e, true);
}
- (void)mouseUp:(NSEvent*)e {
  push_button(self, e, false);
}
- (void)rightMouseDown:(NSEvent*)e {
  push_button(self, e, true);
}
- (void)rightMouseUp:(NSEvent*)e {
  push_button(self, e, false);
}
- (void)otherMouseDown:(NSEvent*)e {
  push_button(self, e, true);
}
- (void)otherMouseUp:(NSEvent*)e {
  push_button(self, e, false);
}
- (void)scrollWheel:(NSEvent*)e {
  push_mouse(self, e, PlatformPointerEvent::wheel);
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (void)keyDown:(NSEvent*)e { // Escape = Back (PXB_EV_BACK); other keys beep as before
  if (e.keyCode == 53) {
    g_events |= PXB_EV_BACK;
  } else {
    [super keyDown:e];
  }
}
@end

static void update_layer() {
  CAMetalLayer* layer = (CAMetalLayer*)g_view.layer;
  CGFloat scale = g_window.backingScaleFactor;
  layer.contentsScale = scale;
  layer.drawableSize = CGSizeMake(g_view.bounds.size.width * scale, g_view.bounds.size.height * scale);
}

@interface PxbDelegate : NSObject <NSWindowDelegate>
@end

@implementation PxbDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  g_events |= PXB_EV_QUIT;
  return NO; // window is released in platform_shutdown
}
- (void)windowDidResize:(NSNotification*)n {
  update_layer();
  g_events |= PXB_EV_RESIZED;
}
- (void)windowDidChangeBackingProperties:(NSNotification*)n {
  update_layer();
  g_events |= PXB_EV_RESIZED;
}
@end

bool platform_init(int width, int height, const char* title) {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable;
  g_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                         styleMask:style
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
  if (!g_window) { return false; }
  g_window.releasedWhenClosed = NO;
  g_window.title = [NSString stringWithUTF8String:title];
  g_delegate = [[PxbDelegate alloc] init];
  g_window.delegate = g_delegate;

  g_view = [[PxbView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
  g_view.wantsLayer = YES;
  g_window.contentView = g_view;
  [g_window makeFirstResponder:g_view];
  update_layer();

  [g_window center];
  [NSApp finishLaunching];
  [g_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  g_events = 0;
  struct sigaction sa = {};
  sa.sa_handler = on_sigint;
  sa.sa_flags = SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  return true;
}

void platform_shutdown() {
  if (g_window) {
    g_window.delegate = nil;
    [g_window close];
  }
  g_window = nil;
  g_view = nil;
  g_delegate = nil;
}

void* platform_surface_handle() { return g_view ? (__bridge void*)g_view.layer : nullptr; }

void platform_drawable_size(uint32_t* w, uint32_t* h) {
  CGSize s = ((CAMetalLayer*)g_view.layer).drawableSize;
  *w = (uint32_t)s.width;
  *h = (uint32_t)s.height;
}

uint32_t platform_pump() {
  @autoreleasepool {
    for (;;) {
      NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES];
      if (!e) { break; }
      [NSApp sendEvent:e];
    }
  }
  uint32_t ev = g_events;
  g_events = 0;
  if (g_sigint) { ev |= PXB_EV_QUIT; }
  return ev;
}

bool platform_wait_surface() { return g_window && !(g_events & PXB_EV_QUIT) && !g_sigint; }

bool platform_read_file(const char* path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { return false; }
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !f.bad();
}

std::vector<std::string> platform_list_scripts() {
  std::vector<std::string> out;
  DIR* d = opendir("scripts");
  if (!d) { return out; }
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (e->d_type != DT_DIR && name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) {
      out.push_back("scripts/" + name);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::pair<std::string, std::string>> platform_device_info() {
  std::vector<std::pair<std::string, std::string>> out = {{"os", "macos"}};
  char model[256] = {};
  size_t size = sizeof(model) - 1;
  if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && model[0]) { out.push_back({"model", model}); }
  @autoreleasepool {
    NSString* os = [[NSProcessInfo processInfo] operatingSystemVersionString];
    if (os.length) { out.push_back({"os_version", os.UTF8String}); }
  }
  return out;
}

bool platform_save_results(const std::string& name, const std::string& json, std::string* where) {
  if (mkdir("results", 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "error: results: cannot create directory 'results': %s\n", strerror(errno));
    return false;
  }
  size_t dot = name.rfind('.');
  std::string stem = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot);
  for (int i = 0; i < 100; i++) {
    std::string path = "results/" + (i ? stem + "-" + std::to_string(i) + ext : name);
    FILE* f = fopen(path.c_str(), "wx"); // exclusive: never overwrite an earlier result
    if (!f && errno == EEXIST) { continue; }
    if (!f) {
      fprintf(stderr, "error: results: cannot create '%s': %s\n", path.c_str(), strerror(errno));
      return false;
    }
    bool ok = fwrite(json.data(), 1, json.size(), f) == json.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
      fprintf(stderr, "error: results: cannot write '%s'\n", path.c_str());
      remove(path.c_str());
      return false;
    }
    *where = path;
    return true;
  }
  fprintf(stderr, "error: results: too many files named '%s'\n", name.c_str());
  return false;
}

bool platform_share_file(const std::string& where, std::string* msg) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:where.c_str()]];
    if (!url || ![url checkResourceIsReachableAndReturnError:nil]) {
      fprintf(stderr, "error: share: '%s' not found\n", where.c_str());
      *msg = "file not found: " + where;
      return false;
    }
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url.absoluteURL ]];
    *msg = "shown in Finder: " + where;
    return true;
  }
}

bool platform_copy_text(const std::string& text, std::string* msg) {
  @autoreleasepool {
    NSString* s = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    if (!s || ![pb setString:s forType:NSPasteboardTypeString]) {
      fprintf(stderr, "error: share: pasteboard write failed\n");
      *msg = "pasteboard write failed";
      return false;
    }
    *msg = "copied";
    return true;
  }
}

bool platform_share_text(const std::string& text, std::string* msg) {
  if (!platform_copy_text(text, msg)) { return false; }
  *msg = "copied to the pasteboard (macOS: no text share)";
  return true;
}

bool platform_http_get(const std::string&, std::string*, std::string* err) {
  *err = "not supported on macOS";
  return false;
}

bool platform_open_url(const std::string&, std::string* msg) {
  *msg = "not supported on macOS";
  return false;
}

float platform_ui_scale() { return g_window ? (float)g_window.backingScaleFactor : 1.0f; }

PlatformInsets platform_safe_insets() { return PlatformInsets{}; } // window content area: nothing covers it

void platform_thermal(PlatformThermal* out) {
  *out = PlatformThermal{};
  out->status = (float)[NSProcessInfo processInfo].thermalState; // nominal 0 .. critical 3
}
