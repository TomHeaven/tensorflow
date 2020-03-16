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

#include "tensorflow/lite/delegates/gpu/common/model_builder.h"

#include <stddef.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fp16.h>
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "tensorflow/lite/builtin_op_data.h"
#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/context.h"
#include "tensorflow/lite/context_util.h"
#include "tensorflow/lite/delegates/gpu/common/custom_parsers.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/tensor.h"
#include "tensorflow/lite/delegates/gpu/common/transformations/general_transformations.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/util.h"

namespace tflite {
namespace gpu {
namespace {

// Creates a node that consumes output from the given node. Because output need
// to stay the same, newly created node will inherit the output from the given
// node, which will in turn get newly created copy of output. This is necessary
// to preserve reference consistency if another node was pointing at that
// output:
//   node(output)
// will turn into:
//   node(copy(output)) <- passthrough_node(output)
Status NewPassthroughNode(GraphFloat32* graph, Node* node,
                          const Value<TensorRef<BHWC>>* output,
                          Node** passthru_node) {
  *passthru_node = graph->NewNode();
  // Make copies for every output in the original node.
  RETURN_IF_ERROR(graph->SetProducer((*passthru_node)->id, output->id));
  Value<TensorRef<BHWC>>* copy_output = graph->NewValue();
  RETURN_IF_ERROR(graph->SetProducer(node->id, copy_output->id));
  RETURN_IF_ERROR(graph->AddConsumer((*passthru_node)->id, copy_output->id));
  copy_output->tensor = output->tensor;
  copy_output->tensor.ref = -1;
  return OkStatus();
}

template <typename T>
Status CreateVectorCopyData(const TfLiteTensor& tensor, T* tensor_data) {
  if (tensor.bytes % sizeof(T) != 0) {
    return InvalidArgumentError(
        absl::StrCat("Input data size ", tensor.bytes,
                     " is not aligned to expected type: ", sizeof(T)));
  }
  std::memcpy(tensor_data, tensor.data.uint8, tensor.bytes);
  return OkStatus();
}

void ConvertFloat16ToFloat32(size_t num_elements, const uint16_t* src,
                             float* dst) {
  for (size_t i = 0; i < num_elements; i++) {
    *dst++ = fp16_ieee_to_fp32_value(*src++);
  }
}

template <>
Status CreateVectorCopyData<float>(const TfLiteTensor& tensor,
                                   float* tensor_data) {
  switch (tensor.type) {
    case kTfLiteFloat32:
      std::memcpy(tensor_data, tensor.data.f, tensor.bytes);
      break;
    case kTfLiteFloat16:
      ConvertFloat16ToFloat32(
          NumElements(&tensor),
          reinterpret_cast<uint16_t const*>(tensor.data.f16), tensor_data);
      break;
    default:
      return InvalidArgumentError("Unsupported data type for float32 tensor");
  }
  return OkStatus();
}

template <typename ShapeT>
Status SetAllDimensions(const TfLiteIntArray* dimensions, ShapeT* shape);

template <>
Status SetAllDimensions<Scalar>(const TfLiteIntArray* dimensions,
                                Scalar* shape) {
  if (dimensions->size < 0) {
    return InvalidArgumentError("Invalid Scalar dimensions");
  }
  for (int i = 0; i < dimensions->size; ++i) {
    if (dimensions->data[i] != 1) {
      return InvalidArgumentError("Dimension can not be reduced to scalar.");
    }
  }
  shape->v = 1;
  return OkStatus();
}

template <>
Status SetAllDimensions<Linear>(const TfLiteIntArray* dimensions,
                                Linear* shape) {
  if (dimensions->size <= 0) {
    return InvalidArgumentError("Dimension is empty.");
  }
  for (int i = 0; i < dimensions->size - 1; ++i) {
    if (dimensions->data[i] != 1) {
      return InvalidArgumentError("Dimension can not be reduced to linear.");
    }
  }
  shape->v = dimensions->data[dimensions->size - 1];
  return OkStatus();
}

template <>
Status SetAllDimensions<HWC>(const TfLiteIntArray* dimensions, HWC* shape) {
  if (dimensions->size != 4) {
    return InvalidArgumentError("Dimensions are not HWC");
  }
  if (dimensions->data[0] != 1) {
    return UnimplementedError("Batch size is not equal to 1.");
  }
  shape->h = dimensions->data[1];
  shape->w = dimensions->data[2];
  shape->c = dimensions->data[3];
  return OkStatus();
}

template <>
Status SetAllDimensions<HW>(const TfLiteIntArray* dimensions, HW* shape) {
  if (dimensions->size != 2) {
    return InvalidArgumentError("Dimensions are not HW");
  }
  shape->h = dimensions->data[0];
  shape->w = dimensions->data[1];
  return OkStatus();
}

template <>
Status SetAllDimensions<OHWI>(const TfLiteIntArray* dimensions, OHWI* shape) {
  if (dimensions->size != 4) {
    return InvalidArgumentError(
        absl::StrCat("Dimensions are not OHWI: ", dimensions->size));
  }
  shape->o = dimensions->data[0];
  shape->h = dimensions->data[1];
  shape->w = dimensions->data[2];
  shape->i = dimensions->data[3];
  return OkStatus();
}

template <>
Status SetAllDimensions<IHWO>(const TfLiteIntArray* dimensions, IHWO* shape) {
  if (dimensions->size != 4) {
    return InvalidArgumentError(
        absl::StrCat("Dimensions are not IHWO: ", dimensions->size));
  }
  shape->i = dimensions->data[0];
  shape->h = dimensions->data[1];
  shape->w = dimensions->data[2];
  shape->o = dimensions->data[3];
  return OkStatus();
}

template <>
Status SetAllDimensions<BHWC>(const TfLiteIntArray* dimensions, BHWC* shape) {
  if (dimensions->size != 4) {
    return InvalidArgumentError("Dimensions are not BHWC");
  }
  shape->b = dimensions->data[0];
  shape->h = dimensions->data[1];
  shape->w = dimensions->data[2];
  shape->c = dimensions->data[3];
  return OkStatus();
}

DataType ToDataType(TfLiteType type) {
  switch (type) {
    case kTfLiteFloat32:
      return DataType::FLOAT32;
    case kTfLiteInt32:
      return DataType::INT32;
    case kTfLiteInt64:
      return DataType::INT64;
    case kTfLiteUInt8:
      return DataType::UINT8;
    default:
      return DataType::UNKNOWN;
  }
}

int GetNumberOfRuntimeInputsForNode(const TfLiteContext* context,
                                    const TfLiteNode* tflite_node) {
  int number_of_runtime_inputs = 0;
  for (int i = 0; i < tflite_node->inputs->size; i++) {
    if (!IsConstantTensor(&context->tensors[tflite_node->inputs->data[i]])) {
      number_of_runtime_inputs++;
    }
  }
  return number_of_runtime_inputs;
}

int GetNumberOfRuntimeOutputsForNode(const TfLiteContext* context,
                                     const TfLiteNode* tflite_node) {
  int number_of_runtime_outputs = 0;
  for (int i = 0; i < tflite_node->outputs->size; i++) {
    if (!IsConstantTensor(&context->tensors[tflite_node->outputs->data[i]])) {
      number_of_runtime_outputs++;
    }
  }
  return number_of_runtime_outputs;
}

Status CheckTensorIsAvailable(const TfLiteContext* context,
                              const TfLiteNode* tflite_node, int idx) {
  // If tensor id is in range, it's guaranteed that it'll be available.
  if (idx >= tflite_node->inputs->size) {
    return OutOfRangeError(
        absl::StrFormat("Requested index goes beyond array size (%d vs %d).",
                        idx, tflite_node->inputs->data[idx]));
  }
  return OkStatus();
}

class ObjectReader {
 public:
  ObjectReader(GraphFloat32* graph, TfLiteContext* context,
               const TfLiteNode* tflite_node,
               std::vector<Value<TensorRef<BHWC>>*>* tensor_to_value)
      : graph_(graph),
        context_(context),
        tflite_node_(tflite_node),
        tensor_to_value_(tensor_to_value) {}

  Status ReadValue(uint32_t idx, Value<TensorRef<BHWC>>** value) const {
    if (idx >= tflite_node_->inputs->size) {
      return OutOfRangeError(
          absl::StrCat("ReadValue: input tensor index: ", idx));
    }
    return ReadValueByTensorIdx(tflite_node_->inputs->data[idx], value);
  }

  int GetNumberOfRuntimeInputs() const {
    return GetNumberOfRuntimeInputsForNode(context_, tflite_node_);
  }

  Status GetTensorDims(uint32_t idx, TfLiteIntArray* dimensions) const {
    if (idx >= tflite_node_->inputs->size) {
      return OutOfRangeError(absl::StrCat("Input tensor index: ", idx));
    }
    const int tensor_idx = tflite_node_->inputs->data[idx];
    if (tensor_idx < 0 || tensor_idx > context_->tensors_size) {
      return OutOfRangeError(absl::StrCat("Tensor index: ", tensor_idx));
    }
    const TfLiteTensor& tflite_tensor = context_->tensors[tensor_idx];
    *dimensions = *tflite_tensor.dims;
    return OkStatus();
  }

  template <typename TensorT>
  Status ReadTensor(uint32_t idx, TensorT* t) const {
    RETURN_IF_ERROR(CheckTensorIsAvailable(context_, tflite_node_, idx));
    const int32_t tensor_idx = tflite_node_->inputs->data[idx];
    const TfLiteTensor* tflite_tensor = context_->tensors + tensor_idx;
    t->data.resize(NumElements(tflite_tensor));
    RETURN_IF_ERROR(CreateVectorCopyData(*tflite_tensor, &t->data[0]));

    // Axis and data layout depend on operation this tensor is used in. So,
    // postpone resolutions until operations are parsed.
    t->id = tensor_idx;
    return SetAllDimensions(tflite_tensor->dims, &t->shape);
  }

  Status AddOutput(const Node* node, int id) {
    if (tflite_node_->outputs->size <= id) {
      return InvalidArgumentError(absl::StrCat(
          "Data id ", id, " must be less than tflite node outputs size ",
          tflite_node_->outputs->size));
    }
    int output_tensor_idx = tflite_node_->outputs->data[id];
    Value<TensorRef<BHWC>>* value;
    RETURN_IF_ERROR(ReadValueByTensorIdx(output_tensor_idx, &value));
    RETURN_IF_ERROR(graph_->SetProducer(node->id, value->id));
    return OkStatus();
  }

  Status AddOutputs(const Node* node) {
    for (int i = 0; i < tflite_node_->outputs->size; ++i) {
      RETURN_IF_ERROR(AddOutput(node, i));
    }
    return OkStatus();
  }

  Status AddInput(const Node* node, uint32_t idx) {
    Value<TensorRef<BHWC>>* input;
    RETURN_IF_ERROR(ReadValue(idx, &input));
    return graph_->AddConsumer(node->id, input->id);
  }

  Status ReadValueByTensorIdx(uint32_t tensor_idx,
                              Value<TensorRef<BHWC>>** value) const {
    if (tensor_idx >= tensor_to_value_->size()) {
      return OutOfRangeError(
          absl::StrCat("ReadValue: input tensor index: ", tensor_idx));
    }
    if ((*tensor_to_value_)[tensor_idx] == nullptr) {
      const TfLiteTensor& tflite_tensor = context_->tensors[tensor_idx];
      if (tflite::IsConstantTensor(&tflite_tensor)) {
        return NotFoundError(absl::StrCat(
            "ReadValue: value is a constant tensor: ", tensor_idx));
      }
      Value<TensorRef<BHWC>>* value = graph_->NewValue();
      RETURN_IF_ERROR(
          ConvertTfLiteTensorToTensorRef(tflite_tensor, &value->tensor));
      value->tensor.ref = tensor_idx;
      (*tensor_to_value_)[tensor_idx] = value;
    }
    *value = (*tensor_to_value_)[tensor_idx];
    return OkStatus();
  }

  TfLiteTensor* GetInputTensor(int index) const {
    return index >= 0 && index < tflite_node_->inputs->size
               ? context_->tensors + tflite_node_->inputs->data[index]
               : nullptr;
  }

  TfLiteTensor* GetOutputTensor(int index) const {
    return index >= 0 && index < tflite_node_->outputs->size
               ? context_->tensors + tflite_node_->outputs->data[index]
               : nullptr;
  }

