#pragma once

#include "apitrace/capture_options.hpp"

#include <memory>

namespace apitrace {

class TraceSession {
public:
  explicit TraceSession(TraceOptions options);
  ~TraceSession();

  void begin();
  void end();

  bool active() const noexcept;
  const TraceOptions &options() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace
