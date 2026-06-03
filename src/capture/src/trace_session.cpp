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

void TraceSession::flush()
{
  impl_->state.flush();
}

void TraceSession::seal_checkpoint()
{
  impl_->state.seal_checkpoint();
}

void TraceSession::append_call_event(const trace::EventRecord &event)
{
  impl_->state.append_call_event(event);
}

void TraceSession::append_call_event(trace::EventRecord &&event)
{
  impl_->state.append_call_event(std::move(event));
}

void TraceSession::append_analysis_line(std::string_view stream_name, std::string_view json_line)
{
  impl_->state.append_analysis_line(stream_name, json_line);
}

trace::AssetRecord TraceSession::register_asset(const trace::AssetRecord &asset)
{
  return impl_->state.register_asset(asset);
}

trace::AssetRecord TraceSession::register_asset(trace::AssetRecord &&asset)
{
  return impl_->state.register_asset(std::move(asset));
}

void TraceSession::record_object(const trace::ObjectRecord &object)
{
  impl_->state.record_object(object);
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
