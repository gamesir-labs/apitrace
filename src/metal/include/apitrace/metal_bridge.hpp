#pragma once

#include <string>
#include <string_view>

namespace apitrace::metal {

struct MetalBridgeOptions {
  bool enable_validation = false;
  bool prefer_low_power_device = false;
  std::string device_label;
};

class MetalBridge {
public:
  explicit MetalBridge(MetalBridgeOptions options);

  bool initialize();
  void shutdown();

  void begin_frame(std::string_view frame_label);
  void submit_command_buffer(std::string_view command_buffer_label);

  bool ready() const noexcept;
  const MetalBridgeOptions &options() const noexcept;
  const std::string &last_error() const noexcept;

private:
  MetalBridgeOptions options_;
  std::string last_error_;
  bool ready_ = false;

  // TODO: split device selection, queue bootstrap, and trace-manager attachment into explicit bridge subcomponents.
  // TODO: expose native Metal handles through narrow adapter interfaces once replay backend requirements are enumerated.
};

} // namespace apitrace::metal
