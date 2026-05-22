#include "apitrace/d3d12_replay.hpp"

namespace apitrace::d3d12 {

D3D12ReplayBackend::D3D12ReplayBackend() = default;

bool D3D12ReplayBackend::initialize()
{
  // TODO: create the replay device/queue bootstrap path.
  last_error_.clear();
  return true;
}

bool D3D12ReplayBackend::replay_event(const trace::EventRecord &event)
{
  // TODO: dispatch typed D3D12 events instead of treating EventRecord as a generic envelope.
  (void)event;
  last_error_.clear();
  return true;
}

void D3D12ReplayBackend::shutdown()
{
  // TODO: release replay-side device, queues, descriptors, and submission state.
}

const std::string &D3D12ReplayBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::d3d12
