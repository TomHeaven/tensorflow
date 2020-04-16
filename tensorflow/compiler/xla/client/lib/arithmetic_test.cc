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

#include "tensorflow/compiler/xla/client/lib/arithmetic.h"

#include <initializer_list>

#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/client_library_test_base.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"

namespace xla {
namespace {

class ArithmeticTest : public ClientLibraryTestBase {
 public:
  template <typename NativeT>
  void TestArgMin(std::initializer_list<std::initializer_list<NativeT>> input,
                  absl::Span<NativeT const> expected_output, int axis) {
    return TestArgMinMax(input, expected_output, axis, /*is_min=*/true);
  }

  template <typename NativeT>
  void TestArgMax(std::initializer_list<std::initializer_list<NativeT>> input,
                  absl::Span<NativeT const> expected_output, int axis) {
    return TestArgMinMax(input, expected_output, axis, /*is_min=*/false);
  }

 private:
  // Test ArgMin/ArgMax implementation, both single- and two- pass.
  template <typename NativeT>
  void TestArgMinMax(
      std::initializer_list<std::initializer_list<NativeT>> input,
      absl::Span<NativeT const> expected_output, int axis, bool is_min) {
    if (is_min) {
      TestArgMinMaxImpl(input, expected_output, axis, &ArgMin);
      TestArgMinMaxImpl(input, expected_output, axis,
                        [](XlaOp op, PrimitiveType type, int axis) {
                          return ArgMinTwoPass(op, type, axis);
                        });
    } else {
      TestArgMinMaxImpl(input, expected_output, axis, &ArgMax);
      TestArgMinMaxImpl(input, expected_output, axis,
                        [](XlaOp op, PrimitiveType type, int axis) {
                          return ArgMaxTwoPass(op, type, axis);
                        });
    }
  }

  template <typename NativeT>
  void TestArgMinMaxImpl(
      std::initializer_list<std::initializer_list<NativeT>> input,
      absl::Span<NativeT const> expected_output, int axis,
      std::function<void(XlaOp, PrimitiveType, int)> MinMaxImpl) {
    XlaBuilder builder(TestName());
    XlaOp x = ConstantR2<NativeT>(&builder, input);
    MinMaxImpl(x, primitive_util::NativeToPrimitiveType<NativeT>(), axis);
    ComputeAndCompareR1<NativeT>(&builder, expected_output, {});
  }
};

XLA_TEST_F(ArithmeticTest, ArgMinR2Axis0) {
  TestArgMin<int32>({{1, 7, 4}, {6, 3, 5}, {8, 3, 3}}, {0, 1, 2},
                    /*axis=*/0);
}

XLA_TEST_F(ArithmeticTest, ArgMinR2Axis1) {
  TestArgMin<int32>({{1, 7, 4}, {6, 3, 5}, {8, 3, 3}}, {0, 1, 1},
                    /*axis=*/1);
}

XLA_TEST_F(ArithmeticTest, ArgMaxR2Axis0) {
  TestArgMax<int32>({{1, 7, 4}, {6, 3, 5}, {8, 3, 3}}, {2, 0, 1},
                    /*axis=*/0);
}

XLA_TEST_F(ArithmeticTest, ArgMaxR2Axis1) {
  TestArgMax<int32>({{1, 7, 4}, {6, 3, 5}, {8, 3, 3}}, {1, 0, 0},
                    /*axis=*/1);
}

}  // namespace
}  // namespace xla
