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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SPACE_TO_DEPTH_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SPACE_TO_DEPTH_H_

#include "tensorflow/lite/delegates/gpu/cl/cl_kernel.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

class SpaceToDepth : public GPUOperation {
 public:
  SpaceToDepth(const OperationDef& op_def, const SpaceToDepthAttributes& attr)
      : GPUOperation(op_def), attr_(attr) {}
  absl::Status BindArguments() override;
  int3 GetGridSize() const override;
  absl::Status Compile(const CreationContext& creation_context) override;

  SpaceToDepth(SpaceToDepth&& operation);
  SpaceToDepth& operator=(SpaceToDepth&& operation);
  SpaceToDepth(const SpaceToDepth&) = delete;
  SpaceToDepth& operator=(const SpaceToDepth&) = delete;

 private:
  std::string GetSpaceToDepthCode(const OperationDef& op_def);

  SpaceToDepthAttributes attr_;
};

SpaceToDepth CreateSpaceToDepth(const OperationDef& op_def,
                                const SpaceToDepthAttributes& attr);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SPACE_TO_DEPTH_H_
