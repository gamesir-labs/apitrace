#include "apitrace/metal_replay_backend_factory.hpp"

#include <map>
#include <mutex>
#include <utility>

namespace apitrace::replay {

namespace {

std::map<std::string, MetalReplayBackendFactory> &registry()
{
  static std::map<std::string, MetalReplayBackendFactory> factories;
  return factories;
}

std::mutex &registry_mutex()
{
  static std::mutex mutex;
  return mutex;
}

} // namespace

#ifdef APITRACE_HAVE_METAL_BACKEND
void register_native_metal_replay_backend();
#endif

void register_metal_replay_backend(std::string name, MetalReplayBackendFactory factory)
{
  if (name.empty() || !factory) {
    return;
  }

  std::lock_guard<std::mutex> lock(registry_mutex());
  registry()[std::move(name)] = std::move(factory);
}

const MetalReplayBackendFactory *find_metal_replay_backend(std::string_view name)
{
  std::lock_guard<std::mutex> lock(registry_mutex());
  const auto it = registry().find(std::string(name));
  return it == registry().end() ? nullptr : &it->second;
}

void register_builtin_metal_replay_backends()
{
  static bool registered = false;
  if (registered) {
    return;
  }

#ifdef APITRACE_HAVE_METAL_BACKEND
  register_native_metal_replay_backend();
#endif
  registered = true;
}

} // namespace apitrace::replay
