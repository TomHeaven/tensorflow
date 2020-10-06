/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/conditional_thunk.h"

#include "absl/memory/memory.h"
#include "tensorflow/compiler/xla/service/gpu/hlo_execution_profiler.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"

namespace xla {
namespace gpu {

ConditionalThunkConfig GetConditionalThunkConfig(
    const HloInstruction* instr,
    std::vector<ThunkSequence>&& branch_thunk_sequences,
    std::vector<absl::optional<size_t>>&& branch_profile_indices) {
  ConditionalThunkConfig config;
  config.branch_index_is_bool =
      instr->operand(0)->shape().element_type() == PRED;
  config.branch_count = instr->branch_count();
  // Pass nullptr as the HloInstruction* to the branch_thunks
  // constructors because these SequentialThunks are logically "part of"
  // this ConditionalThunk, and shouldn't be profiled separately from it.
  config.branch_thunks.reserve(branch_thunk_sequences.size());
  for (auto& branch_thunk_sequence : branch_thunk_sequences) {
    config.branch_thunks.emplace_back(new SequentialThunk(
        Thunk::ThunkInfo(), std::move(branch_thunk_sequence)));
  }
  config.branch_profile_indices = std::move(branch_profile_indices);
  return config;
}

ConditionalThunk::ConditionalThunk(
    ThunkInfo thunk_info, ConditionalThunkConfig&& config,
    const BufferAllocation::Slice& branch_index_buffer_index,
    absl::Span<const BufferAllocation::Slice> branch_operand_buffer_indexes)
    : Thunk(Kind::kConditional, thunk_info),
      config_(std::move(config)),
      branch_index_buffer_index_(branch_index_buffer_index),
      branch_operand_buffer_indexes_(branch_operand_buffer_indexes.begin(),
                                     branch_operand_buffer_indexes.end()) {}

Status ConditionalThunk::Initialize(const GpuExecutable& executable,
                                    se::StreamExecutor* executor) {
  if (config_.branch_index_is_bool) {
    TF_RET_CHECK(config_.branch_thunks.size() == 2);
  } else {
    TF_RET_CHECK(!config_.branch_thunks.empty());
  }
  for (auto& branch_thunk : config_.branch_thunks) {
    TF_RETURN_IF_ERROR(branch_thunk->Initialize(executable, executor));
  }
  return Status::OK();
}

Status ConditionalThunk::ExecuteOnStream(const ExecuteParams& params) {
  auto& profiler = *params.profiler;
  auto& stream = *params.stream;

  auto op_profiler = profiler.MakeScopedInstructionProfiler(profile_index());
  // Copy the predicate value from device.
  int32 branch_index = -1;
  bool pred = false;
  se::DeviceMemoryBase branch_index_address =
      params.buffer_allocations->GetDeviceAddress(branch_index_buffer_index_);
  if (config_.branch_index_is_bool) {
    stream.ThenMemcpy(&pred, branch_index_address, sizeof(bool));
  } else {
    stream.ThenMemcpy(&branch_index, branch_index_address, sizeof(int32));
  }

  Status block_status = stream.BlockHostUntilDone();
  if (!block_status.ok()) {
    return InternalError(
        "Failed to retrieve branch_index value on stream %p: %s.", &stream,
        block_status.error_message());
  }
  if (config_.branch_index_is_bool) {
    branch_index = pred ? 0 : 1;
  } else {
    // Handle default scenario for branch_index not in [0, num_branches).
    if (branch_index < 0 || branch_index >= config_.branch_count) {
      branch_index = config_.branch_count - 1;
    }
  }

  // Execute the branch computation corresponding to the value of branch_index.
  profiler.StartHloComputation();
  TF_RETURN_IF_ERROR(
      config_.branch_thunks[branch_index]->ExecuteOnStream(params));
  profiler.FinishHloComputation(config_.branch_profile_indices[branch_index]);

  return Status::OK();
}

}  // namespace gpu
}  // namespace xla
