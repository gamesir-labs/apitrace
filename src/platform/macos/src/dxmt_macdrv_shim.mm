#include "apitrace/platform/macos_window.hpp"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>

struct ApitraceWinData {
  void *hwnd;
  void *cocoa_window;
  void *cocoa_view;
  void *client_cocoa_view;
};

extern "C" {

ApitraceWinData *get_win_data(void *hwnd)
{
  apitrace::platform::macos::WindowHandles handles;
  if (!apitrace::platform::macos::lookup_window(hwnd, handles)) {
    return nullptr;
  }

  auto *data = static_cast<ApitraceWinData *>(std::calloc(1, sizeof(ApitraceWinData)));
  if (data == nullptr) {
    return nullptr;
  }
  data->hwnd = handles.nswindow;
  data->cocoa_window = handles.nswindow;
  data->cocoa_view = handles.content_view;
  data->client_cocoa_view = handles.content_view;
  return data;
}

void release_win_data(ApitraceWinData *data)
{
  std::free(data);
}

void *macdrv_view_create_metal_view(void *cocoa_view, void *)
{
  return cocoa_view;
}

void *macdrv_view_get_metal_layer(void *view)
{
  if (view == nullptr) {
    return nullptr;
  }
  NSView *native_view = (__bridge NSView *)view;
  return (__bridge void *)native_view.layer;
}

void macdrv_view_release_metal_view(void *)
{
}

} // extern "C"
