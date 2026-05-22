#pragma once

#include "apitrace/event_types.hpp"

#include <string>

namespace apitrace::d3d12 {

class D3D12ReplayBackend {
public:
  D3D12ReplayBackend();

  bool initialize();
  bool replay_event(const trace::EventRecord &event);
  void shutdown();

  const std::string &last_error() const noexcept;

private:
  std::string last_error_;

  // TODO: split device bootstrap, descriptor reconstruction, and queue submission into explicit replay phases.
  // TODO: separate native D3D12 replay from translation-layer-backed D3D12 replay if backend expectations diverge.
};

} // namespace apitrace::d3d12
