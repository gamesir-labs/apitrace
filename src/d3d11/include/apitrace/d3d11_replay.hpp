#pragma once

#include "apitrace/event_types.hpp"

#include <string>

namespace apitrace::d3d11 {

class D3D11ReplayBackend {
public:
  D3D11ReplayBackend();

  bool initialize();
  bool replay_event(const trace::EventRecord &event);
  void shutdown();

  const std::string &last_error() const noexcept;

private:
  std::string last_error_;

  // TODO: add explicit replay phases for device bootstrap, state reconstruction, and draw submission.
  // TODO: separate native D3D11 replay from translation-layer D3D11 replay if they diverge materially.
};

} // namespace apitrace::d3d11
