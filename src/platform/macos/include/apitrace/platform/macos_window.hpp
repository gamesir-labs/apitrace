#pragma once

#include <cstdint>
#include <string>

namespace apitrace::platform::macos {

struct WindowSpec {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string title;
  bool show = true;
};

struct WindowHandles {
  void *nswindow = nullptr;
  void *content_view = nullptr;
  void *cametal_layer = nullptr;
};

bool create_window(const WindowSpec &spec, WindowHandles &handles, std::string &error);
void destroy_window(WindowHandles &handles);
void pump_events(WindowHandles &handles);
void wait_for_close_with_delay_ms(WindowHandles &handles, std::uint32_t delay_ms);
bool lookup_window(void *nswindow, WindowHandles &handles);

} // namespace apitrace::platform::macos
