#include "apitrace/capture_runtime.hpp"

#include "capture_hook_plan.hpp"
#include "capture_runtime_state.hpp"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

CaptureOptions::CaptureRawMode parse_capture_raw_mode()
{
  const char *value = std::getenv("DXMT_CAPTURE_RAW_FORMAT");
  if (!value || *value == '\0') {
    return CaptureOptions::CaptureRawMode::Off;
  }

  const std::string_view text(value);
  if (text == "0") {
    return CaptureOptions::CaptureRawMode::Off;
  }
  if (text == "1" || text == "dual" || text == "dual-write") {
    return CaptureOptions::CaptureRawMode::DualWrite;
  }
  if (text == "2" || text == "raw-only") {
    return CaptureOptions::CaptureRawMode::RawOnly;
  }
  return CaptureOptions::CaptureRawMode::Off;
}

void shutdown_process_capture_impl()
{
  auto &session = process_capture_session();
  if (session && session->active()) {
    session->end();
  }
  session.reset();
}

void seal_process_capture_for_crash() noexcept
{
  static std::atomic<bool> sealing{false};
  if (sealing.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  std::unique_lock<std::mutex> lock(process_capture_mutex(), std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }

  auto &session = process_capture_session();
  if (session && session->active()) {
    session->seal_checkpoint();
  }
}

#ifdef _WIN32
LONG WINAPI apitrace_unhandled_exception_filter(EXCEPTION_POINTERS *)
{
  seal_process_capture_for_crash();
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI apitrace_vectored_exception_handler(EXCEPTION_POINTERS *exception_info)
{
  if (!exception_info || !exception_info->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  switch (exception_info->ExceptionRecord->ExceptionCode) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_FLT_DENORMAL_OPERAND:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
  case EXCEPTION_FLT_INEXACT_RESULT:
  case EXCEPTION_FLT_INVALID_OPERATION:
  case EXCEPTION_FLT_OVERFLOW:
  case EXCEPTION_FLT_STACK_CHECK:
  case EXCEPTION_FLT_UNDERFLOW:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_INT_OVERFLOW:
  case EXCEPTION_INVALID_DISPOSITION:
  case EXCEPTION_NONCONTINUABLE_EXCEPTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_STACK_OVERFLOW:
    seal_process_capture_for_crash();
    break;
  default:
    break;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void register_process_capture_crash_handlers()
{
  static bool registered = false;
  if (registered) {
    return;
  }

  if (const char *value = std::getenv("APITRACE_CRASH_SEAL_FIRST_CHANCE");
      value && *value && *value != '0') {
    AddVectoredExceptionHandler(1, apitrace_vectored_exception_handler);
  }
  SetUnhandledExceptionFilter(apitrace_unhandled_exception_filter);
  registered = true;
}
#else
void register_process_capture_crash_handlers() {}
#endif

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
    options.capture.raw_mode = parse_capture_raw_mode();
    options.bundle_root = resolve_bundle_root(api);
    session = std::make_unique<TraceSession>(std::move(options));
    session->begin();
    register_process_capture_shutdown();
    register_process_capture_crash_handlers();
  }
  return session.get();
}

TraceSession *current_process_trace_session() noexcept
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  return process_capture_session().get();
}

void flush_process_trace_session() noexcept
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  auto &session = process_capture_session();
  if (session && session->active()) {
    session->flush();
  }
}

void seal_process_trace_session_checkpoint() noexcept
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  auto &session = process_capture_session();
  if (session && session->active()) {
    session->seal_checkpoint();
  }
}

void shutdown_process_trace_session() noexcept
{
  std::lock_guard<std::mutex> lock(process_capture_mutex());
  shutdown_process_capture_impl();
}

} // namespace apitrace::runtime
