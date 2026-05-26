#include "apitrace/d3d12_submission.hpp"

#include <algorithm>

namespace apitrace::d3d12 {

void D3D12SubmissionTracker::begin_batch(
    trace::ObjectId queue_object_id,
    trace::ObjectId command_allocator_object_id,
    std::uint64_t execute_sequence)
{
  current_batch_ = {};
  current_batch_.queue_object_id = queue_object_id;
  current_batch_.command_allocator_object_id = command_allocator_object_id;
  current_batch_.execute_sequence = execute_sequence;
  for (auto wait = pending_waits_.begin(); wait != pending_waits_.end();) {
    if (wait->queue_object_id != queue_object_id) {
      ++wait;
      continue;
    }
    current_batch_.waits_before_execute.push_back(*wait);
    wait = pending_waits_.erase(wait);
  }
  has_open_batch_ = true;
}

void D3D12SubmissionTracker::set_command_allocator(trace::ObjectId command_allocator_object_id)
{
  if (!has_open_batch_) {
    return;
  }

  if (current_batch_.command_allocator_object_id == 0) {
    current_batch_.command_allocator_object_id = command_allocator_object_id;
  }
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

  if (std::find(
          current_batch_.descriptor_heap_ids.begin(),
          current_batch_.descriptor_heap_ids.end(),
          descriptor_heap_id) != current_batch_.descriptor_heap_ids.end()) {
    return;
  }

  current_batch_.descriptor_heap_ids.push_back(descriptor_heap_id);
}

void D3D12SubmissionTracker::append_descriptor_heaps(const std::vector<trace::ObjectId> &descriptor_heap_ids)
{
  for (const auto descriptor_heap_id : descriptor_heap_ids) {
    append_descriptor_heap(descriptor_heap_id);
  }
}

void D3D12SubmissionTracker::signal_fence(
    trace::ObjectId fence_object_id,
    std::uint64_t fence_value,
    std::uint64_t sequence)
{
  if (!has_open_batch_) {
    return;
  }

  current_batch_.fence_object_id = fence_object_id;
  current_batch_.fence_value = fence_value;
  current_batch_.fence_sequence = sequence;
}

void D3D12SubmissionTracker::signal_fence_for_queue(
    trace::ObjectId queue_object_id,
    trace::ObjectId fence_object_id,
    std::uint64_t fence_value,
    std::uint64_t sequence)
{
  if (has_open_batch_ && current_batch_.queue_object_id == queue_object_id) {
    signal_fence(fence_object_id, fence_value, sequence);
    return;
  }

  for (auto batch = completed_batches_.rbegin(); batch != completed_batches_.rend(); ++batch) {
    if (batch->queue_object_id != queue_object_id) {
      continue;
    }
    batch->fence_object_id = fence_object_id;
    batch->fence_value = fence_value;
    batch->fence_sequence = sequence;
    return;
  }
}

void D3D12SubmissionTracker::wait_on_fence_for_queue(
    trace::ObjectId queue_object_id,
    trace::ObjectId fence_object_id,
    std::uint64_t fence_value,
    std::uint64_t sequence)
{
  D3D12QueueWait wait;
  wait.queue_object_id = queue_object_id;
  wait.fence_object_id = fence_object_id;
  wait.fence_value = fence_value;
  wait.sequence = sequence;

  if (has_open_batch_ && current_batch_.queue_object_id == queue_object_id) {
    current_batch_.waits_before_execute.push_back(wait);
    return;
  }

  pending_waits_.push_back(wait);
}

void D3D12SubmissionTracker::mark_present(std::uint64_t sequence)
{
  if (has_open_batch_) {
    current_batch_.presented = true;
    current_batch_.present_sequence = sequence;
    return;
  }

  if (!completed_batches_.empty()) {
    completed_batches_.back().presented = true;
    completed_batches_.back().present_sequence = sequence;
  }
}

void D3D12SubmissionTracker::end_batch()
{
  if (has_open_batch_) {
    completed_batches_.push_back(current_batch_);
  }
  has_open_batch_ = false;
  current_batch_ = {};
}

void D3D12SubmissionTracker::clear()
{
  current_batch_ = {};
  completed_batches_.clear();
  pending_waits_.clear();
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

const std::vector<D3D12SubmissionBatch> &D3D12SubmissionTracker::completed_batches() const noexcept
{
  return completed_batches_;
}

} // namespace apitrace::d3d12
