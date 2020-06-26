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

#include "tensorflow/lite/kernels/internal/reference/reduce.h"

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/internal/types.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_utils.h"

namespace tflite {
namespace ops {
namespace micro {
namespace reduce {

constexpr int kMaxNumberOfAxis = 4;
constexpr int kMaxNumberOfReducedAxis = 2;
enum ReduceType {
  kMax,
};

struct OpData {
  int32_t multiplier;
  int shift;
  int temp_buffer_idx;
  int resolved_axis_idx;
};

void* InitMax(TfLiteContext* context, const char* buffer, size_t length) {
  void* raw;
  context->AllocatePersistentBuffer(context, sizeof(OpData), &raw);
  return raw;
}

TfLiteStatus PrepareSimple(TfLiteContext* context, TfLiteNode* node) {
  // Inputs Tensor (dtype depends on quantization):
  // [0] = Input
  // [1] = Axis

  // Outputs Tensor (dtype depends on quantization):
  // [0] = Output

  // Validate number of inputs and outputs

  TF_LITE_ENSURE_EQ(context, node->inputs->size, 2);
  TF_LITE_ENSURE_EQ(context, node->outputs->size, 1);

  // Validate axis type
  const TfLiteTensor* input = GetInput(context, node, 0);
  const TfLiteTensor* axis = GetInput(context, node, 1);
  TF_LITE_ENSURE_TYPES_EQ(context, axis->type, kTfLiteInt32);

  if (input->type == kTfLiteInt8) {
    OpData* data = reinterpret_cast<OpData*>(node->user_data);
    const TfLiteTensor* output = GetOutput(context, node, 0);
    const double real_multiplier = static_cast<double>(input->params.scale) /
                                   static_cast<double>(output->params.scale);
    QuantizeMultiplier(real_multiplier, &data->multiplier, &data->shift);
  }

  return kTfLiteOk;
}

TfLiteStatus PrepareMax(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_OK(context, PrepareSimple(context, node));

  OpData* op_data = static_cast<OpData*>(node->user_data);
  const TfLiteTensor* input = GetInput(context, node, 0);
  const TfLiteTensor* axis = GetInput(context, node, 1);

  // Interpret an axis tensor with null dimensions as a scalar
  int num_elements;
  if (axis->dims == nullptr) {
    num_elements = 1;
  } else {
    num_elements = NumElements(axis);
  }
  context->RequestScratchBufferInArena(context, sizeof(int) * input->dims->size,
                                       &op_data->temp_buffer_idx);
  context->RequestScratchBufferInArena(context, sizeof(int) * num_elements,
                                       &op_data->resolved_axis_idx);

  return kTfLiteOk;
}

TfLiteStatus PrepareMeanOrSum(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_OK(context, PrepareSimple(context, node));
  // TODO(b/144955155): Support uint8_t(b/144955155) and int8_t(b/144955018)
  return kTfLiteOk;
}

void ResolveAxis(const int* axis_data, int axis_count,
                 tflite::MeanParams* op_params) {
  int i = 0;
  for (; i < axis_count; ++i) {
    op_params->axis[i] = static_cast<int16_t>(axis_data[i]);
  }
  for (; i < 4; ++i) {
    op_params->axis[i] = 1;
  }
  op_params->axis_count = axis_count;
}

TfLiteStatus EvalMean(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteEvalTensor* input = tflite::micro::GetEvalInput(context, node, 0);
  const TfLiteEvalTensor* axis = tflite::micro::GetEvalInput(context, node, 1);
  TfLiteEvalTensor* output = tflite::micro::GetEvalOutput(context, node, 0);
  TfLiteReducerParams* params =
      reinterpret_cast<TfLiteReducerParams*>(node->builtin_data);

  // Interpret an axis tensor with null dimensions as a scalar
  int num_axis;
  if (axis->dims == nullptr) {
    num_axis = 1;
  } else {
    num_axis = static_cast<int>(NumElements(axis));
  }

  int temp_index[kMaxNumberOfAxis];
  int resolved_axis[kMaxNumberOfReducedAxis];

  switch (input->type) {
    case kTfLiteFloat32: {
      tflite::MeanParams op_params;
      ResolveAxis(tflite::micro::GetTensorData<int>(axis), num_axis,
                  &op_params);
      // TODO(b/146571391): Support only 4D Input and 2D Axis for Mean until
      // scratch tensor allocation has been implemented in (b/132070898)
      bool is_valid_inputs =
          (input->dims->size == 4 && op_params.axis_count == 2 &&
           ((op_params.axis[0] == 1 && op_params.axis[1] == 2) ||
            (op_params.axis[0] == 2 && op_params.axis[1] == 1)));
      TF_LITE_ENSURE_MSG(
          context, is_valid_inputs == true,
          "Number of Input "
          "dimensions != 4 OR the Axis is not either [1, 2] or [2, 1]");
      // TODO(b/139102329): Handle the below special case in the combined
      // reference method.
      // Defer to specialized implementation for 4D Mean across axes 1 & 2.
      if (params->keep_dims) {
        reference_ops::Mean(op_params, tflite::micro::GetTensorShape(input),
                            tflite::micro::GetTensorData<float>(input),
                            tflite::micro::GetTensorShape(output),
                            tflite::micro::GetTensorData<float>(output));
      } else {
        TF_LITE_ENSURE(
            context,
            reference_ops::Mean(
                tflite::micro::GetTensorData<float>(input), input->dims->data,
                input->dims->size, tflite::micro::GetTensorData<float>(output),
                output->dims->data, output->dims->size,
                tflite::micro::GetTensorData<int>(axis), num_axis,
                params->keep_dims, temp_index, resolved_axis,
                tflite::micro::GetTensorData<float>(output)));
      }
    } break;
    default:
      // TODO(b/144955155): Support uint8_t(b/144955155) and int8_t(b/144955018)
      TF_LITE_ENSURE_MSG(context, false,
                         "Currently, only float32 input type "
                         "is supported.");
  }
  return kTfLiteOk;
}

template <typename T>
TfLiteStatus EvalLogic(TfLiteContext* context, TfLiteNode* node, T init_value,
                       T reducer(const T current, const T in)) {
  const TfLiteTensor* input = GetInput(context, node, 0);
  const TfLiteTensor* axis = GetInput(context, node, 1);
  TfLiteTensor* output = GetOutput(context, node, 0);
  TF_LITE_ENSURE_TYPES_EQ(context, input->type, output->type);
  TfLiteReducerParams* params =
      reinterpret_cast<TfLiteReducerParams*>(node->builtin_data);
  OpData* op_data = static_cast<OpData*>(node->user_data);

  // Interpret an axis tensor with null dimensions as a scalar
  int num_axis;
  if (axis->dims == nullptr) {
    num_axis = 1;
  } else {
    num_axis = static_cast<int>(NumElements(axis));
  }

  int* temp_buffer = static_cast<int*>(
      context->GetScratchBuffer(context, op_data->temp_buffer_idx));
  int* resolved_axis = static_cast<int*>(
      context->GetScratchBuffer(context, op_data->resolved_axis_idx));
  TF_LITE_ENSURE(
      context,
      reference_ops::ReduceGeneric<T>(
          GetTensorData<T>(input), input->dims->data, input->dims->size,
          GetTensorData<T>(output), output->dims->data, output->dims->size,
          GetTensorData<int>(axis), num_axis, params->keep_dims, temp_buffer,
          resolved_axis, init_value, reducer));

  // Convert between different output scales
  if (input->type == kTfLiteInt8 &&
      input->params.scale != output->params.scale) {
    int8_t* output_data = GetTensorData<int8_t>(output);
    for (int i = 0; i < NumElements(output); i++) {
      output_data[i] = static_cast<T>(std::max(
          std::min(MultiplyByQuantizedMultiplier(
                       output_data[i], op_data->multiplier, op_data->shift),
                   static_cast<int>(std::numeric_limits<T>::max())),
          static_cast<int>(std::numeric_limits<T>::min())));
    }
  }
  return kTfLiteOk;
}

// Eval for determined input type and reduce type.
template <typename T>
TfLiteStatus EvalType(TfLiteContext* context, TfLiteNode* node,
                      ReduceType reduce_type) {
  switch (reduce_type) {
    case kMax:
      return EvalLogic<T>(context, node, std::numeric_limits<T>::lowest(),
                          [](const T current, const T in) -> T {
                            return (in > current) ? in : current;
                          });
      break;
    default:
      TF_LITE_KERNEL_LOG(context, "Only reduce_max is supported.\n");
      return kTfLiteError;
  }
}

template <ReduceType reduce_type>
TfLiteStatus EvalGeneric(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input = GetInput(context, node, 0);
  switch (input->type) {
    case kTfLiteInt8:
      return EvalType<int8_t>(context, node, reduce_type);
      break;
    case kTfLiteFloat32:
      return EvalType<float>(context, node, reduce_type);
      break;
    default:
      TF_LITE_KERNEL_LOG(context,
                         "Only float32 and int8 types are supported.\n");
      return kTfLiteError;
  }
}
}  // namespace reduce

TfLiteRegistration Register_MEAN() {
  return {/*init=*/nullptr,
          /*free=*/nullptr,
          /*prepare=*/reduce::PrepareMeanOrSum,
          /*invoke=*/reduce::EvalMean,
          /*profiling_string=*/nullptr,
          /*builtin_code=*/0,
          /*custom_name=*/nullptr,
          /*version=*/0};
}

TfLiteRegistration* Register_REDUCE_MAX() {
  static TfLiteRegistration r = {/*init=*/reduce::InitMax, /*free=*/nullptr,
                                 reduce::PrepareMax,
                                 /*invoke=*/reduce::EvalGeneric<reduce::kMax>};
  return &r;
}

}  // namespace micro
}  // namespace ops
}  // namespace tflite
