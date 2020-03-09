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

#include "tensorflow/lite/delegates/gpu/gl/kernels/softmax.h"

#include <cmath>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/gl/kernels/test_util.h"

using ::testing::FloatNear;
using ::testing::Pointwise;

namespace tflite {
namespace gpu {
namespace gl {
namespace {

TEST(SoftmaxTest, Softmax) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 2, 2, 1);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 2, 2, 1);

  SoftmaxAttributes attr;
  attr.axis = Axis::CHANNELS;

  SingleOpModel model({ToString(OperationType::SOFTMAX), attr}, {input},
                      {output});
  ASSERT_TRUE(model.PopulateTensor(0, {0.1f, 0.2f, 0.3f, 0.4f}));
  ASSERT_OK(model.Invoke(*NewSoftmaxNodeShader()));
  EXPECT_THAT(model.GetOutput(0),
              Pointwise(FloatNear(1e-6f), {1.0f, 1.0f, 1.0f, 1.0f}));
}

TEST(SoftmaxTest, DoesNotWorkForHeightAxis) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 2, 2, 1);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 2, 2, 1);

  SoftmaxAttributes attr;
  attr.axis = Axis::HEIGHT;

  SingleOpModel model({ToString(OperationType::SOFTMAX), attr}, {input},
                      {output});
  ASSERT_TRUE(model.PopulateTensor(0, {0.1f, 0.2f, 0.3f, 0.4f}));
  EXPECT_FALSE(model.Invoke(*NewSoftmaxNodeShader()).ok());
}

TEST(SoftmaxTest, DoesNotWorkForWidthAxis) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 2, 2, 1);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 2, 2, 1);

  SoftmaxAttributes attr;
  attr.axis = Axis::WIDTH;

  SingleOpModel model({ToString(OperationType::SOFTMAX), attr}, {input},
                      {output});
  ASSERT_TRUE(model.PopulateTensor(0, {0.1f, 0.2f, 0.3f, 0.4f}));
  EXPECT_FALSE(model.Invoke(*NewSoftmaxNodeShader()).ok());
}

TEST(SoftmaxTest, Softmax1x1) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 1, 1, 4);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 1, 1, 4);

  SoftmaxAttributes attr;
  attr.axis = Axis::CHANNELS;

  const float sum =
      std::exp(0.1f) + std::exp(0.2f) + std::exp(0.3f) + std::exp(0.4f);

  SingleOpModel model({ToString(OperationType::SOFTMAX), attr}, {input},
                      {output});
  ASSERT_TRUE(model.PopulateTensor(0, {0.1f, 0.2f, 0.3f, 0.4f}));
  ASSERT_OK(model.Invoke(*NewSoftmaxNodeShader()));
  EXPECT_THAT(model.GetOutput(0),
              Pointwise(FloatNear(1e-6f),
                        {std::exp(0.1f) / sum, std::exp(0.2f) / sum,
                         std::exp(0.3f) / sum, std::exp(0.4f) / sum}));
}

}  // namespace
}  // namespace gl
}  // namespace gpu
}  // namespace tflite
