#include "apitrace/platform/macos_window.hpp"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace apitrace::platform::macos {
namespace {

std::mutex &registry_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<void *, WindowHandles> &registry()
{
  static std::unordered_map<void *, WindowHandles> windows;
  return windows;
}

void ensure_application_started()
{
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp finishLaunching];
  [NSApp activateIgnoringOtherApps:YES];
}

void pump_events_on_main()
{
  while (true) {
    NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate dateWithTimeIntervalSinceNow:0.0]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
    if (event == nil) {
      break;
    }
    [NSApp sendEvent:event];
  }
}

bool run_on_main_sync(std::string &error, void (^block)(void))
{
  if ([NSThread isMainThread]) {
    block();
    return true;
  }

  __block bool completed = false;
  dispatch_sync(dispatch_get_main_queue(), ^{
    block();
    completed = true;
  });
  if (!completed) {
    error = "failed to dispatch work to the main thread";
    return false;
  }
  return true;
}

} // namespace

bool create_window(const WindowSpec &spec, WindowHandles &handles, std::string &error)
{
  handles = {};
  if (spec.width == 0 || spec.height == 0) {
    error = "window size must be non-zero";
    return false;
  }

  __block NSWindow *window = nil;
  __block NSView *view = nil;
  __block CAMetalLayer *layer = nil;

  if (!run_on_main_sync(error, ^{
        ensure_application_started();

        const NSRect frame = NSMakeRect(0.0, 0.0, static_cast<CGFloat>(spec.width), static_cast<CGFloat>(spec.height));
        window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable)
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        if (window == nil) {
          return;
        }

        NSString *title = spec.title.empty() ? @"apitrace" : [NSString stringWithUTF8String:spec.title.c_str()];
        [window setTitle:title];
        [window center];

        view = [window contentView];
        if (view == nil) {
          return;
        }
        [view setFrame:frame];
        [view setWantsLayer:YES];

        layer = [CAMetalLayer layer];
        if (layer == nil) {
          return;
        }
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.opaque = YES;
        layer.colorspace = nil;
        layer.drawableSize = CGSizeMake(static_cast<CGFloat>(spec.width), static_cast<CGFloat>(spec.height));
        layer.contentsScale = window.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
        layer.frame = view.bounds;
        view.layer = layer;

        if (spec.show) {
          [window makeKeyAndOrderFront:nil];
        }
        pump_events_on_main();
      })) {
    return false;
  }

  if (window == nil || view == nil || layer == nil) {
    error = "failed to create native macOS Metal window";
    return false;
  }

  handles.nswindow = (__bridge_retained void *)window;
  handles.content_view = (__bridge_retained void *)view;
  handles.cametal_layer = (__bridge_retained void *)layer;

  {
    std::lock_guard lock(registry_mutex());
    registry()[handles.nswindow] = handles;
  }
  return true;
}

void destroy_window(WindowHandles &handles)
{
  if (handles.nswindow == nullptr) {
    return;
  }

  {
    std::lock_guard lock(registry_mutex());
    registry().erase(handles.nswindow);
  }

  WindowHandles local = handles;
  handles = {};
  std::string ignored;
  run_on_main_sync(ignored, ^{
    NSWindow *window = (__bridge_transfer NSWindow *)local.nswindow;
    (void)(__bridge_transfer NSView *)local.content_view;
    (void)(__bridge_transfer CAMetalLayer *)local.cametal_layer;
    [window orderOut:nil];
    [window close];
  });
}

void pump_events(WindowHandles &)
{
  std::string ignored;
  run_on_main_sync(ignored, ^{
    pump_events_on_main();
  });
}

void wait_for_close_with_delay_ms(WindowHandles &handles, std::uint32_t delay_ms)
{
  if (delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  pump_events(handles);
}

bool lookup_window(void *nswindow, WindowHandles &handles)
{
  std::lock_guard lock(registry_mutex());
  auto it = registry().find(nswindow);
  if (it == registry().end()) {
    handles = {};
    return false;
  }
  handles = it->second;
  return true;
}

} // namespace apitrace::platform::macos
