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
#include "tensorflow/lite/experimental/delegates/hexagon/hexagon_delegate_kernel.h"

#include <algorithm>
#include <vector>

#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/context_util.h"
#include "tensorflow/lite/experimental/delegates/hexagon/hexagon_implementation.h"
#include "tensorflow/lite/experimental/delegates/hexagon/utils.h"

namespace tflite {

namespace {
inline const char* StateToString(
    HexagonDelegateKernel::HexagonKernelState state) {
  switch (state) {
    case HexagonDelegateKernel::HexagonKernelState::HEALTHY:
      return "HEALTHY";
    case HexagonDelegateKernel::HexagonKernelState::FAST_RPC_SETUP_FAILED:
      return "FAST_RPC_SETUP_FAILED";
    case HexagonDelegateKernel::HexagonKernelState::FAILED_TO_INIT_GRAPH:
      return "FAILED_TO_INIT_GRAPH";
    case HexagonDelegateKernel::HexagonKernelState::FAILED_TO_PREPARE_GRAPH:
      return "FAILED_TO_PREPARE_GRAPH";
    case HexagonDelegateKernel::HexagonKernelState::MULTIPLE_INPUTS:
      return "MULTIPLE_INPUTS";
    case HexagonDelegateKernel::HexagonKernelState::INPUT_RANK_NOT_SUPPORTED:
      return "INPUT_RANK_NOT_SUPPORTED";
    case HexagonDelegateKernel::HexagonKernelState::MULTIPLE_OUTPUTS:
      return "MULTIPLE_OUTPUTS";
    case HexagonDelegateKernel::HexagonKernelState::FAILED_TO_EXECUTE_GRAPH:
      return "FAILED_TO_EXECUTE_GRAPH";
  }
}

// Returns uint64 representing total cycles in 'perf_info' by
// combining lo and hi counters.
inline uint64_t GetCycles(const hexagon_nn_perfinfo& perf_info) {
  uint64_t res = perf_info.counter_hi;
  res <<= 32;
  res |= perf_info.counter_lo;
  return res;
}

// Comparator for hexagon_nn_perfinfo in descending order based on
// total cycles consumed.
struct PerfInfoCmp {
  bool operator()(const hexagon_nn_perfinfo& a,
                  const hexagon_nn_perfinfo& b) const {
    return GetCycles(a) > GetCycles(b);
  }
};
}  // namespace

void HexagonDelegateKernel::ReportError(TfLiteContext* context,
                                        HexagonKernelState state,
                                        const std::string& msg) {
  PrintLog();
  context->ReportError(context, "Failed: %s. STATE: %s", msg.c_str(),
                       StateToString(state));
}

TfLiteStatus HexagonDelegateKernel::Init(TfLiteContext* context,
                                         const TfLiteDelegateParams* params) {
  hexagon_nn_ = HexagonNNImplementation();
  if (hexagon_nn_ == nullptr) {
    context->ReportError(context, "Hexagon interface not available.");
    return kTfLiteError;
  }
  if (params != nullptr && params->delegate != nullptr) {
    const ::TfLiteHexagonDelegateOptions* options_ptr =
        reinterpret_cast<const ::TfLiteHexagonDelegateOptions*>(
            params->delegate->data_);
    params_ = (options_ptr == nullptr ? ::TfLiteHexagonDelegateOptions()
                                      : *options_ptr);
  }

  // Ensure Hexagon NNLib is ready to start working.
  int error = hexagon_nn_->hexagon_nn_config();
  if (error != 0) {
    context->ReportError(context, "hexagon_nn_config failed. Error: %d", error);
    return kTfLiteError;
  }

  // Initialize an empty graph.
  error = hexagon_nn_->hexagon_nn_init(&graph_id_);
  if (error != 0) {
    state_ = HexagonKernelState::FAILED_TO_INIT_GRAPH;
    ReportError(context, state_, "failed to init");
    return kTfLiteError;
  }
  error =
      hexagon_nn_->hexagon_nn_set_debug_level(graph_id_, params_.debug_level);
  if (error != 0) {
    context->ReportError(context, "Failed to set debug level, error: %d",
                         error);
    return kTfLiteError;
  }
  error = hexagon_nn_->hexagon_nn_set_powersave_level(params_.powersave_level);
  if (error != 0) {
    context->ReportError(context, "Failed to set powersave level, error %d",
                         error);
    return kTfLiteError;
  }

  for (auto node_index : TfLiteIntArrayView(params->nodes_to_replace)) {
    nodes_.push_back(node_index);
  }

  TF_LITE_ENSURE_STATUS(
      BuildGraph(context, params->input_tensors, params->output_tensors));
  return kTfLiteOk;
}

TfLiteStatus HexagonDelegateKernel::Invoke(TfLiteContext* context,
                                           TfLiteNode* node) {
  if (hexagon_nn_ == nullptr) {
    context->ReportError(context, "Hexagon interface not available.");
    return kTfLiteError;
  }
  // Allocate inputs.
  std::vector<hexagon_nn_tensordef> input_tensors;
  for (auto tensor_index : TfLiteIntArrayView(node->inputs)) {
    if (tensor_index == kTfLiteOptionalTensor) {
      continue;
    }
    TfLiteTensor* tensor = &context->tensors[tensor_index];
    // Const tensors should be added as const nodes during graph construction.
    if (tensor->allocation_type != kTfLiteMmapRo) {
      if (tensor->dims->size > 4) {
        ReportError(context, HexagonKernelState::INPUT_RANK_NOT_SUPPORTED,
                    "Only up to 4d tensor are supported.");
        return kTfLiteError;
      }
      input_tensors.emplace_back();
      auto& input_tensor = input_tensors.back();
      input_tensor.data = reinterpret_cast<unsigned char*>(tensor->data.raw);
      input_tensor.dataLen = tensor->bytes;
      input_tensor.data_valid_len = tensor->bytes;
      TF_LITE_ENSURE_STATUS(
          Get4DShape(&input_tensor.batches, &input_tensor.height,
                     &input_tensor.width, &input_tensor.depth, tensor->dims));
    }
  }

  // Allocate outputs.
  std::vector<hexagon_nn_tensordef> output_tensors;
  for (auto tensor_index : TfLiteIntArrayView(node->outputs)) {
    if (tensor_index == kTfLiteOptionalTensor) {
      continue;
    }
    TfLiteTensor* tensor = &context->tensors[tensor_index];
    if (tensor->allocation_type != kTfLiteMmapRo) {
      if (tensor->dims->size > 4) {
        ReportError(context, HexagonKernelState::INPUT_RANK_NOT_SUPPORTED,
                    "Only up to 4d tensor are supported.");
        return kTfLiteError;
      }
      output_tensors.emplace_back();
      auto& output_tensor = output_tensors.back();
      output_tensor.data = reinterpret_cast<unsigned char*>(tensor->data.raw);
      output_tensor.dataLen = tensor->bytes;
    }
  }

  if (params_.print_graph_profile) {
    hexagon_nn_->hexagon_nn_reset_perfinfo(graph_id_, 0);
  }

  // Execute.
  int error = hexagon_nn_->hexagon_nn_execute_new(
      graph_id_, input_tensors.data(), input_tensors.size(),
      output_tensors.data(), output_tensors.size());
  if (error != 0) {
    ReportError(context, HexagonKernelState::FAILED_TO_EXECUTE_GRAPH,
                "Failed to execute graph.");
    return kTfLiteError;
  }
  if (params_.print_graph_profile) {
    PrintPerformanceData();
  }
  return kTfLiteOk;
}

TfLiteStatus HexagonDelegateKernel::Prepare(TfLiteContext* context,
                                            TfLiteNode* node) {
  if (hexagon_nn_ == nullptr) {
    context->ReportError(context, "Hexagon interface not available. prepare");
    return kTfLiteError;
  }
  int status = hexagon_nn_->hexagon_nn_prepare(graph_id_);
  if (status != 0) {
    state_ = HexagonKernelState::FAILED_TO_PREPARE_GRAPH;
    ReportError(context, state_, "Failed to prepare graph.\n");
    return kTfLiteError;
  }

  // Check input/output tensors.
  std::vector<int> tensors;
  for (auto tensor_index : TfLiteIntArrayView(node->inputs)) {
    tensors.push_back(tensor_index);
  }
  for (auto tensor_index : TfLiteIntArrayView(node->outputs)) {
    tensors.push_back(tensor_index);
  }
  for (auto tensor_index : tensors) {
    if (tensor_index == kTfLiteOptionalTensor) {
      continue;
    }
    TfLiteTensor* tensor = &context->tensors[tensor_index];
    // Const tensors should be added as const nodes during graph construction.
    if (tensor->allocation_type != kTfLiteMmapRo && tensor->dims->size > 4) {
      ReportError(context, HexagonKernelState::INPUT_RANK_NOT_SUPPORTED,
                  "Only up to 4d tensor are supported.");
      return kTfLiteError;
    }
  }

  if (params_.print_graph_debug) {
    PrintDebuggingGraph();
  }

  return kTfLiteOk;
}

TfLiteStatus HexagonDelegateKernel::BuildGraph(
    TfLiteContext* context, const TfLiteIntArray* input_tensors,
    const TfLiteIntArray* output_tensors) {
  builder_.reset(
      new delegates::hexagon::GraphBuilder(hexagon_nn_, context, graph_id_));
  // Add inputs to the graph.
  builder_->AddInputTensors(input_tensors, context);

  // Add all ops.
  TfLiteNode* node;
  TfLiteRegistration* reg;
  for (int node_index : nodes_) {
    TF_LITE_ENSURE_STATUS(
        context->GetNodeAndRegistration(context, node_index, &node, &reg));
    auto* op_builder = builder_->AddNodeFromTfLiteOp(reg->builtin_code, node);
    TF_LITE_ENSURE_STATUS(
        op_builder->PopulateSubGraph(node->inputs, node->outputs, context));
    TF_LITE_ENSURE_STATUS(op_builder->RegisterOutputs(node->outputs, context));
  }

  // Add Outputs.
  builder_->AddOutputTensors(output_tensors, context);

  builder_->Build();

  return kTfLiteOk;
}

HexagonDelegateKernel::~HexagonDelegateKernel() {
  if (graph_id_ != -1) {
    hexagon_nn_->hexagon_nn_teardown(graph_id_);
  }
}

void HexagonDelegateKernel::PrintLog() {
  std::vector<unsigned char> buf(3000000);
  time_t my_time = time(nullptr);
  hexagon_nn_->hexagon_nn_getlog(graph_id_, buf.data(), buf.size());
  printf("----------------\n");
  printf("Timestamp: %s\n\n", ctime(&my_time));
  printf("Log\n%s\n", buf.data());
  printf("----------------\n");
  fflush(stdout);
}

void HexagonDelegateKernel::PrintPerformanceData() {
  const int kMaxNodes = 2048;
  const int kMaxNameLen = 100;
  std::vector<hexagon_nn_perfinfo> perf_data(kMaxNodes);
  std::vector<char> op_name(kMaxNameLen);
  uint64_t total_cycles = 0;
  uint64_t cum_cycles = 0;
  uint64_t counter = 0;
  unsigned int num_nodes;
  printf("------- Performance Debug Data Start -------\n");
  if (hexagon_nn_->hexagon_nn_get_perfinfo(graph_id_, perf_data.data(),
                                           kMaxNodes, &num_nodes) != 0) {
    printf("Failed fetching perf data.\n");
    return;
  }
  printf("Total %d nodes.\n", num_nodes);
  std::sort(perf_data.begin(), perf_data.begin() + num_nodes, PerfInfoCmp());
  for (int i = 0; i < num_nodes; i++) {
    total_cycles += GetCycles(perf_data[i]);
  }
  printf("Total %lu cycles\n", static_cast<unsigned long>(total_cycles));
  printf(
      "Node ID,\tOP Name,\tExecutions,\tCycles,\t%% of total,\tCummulative "
      "cycles,\tCummulative %%\n");
  for (int i = 0; i < num_nodes; i++) {
    counter = GetCycles(perf_data[i]);
    cum_cycles += counter;
    int op_type_id = builder_->GetOpTypeId(perf_data[i].node_id);
    if (op_type_id >= 0 && hexagon_nn_->hexagon_nn_op_id_to_name(
                               op_type_id, op_name.data(), kMaxNameLen) != 0) {
      printf("Failed to fetch name for %u with type %d\n", perf_data[i].node_id,
             op_type_id);
      continue;
    }
    printf("0x%x,\t%s,\t%d,\t%lu,\t%f %%,\t%lu,\t%f %%\n", perf_data[i].node_id,
           (op_type_id < 0 ? "" : op_name.data()), perf_data[i].executions,
           static_cast<unsigned long>(counter),
           100.0 * (1.0 * counter / total_cycles),
           static_cast<unsigned long>(cum_cycles),
           100.0 * (1.0 * cum_cycles / total_cycles));
  }
  printf("------- Performance Debug Data End -------\n");
}

void HexagonDelegateKernel::PrintDebuggingGraph() {
  const int kMaxBufLen = 100000;
  std::vector<unsigned char> buf(kMaxBufLen);
  if (hexagon_nn_->hexagon_nn_snpprint(graph_id_, buf.data(), kMaxBufLen) !=
      0) {
    printf("Error fetching graph debug details.\n");
    return;
  }
  printf("------- Graph Debugging Start -------\n");
  printf("%s\n", buf.data());
  printf("------- Graph Debugging End -------\n");
}

void HexagonDelegateKernel::Teardown() {
  auto* hexagon_nn = HexagonNNImplementation();
  if (hexagon_nn != nullptr) {
    hexagon_nn->hexagon_nn_global_teardown();
  }
}

void HexagonDelegateKernel::InitState() {
  auto* hexagon_nn = HexagonNNImplementation();
  if (hexagon_nn != nullptr) {
    hexagon_nn->hexagon_nn_global_init();
  }
}
}  // namespace tflite
