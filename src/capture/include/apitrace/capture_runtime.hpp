#pragma once

#include "apitrace/capture_options.hpp"

#include <memory>
#include <string_view>

namespace apitrace::runtime {

class CaptureRuntime {
public:
  explicit CaptureRuntime(CaptureOptions options);
  ~CaptureRuntime();

  void install_hooks();
  void extend_hooks_for_module(std::string_view module_name);

  bool armed() const noexcept;
  const CaptureOptions &options() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::runtime
