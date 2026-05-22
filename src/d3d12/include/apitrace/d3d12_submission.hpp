#pragma once

#include "apitrace/object_types.hpp"

#include <vector>

namespace apitrace::d3d12 {

struct D3D12SubmissionBatch {
  trace::ObjectId queue_object_id = 0;
  std::vector<trace::ObjectId> command_list_ids;

  // TODO: add fence, barrier, and present boundary metadata once submission reconstruction starts.
};

class D3D12SubmissionTracker {
public:
  void begin_batch(trace::ObjectId queue_object_id);
  void append_command_list(trace::ObjectId command_list_id);
  void end_batch();

  bool has_open_batch() const noexcept;

private:
  D3D12SubmissionBatch current_batch_;
  bool has_open_batch_ = false;

  // TODO: retain completed batches once replay scheduling needs submission history.
};

} // namespace apitrace::d3d12
