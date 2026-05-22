#pragma once

#include "apitrace/replay_options.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace apitrace::replay {

struct ReplayStatistics {
  std::uint64_t calls_replayed = 0;
  std::uint64_t frames_seen = 0;
  std::uint64_t presents_seen = 0;
  std::string backend_name;
};

class ReplaySession {
public:
  explicit ReplaySession(ReplayOptions options);
  ~ReplaySession();

  bool run();
  const ReplayOptions &options() const noexcept;
  const ReplayStatistics &statistics() const noexcept;
  const std::string &last_error() const noexcept;

private:
  // TODO: add explicit replay phases once bundle loading and backend dispatch separate cleanly.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::replay
