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

#ifndef TENSORFLOW_COMPILER_TF2XLA_FUNCTIONALIZE_CONTROL_FLOW_H_
#define TENSORFLOW_COMPILER_TF2XLA_FUNCTIONALIZE_CONTROL_FLOW_H_

#include "tensorflow/compiler/tf2xla/functionalize_control_flow_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/graph/graph.h"

namespace tensorflow {

// Transformation that converts tf.while_loop() loops into functional While
// operators and tf.cond() conditionals into function If operators, suitable for
// XLA compilation.
//
// If `node_filter` is defined, then only loops and conditions for whose
// nodes `node_filter` returns true are functionalized.
//
// Precondition:
// For any node in a loop or condition for which `node_filter` returns true,
// all nodes inside of the same loop or condition must also return true
// (including nodes in other nested loops and conditions inside of that loop or
// condition).
// This means that a "not to be functionalized" loop or condition is not allowed
// inside a "to be functionalized" loop or condition.
//
// The user of this function is responsible for using a node filter that
// satisfies the above conditions.
Status FunctionalizeControlFlow(Graph* graph,
                                FunctionLibraryDefinition* library,
                                const NodeFilter& node_filter = {});

Status FunctionalizeControlFlowForGraphDef(GraphDef* graph_def,
                                           FunctionLibraryDefinition* library,
                                           const NodeFilter& node_filter = {});

// This pass looks at the graph, and turns V1 control flow structure
// (Switch/Merge/etc.) into V2 control flow structure (If/While).
class FunctionalizeControlFlowPass : public GraphOptimizationPass {
 public:
  Status Run(const GraphOptimizationPassOptions& options) override;
};

// Same as the above but only modifies functions that will be executed by XLA.
class FunctionalizeControlFlowForXlaPass : public GraphOptimizationPass {
 public:
  Status Run(const GraphOptimizationPassOptions& options) override;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_TF2XLA_FUNCTIONALIZE_CONTROL_FLOW_H_
