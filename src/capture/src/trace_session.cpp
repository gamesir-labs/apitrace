#include "apitrace/trace_session.hpp"
#include "trace_session_state.hpp"

#include <memory>
#include <utility>

namespace apitrace {

struct TraceSession::Impl
{
  explicit Impl(TraceOptions opts) : state(std::move(opts)) {}

  capture::internal::TraceSessionState state;
};

TraceSession::TraceSession(TraceOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

TraceSession::~TraceSession() = default;

void TraceSession::begin()
{
  impl_->state.begin();
}

void TraceSession::end()
{
  impl_->state.end();
}

bool TraceSession::active() const noexcept
{
  return impl_ && impl_->state.active();
}

const TraceOptions &TraceSession::options() const noexcept
{
  return impl_->state.options();
}

} // namespace apitrace
