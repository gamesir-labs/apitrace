#pragma once

#include "apitrace/metal_bridge.hpp"
#include "apitrace/metal_trace.hpp"

#include <string>

namespace apitrace::metal {

class MetalReplayBackend {
public:
  MetalReplayBackend();

  bool initialize(MetalBridge &bridge);
  bool replay_record(const MetalTraceRecord &record);
  void shutdown();

  const std::string &last_error() const noexcept;

private:
  std::string last_error_;
  bool initialized_ = false;

  // TODO: split pipeline materialization, resource upload, and submission replay once translated Metal trace semantics are fixed.
  // TODO: define the contract between native Metal retrace and Wine-hosted D3D retrace so the two debug paths stay comparable but decoupled.
};

} // namespace apitrace::metal
