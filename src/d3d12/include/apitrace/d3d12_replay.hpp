#pragma once

#include "apitrace/event_types.hpp"
#include "apitrace/d3d12_state.hpp"
#include "apitrace/d3d12_submission.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <string>

namespace apitrace::d3d12 {

class D3D12ReplayBackend {
public:
  D3D12ReplayBackend();

  bool initialize(const trace::TraceBundleReader &reader);
  bool replay_event(const trace::EventRecord &event);
  void shutdown();

  const std::string &last_error() const noexcept;

private:
  std::string last_error_;
  bool initialized_ = false;
  std::uint64_t commands_replayed_ = 0;
  std::uint64_t frames_seen_ = 0;
  std::uint64_t presents_seen_ = 0;
  std::uint64_t pipeline_assets_read_ = 0;
  std::uint64_t last_sequence_ = 0;
  D3D12ObjectRegistry objects_;
  D3D12SubmissionTracker submissions_;

  // TODO: split device bootstrap, descriptor reconstruction, and queue submission into explicit replay phases.
  // TODO: separate native D3D12 replay from translation-layer-backed D3D12 replay if backend expectations diverge.
};

} // namespace apitrace::d3d12
