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

#include "tensorflow/compiler/xla/client/lib/tridiagonal.h"

#include <numeric>
#include <string>
#include <vector>

#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/lib/loops.h"
#include "tensorflow/compiler/xla/client/lib/slicing.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {
namespace tridiagonal {

namespace {

struct TridiagonalSystemShape {
  const int64 rank;
  const int64 num_equations;
};

Status CheckSecondToLastDimension(const Shape& op_shape, int64 rank,
                                  int64 expected, const std::string& op_name) {
  const auto actual_num_dims = ShapeUtil::GetDimension(op_shape, rank - 2);

  if (actual_num_dims != expected) {
    return InvalidArgument(
        "Second to last dimension of %s should be %d but is %d.", op_name,
        expected, actual_num_dims);
  }

  return Status::OK();
}

StatusOr<TridiagonalSystemShape> CheckSystemAndReturnShape(XlaOp lower_diagonal,
                                                           XlaOp main_diagonal,
                                                           XlaOp upper_diagonal,
                                                           XlaOp rhs) {
  XlaBuilder* builder = lower_diagonal.builder();

  TF_ASSIGN_OR_RETURN(Shape lower_diagonal_shape,
                      builder->GetShape(lower_diagonal));
  TF_ASSIGN_OR_RETURN(Shape main_diagonal_shape,
                      builder->GetShape(main_diagonal));
  TF_ASSIGN_OR_RETURN(Shape upper_diagonal_shape,
                      builder->GetShape(upper_diagonal));
  TF_ASSIGN_OR_RETURN(Shape rhs_shape, builder->GetShape(rhs));

  const auto lower_diagonal_rank = lower_diagonal_shape.rank();
  const auto main_diagonal_rank = main_diagonal_shape.rank();
  const auto upper_diagonal_rank = upper_diagonal_shape.rank();
  const auto rhs_rank = rhs_shape.rank();
  if (!((lower_diagonal_rank == main_diagonal_rank) &&
        (lower_diagonal_rank == upper_diagonal_rank) &&
        (lower_diagonal_rank == rhs_rank))) {
    return InvalidArgument(
        "All inputs should have the same rank but got rank "
        "%d for lower diagonal, %d for diagonal, %d for upper diagonal, "
        "%d for rhs",
        lower_diagonal_rank, main_diagonal_rank, upper_diagonal_rank, rhs_rank);
  }
  const auto rank = lower_diagonal_rank;
  if (rank < 2) {
    return InvalidArgument("Arguments must have rank >=2; got rank %d.", rank);
  }

  const auto lower_diagonal_num_eqs =
      ShapeUtil::GetDimension(lower_diagonal_shape, rank - 1);
  const auto main_diagonal_num_eqs =
      ShapeUtil::GetDimension(main_diagonal_shape, rank - 1);
  const auto upper_diagonal_num_eqs =
      ShapeUtil::GetDimension(upper_diagonal_shape, rank - 1);
  const auto rhs_num_eqs = ShapeUtil::GetDimension(rhs_shape, rank - 1);
  if (!((lower_diagonal_num_eqs == main_diagonal_num_eqs) &&
        (lower_diagonal_num_eqs == upper_diagonal_num_eqs) &&
        (lower_diagonal_num_eqs == rhs_num_eqs))) {
    return InvalidArgument(
        "All inputs should have the same innermost dimension but got "
        "%d for lower diagonal, %d for diagonal, %d for upper diagonal, "
        "%d for rhs",
        lower_diagonal_num_eqs, main_diagonal_num_eqs, upper_diagonal_num_eqs,
        rhs_num_eqs);
  }
  const auto num_equations = lower_diagonal_num_eqs;

  TF_RETURN_IF_ERROR(CheckSecondToLastDimension(lower_diagonal_shape, rank, 1,
                                                "lower diagonal"));
  TF_RETURN_IF_ERROR(
      CheckSecondToLastDimension(main_diagonal_shape, rank, 1, "diagonal"));
  TF_RETURN_IF_ERROR(CheckSecondToLastDimension(upper_diagonal_shape, rank, 1,
                                                "upper diagonal"));

  TridiagonalSystemShape result = {.rank = rank,
                                   .num_equations = num_equations};
  return result;
}

XlaOp Coefficient(XlaOp operand, int64 i) {
  return SliceInMinorDims(operand, /*start=*/{i}, /*end=*/{i + 1});
}

}  // namespace

// Applies Thomas algorithm to solve a linear system where the linear operand
// is a tri-diagonal matrix.
// See https://en.wikipedia.org/wiki/Tridiagonal_matrix_algorithm for a simple
// reference on the Thomas algorithm.
// It is expected that the three diagonals are represented as tensors of shape
// [..., 1, num_equations] where num_equations is the number of dimensions of
// the unknowns considered in the linear systems.
// The first innermost dimension of `lower_diagonal` (`lower_diagonal[..., :,
// 0]`) will be ignored. The last innermost dimension of `upper_diagonal`
// (`upper_diagonal[..., :, num_equations - 1]`) will be ignored. The shape of
// the right-hand-side `rhs` should be [..., num_rhs, num_equations]. The
// solution will have the shape [..., num_rhs, num_equations].
StatusOr<XlaOp> ThomasSolver(XlaOp lower_diagonal, XlaOp main_diagonal,
                             XlaOp upper_diagonal, XlaOp rhs) {
  TF_ASSIGN_OR_RETURN(TridiagonalSystemShape system_shape,
                      CheckSystemAndReturnShape(lower_diagonal, main_diagonal,
                                                upper_diagonal, rhs));

  auto rank = system_shape.rank;
  auto num_eqs = system_shape.num_equations;

  std::vector<XlaOp> main_diag_after_elimination(num_eqs);
  std::vector<XlaOp> rhs_after_elimination(num_eqs);
  std::vector<XlaOp> upper_diagonal_coeffs(num_eqs);

  main_diag_after_elimination[0] = Coefficient(main_diagonal, 0);
  rhs_after_elimination[0] = Coefficient(rhs, 0);
  for (int64 i = 0; i < num_eqs - 1; i++) {
    upper_diagonal_coeffs[i] = Coefficient(upper_diagonal, i);
  }

  // Forward transformation.
  for (int64 i = 1; i < num_eqs; i++) {
    auto lower_diagonal_i = Coefficient(lower_diagonal, i);
    auto main_diagonal_i = Coefficient(main_diagonal, i);
    auto rhs_i = Coefficient(rhs, i);

    auto w_i = lower_diagonal_i / main_diag_after_elimination[i - 1];

    main_diag_after_elimination[i] =
        main_diagonal_i - w_i * upper_diagonal_coeffs[i - 1];
    rhs_after_elimination[i] = rhs_i - w_i * rhs_after_elimination[i - 1];
  }

  std::vector<XlaOp> x_coeffs(num_eqs);

  // Backward reduction.
  x_coeffs[num_eqs - 1] = rhs_after_elimination[num_eqs - 1] /
                          main_diag_after_elimination[num_eqs - 1];
  for (int i = num_eqs - 2; i >= 0; i--) {
    x_coeffs[i] = (rhs_after_elimination[i] -
                   upper_diagonal_coeffs[i] * x_coeffs[i + 1]) /
                  main_diag_after_elimination[i];
  }

  return ConcatInDim(lower_diagonal.builder(), x_coeffs, rank - 1);
}

// Applies Thomas algorithm to solve a linear system where the linear operand
// is a tri-diagonal matrix.
// It is expected that the tree diagonals are stacked into a tensors of shape
// [..., 3, num_equations] where num_equations is the number of spatial
// dimensions considered in the system.
// diagonals[..., 0, :] represents the upper diagonal whose last inner
// dimension will be ignored.
// diagonals[..., 1, :] represents the main diagonal.
// diagonals[..., 2, :] represents the lower diagonal whose first inner
// dimension will be ignored.
// The right-hand-side d is expected to have dimension
// [..., num_rhs, num_equations].
// The solution will have size [..., num_rhs, num_equations].
StatusOr<XlaOp> ThomasSolver(XlaOp diagonals, XlaOp rhs) {
  XlaBuilder* builder = diagonals.builder();
  TF_ASSIGN_OR_RETURN(Shape diagonals_shape, builder->GetShape(diagonals));
  const int64 rank = diagonals_shape.rank();

  auto upper_diagonal =
      SliceInDim(diagonals, /*start_index=*/0, /*limit_index=*/1,
                 /*stride=*/1, /*dimno=*/rank - 2);
  auto main_diagonal =
      SliceInDim(diagonals, /*start_index=*/1, /*limit_index=*/2,
                 /*stride=*/1, /*dimno=*/rank - 2);
  auto lower_diagonal =
      SliceInDim(diagonals, /*start_index=*/2, /*limit_index=*/3,
                 /*stride=*/1, /*dimno=*/rank - 2);

  // TODO(belletti): Get rid of the transposes here.
  std::vector<int64> transpose_order(rank);
  std::iota(transpose_order.begin(), transpose_order.end(), 0);
  transpose_order[rank - 2] = rank - 1;
  transpose_order[rank - 1] = rank - 2;
  // Swap the last two dimensions.
  rhs = Transpose(rhs, transpose_order);

  TF_ASSIGN_OR_RETURN(XlaOp x, ThomasSolver(lower_diagonal, main_diagonal,
                                            upper_diagonal, rhs));
  return Transpose(x, transpose_order);
}

}  // namespace tridiagonal
}  // namespace xla
