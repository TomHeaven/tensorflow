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

#include "tensorflow/compiler/mlir/tensorflow/utils/convert_type.h"

#include "llvm/Support/raw_ostream.h"
#include "mlir/IR/Builders.h"  // TF:llvm-project
#include "mlir/IR/MLIRContext.h"  // TF:llvm-project
#include "mlir/IR/StandardTypes.h"  // TF:llvm-project
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/stream_executor/lib/statusor.h"

namespace tensorflow {
namespace {

std::string ConvertToMlirString(const std::vector<int64_t>& dims,
                                bool unknown_rank, DataType dtype) {
  TensorShapeProto shape;
  shape.set_unknown_rank(unknown_rank);
  for (int64_t dim : dims) {
    shape.add_dim()->set_size(dim);
  }
  mlir::MLIRContext context;
  mlir::Builder b(&context);
  auto status_or = ConvertToMlirTensorType(shape, dtype, &b);
  std::string buf;
  llvm::raw_string_ostream os(buf);
  status_or.ValueOrDie().print(os);
  return os.str();
}

TEST(MlirConvertType, ConvertToMlirTensorType) {
  // Simple case of static shapes.
  EXPECT_EQ("tensor<4x8x16xi32>",
            ConvertToMlirString({4, 8, 16}, /*unknown_rank=*/false,
                                DataType::DT_INT32));

  // Partially known shapes.
  EXPECT_EQ("tensor<?x27x?xbf16>",
            ConvertToMlirString({-1, 27, -1}, /*unknown_rank=*/false,
                                DataType::DT_BFLOAT16));

  // Unranked shapes.
  EXPECT_EQ("tensor<*xf32>",
            ConvertToMlirString({}, /*unknown_rank=*/true, DataType::DT_FLOAT));
}

}  // namespace

}  // namespace tensorflow
