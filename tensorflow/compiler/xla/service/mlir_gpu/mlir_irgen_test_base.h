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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_MLIR_GPU_MLIR_IRGEN_TEST_BASE_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_MLIR_GPU_MLIR_IRGEN_TEST_BASE_H_

#include <string>

#include "tensorflow/compiler/xla/service/mlir_gpu/mlir_compiler.h"
#include "tensorflow/compiler/xla/tests/codegen_test_base.h"

namespace xla {
namespace mlir_gpu {

// Tests that verify IR emitted by the CPU/GPU backend is as expected.
class MlirIrGenTestBase : public CodegenTestBase {
 protected:
  // Compiles the given HLO module to MLIR IR and verifies the IR matches the
  // given pattern. `pattern` is in the FileCheck pattern matching syntax
  // (http://llvm.org/docs/CommandGuide/FileCheck.html).
  //
  // This function invokes the JIT compiler.
  //
  // If `match_lowered_ir` is true, match the version of the IR after lowering
  // steps to LLVM IR are applied; otherwise, the IR before lowering is
  // matched.
  void CompileAndVerifyIr(std::unique_ptr<HloModule> hlo_module,
                          const string& pattern, bool match_lowered_ir = false);

  // A thin wrapper around CompileAndVerifyIr that parses `hlo_text` to create
  // an HLO module.
  void CompileAndVerifyIr(const string& hlo_text,
                          const string& expected_llvm_ir,
                          bool match_lowered_ir = false);

  // Compiles and returns module with optimizations from a given HLO.
  StatusOr<std::unique_ptr<HloModule>> GetOptimizedModule(
      absl::string_view hlo);

 private:
  MlirCompiler* GetMLIRCompiler();
};

}  // namespace mlir_gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_MLIR_GPU_MLIR_IRGEN_TEST_BASE_H_
