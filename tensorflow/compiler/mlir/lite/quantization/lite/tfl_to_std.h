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
#ifndef TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_LITE_TFL_TO_STD_H_
#define TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_LITE_TFL_TO_STD_H_

#include "mlir/IR/Function.h"  // TF:llvm-project

namespace mlir {
namespace TFL {

// Converts all the tfl.quantize/tfl.dequantize ops to the ops in the mlir.quant
// dialect ones in the function.
void ConvertTFLQuantOpsToMlirQuantOps(FuncOp func);

// Converts all the mlir.quant dialect ops to the tfl.quantize/tfl.dequantize
// ops in the function.
void ConvertMlirQuantOpsToTFLQuantOps(FuncOp func);

}  // namespace TFL
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_LITE_QUANTIZATION_LITE_TFL_TO_STD_H_
