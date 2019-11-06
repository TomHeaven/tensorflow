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

#ifdef INTEL_MKL

// This file contains the registration of MKL-DNN array ops.

#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/util/mirror_pad_mode.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/util/strided_slice_op.h"
#include "tensorflow/core/util/tensor_format.h"

namespace tensorflow {

using shape_inference::DimensionHandle;
using shape_inference::InferenceContext;
using shape_inference::ShapeHandle;
using shape_inference::UnchangedShape;

// Adding QuantizedConcatV2 op to be able to replace it by
// _MklQuantizedConcatV2 in the graph rewrite.
REGISTER_OP("QuantizedConcatV2")
    .Input("values: N * T")
    .Input("axis: Tidx")
    .Input("input_mins: N * float32")
    .Input("input_maxes: N * float32")
    .Output("output: T")
    .Output("output_min: float")
    .Output("output_max: float")
    .Attr("N: int >= 2")
    .Attr("T: type")
    .Attr("Tidx: {int32, int64} = DT_INT32")
    .SetShapeFn([](InferenceContext* c) {
      const int n = (c->num_inputs() - 1) / 3;
      TF_RETURN_IF_ERROR(shape_inference::QuantizedConcatV2Shape(c, n));
      ShapeHandle unused;
      for (int i = n + 1; i < c->num_inputs(); ++i) {
        TF_RETURN_IF_ERROR(c->WithRank(c->input(i), 0, &unused));
      }
      c->set_output(1, c->Scalar());
      c->set_output(2, c->Scalar());
      return Status::OK();
    });

REGISTER_OP("_MklQuantizedConcatV2")
    .Input("values: N * T")
    .Input("axis: Tidx")
    .Input("input_mins:  N * float32")
    .Input("input_maxes: N * float32")
    .Input("mkl_values: N * uint8")
    .Input("mkl_axis: uint8")
    .Input("mkl_input_mins:  N * uint8")
    .Input("mkl_input_maxes: N * uint8")
    .Output("output: T")
    .Output("output_min: float")
    .Output("output_max: float")
    .Output("mkl_output: uint8")
    .Output("mkl_output_min: uint8")
    .Output("mkl_output_max: uint8")
    .Attr("N: int >= 2")
    .Attr("T: type")
    .Attr("Tidx: {int32, int64} = DT_INT32")
    .SetShapeFn([](InferenceContext* c) {
      const int n = (c->num_inputs() / 2 - 1) / 3;
      TF_RETURN_IF_ERROR(shape_inference::QuantizedConcatV2Shape(c, n));
      ShapeHandle unused;
      for (int i = n + 1; i < c->num_inputs() / 2; ++i) {
        TF_RETURN_IF_ERROR(c->WithRank(c->input(i), 0, &unused));
      }
      c->set_output(1, c->Scalar());
      c->set_output(2, c->Scalar());
      return Status::OK();
    });

REGISTER_OP("_MklQuantizeV2")
    .Input("input: float")
    .Input("min_range: float")
    .Input("max_range: float")
    .Input("mkl_input: uint8")
    .Input("mkl_min_range: uint8")
    .Input("mkl_max_range: uint8")
    .Output("output: T")
    .Output("output_min: float")
    .Output("output_max: float")
    .Output("mkl_output: uint8")
    .Output("mkl_output_min: uint8")
    .Output("mkl_output_max: uint8")
    .Attr("T: quantizedtype")
    .Attr("mode: {'MIN_COMBINED', 'MIN_FIRST', 'SCALED'} = 'SCALED'")
    .Attr(
        "round_mode: {'HALF_AWAY_FROM_ZERO', 'HALF_TO_EVEN'} = "
        "'HALF_TO_EVEN'")
    .Attr("narrow_range: bool = false")
    .Attr("axis: int = -1")
    .Attr("ensure_minimum_range: float = 0.01")
    .SetShapeFn([](InferenceContext* c) {
      int axis = -1;
      Status s = c->GetAttr("axis", &axis);
      if (!s.ok() && s.code() != error::NOT_FOUND) {
        return s;
      }
      const int minmax_rank = (axis == -1) ? 0 : 1;
      TF_RETURN_IF_ERROR(shape_inference::UnchangedShape(c));
      ShapeHandle minmax;
      TF_RETURN_IF_ERROR(c->WithRank(c->input(1), minmax_rank, &minmax));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(2), minmax_rank, &minmax));
      if (axis != -1) {
        ShapeHandle input;
        TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(0), axis + 1, &input));
        DimensionHandle depth;
        TF_RETURN_IF_ERROR(
            c->Merge(c->Dim(minmax, 0), c->Dim(input, axis), &depth));
      }
      c->set_output(1, minmax);
      c->set_output(2, minmax);
      return Status::OK();
    });

REGISTER_OP("_MklDequantize")
    .Input("input: T")
    .Input("min_range: float")
    .Input("max_range: float")
    .Input("mkl_input: uint8")
    .Input("mkl_min_range: uint8")
    .Input("mkl_max_range: uint8")
    .Output("output: float")
    .Output("mkl_output: uint8")
    .Attr("T: quantizedtype")
    .Attr("mode: {'MIN_COMBINED', 'MIN_FIRST', 'SCALED'} = 'SCALED'")
    .SetShapeFn([](InferenceContext* c) {
      TF_RETURN_IF_ERROR(shape_inference::UnchangedShape(c));
      ShapeHandle unused;
      TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 0, &unused));
      TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 0, &unused));
      return Status::OK();
    });

}  // namespace tensorflow

#endif  // INTEL_MKL
