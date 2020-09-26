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

#include "tensorflow/core/grappler/optimizers/data/enable_gradient_descent.h"

#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/mutable_graph_view.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/grappler/optimizers/custom_graph_optimizer_registry.h"
#include "tensorflow/core/grappler/optimizers/data/graph_utils.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/platform/protobuf.h"

namespace tensorflow {
namespace grappler {
namespace {

constexpr char kAlgorithm[] = "algorithm";
constexpr char kModelDataset[] = "ModelDataset";
constexpr int64 HILL_CLIMB = 0;
constexpr int64 GRADIENT_DESCENT = 1;

}  // namespace

Status EnableGradientDescent::OptimizeAndCollectStats(
    Cluster* cluster, const GrapplerItem& item, GraphDef* output,
    OptimizationStats* stats) {
  *output = item.graph;
  if (!autotune_) {
    VLOG(1) << "The optimization enable_gradient_descent is not applied if "
               "autotune is off.";
    return Status::OK();
  }
  MutableGraphView graph(output);

  int index = graph_utils::FindGraphNodeWithOp(kModelDataset, *output);

  NodeDef& model_node = *(output->mutable_node(index));
  if (model_node.attr().at(kAlgorithm).i() == HILL_CLIMB) {
    (*model_node.mutable_attr())[kAlgorithm].set_i(GRADIENT_DESCENT);
    stats->num_changes++;
  }

  return Status::OK();
}

void EnableGradientDescent::Feedback(Cluster* cluster, const GrapplerItem& item,
                                     const GraphDef& optimize_output,
                                     double result) {
  // no-op
}

REGISTER_GRAPH_OPTIMIZER_AS(EnableGradientDescent, "enable_gradient_descent");

}  // namespace grappler
}  // namespace tensorflow
