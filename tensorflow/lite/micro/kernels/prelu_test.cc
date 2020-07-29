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

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/kernel_runner.h"
#include "tensorflow/lite/micro/testing/micro_test.h"
#include "tensorflow/lite/micro/testing/test_utils.h"

namespace tflite {
namespace testing {
namespace {

void TestPreluFloat(std::initializer_list<int> input_dims_data,
                    std::initializer_list<float> input_data,
                    std::initializer_list<int> alpha_dims_data,
                    std::initializer_list<float> alpha_data,
                    std::initializer_list<float> expected_output_data,
                    std::initializer_list<int> output_dims_data,
                    float* output_data) {
  TfLiteIntArray* input_dims = IntArrayFromInitializer(input_dims_data);
  TfLiteIntArray* alpha_dims = IntArrayFromInitializer(alpha_dims_data);
  TfLiteIntArray* output_dims = IntArrayFromInitializer(output_dims_data);
  const int output_dims_count = ElementCount(*output_dims);
  constexpr int inputs_size = 2;
  constexpr int outputs_size = 1;
  constexpr int tensors_size = inputs_size + outputs_size;
  TfLiteTensor tensors[tensors_size] = {
      CreateFloatTensor(input_data, input_dims),
      CreateFloatTensor(alpha_data, alpha_dims),
      CreateFloatTensor(output_data, output_dims),
  };

  int inputs_array_data[] = {2, 0, 1};
  TfLiteIntArray* inputs_array = IntArrayFromInts(inputs_array_data);
  int outputs_array_data[] = {1, 2};
  TfLiteIntArray* outputs_array = IntArrayFromInts(outputs_array_data);

  const TfLiteRegistration registration = tflite::ops::micro::Register_PRELU();
  micro::KernelRunner runner(registration, tensors, tensors_size, inputs_array,
                             outputs_array,
                             /*builtin_data=*/nullptr, micro_test::reporter);

  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, runner.InitAndPrepare());
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, runner.Invoke());

  for (int i = 0; i < output_dims_count; ++i) {
    TF_LITE_MICRO_EXPECT_NEAR(expected_output_data.begin()[i], output_data[i],
                              1e-5f);
  }
}

// Template argument T can be either uint8_t or int8_t depending on which type
// of quantization required to be tested.
template <typename T>
void TestPreluQuantized(std::initializer_list<int> input_dims_data,
                        std::initializer_list<T> input_data, float input_min,
                        float input_max,
                        std::initializer_list<int> alpha_dims_data,
                        std::initializer_list<T> alpha_data, float alpha_min,
                        float alpha_max,
                        std::initializer_list<T> expected_output_data,
                        std::initializer_list<int> output_dims_data,
                        float output_min, float output_max, T* output_data) {
  TfLiteIntArray* input_dims = IntArrayFromInitializer(input_dims_data);
  TfLiteIntArray* alpha_dims = IntArrayFromInitializer(alpha_dims_data);
  TfLiteIntArray* output_dims = IntArrayFromInitializer(output_dims_data);
  const int output_dims_count = ElementCount(*output_dims);
  constexpr int inputs_size = 2;
  constexpr int outputs_size = 1;
  constexpr int tensors_size = inputs_size + outputs_size;
  TfLiteTensor tensors[tensors_size] = {
      CreateQuantizedTensor(input_data, input_dims, input_min, input_max),
      CreateQuantizedTensor(alpha_data, alpha_dims, alpha_min, alpha_max),
      CreateQuantizedTensor(output_data, output_dims, output_min, output_max),
  };

  int inputs_array_data[] = {2, 0, 1};
  TfLiteIntArray* inputs_array = IntArrayFromInts(inputs_array_data);
  int outputs_array_data[] = {1, 2};
  TfLiteIntArray* outputs_array = IntArrayFromInts(outputs_array_data);

  const TfLiteRegistration registration = tflite::ops::micro::Register_PRELU();
  micro::KernelRunner runner(registration, tensors, tensors_size, inputs_array,
                             outputs_array,
                             /*builtin_data=*/nullptr, micro_test::reporter);

  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, runner.InitAndPrepare());
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, runner.Invoke());

  for (int i = 0; i < output_dims_count; ++i) {
    TF_LITE_MICRO_EXPECT_EQ(expected_output_data.begin()[i], output_data[i]);
  }
}
}  // namespace
}  // namespace testing
}  // namespace tflite

TF_LITE_MICRO_TESTS_BEGIN

