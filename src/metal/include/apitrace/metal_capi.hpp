#pragma once

#include "apitrace/metal_capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace apitrace::metal {

class CapiSession {
public:
  CapiSession() = default;
  explicit CapiSession(apitrace_metal_session_t *session) : session_(session) {}

  CapiSession(const CapiSession &) = delete;
  CapiSession &operator=(const CapiSession &) = delete;

  CapiSession(CapiSession &&other) noexcept : session_(other.session_)
  {
    other.session_ = nullptr;
  }

  CapiSession &operator=(CapiSession &&other) noexcept
  {
    if (this != &other) {
      close();
      session_ = other.session_;
      other.session_ = nullptr;
    }
    return *this;
  }

  ~CapiSession()
  {
    close();
  }

  static CapiSession open(const std::string &bundle_root)
  {
    return CapiSession(apitrace_metal_session_open(bundle_root.c_str()));
  }

  void close()
  {
    if (session_ != nullptr) {
      apitrace_metal_session_close(session_);
      session_ = nullptr;
    }
  }

  void set_current_d3d_sequence(std::uint64_t sequence) const
  {
    apitrace_metal_set_current_d3d_sequence(session_, sequence);
  }

  std::uint64_t current_metal_sequence() const
  {
    return apitrace_metal_current_metal_sequence(session_);
  }

  apitrace_metal_session_t *get() const noexcept
  {
    return session_;
  }

  explicit operator bool() const noexcept
  {
    return session_ != nullptr;
  }

private:
  apitrace_metal_session_t *session_ = nullptr;
};

void record_present_frame(
    apitrace_metal_session_t *session,
    std::uint64_t frame_index,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t row_pitch,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    const std::vector<std::uint8_t> &bgra_bytes);

} // namespace apitrace::metal
