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

#include "tensorflow/lite/micro/kernels/kernel_util.h"

namespace tflite {
namespace micro {

const TfLiteEvalTensor* GetEvalInput(const TfLiteContext* context,
                                     const TfLiteNode* node, int index) {
  TFLITE_DCHECK(context != nullptr);
  TFLITE_DCHECK(node != nullptr);
  return context->GetEvalTensor(context, node->inputs->data[index]);
}

TfLiteEvalTensor* GetEvalOutput(const TfLiteContext* context,
                                const TfLiteNode* node, int index) {
  TFLITE_DCHECK(context != nullptr);
  TFLITE_DCHECK(node != nullptr);
  return context->GetEvalTensor(context, node->outputs->data[index]);
}

const RuntimeShape GetTensorShape(const TfLiteEvalTensor* tensor) {
  if (tensor == nullptr) {
    return RuntimeShape();
  }
  TfLiteIntArray* dims = tensor->dims;
  const int dims_size = dims->size;
  const int32_t* dims_data = reinterpret_cast<const int32_t*>(dims->data);
  return RuntimeShape(dims_size, dims_data);
}

bool HaveSameShapes(const TfLiteEvalTensor* input1,
                    const TfLiteEvalTensor* input2) {
  TFLITE_DCHECK(input1 != nullptr);
  TFLITE_DCHECK(input2 != nullptr);
  return TfLiteIntArrayEqual(input1->dims, input2->dims);
}

}  // namespace micro
}  // namespace tflite
