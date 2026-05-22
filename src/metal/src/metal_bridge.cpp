#include "apitrace/metal_bridge.hpp"

#include <utility>

namespace apitrace::metal {

MetalBridge::MetalBridge(MetalBridgeOptions options) : options_(std::move(options)) {}

bool MetalBridge::initialize()
{
  // TODO: initialize the concrete Metal device, queue, and optional trace manager bindings.
  ready_ = true;
  last_error_.clear();
  return true;
}

void MetalBridge::shutdown()
{
  // TODO: release native Metal objects in a deterministic order once ownership rules are defined.
  ready_ = false;
}

void MetalBridge::begin_frame(std::string_view frame_label)
{
  (void)frame_label;
  // TODO: translate frame boundaries into Metal-side trace scopes and debug signposts.
}

void MetalBridge::submit_command_buffer(std::string_view command_buffer_label)
{
  (void)command_buffer_label;
  // TODO: connect submission observation to native command-buffer lifecycle callbacks.
}

bool MetalBridge::ready() const noexcept
{
  return ready_;
}

const MetalBridgeOptions &MetalBridge::options() const noexcept
{
  return options_;
}

const std::string &MetalBridge::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::metal
