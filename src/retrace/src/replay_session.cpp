#include "apitrace/replay_session.hpp"

#include "apitrace/d3d12_replay.hpp"

#include "d3d11/src/d3d11_replay_internal.hpp"
#include "retrace/src/d3d11_replay_parser.hpp"

#include <memory>
#include <utility>

namespace apitrace::replay {

namespace {

const char *backend_name(BackendKind backend)
{
  switch (backend) {
  case BackendKind::NativeD3D11:
    return "NativeD3D11";
  case BackendKind::NativeD3D12:
    return "NativeD3D12";
  case BackendKind::TranslationLayer:
    return "TranslationLayer";
  case BackendKind::MetalTranslation:
    return "MetalTranslation";
  }
  return "UnknownBackend";
}

} // namespace

struct ReplaySession::Impl {
  explicit Impl(ReplayOptions opts) : options(std::move(opts)) {}

  ReplayOptions options;
  ReplayStatistics statistics;
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
  impl_->statistics = ReplayStatistics{};
  impl_->last_error.clear();

  if (impl_->options.bundle_root.empty()) {
    impl_->last_error = "bundle root is empty";
    return false;
  }

  if (!impl_->reader.open(impl_->options.bundle_root)) {
    impl_->last_error = impl_->reader.last_error().empty() ? "failed to open trace bundle" : impl_->reader.last_error();
    return false;
  }

#ifndef _WIN32
  impl_->last_error = "retrace backends are only implemented for Windows retrace.exe in the current MVP.";
  return false;
#else
  if (impl_->reader.metadata().api == trace::ApiKind::D3D11) {
    if (impl_->options.backend != BackendKind::TranslationLayer) {
      impl_->last_error = std::string("backend ") + backend_name(impl_->options.backend) +
                          " is unsupported for the D3D11 retrace MVP";
      return false;
    }

    internal::D3D11ReplayPlan plan;
    if (!internal::build_d3d11_replay_plan(impl_->reader, plan, impl_->last_error)) {
      return false;
    }

    if (!d3d11::internal::replay_translation_layer_plan(plan, impl_->statistics, impl_->last_error)) {
      return false;
    }
    return true;
  }

  if (impl_->reader.metadata().api == trace::ApiKind::D3D12) {
    if (impl_->options.backend == BackendKind::NativeD3D11) {
      impl_->last_error = "backend NativeD3D11 is unsupported for D3D12 bundles";
      return false;
    }

    d3d12::D3D12ReplayBackend backend;
    if (!backend.initialize(impl_->reader)) {
      impl_->last_error = backend.last_error().empty() ? "failed to initialize D3D12 replay backend" : backend.last_error();
      return false;
    }

    impl_->statistics.backend_name = "bundle-d3d12";
    for (const auto &event : impl_->reader.events()) {
      if (!backend.replay_event(event)) {
        if (!backend.last_error().empty()) {
          impl_->last_error = "sequence " + std::to_string(event.callsite.sequence) + " " +
                              event.callsite.function_name + ": " + backend.last_error();
        } else {
          impl_->last_error = "sequence " + std::to_string(event.callsite.sequence) + " " +
                              event.callsite.function_name + ": D3D12 replay failed";
        }
        return false;
      }

      if (event.kind == trace::EventKind::Boundary) {
        switch (event.boundary) {
        case trace::BoundaryKind::Frame:
          ++impl_->statistics.frames_seen;
          break;
        case trace::BoundaryKind::Present:
          ++impl_->statistics.presents_seen;
          break;
        default:
          break;
        }
      } else {
        ++impl_->statistics.calls_replayed;
      }
    }
    if (!backend.finalize_replay()) {
      impl_->last_error = backend.last_error().empty() ? "D3D12 replay finalization failed" : backend.last_error();
      return false;
    }
    backend.shutdown();
    return true;
  }

  impl_->last_error = "only D3D11 and D3D12 bundles are supported by the retrace MVP";
  return false;
#endif
}

const ReplayOptions &ReplaySession::options() const noexcept
{
  return impl_->options;
}

const ReplayStatistics &ReplaySession::statistics() const noexcept
{
  return impl_->statistics;
}

const std::string &ReplaySession::last_error() const noexcept
{
  return impl_->last_error;
}

} // namespace apitrace::replay
