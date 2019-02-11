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

#include "tensorflow/core/grappler/optimizers/data/rebatch.h"

#include "absl/container/flat_hash_map.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/optimizers/custom_graph_optimizer_registry.h"
#include "tensorflow/core/grappler/optimizers/data/graph_utils.h"
#include "tensorflow/core/grappler/utils/functions.h"

namespace tensorflow {
namespace grappler {

Status RebatchOptimizer::Init(
    const tensorflow::RewriterConfig_CustomGraphOptimizer* config) {
  if (!config) return Status::OK();

  num_workers_ = config->parameter_map().at("num_workers").i();
  return Status::OK();
}

namespace {

constexpr char kCastOp[] = "Cast";
constexpr char kRealDivOp[] = "RealDiv";

constexpr std::array<const char*, 5> kBatchDatasetOps = {
    "BatchDataset",
    "BatchDatasetV2",
    "ExperimentalMapAndBatchDataset",
    "PaddedBatchDataset",
    "PaddedBatchDatasetV2"
};

constexpr std::array<const char*, 2> kMultipleInputsDatasetOps = {
    "ConcatenateDataset",
    "ZipDataset"
};

constexpr std::array<const char*, 17> kPassThroughOps = {
    "CacheDataset",
    "FilterDataset",
    "FilterByLastComponentDataset",
    "Identity",
    "MapDataset",
    "ModelDataset",
    "OptimizeDataset",
    "ParallelMapDataset",
    "PrefetchDataset",
    "ReduceDataset",
    "RepeatDataset",
    "ShardDataset",
    "ShuffleAndRepeatDataset",
    "ShuffleDataset",
    "SkipDataset",
    "TakeDataset",
    "WindowDataset"
};

constexpr std::array<const char*, 3> kFuncDatasetOps = {
    "FlatMapDataset",
    "InterleaveDataset",
    "ParallelInterleaveDatasetV2"
};

constexpr std::array<const char*, 9> kSourceDatasetOps = {
    "FixedLengthRecordDataset",
    "FixedLengthRecordDatasetV2",
    "GeneratorDataset",
    "RangeDataset",
    "SparseTensorsSliceDataset",
    "TensorDataset",
    "TensorSliceDataset",
    "TextLineDataset",
    "TFRecordDataset"
};

NodeDef* AddCastNode(const string& input, DataType src_t, DataType dst_t,
                     MutableGraphView* graph) {
  NodeDef cast_node;
  cast_node.set_op(kCastOp);
  cast_node.add_input(input);
  graph_utils::SetUniqueGraphNodeName(cast_node.op(), graph->graph(),
                                      &cast_node);
  AddNodeAttr("SrcT", src_t, &cast_node);
  AddNodeAttr("DstT", dst_t, &cast_node);

  return graph->AddNode(std::move(cast_node));
}

NodeDef* AddBinaryNode(const string& input_x, const string& input_y,
                       const string& op, DataType type,
                       MutableGraphView* graph) {
  NodeDef node;
  node.set_op(op);
  node.add_input(input_x);
  node.add_input(input_y);
  graph_utils::SetUniqueGraphNodeName(op, graph->graph(), &node);
  AddNodeAttr("T", type, &node);

  return graph->AddNode(std::move(node));
}

NodeDef* AddFloatDivNode(const string& input_x, const string& input_y,
                         MutableGraphView* graph) {
  return AddBinaryNode(input_x, input_y, kRealDivOp, DT_FLOAT, graph);
}

template <std::size_t SIZE>
bool IsDatasetNodeOfType(const NodeDef& node,
                         const std::array<const char*, SIZE>& arr) {
  for (const auto& dataset_op_name : arr) {
    if (node.op() == dataset_op_name) return true;
  }
  return false;
}

// Given a "batch" dataset node, modifies the batch_size input to divide the
// current batch size by num_workers.
Status MutateBatchSize(const NodeDef& node, int64 num_workers,
                       MutableGraphView* graph) {
  // TODO(rohanj): Fix up the output_shapes attribute as well. For this Dataset
  // as well as all the downstream datasets.
  // For all the batching datasets the batch_size is input number 1.
  // TODO(rohanj): Add an assertion for batch size not being a multiple of
  // num_workers.
  NodeDef* batch_size_node = graph_utils::GetInputNode(node, *graph, 1);
  NodeDef tmp_node = *batch_size_node;
  graph_utils::SetUniqueGraphNodeName(tmp_node.op(), graph->graph(), &tmp_node);
  NodeDef* copy_batch_size_node = graph->AddNode(std::move(tmp_node));
  NodeDef* float_copy_batch_size_node =
      AddCastNode(copy_batch_size_node->name(), DT_INT64, DT_FLOAT, graph);
  NodeDef* num_worker_node =
      graph_utils::AddScalarConstNode<int64>(num_workers, graph);
  NodeDef* float_num_worker_node =
      AddCastNode(num_worker_node->name(), DT_INT64, DT_FLOAT, graph);
  NodeDef* divided_batch_size_node = AddFloatDivNode(
      float_copy_batch_size_node->name(), float_num_worker_node->name(), graph);
  NodeDef* cast_new_batch_size_node =
      AddCastNode(divided_batch_size_node->name(), DT_FLOAT, DT_INT64, graph);
  // We don't call UpdateFanouts here because CSE elimination might lead to
  // multiple nodes sharing the same batch size constant node. This is also
  // why we don't delete batch_size_node as well.
  TF_RETURN_IF_ERROR(graph->UpdateRegularFaninByPort(
      node.name(), 1, {cast_new_batch_size_node->name(), 0}));
  return Status::OK();
}

// There is one Sink node at least that is added to the end of the graph. We
// find that node and return it. It is possible that there are multiple
// Identity ops from the final Dataset op to that Sink node, but the recursive
// graph traversal handles that.
Status FindSinkNode(const GraphDef& graph_def, NodeDef* sink_node) {
  absl::flat_hash_map<string, int> all_node_names;
  absl::flat_hash_map<string, int> node_input_map;
  for (int i = 0; i < graph_def.node_size(); ++i) {
    all_node_names.insert_or_assign(graph_def.node(i).name(), i);
    node_input_map.insert_or_assign(graph_def.node(i).name(), 0);
  }
  // Counts how many graph nodes is this node the input to. Candidate sink
  // nodes are ones which are inputs into zero nodes.
  for (const NodeDef& node : graph_def.node()) {
    for (const string& input_name : node.input()) {
      node_input_map[input_name]++;
    }
  }
  for (const auto& it : node_input_map) {
    if (it.second == 0) {
      const NodeDef& sink_graph_node = graph_def.node(all_node_names[it.first]);
      // Sometimes the searching surfaces Arg nodes in function cases that
      // have no input. This check rejects those.
      if (sink_graph_node.input_size() == 0) {
        continue;
      }
      *sink_node = sink_graph_node;
      return Status::OK();
    }
  }
  return errors::InvalidArgument("Failed to find a sink node");
}

Status OptimizeGraph(const GrapplerItem& item, int64 num_workers,
                     GraphDef* output);

// Helper function that starts from a node in the graph and recurses into its
// inputs trying to find a BatchDataset type operation to modify. During the
// recursion it handles four kinds of cases.
// 1. BatchDataset type ops: Mutates the batch_size input node and stops.
// 2. Zip / Concatenate dataset ops: Recurses into all inputs to these ops
//      as they are datasets themselves.
// 3. Core dataset ops + Identity op: Recurses into first input parameter.
// 4. FlatMap type mapping dataset ops: Recurses into the function definition.
Status RecursivelyHandleOp(const NodeDef& node, int64 num_workers,
                           FunctionLibraryDefinition* flib,
                           MutableGraphView* graph) {
  if (IsDatasetNodeOfType(node, kBatchDatasetOps)) {
    return MutateBatchSize(node, num_workers, graph);
  } else if (IsDatasetNodeOfType(node, kMultipleInputsDatasetOps)) {
    // For all multiple input datasets, all inputs are datasets themselves.
    for (int i = 0; i < node.input_size(); ++i) {
      NodeDef* input_node = graph_utils::GetInputNode(node, *graph, i);
      TF_RETURN_IF_ERROR(
          RecursivelyHandleOp(*input_node, num_workers, flib, graph));
    }
  } else if (IsDatasetNodeOfType(node, kPassThroughOps)) {
    // For all the dataset ops that are pass through, the input dataset is
    // input 0.
    NodeDef* input_node = graph_utils::GetInputNode(node, *graph, 0);
    TF_RETURN_IF_ERROR(
        RecursivelyHandleOp(*input_node, num_workers, flib, graph));
  } else if (IsDatasetNodeOfType(node, kFuncDatasetOps)) {
    const string func_name = node.attr().at("f").func().name();
    const FunctionDef* fdef = flib->Find(func_name);
    GrapplerFunctionItem f_item;
    TF_RETURN_IF_ERROR(MakeGrapplerFunctionItem(
        *fdef, *flib, graph->graph()->versions().producer(), &f_item));
    GraphDef optimized_func_graph;
    Status s = OptimizeGraph(f_item, num_workers, &optimized_func_graph);
    if (s.ok()) {
      // Function body optimization might have created new specialized
      // functions for each instantiation context. Add them to the library.
      for (const FunctionDef& func_def :
           optimized_func_graph.library().function()) {
        if (flib->Find(func_def.signature().name()) == nullptr) {
          TF_RETURN_IF_ERROR(flib->AddFunctionDef(func_def));
        }
      }

      // Convert optimized graph back to FunctionDef.
      FunctionDef optimized_func;
      f_item.SwapFunctionBody(std::move(optimized_func_graph));
      TF_RETURN_IF_ERROR(MakeFunctionDef(f_item, *flib, &optimized_func));

      // Replace optimized function with a new FunctionDef.
      TF_RETURN_IF_ERROR(flib->ReplaceFunction(func_name, optimized_func));
    }
  } else if (IsDatasetNodeOfType(node, kSourceDatasetOps)) {
    return errors::InvalidArgument(
        "Reached a source dataset: ", node.op(),
        " without encountering a batch transformation.");
  } else {
    return errors::InvalidArgument("Encountered an unsupported op: ",
                                   node.op());
  }
  return Status::OK();
}

// Helper function that given a GrapplerItem generates a mutated graph def
// with the batch size changed. The GrapplerItem could be generated from the
// main graph or could be a function graph.
Status OptimizeGraph(const GrapplerItem& item, int64 num_workers,
                     GraphDef* output) {
  *output = item.graph;
  MutableGraphView graph(output);

  FunctionLibraryDefinition flib(OpRegistry::Global(), item.graph.library());

  NodeDef sink_node;
  TF_RETURN_IF_ERROR(FindSinkNode(item.graph, &sink_node));
  TF_RETURN_IF_ERROR(
      RecursivelyHandleOp(sink_node, num_workers, &flib, &graph));
  *output->mutable_library() = flib.ToProto();
  return Status::OK();
}

}  // anonymous namespace

Status RebatchOptimizer::OptimizeAndCollectStats(Cluster* cluster,
                                                 const GrapplerItem& item,
                                                 GraphDef* output,
                                                 OptimizationStats* stats) {
  *output = item.graph;
  MutableGraphView graph(output);

  TF_RETURN_IF_ERROR(OptimizeGraph(item, num_workers_, output));
  stats->num_changes++;
  return Status::OK();
}

void RebatchOptimizer::Feedback(Cluster* cluster, const GrapplerItem& item,
                                const GraphDef& optimize_output,
                                double result) {}

REGISTER_GRAPH_OPTIMIZER_AS(RebatchOptimizer, "tf_data_rebatcher");

}  // namespace grappler
}  // namespace tensorflow
