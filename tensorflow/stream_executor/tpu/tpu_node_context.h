/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_STREAM_EXECUTOR_TPU_TPU_NODE_CONTEXT_H_
#define TENSORFLOW_STREAM_EXECUTOR_TPU_TPU_NODE_CONTEXT_H_

#include <string>

#include "absl/memory/memory.h"
#include "tensorflow/compiler/xla/service/backend.h"
#include "tensorflow/compiler/xla/service/stream_pool.h"
#include "tensorflow/compiler/xla/service/transfer_manager.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/stream_executor/device_memory_allocator.h"
#include "tensorflow/stream_executor/lib/status.h"
#include "tensorflow/stream_executor/lib/statusor.h"
#include "tensorflow/stream_executor/tpu/status_helper.h"
#include "tensorflow/stream_executor/tpu/tpu_node_context_c_api.h"
#include "tensorflow/stream_executor/tpu/tpu_platform_interface.h"

namespace tensorflow {
namespace tpu {

class TpuNodeContext final {
 public:
  using Status = stream_executor::port::Status;
  template <typename T>
  using StatusOr = stream_executor::port::StatusOr<T>;

  static StatusOr<std::unique_ptr<TpuNodeContext>> Create(int device_ordinal);

  explicit TpuNodeContext(int device_ordinal, XLA_TpuNodeContext* node_context)
      : device_ordinal_(device_ordinal), node_context_(node_context) {
    CHECK_NE(node_context, nullptr);
  }
  ~TpuNodeContext();

  TpuNodeContext(const TpuNodeContext&) = delete;
  TpuNodeContext& operator=(const TpuNodeContext&) = delete;

  static Status Initialize(int device_ordinal);

  static Status StopChipHeartbeats();

  static Status CloseTpuHost();

  static tensorflow::tpu::TpuPlatformInterface* platform();

  static stream_executor::DeviceMemoryAllocator* memory_allocator();

  static xla::TransferManager* transfer_manager();

  static xla::Backend* backend();

  static StatusOr<xla::StreamPool::Ptr> BorrowStream(int device_ordinal);

  static StatusOr<xla::StreamPool::Ptr> BorrowStream(
      stream_executor::StreamExecutor* executor);

  stream_executor::StreamExecutor* stream_executor() {
    LOG(FATAL) << "Not implemented yet.";
  }

  std::string tensor_core_location() { LOG(FATAL) << "Not implemented yet."; }

  int index_on_host() { LOG(FATAL) << "Not implemented yet."; }

  int device_ordinal() const { return device_ordinal_; }

 private:
  const int device_ordinal_;
  XLA_TpuNodeContext* const node_context_;
};

}  // namespace tpu
}  // namespace tensorflow

#endif  // TENSORFLOW_STREAM_EXECUTOR_TPU_TPU_NODE_CONTEXT_H_
