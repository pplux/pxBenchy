#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "platform/platform.h"
#include <signal.h>
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

@implementation PxbView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
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

  NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
  g_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                         styleMask:style
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
  if (!g_window) return false;
  g_window.releasedWhenClosed = NO;
  g_window.title = [NSString stringWithUTF8String:title];
  g_delegate = [[PxbDelegate alloc] init];
  g_window.delegate = g_delegate;

  g_view = [[PxbView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
  g_view.wantsLayer = YES;
  g_window.contentView = g_view;
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

void* platform_surface_handle() {
  return g_view ? (__bridge void*)g_view.layer : nullptr;
}

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
      if (!e) break;
      [NSApp sendEvent:e];
    }
  }
  uint32_t ev = g_events;
  g_events = 0;
  if (g_sigint) ev |= PXB_EV_QUIT;
  return ev;
}

bool platform_wait_surface() {
  return g_window && !(g_events & PXB_EV_QUIT) && !g_sigint;
}

bool platform_read_file(const char* path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !f.bad();
}

void platform_thermal(PlatformThermal* out) {
  *out = PlatformThermal{};
  out->status = (float)[NSProcessInfo processInfo].thermalState; // nominal 0 .. critical 3
}
