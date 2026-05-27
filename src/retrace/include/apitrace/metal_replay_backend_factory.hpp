#pragma once

#include "apitrace/replay_options.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace apitrace::replay {

class MetalReplayBackend {
public:
  virtual ~MetalReplayBackend() = default;

  virtual bool initialize(const trace::TraceBundleReader &reader, const ReplayOptions &options) = 0;
  virtual bool replay_metal_event(const trace::MetalEventRecord &event) = 0;
  virtual bool finalize() = 0;
  virtual const std::string &last_error() const = 0;
};

using MetalReplayBackendFactory = std::function<std::unique_ptr<MetalReplayBackend>()>;

void register_metal_replay_backend(std::string name, MetalReplayBackendFactory factory);
const MetalReplayBackendFactory *find_metal_replay_backend(std::string_view name);
void register_builtin_metal_replay_backends();

} // namespace apitrace::replay
