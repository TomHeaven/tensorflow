/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)

#define EIGEN_USE_GPU

#include "tensorflow/core/kernels/broadcast_to_op.h"
#include "tensorflow/core/framework/register_types.h"

namespace tensorflow {

typedef Eigen::GpuDevice GPUDevice;

#define INSTANTIATE_GPU_KERNEL(Type) \
  template class functor::BroadcastTo<GPUDevice, Type>;
TF_CALL_GPU_ALL_TYPES(INSTANTIATE_GPU_KERNEL);
#undef INSTANTIATE_GPU_KERNEL

}  // namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