TF_LITE_MICRO_TEST(FloatPreluActivationsOpTest) {
  const int output_dims_count = 12;
  float output_data[output_dims_count];
  tflite::testing::TestPreluFloat({3, 2, 2, 3},  // input shape
                                  {
                                      0.0f, 0.0f, 0.0f,     // Row 1, Column 1
                                      1.0f, 1.0f, 1.0f,     // Row 1, Column 2
                                      -1.0f, -1.0f, -1.0f,  // Row 2, Column 1
                                      -2.0f, -2.0f, -2.0f,  // Row 1, Column 2
                                  },
                                  {3, 1, 1, 3},        // alpha shape
                                  {0.0f, 1.0f, 2.0f},  // alpha values
                                  {
                                      0.0f, 0.0f, 0.0f,    // Row 1, Column 1
                                      1.0f, 1.0f, 1.0f,    // Row 1, Column 2
                                      0.0f, -1.0f, -2.0f,  // Row 2, Column 1
                                      0.0f, -2.0f, -4.0f,  // Row 1, Column 2
                                  },
                                  {3, 2, 2, 3},  // output shape
                                  output_data);
}

TF_LITE_MICRO_TEST(QuantizedUint8PreluActivationsOpTest) {
  using tflite::testing::F2Q;
  const float kMin = -4;
  const float kMax = 127.f / 32.f;
  const int output_dims_count = 12;
  uint8_t output_data[output_dims_count];
  tflite::testing::TestPreluQuantized(
      {3, 2, 2, 3},  // input shape
      {F2Q(0.0f, kMin, kMax), F2Q(0.0f, kMin, kMax), F2Q(0.0f, kMin, kMax),
       F2Q(0.5f, kMin, kMax), F2Q(0.5f, kMin, kMax), F2Q(0.5f, kMin, kMax),
       F2Q(-1.0f, kMin, kMax), F2Q(-1.0f, kMin, kMax), F2Q(-1.0f, kMin, kMax),
       F2Q(-0.25f, kMin, kMax), F2Q(-0.25f, kMin, kMax),
       F2Q(-0.25f, kMin, kMax)},
      kMin, kMax, {3, 1, 1, 3},  // alpha shape
      {F2Q(0.0f, kMin, kMax), F2Q(0.5f, kMin, kMax), F2Q(-0.5f, kMin, kMax)},
      kMin, kMax,
      {F2Q(0.0f, kMin, kMax), F2Q(0.0f, kMin, kMax), F2Q(0.0f, kMin, kMax),
       F2Q(0.5f, kMin, kMax), F2Q(0.5f, kMin, kMax), F2Q(0.5f, kMin, kMax),
       F2Q(0.0f, kMin, kMax), F2Q(-0.5f, kMin, kMax), F2Q(0.5f, kMin, kMax),
       F2Q(0.0f, kMin, kMax), F2Q(-0.125f, kMin, kMax),
       F2Q(0.125f, kMin, kMax)},
      {3, 2, 2, 3},  // output shape
      kMin, kMax, output_data);
}

TF_LITE_MICRO_TEST(QuantizedInt8PreluActivationsOpTest) {
  using tflite::testing::F2QS;
  const float kMin = -1;
  const float kMax = 127.f / 128.f;
  const int output_dims_count = 12;
  int8_t output_data[output_dims_count];
  tflite::testing::TestPreluQuantized(
      {3, 2, 2, 3},  // input shape
      {F2QS(0.0f, kMin, kMax), F2QS(0.0f, kMin, kMax), F2QS(0.0f, kMin, kMax),
       F2QS(0.5f, kMin, kMax), F2QS(0.5f, kMin, kMax), F2QS(0.5f, kMin, kMax),
       F2QS(-1.0f, kMin, kMax), F2QS(-1.0f, kMin, kMax),
       F2QS(-1.0f, kMin, kMax), F2QS(-0.25f, kMin, kMax),
       F2QS(-0.25f, kMin, kMax), F2QS(-0.25f, kMin, kMax)},
      kMin, kMax, {3, 1, 1, 3},  // alpha shape
      {F2QS(0.0f, kMin, kMax), F2QS(0.5f, kMin, kMax), F2QS(-0.5f, kMin, kMax)},
      kMin, kMax,
      {F2QS(0.0f, kMin, kMax), F2QS(0.0f, kMin, kMax), F2QS(0.0f, kMin, kMax),
       F2QS(0.5f, kMin, kMax), F2QS(0.5f, kMin, kMax), F2QS(0.5f, kMin, kMax),
       F2QS(0.0f, kMin, kMax), F2QS(-0.5f, kMin, kMax), F2QS(0.5f, kMin, kMax),
       F2QS(0.0f, kMin, kMax), F2QS(-0.125f, kMin, kMax),
       F2QS(0.125f, kMin, kMax)},
      {3, 2, 2, 3},  // output shape
      kMin, kMax, output_data);
}
TF_LITE_MICRO_TESTS_END
