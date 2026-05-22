#include "apitrace/d3d11_replay.hpp"

namespace apitrace::d3d11 {

D3D11ReplayBackend::D3D11ReplayBackend() = default;

bool D3D11ReplayBackend::initialize()
{
  // TODO: create the replay device/context bootstrap path.
  last_error_.clear();
  return true;
}

bool D3D11ReplayBackend::replay_event(const trace::EventRecord &event)
{
  // TODO: dispatch typed D3D11 events instead of treating EventRecord as a generic envelope.
  (void)event;
  last_error_.clear();
  return true;
}

void D3D11ReplayBackend::shutdown()
{
  // TODO: release replay-side device, context, and cached state objects.
}

const std::string &D3D11ReplayBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::d3d11
