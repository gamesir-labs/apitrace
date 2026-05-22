#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace apitrace::metal {

enum class MetalDiagnosticSeverity {
  Note,
  Warning,
  Error,
};

struct MetalDiagnosticMessage {
  MetalDiagnosticSeverity severity = MetalDiagnosticSeverity::Note;
  std::string category;
  std::string text;
};

class MetalDiagnosticsRecorder {
public:
  void record(MetalDiagnosticSeverity severity, std::string_view category, std::string_view text);
  void clear();

  const std::vector<MetalDiagnosticMessage> &messages() const noexcept;

private:
  std::vector<MetalDiagnosticMessage> messages_;

  // TODO: route diagnostics to shared trace-analysis artifacts once bundle-side reporting is defined.
  // TODO: separate transient runtime warnings from replay-fatal diagnostics when backend error policy is settled.
};

} // namespace apitrace::metal
