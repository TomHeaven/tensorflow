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
#include "tensorflow/compiler/mlir/tfr/integration/tfr_decompose_ctx.h"

#include <string>
#include <vector>

#include "absl/types/span.h"
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Dialect.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/stream_executor/lib/statusor.h"

using testing::ElementsAreArray;
using testing::Test;

namespace tensorflow {

REGISTER_OP("MyAddN")
    .Input("inputs: N * T")
    .Output("sum: T")
    .Attr("N: int >= 1")
    .Attr("T: {numbertype, variant}")
    .SetIsCommutative()
    .SetIsAggregate()
    .SetShapeFn(shape_inference::UnchangedShape);

REGISTER_OP("RiscAdd")
    .Input("x: T")
    .Input("y: T")
    .Output("z: T")
    .Attr(
        "T: {bfloat16, half, float, double, uint8, int8, int16, int32, int64, "
        "complex64, complex128, string}")
    .SetShapeFn(shape_inference::UnchangedShape);

namespace {

constexpr char tfr_raw_text[] = R"(

tfr.func @tf__my_add_n(%values: !tfr.tensor_list,
                       %n: i64 {tfr.name="N"}) -> !tfr.tensor {
  %index = constant 0 : index
  %cst = constant 1 : i64
  %eq = cmpi "eq", %n, %cst : i64
  %v1 = tfr.get_element %values[%index] : (!tfr.tensor_list, index) -> !tfr.tensor
  %res = scf.if %eq -> !tfr.tensor {
    scf.yield %v1 : !tfr.tensor
  } else {
    %step = index_cast %cst : i64 to index
    %end = index_cast %n : i64 to index
    %reduce = scf.for %i = %step to %end step %step iter_args(%reduce_iter=%v1) -> !tfr.tensor {
      %v = tfr.get_element %values[%i] : (!tfr.tensor_list, index) -> !tfr.tensor
      %reduce_next =  tfr.call @tf__risc_add(%reduce_iter, %v) : (!tfr.tensor, !tfr.tensor) -> !tfr.tensor
      scf.yield %reduce_next : !tfr.tensor
    }
    scf.yield %reduce : !tfr.tensor
  }
  tfr.return %res : !tfr.tensor
}

tfr.func @tf__risc_add_(!tfr.tensor<T>, !tfr.tensor<T>) -> !tfr.tensor<T> attributes{T}
)";

class TFRDecomposeContextTest : public Test {
 protected:
  void SetUp() override {
    test_ctx_ = TFRDecomposeContext::Get(tfr_raw_text, &ctx_);
  }

  mlir::MLIRContext ctx_;
  std::unique_ptr<TFRDecomposeContext> test_ctx_;
};

std::vector<NodeAndType> NodesSequenceOf(const GraphDef& graph) {
  std::vector<NodeAndType> nodes;
  for (auto& node : graph.node()) {
    nodes.push_back({node.op(), node.attr().at("T").type()});
  }
  return nodes;
}

TEST_F(TFRDecomposeContextTest, FLOAT_1_ins) {
  std::vector<NodeDefBuilder::NodeOut> src_list;
  src_list.emplace_back("input", 0, DT_FLOAT);
  NodeDef test_node;
  auto status = NodeDefBuilder("float_add", "MyAddN")
                    .Input(src_list)
                    .Finalize(&test_node);
  EXPECT_TRUE(status.ok());
  std::vector<NodeAndType> input_node_types{{"input", DT_FLOAT}};
  auto decomposed =
      test_ctx_->Decompose(test_node, absl::MakeSpan(input_node_types));
  EXPECT_TRUE(decomposed.ok());
  std::vector<NodeAndType> expected_results{
      {"_Arg", DT_FLOAT}, {"Identity", DT_FLOAT}, {"_Retval", DT_FLOAT}};
  EXPECT_THAT(NodesSequenceOf(*decomposed.ValueOrDie()),
              ElementsAreArray(expected_results));
}

TEST_F(TFRDecomposeContextTest, FLOAT_3_ins) {
  std::vector<NodeDefBuilder::NodeOut> src_list;
  src_list.emplace_back("in0", 0, DT_FLOAT);
  src_list.emplace_back("in1", 0, DT_FLOAT);
  src_list.emplace_back("in2", 0, DT_FLOAT);
  NodeDef test_node;
  auto status = NodeDefBuilder("float_add_3", "MyAddN")
                    .Input(src_list)
                    .Finalize(&test_node);
  EXPECT_TRUE(status.ok());
  std::vector<NodeAndType> input_node_types{
      {"in0", DT_FLOAT}, {"in1", DT_FLOAT}, {"in2", DT_FLOAT}};
  auto decomposed =
      test_ctx_->Decompose(test_node, absl::MakeSpan(input_node_types));
  EXPECT_TRUE(decomposed.ok());

  std::vector<NodeAndType> expected_results{
      {"_Arg", DT_FLOAT},    {"_Arg", DT_FLOAT},    {"_Arg", DT_FLOAT},
      {"RiscAdd", DT_FLOAT}, {"RiscAdd", DT_FLOAT}, {"EnsureShape", DT_FLOAT},
      {"_Retval", DT_FLOAT}};
  EXPECT_THAT(NodesSequenceOf(*decomposed.ValueOrDie()),
              ElementsAreArray(expected_results));
}

TEST_F(TFRDecomposeContextTest, INT32_3_ins) {
  std::vector<NodeDefBuilder::NodeOut> src_list;
  src_list.emplace_back("in0", 0, DT_INT32);
  src_list.emplace_back("in1", 0, DT_INT32);
  src_list.emplace_back("in2", 0, DT_INT32);
  NodeDef test_node;
  auto status =
      NodeDefBuilder("int_add", "MyAddN").Input(src_list).Finalize(&test_node);
  EXPECT_TRUE(status.ok());
  std::vector<NodeAndType> input_node_types{
      {"in0", DT_INT32}, {"in1", DT_INT32}, {"in2", DT_INT32}};
  auto decomposed =
      test_ctx_->Decompose(test_node, absl::MakeSpan(input_node_types));
  EXPECT_TRUE(decomposed.ok());

  std::vector<NodeAndType> expected_results{
      {"_Arg", DT_INT32},    {"_Arg", DT_INT32},    {"_Arg", DT_INT32},
      {"RiscAdd", DT_INT32}, {"RiscAdd", DT_INT32}, {"EnsureShape", DT_INT32},
      {"_Retval", DT_INT32}};
  EXPECT_THAT(NodesSequenceOf(*decomposed.ValueOrDie()),
              ElementsAreArray(expected_results));
}

}  // namespace
}  // namespace tensorflow
