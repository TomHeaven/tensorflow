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

#ifndef TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_XLA_QUANTIZE_H_
#define TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_XLA_QUANTIZE_H_

#include "tensorflow/compiler/tf2xla/tf2xla.pb.h"
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/core/platform/status.h"

namespace mlir {
namespace xla_hlo {

// Quantizes the model in the computation.
tensorflow::Status XlaQuantize(const tensorflow::tf2xla::Config& config,
                               xla::XlaComputation* computation);

}  // namespace xla_hlo
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_XLA_QUANTIZE_H_
