#include "apitrace/capture_runtime.hpp"

#include "capture_hook_plan.hpp"
#include "capture_runtime_state.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace apitrace::runtime {

namespace {

std::mutex &process_capture_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::unique_ptr<TraceSession> &process_capture_session()
{
  static std::unique_ptr<TraceSession> session;
  return session;
}

std::filesystem::path resolve_bundle_root(trace::ApiKind api)
{
  if (const char *env_path = std::getenv("APITRACE_TRACE_BUNDLE")) {
    if (*env_path != '\0') {
      return std::filesystem::path(env_path);
    }
  }

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  std::ostringstream name;
  name << "capture-";
  switch (api) {
  case trace::ApiKind::D3D11:
    name << "d3d11";
    break;
  case trace::ApiKind::D3D12:
    name << "d3d12";
    break;
  default:
    name << "unknown";
    break;
  }
  name << "-" << millis << ".apitrace";
  return std::filesystem::current_path() / name.str();
}

void shutdown_process_capture_impl()
{
  auto &session = process_capture_session();
  if (session && session->active()) {
    session->end();
  }
  session.reset();
}

void register_process_capture_shutdown()
{
  static bool registered = false;
  if (!registered) {
    std::atexit(shutdown_process_capture_impl);
    registered = true;
  }
}

} // namespace

struct CaptureRuntime::Impl {
  explicit Impl(CaptureOptions opts) : state(std::move(opts)) {}

  capture::internal::CaptureRuntimeState state;

  // TODO: add a diagnostics sink once hook planning and hook installation report separate failure modes.
};

CaptureRuntime::CaptureRuntime(CaptureOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

CaptureRuntime::~CaptureRuntime() = default;

void CaptureRuntime::install_hooks()
{
  // TODO: install planned D3D/DXGI interception surfaces based on hook plan and selected entry mode.
  // TODO: distinguish mandatory hook surfaces from optional late-module surfaces.
  impl_->state.arm();
}

void CaptureRuntime::extend_hooks_for_module(std::string_view module_name)
{
  capture::internal::register_late_module(impl_->state.hook_plan(), module_name);
  // TODO: reconnect this to the dynamic module scanner and late hook installer.
  impl_->state.remember_hooked_module(std::string(module_name));
}

bool CaptureRuntime::armed() const noexcept
{
  return impl_ && impl_->state.armed();
}

const CaptureOptions &CaptureRuntime::options() const noexcept
{
  return impl_->state.options();
}

TraceSession *ensure_process_trace_session(trace::ApiKind api)
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  auto &session = process_capture_session();
  if (!session) {
    TraceOptions options;
    options.api = api;
    options.capture.mode = CaptureMode::ProxyDll;
    options.bundle_root = resolve_bundle_root(api);
    session = std::make_unique<TraceSession>(std::move(options));
    session->begin();
    register_process_capture_shutdown();
  }
  return session.get();
}

TraceSession *current_process_trace_session() noexcept
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  return process_capture_session().get();
}

void shutdown_process_trace_session() noexcept
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  shutdown_process_capture_impl();
}

} // namespace apitrace::runtime
