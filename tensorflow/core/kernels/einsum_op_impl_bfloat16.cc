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

#include "tensorflow/core/kernels/einsum_op_impl.h"

namespace tensorflow {

#define REGISTER_EINSUM(D, TYPE)                                   \
  REGISTER_KERNEL_BUILDER(                                         \
      Name("Einsum").Device(DEVICE_##D).TypeConstraint<TYPE>("T"), \
      EinsumOp<D##Device, TYPE>);

#define REGISTER_CPU(TYPE) REGISTER_EINSUM(CPU, TYPE)
TF_CALL_bfloat16(REGISTER_CPU);
#undef REGISTER_CPU

#undef REGISTER_EINSUM

}  // namespace tensorflow
