#include "apitrace/replay_session.hpp"

#include <memory>
#include <utility>

namespace apitrace::replay {

struct ReplaySession::Impl {
  explicit Impl(ReplayOptions opts) : options(std::move(opts)) {}

  ReplayOptions options;
  trace::TraceBundleReader reader;
  std::string last_error;
};

ReplaySession::ReplaySession(ReplayOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

ReplaySession::~ReplaySession() = default;

bool ReplaySession::run()
{
  if (impl_->options.bundle_root.empty()) {
    impl_->last_error = "bundle root is empty";
    return false;
  }

  if (!impl_->reader.open(impl_->options.bundle_root)) {
    impl_->last_error = "failed to open trace bundle";
    return false;
  }

  // TODO: validate checksums.json before parsing callstream.jsonl.
  // TODO: map readable callstream records back into replay dispatch units.
  // TODO: connect parsed events to backend-specific replay drivers.
  impl_->last_error.clear();
  return true;
}

const ReplayOptions &ReplaySession::options() const noexcept
{
  return impl_->options;
}

const std::string &ReplaySession::last_error() const noexcept
{
  return impl_->last_error;
}

} // namespace apitrace::replay
