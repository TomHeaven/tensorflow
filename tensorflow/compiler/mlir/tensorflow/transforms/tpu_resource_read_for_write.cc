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

#include <memory>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/Module.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"

namespace mlir {
namespace TFTPU {

// A pass that finds TPU clusters with write only resource access and adds an
// associated resource read, so the resource can later be fused into TPUExecute.
namespace {
struct TPUResourceReadForWrite
    : public PassWrapper<TPUResourceReadForWrite, OperationPass<ModuleOp>> {
  void runOnOperation() override;
};

// Helper struct holding a resource value and its associated type.
struct ResourceValueAndSubtype {
  Value resource;
  Type subtype;
};

// Finds resource handle and type for result if result writes to a resource.
ResourceValueAndSubtype GetResourceWriteResult(
    tf_device::ClusterFuncOp cluster_func, Value result) {
  ResourceValueAndSubtype resource;
  if (!result.hasOneUse()) return resource;
  Operation* result_user = *result.getUsers().begin();
  auto assign_var = dyn_cast<TF::AssignVariableOp>(result_user);
  if (!assign_var) return resource;

  auto handle = assign_var.resource();
  // Skip result if cluster writes to the same variable via multiple results.
  for (Operation* handle_user : handle.getUsers()) {
    if (handle_user == assign_var) continue;
    auto assign_var_user = dyn_cast<TF::AssignVariableOp>(handle_user);
    if (!assign_var_user) continue;
    if (assign_var_user.value().getDefiningOp() == cluster_func)
      return resource;
  }

  resource.resource = assign_var.resource();
  resource.subtype = assign_var.value().getType();
  return resource;
}

// Checks if resource is read by TPU cluster.
bool ClusterFuncHasResourceRead(tf_device::ClusterFuncOp cluster_func,
                                Value resource) {
  for (Operation* resource_user : resource.getUsers())
    if (auto read = dyn_cast<TF::ReadVariableOp>(resource_user))
      for (Operation* read_user : read.value().getUsers())
        if (read_user == cluster_func) return true;

  return false;
}

void TPUResourceReadForWrite::runOnOperation() {
  SmallVector<tf_device::ClusterFuncOp, 4> cluster_funcs;
  getOperation().walk([&](tf_device::ClusterFuncOp cluster_func) {
    cluster_funcs.push_back(cluster_func);
  });

  OpBuilder builder(&getContext());
  // Add resource reads for resource writes from TPU cluster where for such
  // resources the TPU cluster does not read from.
  for (tf_device::ClusterFuncOp cluster_func : cluster_funcs) {
    builder.setInsertionPoint(cluster_func);

    SmallVector<Value, 4> read_operands;
    for (Value result : cluster_func.getResults()) {
      // TODO(lyandy): Update pass to use resource alias analysis.
      auto resource_and_type = GetResourceWriteResult(cluster_func, result);
      if (!resource_and_type.resource) continue;
      if (ClusterFuncHasResourceRead(cluster_func, resource_and_type.resource))
        continue;
      auto new_read = builder.create<TF::ReadVariableOp>(
          resource_and_type.resource.getLoc(), resource_and_type.subtype,
          resource_and_type.resource);
      read_operands.push_back(new_read.value());
    }

    if (read_operands.empty()) continue;

    // Update caller and function types with new read operands.
    auto operands = llvm::to_vector<4>(cluster_func.getOperands());
    operands.append(read_operands.begin(), read_operands.end());

    auto new_cluster_func = builder.create<tf_device::ClusterFuncOp>(
        cluster_func.getLoc(), cluster_func.getResultTypes(), operands,
        cluster_func.getAttrs());
    cluster_func.replaceAllUsesWith(new_cluster_func);
    FuncOp func = cluster_func.getFunc();
    Block& block = func.front();
    for (Value read_operand : read_operands)
      block.addArgument(read_operand.getType());

    func.setType(FunctionType::get(block.getArgumentTypes(),
                                   func.getCallableResults(), &getContext()));
    cluster_func.erase();
  }
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> CreateTPUResourceReadForWritePass() {
  return std::make_unique<TPUResourceReadForWrite>();
}

static PassRegistration<TPUResourceReadForWrite> pass(
    "tf-tpu-resource-read-for-write",
    "Inserts tf.ReadVariableOp inputs to a TPU cluster for resource writes "
    "with no reads");

}  // namespace TFTPU
}  // namespace mlir