 private:
  GraphFloat32* graph_ = nullptr;
  const TfLiteContext* context_ = nullptr;
  const TfLiteNode* tflite_node_ = nullptr;
  std::vector<Value<TensorRef<BHWC>>*>* tensor_to_value_;
};

Status CheckInputsOutputs(const TfLiteContext* context,
                          const TfLiteNode* tflite_node, int inputs,
                          int outputs) {
  int runtime_inputs = GetNumberOfRuntimeInputsForNode(context, tflite_node);
  if (runtime_inputs != inputs) {
    return InternalError(
        absl::StrFormat("Expected %d input tensor(s), but node has %d runtime "
                        "input(s).",
                        inputs, runtime_inputs));
  }
  int runtime_outputs = GetNumberOfRuntimeOutputsForNode(context, tflite_node);
  if (runtime_outputs != outputs) {
    return InternalError(
        absl::StrFormat("Expected %d output tensor(s), but node has %d runtime "
                        "output(s).",
                        outputs, runtime_outputs));
  }
  return OkStatus();
}

// The function checks input tensors including 1 constant tensor.
Status CheckInputsOutputsAllowingOneConstInput(const TfLiteContext* context,
                                               const TfLiteNode* tflite_node,
                                               int inputs, int outputs) {
  int number_of_const_inputs = 0;
  int number_of_runtime_inputs = 0;
  for (int i = 0; i < tflite_node->inputs->size; i++) {
    if (IsConstantTensor(&context->tensors[tflite_node->inputs->data[i]])) {
      number_of_const_inputs++;
    } else {
      number_of_runtime_inputs++;
    }
  }
  if (tflite_node->inputs->size != inputs) {
    return InternalError(absl::StrFormat(
        "Expected %d input tensor(s), but node has %d input(s).", inputs,
        tflite_node->inputs->size));
  }
  if (number_of_const_inputs > 1) {
    return InternalError(absl::StrFormat(
        "Expected 1 const input tensor, but node has %d const input(s).",
        number_of_const_inputs));
  }
  int runtime_outputs = GetNumberOfRuntimeOutputsForNode(context, tflite_node);
  if (runtime_outputs != outputs) {
    return InternalError(
        absl::StrFormat("Expected %d output tensor(s), but node has %d runtime "
                        "output(s).",
                        outputs, runtime_outputs));
  }
  return OkStatus();
}

// A parser responsible for parsing TFLite operation and adding it to a graph.
class TFLiteOperationParser {
 public:
  virtual ~TFLiteOperationParser() = default;

  // Parses TFLite operation. This method allows expanding fused operations
  // into more than one node.
  virtual Status Parse(const TfLiteNode* tflite_node,
                       const TfLiteRegistration* registration,
                       GraphFloat32* graph, ObjectReader* reader) = 0;

  // Verifies whether passed tflite node may be built by GPU delegate or not.
  virtual Status IsSupported(const TfLiteContext* context,
                             const TfLiteNode* tflite_node,
                             const TfLiteRegistration* registration) = 0;
};

Status IsActivationSupported(TfLiteFusedActivation fused_activation) {
  switch (fused_activation) {
    case kTfLiteActNone:
    case kTfLiteActRelu:
    case kTfLiteActRelu1:
    case kTfLiteActRelu6:
    case kTfLiteActTanh:
      return OkStatus();
    case kTfLiteActSignBit:
      return UnimplementedError("TfLiteFusedActivation.kTfLiteActSignBit");
    case kTfLiteActSigmoid:
      return UnimplementedError("TfLiteFusedActivation.kTfLiteActSigmoid");

      // Do not add default; we want compilation error rather than run-time
      // error.
  }
}

// If there is fused activation present, then there will be another node created
// that will have identical output as the given node. New operation node will
// depend on the given node output.
Status MaybeFuseActivation(TfLiteFusedActivation fused_activation,
                           const std::vector<uint32_t>& output_indices,
                           GraphFloat32* graph, Node* node) {
  if (fused_activation == kTfLiteActNone) {
    return OkStatus();
  }
  const auto& outputs = graph->FindOutputs(node->id);
  if (outputs.empty()) {
    return InternalError("Empty outputs in fused node");
  }
  switch (fused_activation) {
    case kTfLiteActRelu:
    case kTfLiteActRelu1:
    case kTfLiteActRelu6: {
      ReLUAttributes attr;
      attr.clip = fused_activation == kTfLiteActRelu
                      ? 0.0f
                      : (fused_activation == kTfLiteActRelu1 ? 1.0f : 6.0f);
      for (auto index : output_indices) {
        Node* activation_node;
        RETURN_IF_ERROR(
            NewPassthroughNode(graph, node, outputs[index], &activation_node));
        activation_node->operation.type = ToString(OperationType::RELU);
        activation_node->operation.attributes = attr;
      }
      break;
    }
    case kTfLiteActTanh:
      for (auto index : output_indices) {
        Node* activation_node;
        RETURN_IF_ERROR(
            NewPassthroughNode(graph, node, outputs[index], &activation_node));
        activation_node->operation.type = ToString(OperationType::TANH);
      }
      break;
    default:
      return NotFoundError(
          absl::StrCat("Unsupported fused activation: ", fused_activation));
  }
  return OkStatus();
}

Status MaybeFuseActivationToTheSingleOutput(
    TfLiteFusedActivation fused_activation, GraphFloat32* graph, Node* node) {
  if (graph->FindOutputs(node->id).size() != 1) {
    return InternalError("Number of outputs exceeds 1");
  }
  return MaybeFuseActivation(fused_activation, {0}, graph, node);
}

HW ToHW(int32_t h, int32_t w) { return HW(h > 0 ? h : 1, w > 0 ? w : 1); }

template <typename AttrT>
void UpdatePadding(const TfLitePadding& padding, const BHWC& input_shape,
                   AttrT* attr) {
  if (padding == kTfLitePaddingSame) {
    attr->padding = CalculateSamePadding(input_shape, *attr);
  } else {
    attr->padding.prepended = HW(0, 0);
    attr->padding.appended = HW(0, 0);
  }
}

Status GetFullyConnectedAttributes(int weights_tensor_id, int bias_tensor_id,
                                   ObjectReader* reader,
                                   FullyConnectedAttributes* attr) {
  Tensor<HW, DataType::FLOAT32> weights;
  RETURN_IF_ERROR(reader->ReadTensor(weights_tensor_id, &weights));
  attr->weights.data = std::move(weights.data);
  attr->weights.id = weights.id;
  attr->weights.shape.h = 1;
  attr->weights.shape.w = 1;
  attr->weights.shape.o = weights.shape.h;
  attr->weights.shape.i = weights.shape.w;
  reader->ReadTensor(bias_tensor_id, &attr->bias).IgnoreError();  // optional

  return OkStatus();
}

template <typename ParamsT>
Status RetrieveBuiltinData(const TfLiteNode* tflite_node,
                           ParamsT** tf_options) {
  const auto* params =
      reinterpret_cast<const ParamsT*>(tflite_node->builtin_data);
  if (!params) {
    return InternalError("Unable to retrieve builtin_data.");
  }
  *tf_options = const_cast<ParamsT*>(params);
  return OkStatus();
}

template <typename ParamsType>
Status RetrieveCustomInitialData(const TfLiteNode* tflite_node,
                                 ParamsType** tf_options) {
  const auto* params =
      reinterpret_cast<const ParamsType*>(tflite_node->custom_initial_data);
  if (!params) {
    return InternalError("Unable to retrieve custom_initial_data.");
  }
  *tf_options = const_cast<ParamsType*>(params);
  return OkStatus();
}

Status CheckMaxSupportedOpVersion(const TfLiteRegistration* registration,
                                  int max_version) {
  const int op_version = registration->version;
  if (op_version > max_version) {
    return UnimplementedError(
        absl::StrFormat("Max version supported: %d. Requested version %d.",
                        max_version, op_version));
  }
  return OkStatus();
}

Status CheckExactSupportedOpVersion(const TfLiteRegistration* registration,
                                    int expected_version) {
  int op_version = registration->version;
  if (op_version != expected_version) {
    return UnimplementedError(
        absl::StrFormat("Only version %d is supported. Requested version %d.",
                        expected_version, op_version));
  }
  return OkStatus();
}

Status CheckKernels(int kernel_h, int kernel_w) {
  if (kernel_h <= 0 || kernel_w <= 0) {
    return InvalidArgumentError(absl::StrFormat(
        "Incorrect kernel values: kernel_height = %d, kernel_width = %d.",
        kernel_h, kernel_w));
  }
  return OkStatus();
}

Status CheckStrides(int strides_h, int strides_w) {
  if (strides_h <= 0 || strides_w <= 0) {
    return InvalidArgumentError(absl::StrFormat(
        "Incorrect stride values: stride_height = %d, stride_width = %d.",
        strides_h, strides_w));
  }
  return OkStatus();
}

Status CheckDilation(int dilation_h, int dilation_w) {
  if (dilation_h <= 0 || dilation_w <= 0) {
    return InvalidArgumentError(
        absl::StrFormat("Incorrect dilation values: dilation_factor = %d, "
                        "dilation_factor = %d.",
                        dilation_h, dilation_w));
  }
  return OkStatus();
}

Status CheckStridesAndDilation(int strides_h, int strides_w, int dilation_h,
                               int dilation_w) {
  RETURN_IF_ERROR(CheckStrides(strides_h, strides_w));
  RETURN_IF_ERROR(CheckDilation(dilation_h, dilation_w));
  return OkStatus();
}

Status CheckKernelsAndStrides(int kernel_h, int kernel_w, int strides_h,
                              int strides_w) {
  RETURN_IF_ERROR(CheckKernels(kernel_h, kernel_w));
  RETURN_IF_ERROR(CheckStrides(strides_h, strides_w));
  return OkStatus();
}

// Creates a simple node that holds tensor value.
Status NewConstNode(TensorFloat32 t, GraphFloat32* graph,
                    Value<TensorRef<BHWC>>** value) {
  ConstTensorAttributes attr;
  attr.tensor = std::move(t);
  Node* node = graph->NewNode();
  node->operation.attributes = attr;
  node->operation.type = ToString(OperationType::CONST);
  *value = graph->NewValue();
  RETURN_IF_ERROR(graph->SetProducer(node->id, (*value)->id));
  // Keep data inside this tensor.
  (*value)->tensor.ref = attr.tensor.id;
  (*value)->tensor.type = attr.tensor.kType;
  (*value)->tensor.shape = attr.tensor.shape;
  return OkStatus();
}

Status ParsePoolingAttributes(const TfLitePoolParams* tf_options,
                              const BHWC& input_shape,
                              Pooling2DAttributes* attr) {
  attr->kernel = ToHW(tf_options->filter_height, tf_options->filter_width);
  attr->strides = ToHW(tf_options->stride_height, tf_options->stride_width);
  UpdatePadding(tf_options->padding, input_shape, attr);
  return OkStatus();
}

Status ExtractTensorShape(const TfLiteTensor& tflite_tensor, BHWC* bhwc) {
  const TfLiteIntArray* dims = tflite_tensor.dims;
  switch (dims->size) {
    case 1:
      *bhwc = BHWC(dims->data[0], 1, 1, 1);
      return OkStatus();
    case 2:
      *bhwc = BHWC(dims->data[0], 1, 1, dims->data[1]);
      return OkStatus();
    case 3:
      *bhwc = BHWC(dims->data[0], 1, dims->data[1], dims->data[2]);
      return OkStatus();
    case 4:
      *bhwc = BHWC(dims->data[0], dims->data[1], dims->data[2], dims->data[3]);
      return OkStatus();
    default:
      return InvalidArgumentError(absl::StrCat(
          "Tensor \"", tflite_tensor.name ? tflite_tensor.name : "nullptr",
          "\" has bad input dims size: ", dims->size, "."));
  }
}

Status ParseInputsWithConstTensor(Node* node, ObjectReader* reader,
                                  TensorOrScalar* tensor_or_scalar) {
  const std::string& opname = node->operation.type;

  // Determine runtime/constant tensors.
  const TfLiteTensor* input0 = reader->GetInputTensor(0);
  if (!input0) {
    return InvalidArgumentError("Couldn't get the 1st input tensor for " +
                                opname);
  }
  const TfLiteTensor* input1 = reader->GetInputTensor(1);
  if (!input1) {
    return InvalidArgumentError("Couldn't get the 2nd input tensor for " +
                                opname);
  }
  const bool constant_tensor0 = IsConstantTensor(input0);
  const bool constant_tensor1 = IsConstantTensor(input1);
  if (constant_tensor0 && constant_tensor1) {
    return InvalidArgumentError("No runtime input tensors for " + opname);
  }
  const bool runtime_tensor0 = !constant_tensor0;
  const bool runtime_tensor1 = !constant_tensor1;

  if (runtime_tensor0 && runtime_tensor1) {
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddInput(node, 1));
  } else {
    int runtime_tensor = 0;
    int constant_tensor = 1;
    TfLiteIntArray* constant_dims = input1->dims;
    if (constant_tensor0 && runtime_tensor1) {
      runtime_tensor = 1;
      constant_tensor = 0;
      constant_dims = input0->dims;
    }
    RETURN_IF_ERROR(reader->AddInput(node, runtime_tensor));
    if (constant_dims->size <= 0) {
      Tensor<Scalar, DataType::FLOAT32> tensor;
      RETURN_IF_ERROR(reader->ReadTensor(constant_tensor, &tensor));
      *tensor_or_scalar = tensor.data[0];
    } else {
      Tensor<Linear, DataType::FLOAT32> tensor;
      RETURN_IF_ERROR(reader->ReadTensor(constant_tensor, &tensor));
      *tensor_or_scalar = std::move(tensor);
    }
  }
  return OkStatus();
}

class AddOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    if (tflite_node->inputs->size != 2) {
      return UnimplementedError("ADD requires two input tensors.");
    }
    // TODO(eignasheva): Add shapes check.
    TfLiteAddParams* tf_options = nullptr;
    return RetrieveBuiltinData(tflite_node, &tf_options);
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    // TFLite currently only supports 2 input ADDs.  Thus, the logic below only
    // considers 2 input cases.  The underlying GPU shader programs can accept
    // more inputs, but the logic below would have to be expanded.

    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::ADD);
    RETURN_IF_ERROR(reader->AddOutputs(node));
    AddAttributes attr;
    RETURN_IF_ERROR(ParseInputsWithConstTensor(node, reader, &attr.param));
    node->operation.attributes = std::move(attr);
    const auto* tf_options =
        reinterpret_cast<const TfLiteAddParams*>(tflite_node->builtin_data);
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    return MaybeFuseActivationToTheSingleOutput(tf_options->activation, graph,
                                                node);
  }
};

class ConcatenationOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));

    // TODO(eignasheva): add proper tensor availability checking
    // for (uint32_t idx = 0; idx < tflite_node->inputs->size; ++idx) {
    //   RETURN_IF_ERROR(CheckTensorIsAvailable(context, tflite_node, idx));
    // }
    // TODO(eignasheva): add axis checking.
    TfLiteConcatenationParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    ConcatAttributes attr;
    // Read inputs first to make sure const node is added to a graph before
    // concat node to ensure topological order.
    std::vector<const Value<TensorRef<BHWC>>*> inputs;
    for (uint32_t idx = 0; idx < tflite_node->inputs->size; ++idx) {
      Value<TensorRef<BHWC>>* value;
      const auto status = reader->ReadValue(idx, &value);
      if (status.ok()) {
        inputs.push_back(value);
      } else {
        TensorFloat32 tensor;
        RETURN_IF_ERROR(reader->ReadTensor(idx, &tensor));
        Value<TensorRef<BHWC>>* value;
        RETURN_IF_ERROR(NewConstNode(std::move(tensor), graph, &value));
        inputs.push_back(value);
      }
    }

    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::CONCAT);
    RETURN_IF_ERROR(reader->AddOutputs(node));
    for (const Value<TensorRef<BHWC>>* input : inputs) {
      RETURN_IF_ERROR(graph->AddConsumer(node->id, input->id));
    }

    std::vector<BHWC> input_shapes;
    for (auto input : graph->FindInputs(node->id)) {
      input_shapes.push_back(input->tensor.shape);
    }
    RETURN_IF_ERROR(SetAxis(input_shapes, &attr.axis));

    // Guess axis.
    BHWC output_shape = graph->FindOutputs(node->id)[0]->tensor.shape;
    for (auto input : graph->FindInputs(node->id)) {
      if (input->tensor.shape.h != output_shape.h) {
        attr.axis = Axis::HEIGHT;
        break;
      }
      if (input->tensor.shape.w != output_shape.w) {
        attr.axis = Axis::WIDTH;
        break;
      }
      if (input->tensor.shape.c != output_shape.c) {
        attr.axis = Axis::CHANNELS;
        break;
      }
    }
    const auto* tf_options = reinterpret_cast<const TfLiteConcatenationParams*>(
        tflite_node->builtin_data);
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    RETURN_IF_ERROR(MaybeFuseActivationToTheSingleOutput(tf_options->activation,
                                                         graph, node));
    node->operation.attributes = attr;
    return OkStatus();
  }

 private:
  Status SetAxis(const std::vector<BHWC>& input_shapes, Axis* axis) {
    *axis = Axis::BATCH;
    for (int i = 1; i < input_shapes.size(); i++) {
      if (input_shapes[0].h != input_shapes[i].h &&
          input_shapes[0].w != input_shapes[i].w &&
          input_shapes[0].c != input_shapes[i].c) {
        *axis = Axis::HEIGHT;
        break;
      }
    }
    if (*axis == Axis::BATCH) return OkStatus();
    for (int i = 1; i < input_shapes.size(); i++) {
      if (input_shapes[0].b != input_shapes[i].b &&
          input_shapes[0].w != input_shapes[i].w &&
          input_shapes[0].c != input_shapes[i].c) {
        *axis = Axis::WIDTH;
        break;
      }
    }
    if (*axis == Axis::HEIGHT) return OkStatus();
    for (int i = 1; i < input_shapes.size(); i++) {
      if (input_shapes[0].b != input_shapes[i].b &&
          input_shapes[0].h != input_shapes[i].h &&
          input_shapes[0].c != input_shapes[i].c) {
        *axis = Axis::CHANNELS;
        break;
      }
    }
    if (*axis == Axis::WIDTH) return OkStatus();
    for (int i = 1; i < input_shapes.size(); i++) {
      if (input_shapes[0].b != input_shapes[i].b &&
          input_shapes[0].w != input_shapes[i].w &&
          input_shapes[0].h != input_shapes[i].h) {
        return UnimplementedError(
            "Can concatenate tensors only by batch, height, width, or "
            "channels.");
      }
    }
    return OkStatus();
  }
};

class Conv2DOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 2));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    RETURN_IF_ERROR(CheckTensorIsAvailable(context, tflite_node, 1));
    TfLiteConvParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    RETURN_IF_ERROR(CheckStridesAndDilation(
        tf_options->stride_height, tf_options->stride_width,
        tf_options->dilation_height_factor, tf_options->dilation_width_factor));
    return IsActivationSupported(tf_options->activation);
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::CONVOLUTION_2D);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    Convolution2DAttributes attr;
    RETURN_IF_ERROR(reader->ReadTensor(1, &attr.weights));
    reader->ReadTensor(2, &attr.bias).IgnoreError();  // bias is optional

    const auto* tf_options =
        reinterpret_cast<const TfLiteConvParams*>(tflite_node->builtin_data);
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    attr.strides = ToHW(tf_options->stride_height, tf_options->stride_width);
    attr.dilations = HW(tf_options->dilation_height_factor,
                        tf_options->dilation_width_factor);
    UpdatePadding(tf_options->padding,
                  graph->FindInputs(node->id)[0]->tensor.shape, &attr);
    RETURN_IF_ERROR(MaybeFuseActivationToTheSingleOutput(tf_options->activation,
                                                         graph, node));
    node->operation.attributes = std::move(attr);
    return OkStatus();
  }
};

class Convolution2DTransposeBiasParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckTensorIsAvailable(context, tflite_node, 1));
    TfLiteTransposeConvParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveCustomInitialData(tflite_node, &tf_options));
    RETURN_IF_ERROR(
        CheckStrides(tf_options->stride_height, tf_options->stride_width));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    auto* node = graph->NewNode();
    node->operation.type = ToString(OperationType::CONVOLUTION_TRANSPOSED);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    const auto* params = reinterpret_cast<const TfLiteTransposeConvParams*>(
        tflite_node->custom_initial_data);
    ConvolutionTransposedAttributes attr;
    attr.stride =
        params ? HW(params->stride_height, params->stride_width) : HW(1, 1);

    RETURN_IF_ERROR(reader->ReadTensor(1, &attr.weights));
    reader->ReadTensor(2, &attr.bias).IgnoreError();  // bias is optional

    UpdatePadding(params->padding, graph->FindInputs(node->id)[0]->tensor.shape,
                  &attr);

    node->operation.attributes = std::move(attr);
    return OkStatus();
  }
};

class DepthwiseConvolutionOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 2));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    RETURN_IF_ERROR(CheckTensorIsAvailable(context, tflite_node, 1));
    TfLiteDepthwiseConvParams* tf_options;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    RETURN_IF_ERROR(CheckStridesAndDilation(
        tf_options->stride_height, tf_options->stride_width,
        tf_options->dilation_height_factor, tf_options->dilation_width_factor));
    RETURN_IF_ERROR(IsActivationSupported(tf_options->activation));

    const int depth_multiplier = tf_options->depth_multiplier;
    const auto* input = context->tensors + tflite_node->inputs->data[0];
    const auto* filter = context->tensors + tflite_node->inputs->data[1];
    const auto* bias = tflite_node->inputs->size > 2
                           ? context->tensors + tflite_node->inputs->data[2]
                           : nullptr;
    const auto* output = context->tensors + tflite_node->outputs->data[0];
    if (!input->dims || input->dims->size != 4) {
      return InvalidArgumentError("input.dims.size != 4");
    }
    if (!filter->dims || filter->dims->size != 4) {
      return InvalidArgumentError("filter.dims.size != 4");
    }
    if (!output->dims || output->dims->size != 4) {
      return InvalidArgumentError("output.dims.size != 4");
    }
    if (input->dims->data[0] != output->dims->data[0]) {
      return InvalidArgumentError("input.b != output.b");
    }
    const int input_depth = input->dims->data[3];
    const int output_depth = output->dims->data[3];
    if (filter->dims->data[3] != output_depth) {
      return InvalidArgumentError("filter.i != output.c");
    }
    if (output_depth != input_depth * depth_multiplier) {
      return InvalidArgumentError("output.c != input.c * depth_multiplier");
    }
    if (bias && NumElements(bias) != output_depth) {
      return InvalidArgumentError("bias.size != output.c");
    }
    if (depth_multiplier != 1 && input_depth != 1) {
      return UnimplementedError("depth_multiplier != 1 && input.c != 1");
    }
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::DEPTHWISE_CONVOLUTION);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    DepthwiseConvolution2DAttributes attr;
    RETURN_IF_ERROR(reader->ReadTensor(1, &attr.weights));
    reader->ReadTensor(2, &attr.bias).IgnoreError();  // bias is optional
    TfLiteDepthwiseConvParams* tf_options;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    attr.strides = ToHW(tf_options->stride_height, tf_options->stride_width);
    attr.dilations = HW(std::max(1, tf_options->dilation_height_factor),
                        std::max(1, tf_options->dilation_width_factor));
    UpdatePadding(tf_options->padding,
                  graph->FindInputs(node->id)[0]->tensor.shape, &attr);
    RETURN_IF_ERROR(MaybeFuseActivationToTheSingleOutput(tf_options->activation,
                                                         graph, node));
    const int depth_multiplier = tf_options->depth_multiplier;
    if (depth_multiplier != 1) {
      const TfLiteTensor* input = reader->GetInputTensor(0);
      const TfLiteTensor* filter = reader->GetInputTensor(1);
      const TfLiteTensor* output = reader->GetOutputTensor(0);
      TransposeWeights(input, filter, output, depth_multiplier, &attr);
    }
    node->operation.attributes = std::move(attr);
    return OkStatus();
  }

 private:
  // TFLite CPU stores weights as:
  //   [1, kernel_height, kernel_width, input_depth * depth_multiplier]
  // TFLite GPU stores weights as:
  //   [depth_multiplier, kernel_height, kernel_width, input_depth]
  static void TransposeWeights(const TfLiteTensor* input,
                               const TfLiteTensor* filter,
                               const TfLiteTensor* output, int depth_multiplier,
                               DepthwiseConvolution2DAttributes* attr) {
    const int input_depth = input->dims->data[3];
    const int filter_height = filter->dims->data[1];
    const int filter_width = filter->dims->data[2];
    const int output_depth = output->dims->data[3];
    Tensor<OHWI, DataType::FLOAT32> weights;
    weights.id = attr->weights.id;
    weights.shape =
        OHWI(output_depth, filter_height, filter_width, input_depth);
    weights.data.resize(weights.shape.DimensionsProduct());
    float* dst = &weights.data[0];
    for (int j = 0; j < output_depth; ++j) {
      const float* src = attr->weights.data.data() + j;
      for (int i = 0; i < filter_height * filter_width; ++i) {
        *dst = *src;
        dst++;
        src += output_depth;
      }
    }
    attr->weights = std::move(weights);
  }
};

class ElementwiseOperationParser : public TFLiteOperationParser {
 public:
  explicit ElementwiseOperationParser(OperationType operation_type)
      : operation_type_(operation_type) {}

  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    if (IsOneArgumentOperation()) {
      RETURN_IF_ERROR(CheckInputsOutputs(context, tflite_node, /*inputs=*/1,
                                         /*outputs=*/1));
    } else if (IsTwoArgumentOperation()) {
      RETURN_IF_ERROR(CheckInputsOutputs(context, tflite_node, /*inputs=*/2,
                                         /*outputs=*/1));
    } else if (IsTwoArgumentOperationWithConst()) {
      RETURN_IF_ERROR(CheckInputsOutputsAllowingOneConstInput(context,
                                                              tflite_node,
                                                              /*inputs=*/2,
                                                              /*outputs=*/1));
    } else {
      return InvalidArgumentError("Op can only handle 1 or 2 operand(s).");
    }
    TfLiteFusedActivation activation;
    RETURN_IF_ERROR(GetActivation(tflite_node, &activation));
    return IsActivationSupported(activation);
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(operation_type_);

    if (IsOneArgumentOperation()) {
      RETURN_IF_ERROR(reader->AddInput(node, 0));
    } else if (IsTwoArgumentOperation()) {
      if (tflite_node->inputs->size != 2) {
        return InvalidArgumentError("Applies only two input tensors");
      }
      RETURN_IF_ERROR(reader->AddInput(node, 0));
      RETURN_IF_ERROR(reader->AddInput(node, 1));

      TfLiteFusedActivation activation = kTfLiteActNone;
      switch (operation_type_) {
        case OperationType::SUB: {
          const auto* tf_options = reinterpret_cast<const TfLiteSubParams*>(
              tflite_node->builtin_data);
          if (tf_options != nullptr) {
            activation = tf_options->activation;
          }
          break;
        }
        case OperationType::DIV: {
          const auto* tf_options = reinterpret_cast<const TfLiteDivParams*>(
              tflite_node->builtin_data);
          if (tf_options != nullptr) {
            activation = tf_options->activation;
          }
          break;
        }
        default:
          // No activation expected.
          activation = kTfLiteActNone;
      }

      if (activation) {
        RETURN_IF_ERROR(
            MaybeFuseActivationToTheSingleOutput(activation, graph, node));
      }
    } else if (IsTwoArgumentOperationWithConst()) {
      ElementwiseAttributes attr;
      RETURN_IF_ERROR(ParseInputsWithConstTensor(node, reader, &attr.param));
      auto const_vector =
          absl::get_if<::tflite::gpu::Tensor<Linear, DataType::FLOAT32>>(
              &attr.param);
      if (const_vector) {
        return InvalidArgumentError("Constant vector is not supported");
      }
      node->operation.attributes = std::move(attr);
    } else {
      return InvalidArgumentError("Incorrect operation type passed");
    }

