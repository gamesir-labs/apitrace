#pragma once

#include "apitrace/replay_options.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <memory>
#include <string>

namespace apitrace::replay {

class ReplaySession {
public:
  explicit ReplaySession(ReplayOptions options);
  ~ReplaySession();

  bool run();
  const ReplayOptions &options() const noexcept;
  const std::string &last_error() const noexcept;

private:
  // TODO: add explicit replay phases once bundle loading and backend dispatch separate cleanly.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::replay
