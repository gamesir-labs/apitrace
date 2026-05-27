#pragma once

#include <filesystem>
#include <string>

namespace apitrace::replay {

enum class BackendKind {
  NativeD3D11,
  NativeD3D12,
  TranslationLayer,
  MetalTranslation,
};

struct ReplayOptions {
  std::filesystem::path bundle_root;
  BackendKind backend = BackendKind::TranslationLayer;
  bool enable_validation = true;
  bool enable_metal_trace = false;
  bool enable_metal_retrace = false;
  bool enable_metal_present_capture = false;
  std::string metal_backend_name = "native";

  // TODO: split backend selection from replay policy once per-backend settings appear.
  // TODO: separate primary D3D retrace settings from translated Metal debug-retrace settings once both paths are wired.
  // TODO: add explicit schema/version expectations when bundle compatibility checks exist.
};

} // namespace apitrace::replay