    return reader->AddOutputs(node);
  }

 private:
  Status GetActivation(const TfLiteNode* tflite_node,
                       TfLiteFusedActivation* activation) const {
    if (operation_type_ == OperationType::DIV) {
      TfLiteDivParams* tf_options;
      RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
      *activation = tf_options ? tf_options->activation : kTfLiteActNone;
      return OkStatus();
    }
    if (operation_type_ == OperationType::SUB) {
      TfLiteSubParams* tf_options;
      RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
      *activation = tf_options ? tf_options->activation : kTfLiteActNone;
      return OkStatus();
    }

    // Return kTfLiteActNone as other ops either do not have TfLiteXxxParams or
    // TfLiteXxxParams.activation.
    *activation = kTfLiteActNone;
    return OkStatus();
  }

  bool IsOneArgumentOperation() const {
    switch (operation_type_) {
      case OperationType::ABS:
      case OperationType::COS:
      case OperationType::EXP:
      case OperationType::LOG:
      case OperationType::RSQRT:
      case OperationType::SIGMOID:
      case OperationType::SIN:
      case OperationType::SQRT:
      case OperationType::SQUARE:
      case OperationType::TANH:
        return true;
      default:
        return false;
    }
  }

  bool IsTwoArgumentOperation() const {
    switch (operation_type_) {
      case OperationType::DIV:
      case OperationType::POW:
      case OperationType::SQUARED_DIFF:
      case OperationType::SUB:
        return true;
      default:
        return false;
    }
  }

  bool IsTwoArgumentOperationWithConst() const {
    switch (operation_type_) {
      case OperationType::MINIMUM:
      case OperationType::MAXIMUM:
        return true;
      default:
        return false;
    }
  }

  OperationType operation_type_;
};

class FullyConnectedOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    TfLiteFullyConnectedParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    if (tf_options->weights_format !=
        kTfLiteFullyConnectedWeightsFormatDefault) {
      return UnimplementedError("Unsupported FullyConnected weights format.");
    }
    // TODO(eignasheva): check input shape
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    RETURN_IF_ERROR(reader->AddInput(node, 0));

    const auto* tf_options =
        reinterpret_cast<const TfLiteFullyConnectedParams*>(
            tflite_node->builtin_data);
    if (tf_options->weights_format !=
        kTfLiteFullyConnectedWeightsFormatDefault) {
      return UnimplementedError("Unsupported FullyConnected weights format.");
    }

    FullyConnectedAttributes attr;
    RETURN_IF_ERROR(GetFullyConnectedAttributes(1, 2, reader, &attr));

    Tensor<HW, DataType::FLOAT32> weights;
    RETURN_IF_ERROR(reader->ReadTensor(1, &weights));
    auto input = graph->FindInputs(node->id)[0];
    int batch_size = input->tensor.shape.b;
    if (input->tensor.shape.DimensionsProduct() / batch_size !=
        weights.shape.w) {
      return UnimplementedError(
          "Amount of input data should match weights width");
    }

    Node* conv = node;
    if (input->tensor.shape.h != 1 || input->tensor.shape.w != 1) {
      auto& reshape = node;
      conv = graph->NewNode();  // reset conv pointer!
      Value<TensorRef<BHWC>>* reshaped_value = graph->NewValue();
      reshaped_value->tensor.type = DataType::FLOAT32;
      reshaped_value->tensor.shape =
          BHWC(input->tensor.shape.b, 1, 1, weights.shape.w);
      RETURN_IF_ERROR(graph->SetProducer(reshape->id, reshaped_value->id));
      reshape->operation.type = ToString(OperationType::RESHAPE);
      ReshapeAttributes attr;
      attr.new_shape = reshaped_value->tensor.shape;
      reshape->operation.attributes = attr;
      RETURN_IF_ERROR(graph->AddConsumer(conv->id, reshaped_value->id));
    }

    conv->operation.type = ToString(OperationType::FULLY_CONNECTED);
    conv->operation.attributes = std::move(attr);
    Status result = reader->AddOutputs(conv);
    RETURN_IF_ERROR(MaybeFuseActivationToTheSingleOutput(tf_options->activation,
                                                         graph, conv));

    return result;
  }
};

class HardSwishOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration*) final {
    return CheckInputsOutputs(context, tflite_node, /*inputs=*/1,
                              /*outputs=*/1);
  }

  Status Parse(const TfLiteNode*, const TfLiteRegistration*,
               GraphFloat32* graph, ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::HARD_SWISH);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    return reader->AddOutputs(node);
  }
};

// Basic LSTM Cell:
//
//  1name = name is at input  index 1
//  name1 = name is at output index 1
//
//    0input     1prev_activ
//       \        /
//        [[concat]]
//             \
//       concat_temp2  2weights  3biases
//              \      /        /
//             [[fully-connected]]
//               \
//         activ_temp3    4prev_state
//                 \      /
//                 [[LSTM]]
//                 /      \
//           new_state1    activation0
//
class LSTMOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckExactSupportedOpVersion(registration, 2));
    // TODO(eignasheva): Fix bad check.
    // RETURN_IF_ERROR(CheckInputsOutputs(context, tflite_node, /*inputs=*/5,
    //                                    /*outputs=*/4));
    TfLiteLSTMParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    RETURN_IF_ERROR(CheckParameters(tf_options));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    if (tflite_node->inputs->size != 5) {
      return InvalidArgumentError("LSTM should have 5 input tensors");
    }
    if (tflite_node->outputs->size != 4) {
      return InvalidArgumentError("LSTM should have 4 output tensors");
    }

    const auto* params =
        reinterpret_cast<const TfLiteLSTMParams*>(tflite_node->builtin_data);
    if (!params) {
      return InternalError("Missing tflite params");
    }
    RETURN_IF_ERROR(CheckParameters(params));

    Node* concat_node = graph->NewNode();
    concat_node->operation.type = ToString(OperationType::CONCAT);
    ConcatAttributes concat_attr;
    concat_attr.axis = Axis::CHANNELS;
    concat_node->operation.attributes = concat_attr;

    Node* fc_node = graph->NewNode();
    fc_node->operation.type = ToString(OperationType::FULLY_CONNECTED);
    FullyConnectedAttributes fc_attr;
    RETURN_IF_ERROR(GetFullyConnectedAttributes(2, 3, reader, &fc_attr));
    fc_node->operation.attributes = std::move(fc_attr);

    Node* lstm_node = graph->NewNode();
    lstm_node->operation.type = ToString(OperationType::LSTM);
    LstmAttributes lstm_attr;
    lstm_attr.kernel_type = LstmKernelType::BASIC;
    lstm_node->operation.attributes = lstm_attr;

    Value<TensorRef<BHWC>>* concat_temp;
    int concat_tensor_idx = tflite_node->outputs->data[2];
    RETURN_IF_ERROR(
        reader->ReadValueByTensorIdx(concat_tensor_idx, &concat_temp));
    Value<TensorRef<BHWC>>* activ_temp;
    int activ_tensor_idx = tflite_node->outputs->data[3];
    RETURN_IF_ERROR(
        reader->ReadValueByTensorIdx(activ_tensor_idx, &activ_temp));

    RETURN_IF_ERROR(reader->AddInput(concat_node, 0));  // input
    RETURN_IF_ERROR(reader->AddInput(concat_node, 1));  // prev_activ
    RETURN_IF_ERROR(graph->SetProducer(concat_node->id, concat_temp->id));

    RETURN_IF_ERROR(graph->AddConsumer(fc_node->id, concat_temp->id));
    RETURN_IF_ERROR(graph->SetProducer(fc_node->id, activ_temp->id));

    RETURN_IF_ERROR(graph->AddConsumer(lstm_node->id, activ_temp->id));
    RETURN_IF_ERROR(reader->AddInput(lstm_node, 4));   // prev_state
    RETURN_IF_ERROR(reader->AddOutput(lstm_node, 1));  // new_state
    RETURN_IF_ERROR(reader->AddOutput(lstm_node, 0));  // activation

    return OkStatus();
  }

 private:
  Status CheckParameters(const TfLiteLSTMParams* tf_options) {
    if (tf_options->kernel_type !=
        TfLiteLSTMKernelType::kTfLiteLSTMBasicKernel) {
      return UnimplementedError("Only kTfLiteLSTMBasicKernel is supported.");
    }
    if (tf_options->activation != kTfLiteActTanh) {
      return UnimplementedError("Only TANH activation is supported.");
    }
    if (tf_options->cell_clip != 0.0f) {
      return UnimplementedError("cell_clip is not supported.");
    }
    if (tf_options->proj_clip != 0.0f) {
      return UnimplementedError("proj_clip is not supported.");
    }
    return OkStatus();
  }
};

class MulOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    if (tflite_node->inputs->size != 2) {
      return UnimplementedError("MUL requires two input tensors.");
    }
    // TODO(eignasheva): Add params check.
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    // Determine runtime/constant tensors.
    const TfLiteTensor* input0 = reader->GetInputTensor(0);
    if (!input0) {
      return InvalidArgumentError("Couldn't get the 1st input tensor for MUL.");
    }
    const TfLiteTensor* input1 = reader->GetInputTensor(1);
    if (!input1) {
      return InvalidArgumentError("Couldn't get the 2nd input tensor for MUL.");
    }
    const bool constant_tensor0 = IsConstantTensor(input0);
    const bool constant_tensor1 = IsConstantTensor(input1);
    if (constant_tensor0 && constant_tensor1) {
      return InvalidArgumentError("No runtime input tensors for MUL.");
    }
    const bool runtime_tensor0 = !constant_tensor0;
    const bool runtime_tensor1 = !constant_tensor1;

    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::MUL);

    // The "larger" input tensor must be bound to 1st input and the "smaller"
    // input tensor ("mask") must be bound to 2nd input.
    if (runtime_tensor0 && runtime_tensor1) {
      BHWC shape0;
      RETURN_IF_ERROR(ExtractTensorShape(*input0, &shape0));
      BHWC shape1;
      RETURN_IF_ERROR(ExtractTensorShape(*input1, &shape1));
      int input_tensor0 = 0;
      int input_tensor1 = 1;
      if (shape0.h <= shape1.h && shape0.w <= shape1.w &&
          shape0.c == shape1.c) {
        input_tensor0 = 1;
        input_tensor1 = 0;
      }
      return ParseApplyMask(node, input_tensor0, input_tensor1, graph, reader);
    }

    // The runtime input tensor must be bound to 1st input and the constant
    // input tensor must be bound to 2nd input.
    int runtime_tensor = 0;
    int constant_tensor = 1;
    TfLiteIntArray* constant_dims = input1->dims;
    if (constant_tensor0 && runtime_tensor1) {
      runtime_tensor = 1;
      constant_tensor = 0;
      constant_dims = input0->dims;
    }
    return ParseMultiplyScalar(node, runtime_tensor, constant_tensor,
                               constant_dims, graph, reader);
  }

 private:
  Status ParseApplyMask(Node* node, int input_tensor0, int input_tensor1,
                        GraphFloat32* graph, ObjectReader* reader) {
    RETURN_IF_ERROR(reader->AddInput(node, input_tensor0));
    RETURN_IF_ERROR(reader->AddInput(node, input_tensor1));
    return reader->AddOutputs(node);
  }

  Status ParseMultiplyScalar(Node* node, int runtime_tensor,
                             int constant_tensor,
                             const TfLiteIntArray* constant_dims,
                             GraphFloat32* graph, ObjectReader* reader) {
    RETURN_IF_ERROR(reader->AddInput(node, runtime_tensor));
    MultiplyAttributes attr;
    if (constant_dims->size <= 0) {
      Tensor<Scalar, DataType::FLOAT32> tensor;
      RETURN_IF_ERROR(reader->ReadTensor(constant_tensor, &tensor));
      attr.param = tensor.data[0];
    } else {
      Tensor<Linear, DataType::FLOAT32> tensor;
      RETURN_IF_ERROR(reader->ReadTensor(constant_tensor, &tensor));
      attr.param = std::move(tensor);
    }
    node->operation.attributes = std::move(attr);
    return reader->AddOutputs(node);
  }
};

class PReLUOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    // TODO(eignasheva): add params check
    return OkStatus();
  }
  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::PRELU);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    auto input_shape = graph->FindInputs(node->id)[0]->tensor.shape;

    PReLUAttributes attr;
    Tensor<Linear, DataType::FLOAT32> linear_alpha;
    Status status = reader->ReadTensor(1, &linear_alpha);
    if (status.ok()) {
      if (linear_alpha.shape.v != input_shape.c) {
        return InvalidArgumentError(
            "Linear alpha shape does not match the number of input channels.");
      }
      attr.alpha = std::move(linear_alpha);
    } else {
      Tensor<HWC, DataType::FLOAT32> hwc_alpha;
      RETURN_IF_ERROR(reader->ReadTensor(1, &hwc_alpha));
      if (hwc_alpha.shape.h != input_shape.h ||
          hwc_alpha.shape.w != input_shape.w ||
          hwc_alpha.shape.c != input_shape.c) {
        return InvalidArgumentError("Alpha shape does not match input shape.");
      }
      attr.alpha = std::move(hwc_alpha);
    }
    node->operation.attributes = std::move(attr);
    return reader->AddOutputs(node);
  }
};

