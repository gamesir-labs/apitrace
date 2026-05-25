#pragma once

#include "apitrace/object_types.hpp"

#include <cstdint>
#include <vector>

namespace apitrace::d3d12 {

struct D3D12SubmissionBatch {
  trace::ObjectId queue_object_id = 0;
  trace::ObjectId command_allocator_object_id = 0;
  std::vector<trace::ObjectId> command_list_ids;
  std::vector<trace::ObjectId> descriptor_heap_ids;
  trace::ObjectId fence_object_id = 0;
  std::uint64_t fence_value = 0;
  bool presented = false;

  // TODO: add fence, barrier, and present boundary metadata once submission reconstruction starts.
};

class D3D12SubmissionTracker {
public:
  void begin_batch(trace::ObjectId queue_object_id, trace::ObjectId command_allocator_object_id = 0);
  void append_command_list(trace::ObjectId command_list_id);
  void append_descriptor_heap(trace::ObjectId descriptor_heap_id);
  void signal_fence(trace::ObjectId fence_object_id, std::uint64_t fence_value);
  void mark_present();
  void end_batch();
  void clear();

  bool has_open_batch() const noexcept;
  const D3D12SubmissionBatch &current_batch() const noexcept;

private:
  D3D12SubmissionBatch current_batch_;
  bool has_open_batch_ = false;

  // TODO: retain completed batches once replay scheduling needs submission history.
};

} // namespace apitrace::d3d12
