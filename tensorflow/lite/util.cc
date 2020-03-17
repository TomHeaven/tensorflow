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
#include "tensorflow/lite/util.h"

#include <complex>
#include <cstring>

#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace tflite {
namespace {

TfLiteStatus UnresolvedOpInvoke(TfLiteContext* context, TfLiteNode* node) {
  context->ReportError(context,
                       "Encountered an unresolved custom op. Did you miss "
                       "a custom op or delegate?");
  return kTfLiteError;
}

}  // namespace

bool IsFlexOp(const char* custom_name) {
  return custom_name && strncmp(custom_name, kFlexCustomCodePrefix,
                                strlen(kFlexCustomCodePrefix)) == 0;
}

TfLiteIntArray* ConvertVectorToTfLiteIntArray(const std::vector<int>& input) {
  return ConvertArrayToTfLiteIntArray(static_cast<int>(input.size()),
                                      input.data());
}

TfLiteIntArray* ConvertArrayToTfLiteIntArray(const int rank, const int* dims) {
  TfLiteIntArray* output = TfLiteIntArrayCreate(rank);
  for (size_t i = 0; i < rank; i++) {
    output->data[i] = dims[i];
  }
  return output;
}

bool EqualArrayAndTfLiteIntArray(const TfLiteIntArray* a, const int b_size,
                                 const int* b) {
  if (!a) return false;
  if (a->size != b_size) return false;
  for (int i = 0; i < a->size; ++i) {
    if (a->data[i] != b[i]) return false;
  }
  return true;
}

size_t CombineHashes(std::initializer_list<size_t> hashes) {
  size_t result = 0;
  // Hash combiner used by TensorFlow core.
  for (size_t hash : hashes) {
    result = result ^
             (hash + 0x9e3779b97f4a7800ULL + (result << 10) + (result >> 4));
  }
  return result;
}

TfLiteStatus GetSizeOfType(TfLiteContext* context, const TfLiteType type,
                           size_t* bytes) {
  // TODO(levp): remove the default case so that new types produce compilation
  // error.
  switch (type) {
    case kTfLiteFloat32:
      *bytes = sizeof(float);
      break;
    case kTfLiteInt32:
      *bytes = sizeof(int);
      break;
    case kTfLiteUInt8:
      *bytes = sizeof(uint8_t);
      break;
    case kTfLiteInt64:
      *bytes = sizeof(int64_t);
      break;
    case kTfLiteBool:
      *bytes = sizeof(bool);
      break;
    case kTfLiteComplex64:
      *bytes = sizeof(std::complex<float>);
      break;
    case kTfLiteInt16:
      *bytes = sizeof(int16_t);
      break;
    case kTfLiteInt8:
      *bytes = sizeof(int8_t);
      break;
    case kTfLiteFloat16:
      *bytes = sizeof(TfLiteFloat16);
      break;
    default:
      if (context) {
        context->ReportError(
            context,
            "Type %d is unsupported. Only float32, int8, int16, int32, int64, "
            "uint8, bool, complex64 supported currently.",
            type);
      }
      return kTfLiteError;
  }
  return kTfLiteOk;
}

TfLiteRegistration CreateUnresolvedCustomOp(const char* custom_op_name) {
  return TfLiteRegistration{nullptr,
                            nullptr,
                            nullptr,
                            /*invoke*/ &UnresolvedOpInvoke,
                            nullptr,
                            BuiltinOperator_CUSTOM,
                            custom_op_name,
                            1};
}

bool IsUnresolvedCustomOp(const TfLiteRegistration& registration) {
  return registration.builtin_code == tflite::BuiltinOperator_CUSTOM &&
         registration.invoke == &UnresolvedOpInvoke;
}

std::string GetOpNameByRegistration(const TfLiteRegistration& registration) {
  auto op = registration.builtin_code;
  std::string result =
      EnumNameBuiltinOperator(static_cast<BuiltinOperator>(op));
  if ((op == kTfLiteBuiltinCustom || op == kTfLiteBuiltinDelegate) &&
      registration.custom_name) {
    result += " " + std::string(registration.custom_name);
  }
  return result;
}

}  // namespace tflite
