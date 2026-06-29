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

trace::AssetRecord TraceSession::stage_raw_asset(trace::AssetRecord &&asset)
{
  return impl_->state.stage_raw_asset(std::move(asset));
}

void TraceSession::record_object(const trace::ObjectRecord &object)
{
  impl_->state.record_object(object);
}

bool TraceSession::active() const noexcept
{
  return impl_ && impl_->state.active();
}

std::uint64_t TraceSession::initial_call_sequence() const noexcept
{
  return impl_ ? impl_->state.initial_call_sequence() : 0;
}

const TraceOptions &TraceSession::options() const noexcept
{
  return impl_->state.options();
}

trace::raw::RawCaptureWriter *TraceSession::raw_capture_writer() noexcept
{
  return impl_ ? impl_->state.raw_capture_writer() : nullptr;
}

const trace::raw::RawCaptureWriter *TraceSession::raw_capture_writer() const noexcept
{
  return impl_ ? impl_->state.raw_capture_writer() : nullptr;
}

std::uint64_t TraceSession::raw_commit_cadence_bytes() const noexcept
{
  return impl_ ? impl_->state.raw_commit_cadence_bytes() : 0;
}

} // namespace apitrace