class PadOperationParser : public TFLiteOperationParser {
 public:
  explicit PadOperationParser(bool mirror_pad) : mirror_pad_(mirror_pad) {}

  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    if (mirror_pad_) {
      auto* tf_options = reinterpret_cast<const TfLiteMirrorPaddingParams*>(
          tflite_node->builtin_data);
      if (tf_options->mode !=
          TfLiteMirrorPaddingMode::kTfLiteMirrorPaddingReflect) {
        return InvalidArgumentError(
            "Only Reflective padding is supported for Mirror Pad operation.");
      }
    }
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    RETURN_IF_ERROR(CheckTensorIsAvailable(context, tflite_node, 1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::PAD);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    PadAttributes attr;
    if (mirror_pad_) {
      attr.type = PaddingContentType::REFLECT;
    } else /*zero pad*/ {
      attr.type = PaddingContentType::ZEROS;
    }

    Tensor<HW, DataType::INT32> paddings;
    RETURN_IF_ERROR(reader->ReadTensor(1, &paddings));

    // 4x2 tensor with paddings.
    if (paddings.shape.h != 4 || paddings.shape.w != 2) {
      return InvalidArgumentError("Paddings tensor has unexpected shape.");
    }
    attr.prepended = BHWC(paddings.data[0], paddings.data[2], paddings.data[4],
                          paddings.data[6]);
    attr.appended = BHWC(paddings.data[1], paddings.data[3], paddings.data[5],
                         paddings.data[7]);
    node->operation.attributes = attr;
    return OkStatus();
  }

 private:
  bool mirror_pad_ = false;
};

class Pooling2DOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    TfLitePoolParams* tf_options = nullptr;
    auto status = RetrieveCustomInitialData(tflite_node, &tf_options);
    if (status.ok()) {  // custom case with indices as a second output
      RETURN_IF_ERROR(CheckInputsOutputs(context, tflite_node, /*inputs=*/1,
                                         /*outputs=*/2));
    } else {  // common pooling with 1 output
      RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
      RETURN_IF_ERROR(CheckInputsOutputs(context, tflite_node, /*inputs=*/1,
                                         /*outputs=*/1));
    }
    RETURN_IF_ERROR(CheckKernelsAndStrides(
        tf_options->filter_height, tf_options->filter_width,
        tf_options->stride_height, tf_options->stride_width));
    return IsActivationSupported(tf_options->activation);
  }

 public:
  explicit Pooling2DOperationParser(PoolingType type) : type_(type) {}

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::POOLING_2D);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutput(node, 0));

    Pooling2DAttributes attr;
    attr.type = type_;

    auto input_shape = graph->FindInputs(node->id)[0]->tensor.shape;

    // check whether there are custom options encoded. It happens if operation
    // is MaxPoolingWithArgmax2D. There is no way to read
    // tflite_node->builtin_code, so, simply check whether custom data is
    // available.
    auto* tf_options = reinterpret_cast<const TfLitePoolParams*>(
        tflite_node->custom_initial_data);
    if (!tf_options) {
      tf_options =
          reinterpret_cast<const TfLitePoolParams*>(tflite_node->builtin_data);
    }
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }

    std::vector<uint32_t> max_tensor_id{0};
    RETURN_IF_ERROR(MaybeFuseActivation(tf_options->activation, max_tensor_id,
                                        graph, node));
    // Second output is optional. It is not required, it but must be added after
    // MaybeAddFusedActivation function is called
    reader->AddOutput(node, 1).IgnoreError();

    // First output is the result of pooling operation, while second output is
    // indices used for pooling.
    auto outputs = graph->FindOutputs(node->id);
    attr.output_indices = outputs.size() == 2;
    if (attr.output_indices) {
      // Fix data type for output indices. In the model it is set as float32.
      outputs[1]->tensor.type = DataType::INT32;
    }
    RETURN_IF_ERROR(ParsePoolingAttributes(tf_options, input_shape, &attr));
    node->operation.attributes = attr;
    return OkStatus();
  }

 private:
  const PoolingType type_;
};

class ReLUOperationParser : public TFLiteOperationParser {
 public:
  explicit ReLUOperationParser(int clip) : clip_(clip) {}

  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::RELU);
    RETURN_IF_ERROR(reader->AddInput(node, 0));

    ReLUAttributes attr;
    TfLiteLeakyReluParams* tf_options = nullptr;
    RetrieveBuiltinData(tflite_node, &tf_options).IgnoreError();
    attr.alpha = tf_options ? tf_options->alpha : 0;
    attr.clip = clip_;
    node->operation.attributes = attr;
    return reader->AddOutputs(node);
  }

 private:
  const int clip_;
};

class ReshapeOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    // TODO(eignasheva): add shape checking
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::RESHAPE);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));
    // Here we may have extra inputs. Other tensors were supposed to
    // define new shape, but in TFLite these are ignored.
    // TODO(akulik): check that shapes match?

    // New shape comes from output shape.
    ReshapeAttributes attr;
    attr.new_shape = graph->FindOutputs(node->id)[0]->tensor.shape;
    node->operation.attributes = attr;
    return OkStatus();
  }
};

class Resize2DOperationParser : public TFLiteOperationParser {
 public:
  explicit Resize2DOperationParser(SamplingType sampling_type)
      : sampling_type_(sampling_type) {}

  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 3));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));

    RETURN_IF_ERROR(CheckOnlyUpsamplingIsSupported(context, tflite_node));
    bool align_corners;
    RETURN_IF_ERROR(GetAlignCornersValue(tflite_node, &align_corners));
    bool half_pixel_centers;
    RETURN_IF_ERROR(GetHalfPixelCentersValue(tflite_node, &half_pixel_centers));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::RESIZE);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));
    // Here we may have extra inputs. Other tensors were supposed to
    // define new shape, but in TFLite these are ignored.

    Resize2DAttributes attr;
    RETURN_IF_ERROR(GetAlignCornersValue(tflite_node, &attr.align_corners));
    RETURN_IF_ERROR(
        GetHalfPixelCentersValue(tflite_node, &attr.half_pixel_centers));
    attr.type = sampling_type_;
    attr.new_shape.CopyAllDefinedAxis(
        graph->FindOutputs(node->id)[0]->tensor.shape);
    node->operation.attributes = attr;
    return OkStatus();
  }

 private:
  Status GetAlignCornersValue(const TfLiteNode* tflite_node,
                              bool* align_corners) {
    switch (sampling_type_) {
      case SamplingType::BILINEAR:
        return GetAlignCornersValueForType<TfLiteResizeBilinearParams>(
            tflite_node, align_corners);
      case SamplingType::NEAREST:
        return GetAlignCornersValueForType<TfLiteResizeNearestNeighborParams>(
            tflite_node, align_corners);
      case SamplingType::UNKNOWN:
        return InternalError("Sampling type is not specified");
    }
    return OkStatus();
  }

  template <class T>
  Status GetAlignCornersValueForType(const TfLiteNode* tflite_node,
                                     bool* align_corners) {
    const auto* tf_options =
        reinterpret_cast<const T*>(tflite_node->builtin_data);
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    *align_corners = tf_options->align_corners;
    return OkStatus();
  }

  Status GetHalfPixelCentersValue(const TfLiteNode* tflite_node,
                                  bool* half_pixel_centers) {
    if (sampling_type_ == SamplingType::BILINEAR) {
      const auto* tf_options = reinterpret_cast<TfLiteResizeBilinearParams*>(
          tflite_node->builtin_data);
      if (!tf_options) {
        return InternalError("Missing tflite params for ResizeBilinear op");
      }
      if (tf_options->align_corners && tf_options->half_pixel_centers) {
        return InternalError(
            "If half_pixel_centers is True, align_corners must be False.");
      }
      *half_pixel_centers = tf_options->half_pixel_centers;
    } else {
      *half_pixel_centers = false;
    }
    return OkStatus();
  }

  Status CheckOnlyUpsamplingIsSupported(const TfLiteContext* context,
                                        const TfLiteNode* tflite_node) {
    const auto* input = context->tensors + tflite_node->inputs->data[0];
    const auto* output = context->tensors + tflite_node->outputs->data[0];

    if (!input->dims || input->dims->size != 4) {
      return InvalidArgumentError("input.dims.size != 4");
    }
    if (!output->dims || output->dims->size != 4) {
      return InvalidArgumentError("output.dims.size != 4");
    }
    if (output->dims->data[1] < input->dims->data[1] ||
        output->dims->data[2] < input->dims->data[2]) {
      return InvalidArgumentError(absl::StrCat(
          "Only upsampling is supported, received output h,w = ",
          output->dims->data[1], ",", output->dims->data[2],
          " input h,w = ", input->dims->data[1], ",", input->dims->data[2]));
    }
    return OkStatus();
  }

  SamplingType sampling_type_ = SamplingType::UNKNOWN;
};

class SliceOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::SLICE);
    RETURN_IF_ERROR(reader->AddOutputs(node));
    Value<TensorRef<BHWC>>* input;
    RETURN_IF_ERROR(reader->ReadValue(0, &input));
    RETURN_IF_ERROR(graph->AddConsumer(node->id, input->id));

    SliceAttributes attr;
    attr.strides = BHWC(1, 1, 1, 1);
    Tensor<Linear, DataType::INT32> starts, sizes;
    RETURN_IF_ERROR(reader->ReadTensor(1, &starts));
    RETURN_IF_ERROR(reader->ReadTensor(2, &sizes));
    if (starts.data.size() != sizes.data.size()) {
      return InvalidArgumentError("Starts amount != sizes amount.");
    }
    if (starts.data.size() == 4) {
      attr.starts =
          BHWC(starts.data[0], starts.data[1], starts.data[2], starts.data[3]);
      attr.ends =
          BHWC(starts.data[0] + sizes.data[0], starts.data[1] + sizes.data[1],
               starts.data[2] + sizes.data[2], starts.data[3] + sizes.data[3]);
    } else if (starts.data.size() == 3) {
      attr.starts = BHWC(0, starts.data[0], starts.data[1], starts.data[2]);
      attr.ends =
          BHWC(input->tensor.shape.b, starts.data[0] + sizes.data[0],
               starts.data[1] + sizes.data[1], starts.data[2] + sizes.data[2]);
    } else {
      return UnimplementedError(
          "Slicing is supported for 3 or 4 dimensional tensors only.");
    }
    RETURN_IF_ERROR(UpdateIfNegative(input->tensor.shape, &attr));

    auto out_shape = graph->FindOutputs(node->id)[0]->tensor.shape;
    if ((attr.ends.b - attr.starts.b) != out_shape.b) {
      return UnimplementedError("Output batch don't match");
    }
    if ((attr.ends.h - attr.starts.h) != out_shape.h) {
      return UnimplementedError("Output height doesn't match");
    }
    if ((attr.ends.w - attr.starts.w) != out_shape.w) {
      return UnimplementedError("Output width doesn't match");
    }
    if ((attr.ends.c - attr.starts.c) != out_shape.c) {
      return UnimplementedError("Output channels don't match");
    }
    node->operation.attributes = attr;
    return OkStatus();
  }

 private:
  Status UpdateIfNegative(const BHWC& input_shape, SliceAttributes* attr) {
    if (attr->ends.h < 0) {
      attr->ends.h = input_shape.h + attr->ends.h;
    }
    if (attr->ends.w < 0) {
      attr->ends.w = input_shape.w + attr->ends.w;
    }
    if (attr->ends.c < 0) {
      attr->ends.c = input_shape.c + attr->ends.c;
    }
    if (attr->ends.b < 0) {
      attr->ends.b = input_shape.b + attr->ends.b;
    }
    return OkStatus();
  }
};

class SoftmaxOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    TfLiteSoftmaxParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    if (tf_options->beta != 1) {
      // TODO(eignasheva): figure out, what's wrong with softmax.
      return UnimplementedError("Softmax.beta != 1 is not supported.");
    }
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::SOFTMAX);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    const auto* tf_options =
        reinterpret_cast<const TfLiteSoftmaxParams*>(tflite_node->builtin_data);
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    if (tf_options->beta != 1) {
      // there is multiply by scalar operation fused in softmax. Make a layer
      // out of it before softmax.
      return UnimplementedError("Softmax.beta != 1 is not supported.");
      // auto mul_node = reader->NewPassthroughNode(node);
      // mul_node->operation.type = ToString(OperationType::MUL);
    }
    SoftmaxAttributes attr;
    attr.axis = Axis::CHANNELS;  // always by channels
    node->operation.attributes = attr;
    return OkStatus();
  }
};

class SpaceToDepthOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    // TODO(impjdi): Dims check.
    TfLiteSpaceToDepthParams* s2d_params = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &s2d_params));
    if (s2d_params->block_size == 1) {
      return InvalidArgumentError("SPACE_TO_DEPTH block_size = 1 is a no-op.");
    }
    if (s2d_params->block_size < 1) {
      return InvalidArgumentError("SPACE_TO_DEPTH block_size must be > 1.");
    }
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::SPACE_TO_DEPTH);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));
    const auto* tf_options = reinterpret_cast<const TfLiteSpaceToDepthParams*>(
        tflite_node->builtin_data);
    SpaceToDepthAttributes attr;
    attr.block_size = tf_options->block_size;
    node->operation.attributes = attr;
    return OkStatus();
  }
};

class StridedSliceOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    TfLiteStridedSliceParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    RETURN_IF_ERROR(CheckOptionsSupport(tf_options));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::SLICE);
    RETURN_IF_ERROR(reader->AddOutputs(node));
    Value<TensorRef<BHWC>>* input;
    RETURN_IF_ERROR(reader->ReadValue(0, &input));
    RETURN_IF_ERROR(graph->AddConsumer(node->id, input->id));

    Tensor<Linear, DataType::INT32> tmp;
    RETURN_IF_ERROR(reader->ReadTensor(1, &tmp));

    bool read_without_batch = tmp.data.size() == 3;
    bool read_with_batch = tmp.data.size() == 4;
    if (!read_without_batch && !read_with_batch) {
      return UnimplementedError(
          "Slicing is supported for 3 or 4 dimensional tensors only.");
    }

    const auto* tf_options = reinterpret_cast<const TfLiteStridedSliceParams*>(
        tflite_node->builtin_data);
    auto out_shape = graph->FindOutputs(node->id)[0]->tensor.shape;
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    RETURN_IF_ERROR(CheckOptionsSupport(tf_options));

    SliceAttributes attr;
    if (read_without_batch) {
      RETURN_IF_ERROR(ReadAttribsWithoutBatch(reader, tf_options,
                                              input->tensor.shape, &attr));
    }
    if (read_with_batch) {
      RETURN_IF_ERROR(
          ReadAttribsWithBatch(reader, tf_options, input->tensor.shape, &attr));
    }
    if (attr.strides.b == 0 || attr.strides.h == 0 || attr.strides.w == 0 ||
        attr.strides.c == 0) {
      return InvalidArgumentError("stride values must be non-zero");
    }
    if (attr.strides.b < 0 || attr.strides.h < 0 || attr.strides.w < 0 ||
        attr.strides.c < 0) {
      return UnimplementedError("Reverse slices are not supported.");
    }
    if ((attr.ends.b - attr.starts.b + attr.strides.b - 1) / attr.strides.b !=
        out_shape.b) {
      return UnimplementedError("Output batch don't match");
    }
    if ((attr.ends.h - attr.starts.h + attr.strides.h - 1) / attr.strides.h !=
        out_shape.h) {
      return UnimplementedError("Output height doesn't match");
    }
    if ((attr.ends.w - attr.starts.w + attr.strides.w - 1) / attr.strides.w !=
        out_shape.w) {
      return UnimplementedError("Output width doesn't match");
    }
    if ((attr.ends.c - attr.starts.c + attr.strides.c - 1) / attr.strides.c !=
        out_shape.c) {
      return UnimplementedError("Output channels don't match");
    }
    node->operation.attributes = attr;
    return OkStatus();
  }

 private:
  Status UpdateWithMask(const TfLiteStridedSliceParams* tf_options,
                        const BHWC& input_shape, int ignore_b, int ignore_h,
                        int ignore_w, int ignore_c, SliceAttributes* attr) {
    if (tf_options->begin_mask & ignore_h) {
      attr->starts.h = 0;
    }
    if (tf_options->begin_mask & ignore_w) {
      attr->starts.w = 0;
    }
    if (tf_options->begin_mask & ignore_c) {
      attr->starts.c = 0;
    }
    if (tf_options->begin_mask & ignore_b) {
      attr->starts.b = 0;
    }

    if (tf_options->end_mask & ignore_h) {
      attr->ends.h = input_shape.h;
    }
    if (tf_options->end_mask & ignore_w) {
      attr->ends.w = input_shape.w;
    }
    if (tf_options->end_mask & ignore_c) {
      attr->ends.c = input_shape.c;
    }
    if (tf_options->end_mask & ignore_b) {
      attr->ends.b = input_shape.b;
    }
    return OkStatus();
  }

  Status UpdateIfNegative(const BHWC& input_shape, SliceAttributes* attr) {
    if (attr->ends.h < 0) {
      attr->ends.h = input_shape.h + attr->ends.h;
    }
    if (attr->ends.w < 0) {
      attr->ends.w = input_shape.w + attr->ends.w;
    }
    if (attr->ends.c < 0) {
      attr->ends.c = input_shape.c + attr->ends.c;
    }
    if (attr->ends.b < 0) {
      attr->ends.b = input_shape.b + attr->ends.b;
    }
    return OkStatus();
  }

  Status ReadAttribsWithBatch(const ObjectReader* reader,
                              const TfLiteStridedSliceParams* tf_options,
                              const BHWC& input_shape, SliceAttributes* attr) {
    auto read_bhwc = [&](int tensor_index, BHWC* bhwc) -> Status {
      Tensor<Linear, DataType::INT32> t;
      RETURN_IF_ERROR(reader->ReadTensor(tensor_index, &t));
      *bhwc = BHWC(t.data[0], t.data[1], t.data[2], t.data[3]);
      return OkStatus();
    };

    RETURN_IF_ERROR(read_bhwc(1, &attr->starts));
    RETURN_IF_ERROR(read_bhwc(2, &attr->ends));
    RETURN_IF_ERROR(read_bhwc(3, &attr->strides));
    RETURN_IF_ERROR(UpdateIfNegative(input_shape, attr));
    RETURN_IF_ERROR(UpdateWithMask(tf_options, input_shape, 1, 2, 4, 8, attr));
    return OkStatus();
  }

  Status ReadAttribsWithoutBatch(const ObjectReader* reader,
                                 const TfLiteStridedSliceParams* tf_options,
                                 const BHWC& input_shape,
                                 SliceAttributes* attr) {
    auto read_hwc = [&](int tensor_index, BHWC* bhwc) -> Status {
      Tensor<Linear, DataType::INT32> t;
      RETURN_IF_ERROR(reader->ReadTensor(tensor_index, &t));
      *bhwc = BHWC(0, t.data[0], t.data[1], t.data[2]);
      return OkStatus();
    };

    RETURN_IF_ERROR(read_hwc(1, &attr->starts));
    RETURN_IF_ERROR(read_hwc(2, &attr->ends));
    RETURN_IF_ERROR(read_hwc(3, &attr->strides));
    RETURN_IF_ERROR(UpdateIfNegative(input_shape, attr));
    RETURN_IF_ERROR(UpdateWithMask(tf_options, input_shape, 0, 1, 2, 4, attr));
    attr->starts.b = 0;
    attr->ends.b = input_shape.b;
    attr->strides.b = 1;
    return OkStatus();
  }
  Status CheckOptionsSupport(const TfLiteStridedSliceParams* tf_options) {
    if (tf_options->ellipsis_mask) {
      return UnimplementedError("Slice does not support ellipsis_mask.");
    }
    if (tf_options->new_axis_mask) {
      return UnimplementedError("Slice does not support new_axis_mask.");
    }
    if (tf_options->shrink_axis_mask) {
      return UnimplementedError(
          "Slice does not support shrink_axis_mask parameter. ");
    }
    return OkStatus();
  }
};

class TransposeConvOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    RETURN_IF_ERROR(CheckTensorIsAvailable(context, tflite_node, 1));
    TfLiteTransposeConvParams* tf_options = nullptr;
    RETURN_IF_ERROR(RetrieveBuiltinData(tflite_node, &tf_options));
    RETURN_IF_ERROR(
        CheckStrides(tf_options->stride_height, tf_options->stride_width));
    return OkStatus();
  }

  // TFLite's TRANSPOSE_CONV expects 3 input (output shape, weights, and input)
  // and allows configurable padding & stride.
  // TODO(impjdi): Translate output_shape to attr.adjacent.
  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    auto* node = graph->NewNode();
    node->operation.type = ToString(OperationType::CONVOLUTION_TRANSPOSED);
    Value<TensorRef<BHWC>>* input;
    RETURN_IF_ERROR(reader->ReadValue(2, &input));
    RETURN_IF_ERROR(graph->AddConsumer(node->id, input->id));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    const auto* tf_options = reinterpret_cast<const TfLiteTransposeConvParams*>(
        tflite_node->builtin_data);
    if (!tf_options) {
      return InternalError("Missing tflite options.");
    }
    ConvolutionTransposedAttributes attr;
    attr.stride = tf_options
                      ? HW(tf_options->stride_height, tf_options->stride_width)
                      : HW(1, 1);
    RETURN_IF_ERROR(reader->ReadTensor(1, &attr.weights));

    // TFLite does not support bias.

    UpdatePadding(tf_options->padding,
                  graph->FindInputs(node->id)[0]->tensor.shape, &attr);
    node->operation.attributes = std::move(attr);
    return OkStatus();
  }
};

class TransposeOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(CheckMaxSupportedOpVersion(registration, 1));
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::TRANSPOSE);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    TransposeAttributes attr;
    Tensor<Linear, DataType::INT32> perm;
    RETURN_IF_ERROR(reader->ReadTensor(1, &perm));
    if (perm.data.size() == 4) {
      attr.perm = BHWC(perm.data[0], perm.data[1], perm.data[2], perm.data[3]);
    } else if (perm.data.size() == 3) {
      attr.perm = BHWC(0, perm.data[0] + 1, perm.data[1] + 1, perm.data[2] + 1);
    } else if (perm.data.size() == 2) {
      attr.perm = BHWC(0, 1, perm.data[0] + 2, perm.data[1] + 2);
    } else {
      return InvalidArgumentError("Permutation for transpose is invalid.");
    }

    node->operation.attributes = attr;
    return OkStatus();
  }
};

class Unpooling2DOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    TfLitePoolParams* tf_options = nullptr;
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/2, /*outputs=*/1));
    RETURN_IF_ERROR(RetrieveCustomInitialData(tflite_node, &tf_options));
    RETURN_IF_ERROR(CheckKernelsAndStrides(
        tf_options->filter_height, tf_options->filter_width,
        tf_options->stride_height, tf_options->stride_width));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    node->operation.type = ToString(OperationType::MAX_UNPOOLING_2D);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddInput(node, 1));
    RETURN_IF_ERROR(reader->AddOutputs(node));
    auto input_shape = graph->FindInputs(node->id)[0]->tensor.shape;
    MaxUnpooling2DAttributes attr;
    const auto* tf_options = reinterpret_cast<const TfLitePoolParams*>(
        tflite_node->custom_initial_data);
    if (!tf_options) {
      return InternalError("Missing tflite params");
    }
    attr.kernel = ToHW(tf_options->filter_height, tf_options->filter_width);
    attr.strides = ToHW(tf_options->stride_height, tf_options->stride_width);
    UpdatePadding(tf_options->padding, input_shape, &attr);

    node->operation.attributes = attr;

    auto output_value = graph->FindOutputs(node->id)[0];
    output_value->tensor.shape = CalculateOutputShape(input_shape, attr);
    return OkStatus();
  }
};

// TODO(impjdi): BATCH_TO_SPACE/SPACE_TO_BATCH shouldn't be supported.
class BatchToSpaceOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    auto* node = graph->NewNode();
    node->operation.type = ToString(OperationType::BATCH_TO_SPACE);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    BatchToSpaceAttributes bs_attr;
    Tensor<Linear, DataType::INT32> block;
    RETURN_IF_ERROR(reader->ReadTensor(1, &block));
    if (block.shape.v != 2) {
      return InternalError("Space has to be HxW.");
    }
    bs_attr.block.h = block.data[0];
    bs_attr.block.w = block.data[1];

    Tensor<HW, DataType::INT32> crop;
    RETURN_IF_ERROR(reader->ReadTensor(2, &crop));
    auto crop_shape = crop.shape;
    if (crop_shape.h != 2 && crop_shape.w != 2) {
      return InternalError("Space has to be HxW.");
    }

    bs_attr.crop.prepended.h = crop.data[0];
    bs_attr.crop.prepended.w = crop.data[2];

    bs_attr.crop.appended.h = crop.data[1];
    bs_attr.crop.appended.w = crop.data[3];

    node->operation.attributes = std::move(bs_attr);
    return OkStatus();
  }
};

class SpaceToBatchOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    auto* node = graph->NewNode();
    node->operation.type = ToString(OperationType::SPACE_TO_BATCH);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));
    SpaceToBatchAttributes sb_attr;
    Tensor<Linear, DataType::INT32> block;
    RETURN_IF_ERROR(reader->ReadTensor(1, &block));
    if (block.shape.v != 2) {
      return InternalError("Space has to be HxW.");
    }
    sb_attr.block.h = block.data[0];
    sb_attr.block.w = block.data[1];

    Tensor<HW, DataType::INT32> padding;
    RETURN_IF_ERROR(reader->ReadTensor(2, &padding));
    auto padding_shape = padding.shape;

    if (padding_shape.h != 2 && padding_shape.w != 2) {
      return InternalError("Space has to be HxW.");
    }

    sb_attr.padding.prepended.h = padding.data[0];
    sb_attr.padding.prepended.w = padding.data[2];

    sb_attr.padding.appended.h = padding.data[1];
    sb_attr.padding.appended.w = padding.data[3];

    node->operation.attributes = std::move(sb_attr);
    return OkStatus();
  }
};

class RoIToTransformMatrixOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/1, /*outputs=*/1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    RETURN_IF_ERROR(reader->AddInput(node, 0));  // bbox
    RETURN_IF_ERROR(reader->AddOutputs(node));

    std::string op_name = "roi_to_transform_matrix";
    node->operation.type = op_name;
    BHWC output_shape;
    RETURN_IF_ERROR(
        ParseCustomAttributes(op_name, tflite_node->custom_initial_data,
                              tflite_node->custom_initial_data_size,
                              &(node->operation.attributes), &output_shape));

    auto output_value = graph->FindOutputs(node->id)[0];
    output_value->tensor.shape = output_shape;
    return OkStatus();
  }

 private:
};

class TransformTensorOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/2, /*outputs=*/1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    RETURN_IF_ERROR(reader->AddInput(node, 0));  // data
    RETURN_IF_ERROR(reader->AddInput(node, 1));  // bbox
    RETURN_IF_ERROR(reader->AddOutputs(node));

    std::string op_name = "transform_tensor";
    node->operation.type = op_name;
    BHWC output_shape;
    RETURN_IF_ERROR(
        ParseCustomAttributes(op_name, tflite_node->custom_initial_data,
                              tflite_node->custom_initial_data_size,
                              &(node->operation.attributes), &output_shape));

    auto output_value = graph->FindOutputs(node->id)[0];

    output_value->tensor.shape =
        BHWC(1, output_shape.h, output_shape.w,
             graph->FindInputs(node->id)[0]->tensor.shape.c);
    return OkStatus();
  }

 private:
};

class TransformLandmarksOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    RETURN_IF_ERROR(
        CheckInputsOutputs(context, tflite_node, /*inputs=*/2, /*outputs=*/1));
    return OkStatus();
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    RETURN_IF_ERROR(reader->AddInput(node, 0));  // data
    RETURN_IF_ERROR(reader->AddInput(node, 1));  // bbox
    RETURN_IF_ERROR(reader->AddOutputs(node));
    std::string op_name = "transform_landmarks";
    node->operation.type = op_name;
    BHWC output_shape;
    RETURN_IF_ERROR(
        ParseCustomAttributes(op_name, tflite_node->custom_initial_data,
                              tflite_node->custom_initial_data_size,
                              &(node->operation.attributes), &output_shape));

    auto output_value = graph->FindOutputs(node->id)[0];

    output_value->tensor.shape = graph->FindInputs(node->id)[0]->tensor.shape;
    return OkStatus();
  }

 private:
};

class Landmarks2TransformMatrixOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    return CheckInputsOutputs(context, tflite_node, /*inputs=*/1,
                              /*outputs=*/1);
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    Node* node = graph->NewNode();
    RETURN_IF_ERROR(reader->AddInput(node, 0));  // landmarks
    RETURN_IF_ERROR(reader->AddOutputs(node));   // transform matrix

    const std::string op_name = "landmarks_to_transform_matrix";
    node->operation.type = op_name;
    BHWC output_shape;
    RETURN_IF_ERROR(
        ParseCustomAttributes(op_name, tflite_node->custom_initial_data,
                              tflite_node->custom_initial_data_size,
                              &(node->operation.attributes), &output_shape));

    auto output_value = graph->FindOutputs(node->id)[0];
    output_value->tensor.shape = output_shape;
    return OkStatus();
  }

 private:
};

class MeanOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    return CheckInputsOutputs(context, tflite_node, /*inputs=*/1,
                              /*outputs=*/1);
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    auto* node = graph->NewNode();
    node->operation.type = ToString(OperationType::MEAN);
    RETURN_IF_ERROR(reader->AddInput(node, 0));
    RETURN_IF_ERROR(reader->AddOutputs(node));

    MeanAttributes attr;
    Tensor<Linear, DataType::INT32> channel;
    RETURN_IF_ERROR(reader->ReadTensor(1, &channel));
    for (int i = 0; i < channel.data.size(); i++) {
      std::string unsupported;
      switch (channel.data[i]) {
        case 1:
          attr.dims.insert(Axis::HEIGHT);
          break;
        case 2:
          attr.dims.insert(Axis::WIDTH);
          break;
        case 0:
          unsupported = unsupported.empty() ? "batch" : unsupported;
          ABSL_FALLTHROUGH_INTENDED;
        case 3:
          unsupported = unsupported.empty() ? "channels" : unsupported;
          ABSL_FALLTHROUGH_INTENDED;
        default:
          return UnimplementedError(
              absl::StrCat("Unsupported mean dimension: ", unsupported));
      }
    }
    node->operation.attributes = attr;
    return OkStatus();
  }
};

class UnsupportedOperationParser : public TFLiteOperationParser {
 public:
  Status IsSupported(const TfLiteContext* context,
                     const TfLiteNode* tflite_node,
                     const TfLiteRegistration* registration) final {
    return UnimplementedError("Operation is not supported.");
  }

  Status Parse(const TfLiteNode* tflite_node,
               const TfLiteRegistration* registration, GraphFloat32* graph,
               ObjectReader* reader) final {
    return UnimplementedError("Operation is not supported.");
  }
};

std::unique_ptr<TFLiteOperationParser> NewOperationParser(
    const TfLiteRegistration* registration) {
  const auto builtin_code = registration->builtin_code;
  switch (builtin_code) {
    case kTfLiteBuiltinAbs:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::ABS);
    case kTfLiteBuiltinAdd:
      return absl::make_unique<AddOperationParser>();
    case kTfLiteBuiltinAveragePool2d:
      return absl::make_unique<Pooling2DOperationParser>(PoolingType::AVERAGE);
    case kTfLiteBuiltinConcatenation:
      return absl::make_unique<ConcatenationOperationParser>();
    case kTfLiteBuiltinConv2d:
      return absl::make_unique<Conv2DOperationParser>();
    case kTfLiteBuiltinCos:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::COS);
    case kTfLiteBuiltinDepthwiseConv2d:
      return absl::make_unique<DepthwiseConvolutionOperationParser>();
    case kTfLiteBuiltinDiv:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::DIV);
    case kTfLiteBuiltinFullyConnected:
      return absl::make_unique<FullyConnectedOperationParser>();
    case kTfLiteBuiltinHardSwish:
      return absl::make_unique<HardSwishOperationParser>();
    case kTfLiteBuiltinLogistic:
      return absl::make_unique<ElementwiseOperationParser>(
          OperationType::SIGMOID);
    case kTfLiteBuiltinLog:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::LOG);
    case kTfLiteBuiltinLstm:
      return absl::make_unique<LSTMOperationParser>();
    case kTfLiteBuiltinMaximum:
      return absl::make_unique<ElementwiseOperationParser>(
          OperationType::MAXIMUM);
    case kTfLiteBuiltinMaxPool2d:
      return absl::make_unique<Pooling2DOperationParser>(PoolingType::MAX);
    case kTfLiteBuiltinMean:
      return absl::make_unique<MeanOperationParser>();
    case kTfLiteBuiltinMinimum:
      return absl::make_unique<ElementwiseOperationParser>(
          OperationType::MINIMUM);
    case kTfLiteBuiltinMirrorPad:
      return absl::make_unique<PadOperationParser>(/*mirror_pad=*/true);
    case kTfLiteBuiltinMul:
      return absl::make_unique<MulOperationParser>();
    case kTfLiteBuiltinPad:
      return absl::make_unique<PadOperationParser>(/*mirror_pad=*/false);
    case kTfLiteBuiltinPow:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::POW);
    case kTfLiteBuiltinRelu:
      return absl::make_unique<ReLUOperationParser>(0);
    case kTfLiteBuiltinRelu6:
      return absl::make_unique<ReLUOperationParser>(6);
    case kTfLiteBuiltinLeakyRelu:
      return absl::make_unique<ReLUOperationParser>(0);
    case kTfLiteBuiltinPrelu:
      return absl::make_unique<PReLUOperationParser>();
    case kTfLiteBuiltinReshape:
      return absl::make_unique<ReshapeOperationParser>();
    case kTfLiteBuiltinResizeBilinear:
      return absl::make_unique<Resize2DOperationParser>(SamplingType::BILINEAR);
    case kTfLiteBuiltinResizeNearestNeighbor:
      return absl::make_unique<Resize2DOperationParser>(SamplingType::NEAREST);
    case kTfLiteBuiltinRsqrt:
      return absl::make_unique<ElementwiseOperationParser>(
          OperationType::RSQRT);
    case kTfLiteBuiltinSin:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::SIN);
    case kTfLiteBuiltinSlice:
      return absl::make_unique<SliceOperationParser>();
    case kTfLiteBuiltinSoftmax:
      return absl::make_unique<SoftmaxOperationParser>();
    case kTfLiteBuiltinSpaceToDepth:
      return absl::make_unique<SpaceToDepthOperationParser>();
    case kTfLiteBuiltinSqrt:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::SQRT);
    case kTfLiteBuiltinSquare:
      return absl::make_unique<ElementwiseOperationParser>(
          OperationType::SQUARE);
    case kTfLiteBuiltinSquaredDifference:
      return absl::make_unique<ElementwiseOperationParser>(
          OperationType::SQUARED_DIFF);
    case kTfLiteBuiltinStridedSlice:
      return absl::make_unique<StridedSliceOperationParser>();
    case kTfLiteBuiltinSub:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::SUB);
    case kTfLiteBuiltinTanh:
      return absl::make_unique<ElementwiseOperationParser>(OperationType::TANH);
    case kTfLiteBuiltinTranspose:
      return absl::make_unique<TransposeOperationParser>();
    case kTfLiteBuiltinTransposeConv:
      return absl::make_unique<TransposeConvOperationParser>();

    case kTfLiteBuiltinCustom:
      const absl::string_view custom_name = registration->custom_name;
      if (custom_name == "Convolution2DTransposeBias") {
        return absl::make_unique<Convolution2DTransposeBiasParser>();
      }
      if (custom_name == "MaxPoolingWithArgmax2D") {
        return absl::make_unique<Pooling2DOperationParser>(PoolingType::MAX);
      }
      if (custom_name == "MaxUnpooling2D") {
        return absl::make_unique<Unpooling2DOperationParser>();
      }
      if (custom_name == "RoIToTransformMatrix") {
        return absl::make_unique<RoIToTransformMatrixOperationParser>();
      }

      if (custom_name == "TransformTensor") {
        return absl::make_unique<TransformTensorOperationParser>();
      }

      if (custom_name == "TransformLandmarks") {
        return absl::make_unique<TransformLandmarksOperationParser>();
      }

      if (custom_name == "Landmarks2TransformMatrix") {
        return absl::make_unique<Landmarks2TransformMatrixOperationParser>();
      }

      break;
  }
  return absl::make_unique<UnsupportedOperationParser>();
}

Status GetNodeAndRegistration(TfLiteContext* context, int node_id,
                              TfLiteNode** tflite_node,
                              TfLiteRegistration** registration) {
  if (context->GetNodeAndRegistration(context, node_id, tflite_node,
                                      registration) != kTfLiteOk) {
    return InvalidArgumentError(absl::StrCat(
        "Couldn't get node and registration info for op: ", node_id));
  }
  return OkStatus();
}

using IsNodeSupportedFn =
    std::function<Status(TfLiteContext*, TfLiteNode*, TfLiteRegistration*)>;

// A utility class to help model graph parition and decide the partition to be
// offloaded to GPU.
// TODO(b/151152967): move the following to lite/delegates/utils
class GraphPartitionHelper {
 public:
  GraphPartitionHelper(TfLiteContext* context,
                       IsNodeSupportedFn is_node_supported_fn)
      : is_node_supported_fn_(is_node_supported_fn), context_(context) {}

  virtual ~GraphPartitionHelper() { TfLiteIntArrayFree(supported_nodes_); }

  // Partitions the graph into multiple subgraphs, each of which is in
  // dependency order with others
  virtual Status Partition(std::set<std::string>* unsupported_nodes_info) {
    RETURN_IF_ERROR(PrepareSupportedNodes(unsupported_nodes_info));

    TfLiteDelegateParams* partition_params_array_ = nullptr;
    int num_partitions_ = 0;
    if (context_->PreviewDelegatePartitioning(context_, supported_nodes_,
                                              &partition_params_array_,
                                              &num_partitions_) != kTfLiteOk) {
      return InvalidArgumentError("Unable to preview delegate partition.");
    }

    for (int i = 0; i < num_partitions_; ++i) {
      partitions_.push_back(partition_params_array_ + i);
    }

    return OkStatus();
  }

  // Returns the first n largest partitions or all if #partitions is less than
  // 'n'. Note that partitions are ranked according to the number of nodes that
  // a partition has, and the returned TfLiteDelegateParams objects are *owned*
  // by the TfLite runtime.
  std::vector<TfLiteDelegateParams*> GetFirstNLargestPartitions(int n) {
    const int total = num_partitions();
    // We only sort partitions according to their sizes if necessary.
    if (n < total) {
      partitions_.sort(CompareTwoPartitions);
    }
    std::vector<TfLiteDelegateParams*> results;
    auto p_it = partitions_.begin();
    for (int i = 0; i < std::min(total, n); ++i, ++p_it) {
      results.push_back(*p_it);
    }
    return results;
  }

  int num_total_nodes() const { return num_total_nodes_; }
  int num_partitions() const { return partitions_.size(); }

 private:
  static bool CompareTwoPartitions(TfLiteDelegateParams* left,
                                   TfLiteDelegateParams* right) {
    // Reverse sort
    return left->nodes_to_replace->size > right->nodes_to_replace->size;
  }

  Status PrepareSupportedNodes(
      std::set<std::string>* unsupported_nodes_info = nullptr) {
    TfLiteIntArray* execution_plan = nullptr;
    if (context_->GetExecutionPlan(context_, &execution_plan) != kTfLiteOk) {
      return InvalidArgumentError("Unable to get graph execution plan.");
    }

    num_total_nodes_ = execution_plan->size;
    supported_nodes_ = TfLiteIntArrayCreate(num_total_nodes_);
    supported_nodes_->size = 0;
    for (int node_id : TfLiteIntArrayView(execution_plan)) {
      TfLiteNode* node;
      TfLiteRegistration* registration;
      auto status =
          GetNodeAndRegistration(context_, node_id, &node, &registration);
      if (!status.ok()) {
        supported_nodes_->size = 0;
        return status;
      }

      status = IsNodeSupported(context_, node, registration, node_id);
      if (status.ok()) {
        supported_nodes_->data[supported_nodes_->size++] = node_id;
      } else if (unsupported_nodes_info) {
        unsupported_nodes_info->insert(
            absl::StrCat(GetOpNameByRegistration(*registration), ": ",
                         status.error_message()));
      }
    }
    return OkStatus();
  }

