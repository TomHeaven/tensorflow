/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_CONCAT_Z_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_CONCAT_Z_H_

#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/cl_kernel.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

class ConcatZ : public GPUOperation {
 public:
  ConcatZ(const OperationDef& definition, const std::vector<int>& channels)
      : GPUOperation(definition), channels_(channels) {}
  absl::Status Compile(const CreationContext& creation_context) override;
  absl::Status BindArguments() override;
  int3 GetGridSize() const override;

  // Move only
  ConcatZ(ConcatZ&& kernel);
  ConcatZ& operator=(ConcatZ&& kernel);
  ConcatZ(const ConcatZ&) = delete;
  ConcatZ& operator=(const ConcatZ&) = delete;

 private:
  std::vector<int> channels_;
};

ConcatZ CreateConcatZ(const OperationDef& definition,
                      const std::vector<int>& channels);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_CONCAT_Z_H_
