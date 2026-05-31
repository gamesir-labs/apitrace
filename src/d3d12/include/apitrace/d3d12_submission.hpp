#pragma once

#include "apitrace/object_types.hpp"

#include <cstdint>
#include <vector>

namespace apitrace::d3d12 {

struct D3D12QueueWait {
  trace::ObjectId queue_object_id = 0;
  trace::ObjectId fence_object_id = 0;
  std::uint64_t fence_value = 0;
  std::uint64_t sequence = 0;
};

struct D3D12SubmissionBatch {
  trace::ObjectId queue_object_id = 0;
  trace::ObjectId command_allocator_object_id = 0;
  std::vector<trace::ObjectId> command_list_ids;
  std::vector<trace::ObjectId> descriptor_heap_ids;
  std::vector<D3D12QueueWait> waits_before_execute;
  trace::ObjectId fence_object_id = 0;
  std::uint64_t execute_sequence = 0;
  std::uint64_t fence_sequence = 0;
  std::uint64_t present_sequence = 0;
  std::uint64_t present_frame_index = 0;
  std::uint64_t fence_value = 0;
  bool presented = false;
};

class D3D12SubmissionTracker {
public:
  void begin_batch(
      trace::ObjectId queue_object_id,
      trace::ObjectId command_allocator_object_id = 0,
      std::uint64_t execute_sequence = 0);
  void set_command_allocator(trace::ObjectId command_allocator_object_id);
  void append_command_list(trace::ObjectId command_list_id);
  void append_descriptor_heap(trace::ObjectId descriptor_heap_id);
  void append_descriptor_heaps(const std::vector<trace::ObjectId> &descriptor_heap_ids);
  void signal_fence(trace::ObjectId fence_object_id, std::uint64_t fence_value, std::uint64_t sequence = 0);
  void signal_fence_for_queue(
      trace::ObjectId queue_object_id,
      trace::ObjectId fence_object_id,
      std::uint64_t fence_value,
      std::uint64_t sequence = 0);
  void wait_on_fence_for_queue(
      trace::ObjectId queue_object_id,
      trace::ObjectId fence_object_id,
      std::uint64_t fence_value,
      std::uint64_t sequence = 0);
  void mark_present(std::uint64_t sequence = 0, std::uint64_t frame_index = 0);
  void end_batch();
  void clear();

  bool has_open_batch() const noexcept;
  const D3D12SubmissionBatch &current_batch() const noexcept;
  const std::vector<D3D12SubmissionBatch> &completed_batches() const noexcept;

private:
  D3D12SubmissionBatch current_batch_;
  std::vector<D3D12SubmissionBatch> completed_batches_;
  std::vector<D3D12QueueWait> pending_waits_;
  bool has_open_batch_ = false;

  // TODO: add queue timeline diagnostics once native D3D12 replay consumes completed batches.
};

} // namespace apitrace::d3d12