  // The number of total nodes passed in for partition (i.e. the
  // execution_plan size)
  int num_total_nodes_ = 0;

  // Tells whether a node is replaceable.
  const IsNodeSupportedFn is_node_supported_fn_;
  TfLiteIntArray* supported_nodes_;  // owns the memory

 protected:
  virtual Status IsNodeSupported(TfLiteContext* context, TfLiteNode* node,
                                 TfLiteRegistration* registration,
                                 int node_id) {
    return is_node_supported_fn_(context, node, registration);
  }

  TfLiteContext* const context_ = nullptr;

  // Doesn't own the memory of each TfLiteDelegateParams object as it's
  // managed by the TfLite runtime itself. See
  // TfLiteContext::PreviewDelegatePartitioning for details.
  std::list<TfLiteDelegateParams*> partitions_;
};

class GraphWithDequantPartitionHelper : public GraphPartitionHelper {
 public:
  GraphWithDequantPartitionHelper(TfLiteContext* context,
                                  IsNodeSupportedFn is_node_supported_fn)
      : GraphPartitionHelper(context, std::move(is_node_supported_fn)) {}

  Status Partition(std::set<std::string>* unsupported_nodes_info) override {
    auto status = GraphPartitionHelper::Partition(unsupported_nodes_info);
    // Clean up those partitions that have a single dequant op. NoteThose
    // removed dequant ops have to be reserved in the graph and should not be
    // delegated.
    RemoveSingleDequantNodePartitions();
    return status;
  }

  // Returns a list of node indices of all nodes from the first n largest
  // partitions. If there are fewer paritions than n, all nodes will be
  // returned. The partition is ranked according to the number of nodes.
  std::vector<int> GetNodesOfFirstNLargestPartitions(int n) {
    // We first get partitions to reduce the number of nodes to be checked in
    // deciding which dequant ops could actually be replaced. And then we
    // remap input-tensor to dequant nodes' inputs and remove those
    // to-be-reserved dequant nodes.
    auto first_nps = GetFirstNLargestPartitions(n);
    std::vector<int> ops_to_replace;
    for (const auto p : first_nps) {
      auto nodes = p->nodes_to_replace;
      ops_to_replace.insert(ops_to_replace.end(), nodes->data,
                            nodes->data + nodes->size);
    }
    RemapInputTensors(ops_to_replace);
    RemoveReservedDequantsFromNodes(&ops_to_replace);
    return ops_to_replace;
  }

 protected:
  Status IsNodeSupported(TfLiteContext* context, TfLiteNode* node,
                         TfLiteRegistration* registration,
                         int node_id) override {
    // If we need to handle dequant nodes, we have to remap input tensors of
    // this node if some of them come from a dequant node before testing if
    // the node is supported.
    std::vector<int> orig_inputs;
    if (RecordAndRemapInputTensors(registration->builtin_code, node_id, node,
                                   &orig_inputs)) {
      // We have a dequant op here. Note that we retrun an Ok status because a
      // dequant node is first added as supported. Later, this dequant node
      // will be removed if it has to be preserved in the graph which happens
      // when its immediate downstream nodes cannot be supported.
      return OkStatus();
    }
    const auto status = GraphPartitionHelper::IsNodeSupported(
        context, node, registration, node_id);
    RestoreToOrigInputTensors(node, orig_inputs);
    return status;
  }

 private:
  // Record 'node' if it is a dequant op (i.e. a fp16 one here) and return true.
  // When it's not a dequant op, remap its inputs to the inputs of the preceding
  // dequant if there's a one and returns false. 'orig_inputs' records original
  // input tensor ids of this node if any input is remapped.
  bool RecordAndRemapInputTensors(int32_t op_code, int node_id,
                                  TfLiteNode* node,
                                  std::vector<int>* orig_inputs) {
    orig_inputs->clear();
    // Record the dequant node.
    if (op_code == kTfLiteBuiltinDequantize &&
        context_->tensors[node->inputs->data[0]].type ==
            TfLiteType::kTfLiteFloat16) {
      dequant_nodes_[node->outputs->data[0]] = node->inputs->data[0];
      return true;
    }
    // For a dequantize op, there's no need to remap its input tensors.
    if (dequant_nodes_.empty()) return false;
    RemapInputTensors(node, orig_inputs);
    return false;
  }

  // Restore inputs of 'node' to 'orig_inputs' only if two sizes match.
  void RestoreToOrigInputTensors(TfLiteNode* node,
                                 const std::vector<int>& orig_inputs) {
    if (node->inputs->size != orig_inputs.size()) return;
    for (int j = 0; j < node->inputs->size; ++j) {
      node->inputs->data[j] = orig_inputs[j];
    }
  }

  // Remap input tensors of every node in 'nodes' (i.e. node indices) if some of
  // them are from dequant ops.
  void RemapInputTensors(const std::vector<int>& nodes) const {
    for (int node_id : nodes) {
      TfLiteNode* node;
      TfLiteRegistration* registration;
      GetNodeAndRegistration(context_, node_id, &node, &registration)
          .IgnoreError();
      RemapInputTensors(node, nullptr /* orig_inputs*/);
    }
  }

  void RemoveSingleDequantNodePartitions() {
    auto it = partitions_.begin();
    while (it != partitions_.end()) {
      auto p = *it;
      if (p->nodes_to_replace->size != 1) {
        ++it;
        continue;
      }
      int node_id = p->nodes_to_replace->data[0];
      TfLiteNode* node = nullptr;
      TfLiteRegistration* registration = nullptr;
      GetNodeAndRegistration(context_, node_id, &node, &registration)
          .IgnoreError();
      if (registration->builtin_code != kTfLiteBuiltinDequantize) {
        ++it;
        continue;
      }
      // Note such dequant nodes have to be preserved in the graph as dequant
      // ops are not actually supported in the GPU delegate.
      dequant_nodes_to_save_.insert(node_id);
      it = partitions_.erase(it);
    }
  }

  void RemoveReservedDequantsFromNodes(std::vector<int>* nodes) {
    if (dequant_nodes_to_save_.empty()) return;
    auto it = nodes->begin();
    while (it != nodes->end()) {
      if (dequant_nodes_to_save_.find(*it) == dequant_nodes_to_save_.end()) {
        ++it;
        continue;
      }
      it = nodes->erase(it);
    }
  }

  // Remap input tensors of a single 'node' if some of come from a dequant op.
  // If 'orig_inputs' isn't nullptr, it records original input tensor ids of
  // this node if any input is remapped.
  void RemapInputTensors(TfLiteNode* node,
                         std::vector<int>* orig_inputs) const {
    TfLiteIntArray* inputs = node->inputs;
    auto inputs_view = TfLiteIntArrayView(inputs);
    // Prepopulate 'orig_inputs' first and clear it if there's no input from a
    // dequant op.
    if (orig_inputs) {
      orig_inputs->clear();
      orig_inputs->reserve(inputs->size);
      for (auto tid : inputs_view) {
        orig_inputs->push_back(tid);
      }
    }
    // Fix this node's inputs (i.e. prune out the preceding dequantize node) in
    // order to test if it is supported.
    bool is_remapped = false;
    for (int j = 0; j < inputs->size; ++j) {
      const int input_tid = inputs->data[j];
      const auto it = dequant_nodes_.find(input_tid);
      if (it != dequant_nodes_.end()) {
        inputs->data[j] = it->second;
        is_remapped = true;
      }
    }
    if (!is_remapped && orig_inputs) orig_inputs->clear();
  }

  // A map recording dequantize nodes's input/output tensors of this selected
  // graph. The key is the output tensor id, and the value is the input tensor
  // id.
  std::unordered_map<int, int> dequant_nodes_;

  // A set of dequant nodes as in node indices that have to be preserved in the
  // graph.
  std::set<int> dequant_nodes_to_save_;
};

Status IsSupported(const TfLiteContext* context, TfLiteNode* node,
                   const TfLiteRegistration* registration) {
  return NewOperationParser(registration)
      ->IsSupported(context, node, registration);
}

bool IsAllFloatTensors(const TfLiteContext* context,
                       const TfLiteIntArray* array) {
  for (int i = 0; i < array->size; ++i) {
    const TfLiteTensor* t = context->tensors + array->data[i];
    bool const type_supported =
        (t->type == kTfLiteFloat32 || t->type == kTfLiteFloat16);
    if (t->allocation_type == kTfLiteArenaRw && !type_supported) {
      return false;
    }
  }
  return true;
}

}  // namespace

Status ConvertTfLiteTensorToTensorRef(const TfLiteTensor& tflite_tensor,
                                      TensorRef<BHWC>* tensor_ref) {
  tensor_ref->type = ToDataType(tflite_tensor.type);
  return ExtractTensorShape(tflite_tensor, &tensor_ref->shape);
}

// TODO(impjdi): Check number of input/output tensors and their dimensions.
// TODO(impjdi): Check ops' parameters.
TfLiteIntArray* GetOpsToReplace(TfLiteContext* context) {
  IsNodeSupportedFn node_supported_fn =
      [=](TfLiteContext* context, TfLiteNode* node,
          TfLiteRegistration* registration) -> Status {
    RETURN_IF_ERROR(IsSupported(context, node, registration));
    return (IsAllFloatTensors(context, node->inputs) &&
            IsAllFloatTensors(context, node->outputs))
               ? OkStatus()
               : FailedPreconditionError(
                     "OP is supported, but tensor type isn't matched!");
  };

  GraphWithDequantPartitionHelper partition_helper(context, node_supported_fn);
  std::set<std::string> unsupported_nodes_info;
  auto status = partition_helper.Partition(&unsupported_nodes_info);
  if (!status.ok()) {
    TF_LITE_KERNEL_LOG(context, status.error_message().c_str());
    return nullptr;
  }

  // We simply get 1st largest partition, but we could later explore whether
  // getting more partitions could lead to better performance, i.e. by
  // parameterizing '1' here.
  std::vector<int> ops_to_replace =
      partition_helper.GetNodesOfFirstNLargestPartitions(1);

  if (!unsupported_nodes_info.empty()) {
    std::string unsupported = absl::StrJoin(unsupported_nodes_info, "\n");
    std::string error_message = absl::StrCat(
        "Following operations are not supported by GPU delegate:\n",
        unsupported, "\n");
    if (!ops_to_replace.empty()) {
      absl::StrAppendFormat(
          &error_message,
          "%d operations will run on the GPU (first node: "
          "%d, last node: %d), and the remaining %d",
          ops_to_replace.size(), ops_to_replace.front(), ops_to_replace.back(),
          partition_helper.num_total_nodes() - ops_to_replace.size());
    } else {
      absl::StrAppend(&error_message,
                      "No operations will run on the GPU, and all ",
                      partition_helper.num_total_nodes());
    }
    absl::StrAppend(&error_message, " operations will run on the CPU.");
    TF_LITE_KERNEL_LOG(context, error_message.c_str());
  }
  return ConvertVectorToTfLiteIntArray(ops_to_replace);
}

Status BuildModel(TfLiteContext* context,
                  const TfLiteDelegateParams* delegate_params,
                  GraphFloat32* graph) {
  std::vector<std::unique_ptr<TFLiteOperationParser>> operations;
  std::vector<int> tflite_nodes;
  for (int i = 0; i < delegate_params->nodes_to_replace->size; ++i) {
    TfLiteNode* tflite_node = nullptr;
    TfLiteRegistration* registration = nullptr;
    RETURN_IF_ERROR(GetNodeAndRegistration(
        context, delegate_params->nodes_to_replace->data[i], &tflite_node,
        &registration));
    if (registration->builtin_code == kTfLiteBuiltinDequantize) {
      // Ignore Dequantize nodes.
      continue;
    }
    auto op_parser = NewOperationParser(registration);
    if (!op_parser) {
      return UnimplementedError(
          absl::StrCat("Operation ", registration->builtin_code, "(",
                       registration->custom_name,
                       ") is not supported by TFLite GPU Delegate."));
    }
    operations.push_back(std::move(op_parser));
    tflite_nodes.push_back(i);
  }
  std::vector<Value<TensorRef<BHWC>>*> tensor_to_value(context->tensors_size,
                                                       nullptr);
  for (int i = 0; i < operations.size(); ++i) {
    TfLiteNode* tflite_node;
    TfLiteRegistration* registration;
    RETURN_IF_ERROR(GetNodeAndRegistration(
        context, delegate_params->nodes_to_replace->data[tflite_nodes[i]],
        &tflite_node, &registration));
    ObjectReader reader(graph, context, tflite_node, &tensor_to_value);
    const auto status =
        operations[i]->Parse(tflite_node, registration, graph, &reader);
    if (!status.ok()) {
      return InternalError(absl::StrCat(GetOpNameByRegistration(*registration),
                                        ": ", status.error_message()));
    }
  }
  return OkStatus();
}

Status BuildFinalModel(TfLiteContext* context,
                       const TfLiteDelegateParams* delegate_params,
                       GraphFloat32* graph) {
  RETURN_IF_ERROR(BuildModel(context, delegate_params, graph));

  // Apply general transformations on the graph.
  NullTransformationReporter reporter;
  ModelTransformer transformer(graph, &reporter);
  if (!ApplyGeneralTransformations(&transformer)) {
    return InternalError("Graph general transformations failed");
  }
  return OkStatus();
}

}  // namespace gpu
}  // namespace tflite
