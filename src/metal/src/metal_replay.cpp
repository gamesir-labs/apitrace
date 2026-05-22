#include "apitrace/metal_replay.hpp"

namespace apitrace::metal {

MetalReplayBackend::MetalReplayBackend() = default;

bool MetalReplayBackend::initialize(MetalBridge &bridge)
{
  if (!bridge.ready()) {
    last_error_ = "Metal bridge must be initialized before Metal retrace.";
    initialized_ = false;
    return false;
  }

  // TODO: bind replay phases to native Metal services once translated Metal trace semantics are finalized.
  last_error_.clear();
  initialized_ = true;
  return true;
}

bool MetalReplayBackend::replay_record(const MetalTraceRecord &record)
{
  (void)record;

  if (!initialized_) {
    last_error_ = "Metal replay backend is not initialized.";
    return false;
  }

  // TODO: dispatch translated Metal records into resource, pipeline, encoder, and submission replay paths.
  return true;
}

void MetalReplayBackend::shutdown()
{
  initialized_ = false;
}

const std::string &MetalReplayBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::metal
