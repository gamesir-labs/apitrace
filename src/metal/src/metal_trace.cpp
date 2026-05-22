#include "apitrace/metal_trace.hpp"

#include <utility>

namespace apitrace::metal {

MetalTraceBackend::MetalTraceBackend(MetalTraceOptions options)
    : options_(std::move(options))
{
}

bool MetalTraceBackend::initialize(MetalBridge &bridge)
{
  if (!bridge.ready()) {
    last_error_ = "Metal bridge must be initialized before Metal trace starts.";
    return false;
  }

  // TODO: resolve trace-sink ownership between translation-layer-driven and standalone Metal debug sessions.
  last_error_.clear();
  return true;
}

void MetalTraceBackend::begin_trace()
{
  // TODO: start a concrete Metal trace scope once translation-layer lifecycle and output-target policies are defined.
  active_ = true;
}

void MetalTraceBackend::record_translated_call(const MetalTraceRecord &record)
{
  (void)record;
  // TODO: persist translated Metal call records and any opaque translation-owned link payload without interpreting the linkage here.
}

void MetalTraceBackend::record_frame_boundary(std::string_view frame_label)
{
  (void)frame_label;
  // TODO: map frame boundaries to readable translated-call events and optional Metal signposts.
}

void MetalTraceBackend::record_command_buffer(std::string_view command_buffer_label)
{
  (void)command_buffer_label;
  // TODO: decide whether command-buffer observation is eager, sampled, or replay-only for translated Metal traces.
}

void MetalTraceBackend::end_trace()
{
  active_ = false;
}

bool MetalTraceBackend::active() const noexcept
{
  return active_;
}

const MetalTraceOptions &MetalTraceBackend::options() const noexcept
{
  return options_;
}

const std::string &MetalTraceBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::metal
