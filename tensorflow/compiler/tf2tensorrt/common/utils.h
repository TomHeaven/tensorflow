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

#ifndef TENSORFLOW_COMPILER_TF2TENSORRT_COMMON_UTILS_H_
#define TENSORFLOW_COMPILER_TF2TENSORRT_COMMON_UTILS_H_

#if GOOGLE_CUDA
#if GOOGLE_TENSORRT

#include "tensorflow/core/platform/logging.h"

namespace tensorflow {
namespace tensorrt {

#define LOG_WARNING_WITH_PREFIX LOG(WARNING) << "TF-TRT Warning: "

}  // namespace tensorrt
}  // namespace tensorflow

#endif
#endif

#endif  // TENSORFLOW_COMPILER_TF2TENSORRT_COMMON_UTILS_H_
