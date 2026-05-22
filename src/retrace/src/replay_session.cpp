#include "apitrace/replay_session.hpp"

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

  if (impl_->options.backend != BackendKind::TranslationLayer) {
    impl_->last_error = std::string("backend ") + backend_name(impl_->options.backend) +
                        " is unsupported for the D3D11 retrace MVP";
    return false;
  }

#ifndef _WIN32
  impl_->last_error = "Translation-layer D3D11 replay is only implemented for Windows retrace.exe.";
  return false;
#else
  if (impl_->reader.metadata().api != trace::ApiKind::D3D11) {
    impl_->last_error = "only D3D11 bundles are supported by the retrace MVP";
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
