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

#include <cstdint>
#include <iterator>
#include <numeric>
#include <string>

#include "absl/memory/memory.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Quant/QuantOps.h"  // from @llvm-project
#include "mlir/Dialect/SCF/SCF.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Module.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/SymbolTable.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "mlir/Transforms/InliningUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tfr/ir/tfr_ops.h"
#include "tensorflow/compiler/mlir/tfr/ir/tfr_types.h"
#include "tensorflow/compiler/mlir/tfr/passes/passes.h"
#include "tensorflow/compiler/mlir/tfr/utils/utils.h"

//===----------------------------------------------------------------------===//
// The pass to decompose unregistered TF ops with the TFR compose function.
//
namespace mlir {
namespace TFR {

namespace {

// Decompose the TF ops with the registered composition library.
struct DecomposeTFOpsPass
    : public PassWrapper<DecomposeTFOpsPass, FunctionPass> {

  explicit DecomposeTFOpsPass(llvm::Optional<ModuleOp> external_tfr_module)
      : external_tfr_module(external_tfr_module) {}

  void runOnFunction() override;

 private:
  // Apply canonicalization, mainly constant folding, on the function.
  void ApplyCanonicalization();

  // Rewrite unregistered TF ops to TFR func call ops. Return failure if all the
  // ops are registered or the compose function doesn't exist.
  LogicalResult RewriteUnregisteredTFOps();

  // Inline the TFR func call ops.
  LogicalResult InlineTFRFuncCalls();

  // Optional external symbol table to look up the TFR function.
  llvm::Optional<ModuleOp> external_tfr_module;
};

void DecomposeTFOpsPass::ApplyCanonicalization() {
  OwningRewritePatternList patterns;

  auto* context = &getContext();
  for (auto* op : context->getRegisteredOperations()) {
    op->getCanonicalizationPatterns(patterns, context);
  }
  populateSCFOpsCanonicalizationPatterns(patterns, context);

  applyPatternsAndFoldGreedily(getFunction(), patterns);
}

LogicalResult DecomposeTFOpsPass::RewriteUnregisteredTFOps() {
  FuncOp func = getFunction();
  SymbolTable table(external_tfr_module.hasValue()
                        ? *external_tfr_module
                        : func.getParentOfType<ModuleOp>());
  OpBuilder builder(func);
  bool changed = false;
  func.walk([&table, &builder, &changed](Operation* op) {
    // Only the un-registered ops requires decomposition. The remaining ones
    // either will be constant folded or lowered by the rules defined in the
    // bridge.
    if (op->isRegistered()) {
      return;
    }

    // Find out the compose function
    auto compose_func_name = GetComposeFuncName(op->getName().getStringRef());
    auto compose_func = table.lookup<TFRFuncOp>(compose_func_name);
    if (!compose_func || compose_func.isExternal()) {
      // There are no decomposition methods defined for this op, skip.
      return;
    }

    auto compose_func_type = compose_func.getType();
    builder.setInsertionPoint(op);
    TFRTensorType unconstrainted_tensor_type = builder.getType<TFRTensorType>();

    // Create the new operands. This is mapping the operands from the target
    // TF ops to the TFR function arguments. If the TFR function argument is
    // a tensor_list, a "tfr.build_list" op is used to concat the available
    // TF op operands. If the TFR function argument isn't a tensor/tensor_list,
    // a constant is created by using the attribute stored in the TF op or the
    // default value in the argument attribute.
    llvm::SmallVector<Value, 4> new_operands;
    for (auto arg : llvm::enumerate(compose_func_type.getInputs())) {
      if (auto tensor_type = arg.value().dyn_cast<TFRTensorType>()) {
        auto casted = builder.create<CastOp>(op->getLoc(), tensor_type,
                                             op->getOperand(arg.index()));
        new_operands.push_back(casted);
      } else if (auto list_type = arg.value().dyn_cast<TFRTensorListType>()) {
        llvm::SmallVector<Value, 4> variadic_operands;
        for (int i = arg.index(); i < op->getNumOperands(); i++) {
          auto casted = builder.create<CastOp>(
              op->getLoc(), unconstrainted_tensor_type, op->getOperand(i));
          variadic_operands.push_back(casted);
        }
        auto build_list_op = builder.create<BuildListOp>(
            op->getLoc(), list_type, variadic_operands);
        new_operands.push_back(build_list_op.out());
      } else {
        auto attr_name = compose_func.getArgAttrOfType<StringAttr>(
            arg.index(), kAttrArgumentNameAttr);
        auto attribute = op->getAttr(attr_name.getValue());
        if (!attribute) {
          attribute =
              compose_func.getArgAttr(arg.index(), kAttrArgumentDefaultAttr);
        }
        Value attr_cst;
        // Wrap these special attributes as a special TFR constant, so the SSA
        // value has a valid type to be used as TFR function argument. These
        // attributes are not expected to be manipulated by the lowering passes.
        if (attribute.isa<TypeAttr>() || attribute.isa<ArrayAttr>() ||
            attribute.isa<StringAttr>() || attribute.isa<FlatSymbolRefAttr>()) {
          TFRAttrType output_type = TFRAttrType::get(builder.getContext());
          attr_cst =
              builder.create<ConstOp>(op->getLoc(), output_type, attribute);
        } else {
          attr_cst = builder.create<ConstantOp>(op->getLoc(), attribute);
        }
        new_operands.push_back(attr_cst);
      }
    }

    // Create the TFR call op
    auto new_op = builder.create<CallOp>(
        op->getLoc(), compose_func_type.getResults(),
        builder.getSymbolRefAttr(compose_func.getName()), new_operands);

    // Replace the use of the old op. This is mapping the results from the
    // target TF ops to the TFR function returns. If the TFR function return is
    // a tensor_list, "tfr.get_element" op is used to extract the required TF
    // op result.
    llvm::SmallVector<Value, 4> new_results;
    for (auto res : llvm::enumerate(compose_func_type.getResults())) {
      if (res.value().dyn_cast<TFRTensorType>()) {
        new_results.push_back(new_op.getResult(res.index()));
      } else if (auto list_type = res.value().dyn_cast<TFRTensorListType>()) {
        for (int i = res.index(), j = 0; i < op->getNumResults(); i++, j++) {
          auto index =
              builder.create<ConstantOp>(op->getLoc(), builder.getIndexAttr(j));
          auto element_op = builder.create<GetElementOp>(
              op->getLoc(), unconstrainted_tensor_type,
              new_op.getResult(res.index()), index.getResult());
          new_results.push_back(element_op.out());
        }
      }
    }
    for (auto res : llvm::zip(op->getResults(), new_results)) {
      auto casted = builder.create<CastOp>(
          op->getLoc(), std::get<0>(res).getType(), std::get<1>(res));
      std::get<0>(res).replaceAllUsesWith(casted.out());
    }
    op->erase();
    changed |= true;
  });

  // If `changed` is false, it is considered as a failure, so the recursive
  // rewrite will stop.
  return success(changed);
}

LogicalResult DecomposeTFOpsPass::InlineTFRFuncCalls() {
  // The Inliner will automatically use the registered dialect inliner.
  InlinerInterface inliner(&getContext());
  FuncOp func = getFunction();
  SymbolTable table(external_tfr_module.hasValue()
                        ? *external_tfr_module
                        : func.getParentOfType<ModuleOp>());

  // The inliner only inlines the TFR call op.
  bool changed = false;
  auto walk_result = func.walk([&](CallOp call_op) {
    auto callee = table.lookup<TFRFuncOp>(call_op.callee());
    if (!callee || callee.isExternal()) return WalkResult::advance();
    if (failed(inlineCall(inliner,
                          cast<CallOpInterface>(call_op.getOperation()),
                          cast<CallableOpInterface>(callee.getOperation()),
                          callee.getCallableRegion(),
                          /**shouldCloneInLinedRegion=*/true))) {
      // This failure is usually because the decompose function is not defined.
      // This call will be raised to TF ops.
      return WalkResult::interrupt();
    }
    call_op.erase();
    changed |= true;
    return WalkResult::advance();
  });

  if (walk_result.wasInterrupted()) {
    signalPassFailure();
    return failure();
  }

  // If `changed` is false, it is considered as a failure, so the recursive
  // rewrite will stop.
  return success(changed);
}

void DecomposeTFOpsPass::runOnFunction() {
  // Set a maximum iteration threshold in case there are infinite loops in the
  // call stack.
  int max_iterators = 10;
  do {
    // canonicalization
    ApplyCanonicalization();

    // rewrite unregistered tf ops. Failed either because no ops can be
    // decomposed or the compose function isn't defined.
    auto rewrite_status = RewriteUnregisteredTFOps();
    // inline the tfr call op until there are no tfr.call op can be inlined.
    auto inline_status = InlineTFRFuncCalls();

    if (failed(rewrite_status) && failed(inline_status)) {
      break;
    }
  } while (max_iterators-- >= 0);
}

}  // namespace

// Creates an instance of the pass to decompose the TF ops.
std::unique_ptr<OperationPass<FuncOp>> CreateDecomposeTFOpsPass(
    llvm::Optional<ModuleOp> tfr_module) {
  return std::make_unique<DecomposeTFOpsPass>(tfr_module);
}

static PassRegistration<DecomposeTFOpsPass> pass(
    "tfr-decompose",
    "Decompose TF ops with the registered composition library.",
    [] { return CreateDecomposeTFOpsPass(); });

}  // namespace TFR
}  // namespace mlir
