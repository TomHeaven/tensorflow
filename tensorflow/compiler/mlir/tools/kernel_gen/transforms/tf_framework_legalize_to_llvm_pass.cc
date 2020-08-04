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

#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tools/kernel_gen/ir/tf_framework_ops.h"
#include "tensorflow/compiler/mlir/tools/kernel_gen/transforms/passes.h"
#include "tensorflow/compiler/mlir/tools/kernel_gen/transforms/rewriters.h"

namespace mlir {
namespace kernel_gen {
namespace tf_framework {
namespace {

#define GEN_PASS_CLASSES
#include "tensorflow/compiler/mlir/tools/kernel_gen/transforms/kernel_gen_passes.h.inc"

class TestTFFrameworkToLLVMPass
    : public TestTFFrameworkLegalizeToLLVMPassBase<TestTFFrameworkToLLVMPass> {
 public:
  void runOnOperation() override {
    ModuleOp m = getOperation();

    // Populate type conversions.
    LLVMTypeConverter type_converter(m.getContext());
    type_converter.addConversion([&](tf_framework::OpKernelContextType type) {
      return LLVM::LLVMType::getInt8PtrTy(type_converter.getDialect());
    });

    // Populate patterns.
    OwningRewritePatternList patterns;
    populateStdToLLVMConversionPatterns(type_converter, patterns);
    PopulateTFFrameworkToLLVMConversionPatterns(&type_converter, &patterns);

    // Set target.
    ConversionTarget target(getContext());
    target.addLegalDialect<LLVM::LLVMDialect>();
    target.addIllegalDialect<tf_framework::TFFrameworkDialect>();
    target.addLegalOp<ModuleOp, ModuleTerminatorOp>();

    if (failed(applyFullConversion(m, target, patterns))) {
      signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<ModuleOp> >
createTestTFFrameworkLegalizeToLLVMPass() {
  return std::make_unique<TestTFFrameworkToLLVMPass>();
}

}  // namespace tf_framework
}  // namespace kernel_gen
}  // namespace mlir
