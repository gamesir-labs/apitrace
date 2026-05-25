#include "apitrace/d3d12_submission.hpp"

namespace apitrace::d3d12 {

void D3D12SubmissionTracker::begin_batch(trace::ObjectId queue_object_id, trace::ObjectId command_allocator_object_id)
{
  current_batch_ = {};
  current_batch_.queue_object_id = queue_object_id;
  current_batch_.command_allocator_object_id = command_allocator_object_id;
  has_open_batch_ = true;

  // TODO: reject nested batches once queue-level replay scheduling is explicit.
}

void D3D12SubmissionTracker::append_command_list(trace::ObjectId command_list_id)
{
  if (!has_open_batch_) {
    // TODO: surface diagnostics once submission sequencing is validated instead of silently ignoring misuse.
    return;
  }

  current_batch_.command_list_ids.push_back(command_list_id);
}

void D3D12SubmissionTracker::append_descriptor_heap(trace::ObjectId descriptor_heap_id)
{
  if (!has_open_batch_) {
    return;
  }

  current_batch_.descriptor_heap_ids.push_back(descriptor_heap_id);
}

void D3D12SubmissionTracker::signal_fence(trace::ObjectId fence_object_id, std::uint64_t fence_value)
{
  if (!has_open_batch_) {
    return;
  }

  current_batch_.fence_object_id = fence_object_id;
  current_batch_.fence_value = fence_value;
}

void D3D12SubmissionTracker::mark_present()
{
  if (!has_open_batch_) {
    return;
  }

  current_batch_.presented = true;
}

void D3D12SubmissionTracker::end_batch()
{
  has_open_batch_ = false;

  // TODO: retain or emit completed submission batches once replay scheduling consumes them.
}

void D3D12SubmissionTracker::clear()
{
  current_batch_ = {};
  has_open_batch_ = false;
}

bool D3D12SubmissionTracker::has_open_batch() const noexcept
{
  return has_open_batch_;
}

const D3D12SubmissionBatch &D3D12SubmissionTracker::current_batch() const noexcept
{
  return current_batch_;
}

} // namespace apitrace::d3d12
