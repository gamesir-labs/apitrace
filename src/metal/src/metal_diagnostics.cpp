#include "apitrace/metal_diagnostics.hpp"

#include <utility>

namespace apitrace::metal {

void MetalDiagnosticsRecorder::record(MetalDiagnosticSeverity severity,
                                      std::string_view category,
                                      std::string_view text)
{
  MetalDiagnosticMessage message;
  message.severity = severity;
  message.category = std::string(category);
  message.text = std::string(text);
  messages_.push_back(std::move(message));
}

void MetalDiagnosticsRecorder::clear()
{
  messages_.clear();
}

const std::vector<MetalDiagnosticMessage> &MetalDiagnosticsRecorder::messages() const noexcept
{
  return messages_;
}

} // namespace apitrace::metal
