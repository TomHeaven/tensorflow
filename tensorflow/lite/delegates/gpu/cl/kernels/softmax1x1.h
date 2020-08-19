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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SOFTMAX1X1_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SOFTMAX1X1_H_

#include "tensorflow/lite/delegates/gpu/cl/cl_kernel.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor.h"

namespace tflite {
namespace gpu {
namespace cl {

class Softmax1x1 : public GPUOperation {
 public:
  Softmax1x1() = default;
  explicit Softmax1x1(const OperationDef& definition);
  void GetPossibleKernelWorkGroups(
      TuningType tuning_type, const DeviceInfo& device_info,
      const KernelInfo& kernel_info,
      std::vector<int3>* work_groups) const override {
    work_groups->push_back(work_group_size_);
  }
  absl::Status BindArguments() override;
  int3 GetGridSize() const override;

  // Move only
  Softmax1x1(Softmax1x1&& kernel);
  Softmax1x1& operator=(Softmax1x1&& kernel);
  Softmax1x1(const Softmax1x1&) = delete;
  Softmax1x1& operator=(const Softmax1x1&) = delete;

  friend Softmax1x1 CreateSoftmax1x1();

 private:
  std::string GetSoftmaxKernelCode(const OperationDef& op_def);
};

Softmax1x1 CreateSoftmax1x1(const OperationDef& definition);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SOFTMAX1X1_H_
