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
#include "tensorflow/lite/kernels/lstm_eval.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "ruy/profiler/instrumentation.h"  // from @ruy
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/cpu_backend_context.h"
#include "tensorflow/lite/kernels/internal/compatibility.h"
#include "tensorflow/lite/kernels/internal/kernel_utils.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/internal/tensor_utils.h"
#include "tensorflow/lite/kernels/op_macros.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace lstm_eval {
namespace {

void ComputeRowSums(
    int32_t* input_to_input_row_sums, int32_t* input_to_forget_row_sums,
    int32_t* input_to_cell_row_sums, int32_t* input_to_output_row_sums,
    int32_t* aux_input_to_input_row_sums, int32_t* aux_input_to_forget_row_sums,
    int32_t* aux_input_to_cell_row_sums, int32_t* aux_input_to_output_row_sums,
    int32_t* recurrent_to_input_row_sums, int32_t* recurrent_to_forget_row_sums,
    int32_t* recurrent_to_cell_row_sums, int32_t* recurrent_to_output_row_sums,
    int32_t* projection_weights_row_sums, int32_t* row_sums, int n_cell,
    int n_input, int n_aux_input, int n_output,
    const int8_t* input_to_input_weights_ptr,
    const int8_t* input_to_forget_weights_ptr,
    const int8_t* input_to_cell_weights_ptr,
    const int8_t* input_to_output_weights_ptr,
    const int8_t* aux_input_to_input_weights_ptr,
    const int8_t* aux_input_to_forget_weights_ptr,
    const int8_t* aux_input_to_cell_weights_ptr,
    const int8_t* aux_input_to_output_weights_ptr,
    const int8_t* recurrent_to_input_weights_ptr,
    const int8_t* recurrent_to_forget_weights_ptr,
    const int8_t* recurrent_to_cell_weights_ptr,
    const int8_t* recurrent_to_output_weights_ptr,
    const int8_t* projection_weights_ptr, bool use_cifg,
    const float* aux_input_ptr) {
  // Compute the row sums for dequantization
  if (!use_cifg) {
    std::fill_n(input_to_input_row_sums, n_cell, 0);
    tensor_utils::ReductionSumVector(input_to_input_weights_ptr,
                                     input_to_input_row_sums, n_cell, n_input);
  }
  std::fill_n(input_to_forget_row_sums, n_cell, 0);
  tensor_utils::ReductionSumVector(input_to_forget_weights_ptr,
                                   input_to_forget_row_sums, n_cell, n_input);
  std::fill_n(input_to_cell_row_sums, n_cell, 0);
  tensor_utils::ReductionSumVector(input_to_cell_weights_ptr,
                                   input_to_cell_row_sums, n_cell, n_input);
  std::fill_n(input_to_output_row_sums, n_cell, 0);
  tensor_utils::ReductionSumVector(input_to_output_weights_ptr,
                                   input_to_output_row_sums, n_cell, n_input);

  if (aux_input_ptr) {
    if (!use_cifg) {
      std::fill_n(aux_input_to_input_row_sums, n_cell, 0);
      tensor_utils::ReductionSumVector(aux_input_to_input_weights_ptr,
                                       aux_input_to_input_row_sums, n_cell,
                                       n_aux_input);
    }
    std::fill_n(aux_input_to_forget_row_sums, n_cell, 0);
    tensor_utils::ReductionSumVector(aux_input_to_forget_weights_ptr,
                                     aux_input_to_forget_row_sums, n_cell,
                                     n_aux_input);
    std::fill_n(aux_input_to_cell_row_sums, n_cell, 0);
    tensor_utils::ReductionSumVector(aux_input_to_cell_weights_ptr,
                                     aux_input_to_cell_row_sums, n_cell,
                                     n_aux_input);
    std::fill_n(aux_input_to_output_row_sums, n_cell, 0);
    tensor_utils::ReductionSumVector(aux_input_to_output_weights_ptr,
                                     aux_input_to_output_row_sums, n_cell,
                                     n_aux_input);
  }
  if (!use_cifg) {
    std::fill_n(recurrent_to_input_row_sums, n_cell, 0);
    tensor_utils::ReductionSumVector(recurrent_to_input_weights_ptr,
                                     recurrent_to_input_row_sums, n_cell,
                                     n_output);
  }
  std::fill_n(recurrent_to_forget_row_sums, n_cell, 0);
  tensor_utils::ReductionSumVector(recurrent_to_forget_weights_ptr,
                                   recurrent_to_forget_row_sums, n_cell,
                                   n_output);
  std::fill_n(recurrent_to_cell_row_sums, n_cell, 0);
  tensor_utils::ReductionSumVector(recurrent_to_cell_weights_ptr,
                                   recurrent_to_cell_row_sums, n_cell,
                                   n_output);
  std::fill_n(recurrent_to_output_row_sums, n_cell, 0);
  tensor_utils::ReductionSumVector(recurrent_to_output_weights_ptr,
                                   recurrent_to_output_row_sums, n_cell,
                                   n_output);

  if (projection_weights_ptr != nullptr) {
    std::fill_n(projection_weights_row_sums, n_output, 0);
    tensor_utils::ReductionSumVector(
        projection_weights_ptr, projection_weights_row_sums, n_output, n_cell);
  }
}

inline float GetTensorScale(const TfLiteTensor* tensor) {
  return tensor == nullptr ? 1.0f : tensor->params.scale;
}

// LINT.IfChange
// Updates the LSTM cell state, used by both float and hybrid LSTM versions.
//
// Implements the following formula:
//   cell_state_new = clip(forget_gate * cell_state + input_gate * cell_gate)
//
// With CIFG LSTM, input gate is replaced by (1-forget_gate).
//
// Parameters:
//  - n_batch, n_cell: sizes of vectors
//  - cell_state: input/output vector, size n_batch*n_cell
//  - input_gate: input vector, size n_batch*n_cell.
//  - forget_gate: input/scratch vector, size n_batch*n_cell, modified with CIFG
//  - cell_gate: input vector, size n_batch*n_cell.
//  - use_cifg: use 1-forget_gate instead of input_gate.
//  - clip: if > 0, clip the resulting cell state to [-clip, +clip].
void UpdateLstmCellFloat(int n_batch, int n_cell, float* cell_state,
                         const float* input_gate, float* forget_gate,
                         const float* cell_gate, bool use_cifg, float clip) {
  tensor_utils::VectorVectorCwiseProduct(forget_gate, cell_state,
                                         n_batch * n_cell, cell_state);

  if (use_cifg) {
    // With CIFG, input_gate = 1-forget_gate. Use the forget_gate array as
    // scratch, as input_gate array is not allocated in this case. (Be careful
    // not to write to the scratch before reading the forget gate data.)
    float* scratch = forget_gate;
    tensor_utils::Sub1Vector(forget_gate, n_batch * n_cell, scratch);
    tensor_utils::VectorVectorCwiseProductAccumulate(
        cell_gate, scratch, n_batch * n_cell, cell_state);
  } else {
    tensor_utils::VectorVectorCwiseProductAccumulate(
        cell_gate, input_gate, n_batch * n_cell, cell_state);
  }
  if (clip > 0.0f) {
    tensor_utils::CwiseClipping(cell_state, n_batch * n_cell, clip);
  }
}

// Calculates the output state tensor of an LSTM step.
//
// Implements the following formula:
//   output_no_projection = output_gate .* activate(cell_state)
//     (elementwise vector product)
// If no projection is used:
//   output = output_state = output_no_projection
// With projection:
//   output = output_state = clip(W*output_no_projection + bias)
//
// Output might not have a different 'stride' than n_batch, so we need to copy.
//
// Parameters:
//  - n_batch: batches: the number of distinct vectors in each array.
//  - n_cell, n_output: sizes of vectors.
//  - cell_state, output_gate: input vectors, size n_batch*n_cell.
//  - projection_weights, projection_weights_scale, projection_bias:
//      constant inputs, describing projection matrix and bias.
//  - proj_clip: if > 0, clip the output of the projection.
//  - output_state: output vector, size n_batch*n_output. Must be contigous.
//  - scratch: scratch area, size n_batch*n_cell.
void CalculateLstmOutputFloat(int n_batch, int n_cell, int n_output,
                              const float* cell_state, const float* output_gate,
                              TfLiteFusedActivation activation,
                              const float* projection_weights,
                              const float* projection_bias,
                              const float proj_clip, float* output_state,
                              float* scratch) {
  tensor_utils::ApplyActivationToVector(cell_state, n_batch * n_cell,
                                        activation, scratch);
  tensor_utils::VectorVectorCwiseProduct(output_gate, scratch, n_batch * n_cell,
                                         scratch);

  const bool use_projection = (projection_weights != nullptr);
  const bool use_projection_bias = (projection_bias != nullptr);

  if (use_projection) {
    if (use_projection_bias) {
      tensor_utils::VectorBatchVectorAssign(projection_bias, n_output, n_batch,
                                            output_state);
    } else {
      std::fill_n(output_state, n_batch * n_output, 0.0f);
    }
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        projection_weights, n_output, n_cell, scratch, n_batch, output_state);
    if (proj_clip > 0.0f) {
      tensor_utils::CwiseClipping(output_state, n_batch * n_output, proj_clip);
    }
  } else {
    std::copy_n(scratch, n_batch * n_output, output_state);
  }
}
// LINT.ThenChange(../tools/optimize/calibration/builtin_logging_ops/lstm.cc,\
//                 ../experimental/kernels/fp16/lstm_eval.cc)

// Calculates the output state tensor of an LSTM step. See Float version too.
//
// Parameters:
//  - n_batch: batches: the number of distinct vectors in each array.
//  - n_cell, n_output: sizes of vectors.
//  - cell_state, output_gate: input vectors, size n_batch*n_cell.
//  - projection_weights, projection_weights_scale, projection_bias:
//      constant inputs, describing projection matrix and bias.
//  - proj_clip: if > 0, clip the output of the projection.
//  - output_state: output vector, size n_batch*n_output. Must be contigous.
//  - asymmetric_quantize_inputs: parameter to control quantization.
//  - projection_weights_row_sums, compute_row_sums, context: Data for optimized
//      MatrixBatchVectorMultiplyAccumulate.
//  - scratch0: scratch area of size n_batch*n_cell
//  - scratch1: scratch area of size n_batch*n_cell
//  - scratch2: scratch area of size n_batch
//  - scratch3: scratch area of size n_batch
//  - scratch4: scratch area used by MatrixBatchVectorMultiplyAccumulate
void CalculateLstmOutputHybrid(
    int n_batch, int n_cell, int n_output, const float* cell_state,
    const float* output_gate, TfLiteFusedActivation activation,
    const int8_t* projection_weights, float projection_weights_scale,
    const float* projection_bias, const float proj_clip, float* output_state,
    bool asymmetric_quantize_inputs, int32_t* projection_weights_row_sums,
    bool* compute_row_sums, CpuBackendContext* context, float* scratch0,
    int8_t* scratch1, float* scratch2, int32_t* scratch3, int32_t* scratch4) {
  tensor_utils::ApplyActivationToVector(cell_state, n_batch * n_cell,
                                        activation, scratch0);
  tensor_utils::VectorVectorCwiseProduct(output_gate, scratch0,
                                         n_batch * n_cell, scratch0);

  const bool use_projection = (projection_weights != nullptr);
  const bool use_projection_bias = (projection_bias != nullptr);

  if (use_projection) {
    if (use_projection_bias) {
      tensor_utils::VectorBatchVectorAssign(projection_bias, n_output, n_batch,
                                            output_state);
    } else {
      std::fill_n(output_state, n_batch * n_output, 0.0f);
    }
    if (!tensor_utils::IsZeroVector(scratch0, n_batch * n_cell)) {
      // Save quantization and matmul computation for all zero output.
      tensor_utils::BatchQuantizeFloats(scratch0, n_batch, n_cell, scratch1,
                                        scratch2, scratch3,
                                        asymmetric_quantize_inputs);
      tensor_utils::MatrixBatchVectorMultiplyAccumulate(
          projection_weights, n_output, n_cell, scratch1,
          projection_weights_scale, scratch2, n_batch, output_state,
          /*per_channel_scale=*/nullptr, scratch3, scratch4,
          projection_weights_row_sums, compute_row_sums, scratch2, context);
    }
    if (proj_clip > 0.0f) {
      tensor_utils::CwiseClipping(output_state, n_batch * n_output, proj_clip);
    }
  } else {
    std::copy_n(scratch0, n_batch * n_output, output_state);
  }
}

// Updates the LSTM cell state, used by both integer LSTM versions.
// Also see UpdateLstmCellFloat.
//
// Parameters:
//  - n_batch, n_cell: sizes of vectors
//  - cell_state: input/output vector, size n_batch*n_cell
//  - cell_state_scale: scaling factor of cell state.
//  - input_gate: input vector, size n_batch*n_cell.
//  - forget_gate: input/scratch vector, size n_batch*n_cell, always modified.
//  - cell_gate: input vector, size n_batch*n_cell.
//  - use_cifg: use 1-forget_gate instead of input_gate.
//  - clip: if > 0, clip the resulting cell state to [-clip, +clip].
void UpdateLstmCellInteger(int n_batch, int n_cell, int16_t* cell_state,
                           int32_t cell_state_scale, const int16_t* input_gate,
                           int16_t* forget_gate, const int16_t* cell_gate,
                           bool use_cifg, int16_t clip) {
  // Use the forget_gate array as scratch, as input_gate array is not allocated
  // in CIFG case. (Be careful not to write to the scratch before reading the
  // forget gate data.)
  int16_t* scratch = forget_gate;

  tensor_utils::CwiseMul(forget_gate, cell_state, n_batch, n_cell, 15,
                         cell_state);
  if (use_cifg) {
    tensor_utils::Sub1Vector(forget_gate, n_batch * n_cell, scratch);
    tensor_utils::CwiseMul(scratch, cell_gate, n_batch, n_cell,
                           30 + cell_state_scale, scratch);
  } else {
    tensor_utils::CwiseMul(input_gate, cell_gate, n_batch, n_cell,
                           30 + cell_state_scale, scratch);
  }
  tensor_utils::CwiseAdd(cell_state, scratch, n_batch, n_cell, cell_state);

  if (clip > 0) {
    tensor_utils::CwiseClipping(cell_state, n_batch * n_cell, clip);
  }
}

// Calculates the output state tensor of an LSTM step. See Float and hybrid
// versions as well.
//
// Parameters:
//  - n_batch: batches: the number of distinct vectors in each array.
//  - n_cell, n_output: sizes of vectors.
//  - cell_state, output_gate: input vectors, size n_batch*n_cell.
//  - cell_state_scale: scaling of cell_state.
//  - effective_hidden_scale_[a|b]: effective scale of cell_state.*output_gate
//  - hidden_zp: zero_point for cell_state.*output_gate
//  - projection_weights, effective_proj_scale_[a|b], projection_effective_bias:
//      constant inputs, describing projection matrix and bias.
//  - output_state_zp: zero point of output_state. (Input, calibrated value.)
//  - quantized_proj_clip: if > 0, clip the output of the projection.
//  - output_state: output vector, size n_batch*n_output. Must be contigous.
//  - context: data for optimized MatrixBatchVectorMultiplyAccumulate.
//  - scratch0: scratch area of size n_batch*n_cell
//  - scratch1: scratch area of size n_batch*n_cell
//  - scratch2: scratch area used by MatrixBatchVectorMultiplyAccumulate
void CalculateLstmOutputInteger8x8_16(
    int n_batch, int n_cell, int n_output, const int16_t* cell_state,
    int32_t cell_state_scale, const int16_t* output_gate,
    int32_t effective_hidden_scale_a, int32_t effective_hidden_scale_b,
    int32_t hidden_zp, const int8_t* projection_weights,
    int32_t effective_proj_scale_a, int32_t effective_proj_scale_b,
    const int32_t* projection_effective_bias, int32_t output_state_zp,
    int8_t quantized_proj_clip, int8_t* output_state,
    CpuBackendContext* context, int16_t* scratch0, int8_t* scratch1,
    int32_t* scratch2) {
  // Note: unlike float/hybrid, the activation is always Tanh.
  tensor_utils::ApplyTanh(15 + cell_state_scale, cell_state, n_batch, n_cell,
                          scratch0);
  tensor_utils::CwiseMul(output_gate, scratch0, effective_hidden_scale_a,
                         effective_hidden_scale_b, n_batch, n_cell, hidden_zp,
                         scratch1);

  const bool use_projection = (projection_weights != nullptr);

  if (use_projection) {
    // Note: no bias like in float/hybrid
    std::fill_n(output_state, n_batch * n_output, 0);
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        scratch1, projection_effective_bias, projection_weights,
        effective_proj_scale_a, effective_proj_scale_b, n_batch, n_cell,
        n_output, output_state_zp, scratch2, output_state, context);
    if (quantized_proj_clip > 0) {
      tensor_utils::CwiseClipping(output_state, n_batch * n_output,
                                  quantized_proj_clip);
    }
  } else {
    std::copy_n(scratch1, n_batch * n_output, output_state);
  }
}

// Calculates the output state tensor of an LSTM step. See Float and hybrid
// versions as well.
//
// Parameters:
//  - n_batch: batches: the number of distinct vectors in each array.
//  - n_cell, n_output: sizes of vectors.
//  - cell_state, output_gate: input vectors, size n_batch*n_cell.
//  - projection_weights, effective_proj_scale_[a|b], projection_bias:
//      constant inputs, describing projection matrix and bias.
//  - output_state_zp: zero point of the output state.
//  - quantized_proj_clip: if > 0, clip the output of the projection.
//  - output_state: output vector, size n_batch*n_output. Must be contigous.
//  - scratch: scratch area of size n_batch*n_cell
void CalculateLstmOutputInteger8x8_8(
    int n_batch, int n_cell, int n_output, const int16_t* cell_state,
    const int16_t* output_gate, const int8_t* projection_weights,
    int32_t effective_proj_scale_a, int32_t effective_proj_scale_b,
    const int32_t* projection_bias, int32_t output_state_zp,
    int32_t quantized_proj_clip, int8_t* output_state, int16_t* scratch) {
  // Note: unlike float/hybrid, the activation is always Tanh.
  tensor_utils::ApplyTanhFloat(cell_state, n_batch, n_cell, -15, scratch);
  tensor_utils::CwiseMul(output_gate, scratch, n_batch, n_cell, 15 + 15 - 15,
                         scratch);
  // Note: no bias like in float/hybrid
  tensor_utils::MatrixBatchVectorMultiply(
      scratch, projection_weights, effective_proj_scale_a,
      effective_proj_scale_b, projection_bias, n_batch, n_cell, n_output,
      output_state_zp, output_state);
  if (quantized_proj_clip > 0) {
    tensor_utils::CwiseClipping(output_state, n_batch * n_output,
                                quantized_proj_clip);
  }
}

// Performs an LSTM batch inference step for input specified by input_ptr.
// The LSTM cell is specified by the pointers to its weights (*_weights_ptr) and
// biases (*_bias_ptr), and buffers (*_scratch), along with additional
// parameters:
//  - params: various LSTM params including activation, clipping, etc.,
//  - n_batch: size of batch,
//  - n_cell: number of cells (or units),
//  - n_input: the input size,
//  - n_aux_input: the auxiliary input size.
//  - n_output: the output size.
//  - output_batch_leading_dim: the leading dimension of the output buffer.
//
// Input of size 'n_batch * n_input':
//   input_ptr
// Input of size 'n_batch * n_aux_input':
//   aux_input_ptr                     - optional (can be nullptr)
//
// LSTM weights:
// Input weights of size 'n_cell * n_input':
//   input_to_input_weights            - optional
//   input_to_forget_weights
//   input_to_cell_weights
//   input_to_output_weights
// Auxiliary input weights of size 'n_cell * n_aux_input':
//   aux_input_to_input_weights        - optional
//   aux_input_to_forget_weights       - optional
//   aux_input_to_cell_weights         - optional
//   aux_input_to_output_weights       - optional
// Recurrent weights of size 'n_cell * n_output':
//   recurrent_to_input_weights        - optional
//   recurrent_to_forget_weights
//   recurrent_to_cell_weights
//   recurrent_to_input_weights
// Peephole weights of size 'n_cell', representing diagonal matrices.
//   cell_to_input_weights             - optional
//   cell_to_cell_weights              - optional
//   cell_to_output_weights            - optional
// Projection weights of size 'n_output * n_cell'
//   projection_weights_ptr            - optional
// Gate biases of size 'n_cell':
//   input_gate_bias_ptr               - optional
//   forget_gate_bias_ptr
//   cell_gate_bias_ptr
//   output_gate_bias_ptr
//
// Layer norm coefficients of size 'n_cell', representing diagonal matrices.
//   input_layer_norm_coefficients_ptr  - optional
//   forget_layer_norm_coefficients_ptr - optional
//   cell_layer_norm_coefficients_ptr   - optional
//   output_layer_norm_coefficients_ptr - optional
//
// The pointers to the cell and output state and the output are updated.
//
// The pointers input_ptr, aux_input_ptr, and output_ptr point to data aligned
// in batch_major order, and each step processes batch_size many inputs from
// input_ptr, and updates batch_size many cell and output states.
//
// The output_batch_dim is output.shape[-1], i.e. the outermost dimension of the
// output tensor, and in most cases will be equal to n_output. It is usually not
// when we want to store the LSTM output into a slice of the output tensor, e.g.
// for bidirectional LSTMs with merge_outputs. In this case, the batched
// operations cannot be used since they assume that the batched outputs are
// contiguous, and we manually loop over the batched outputs.
// LINT.IfChange
inline void LstmStepFloat(
    const float* input_ptr, const float* input_to_input_weights_ptr,
    const float* input_to_forget_weights_ptr,
    const float* input_to_cell_weights_ptr,
    const float* input_to_output_weights_ptr, const float* aux_input_ptr,
    const float* aux_input_to_input_weights_ptr,
    const float* aux_input_to_forget_weights_ptr,
    const float* aux_input_to_cell_weights_ptr,
    const float* aux_input_to_output_weights_ptr,
    const float* recurrent_to_input_weights_ptr,
    const float* recurrent_to_forget_weights_ptr,
    const float* recurrent_to_cell_weights_ptr,
    const float* recurrent_to_output_weights_ptr,
    const float* cell_to_input_weights_ptr,
    const float* cell_to_forget_weights_ptr,
    const float* cell_to_output_weights_ptr,
    const float* input_layer_norm_coefficients_ptr,
    const float* forget_layer_norm_coefficients_ptr,
    const float* cell_layer_norm_coefficients_ptr,
    const float* output_layer_norm_coefficients_ptr,
    const float* input_gate_bias_ptr, const float* forget_gate_bias_ptr,
    const float* cell_gate_bias_ptr, const float* output_gate_bias_ptr,
    const float* projection_weights_ptr, const float* projection_bias_ptr,
    const TfLiteLSTMParams* params, int n_batch, int n_cell, int n_input,
    int n_aux_input, int n_output, int output_batch_leading_dim,
    float* output_state_ptr, float* cell_state_ptr, float* scratch0,
    float* scratch1, float* scratch2, float* scratch3, float* output_ptr) {
  ruy::profiler::ScopeLabel label("LstmStepFloat");
  // Since we have already checked that weights are all there or none, we can
  // check the existence of only one to the get the condition.
  const bool use_cifg = (input_to_input_weights_ptr == nullptr);
  const bool use_peephole = (cell_to_output_weights_ptr != nullptr);
  const bool use_layer_norm = (forget_layer_norm_coefficients_ptr != nullptr);

  // Make named scratch buffers for the different gates.
  float* input_gate_scratch = scratch0;
  float* forget_gate_scratch = scratch1;
  float* cell_gate_scratch = scratch2;
  float* output_gate_scratch = scratch3;

  const bool is_input_all_zeros =
      tensor_utils::IsZeroVector(input_ptr, n_batch * n_input);
  const bool is_aux_input_all_zeros =
      (aux_input_ptr == nullptr ||
       tensor_utils::IsZeroVector(aux_input_ptr, n_batch * n_aux_input));

  // Initialize scratch buffers with bias for regular lstm or initialize with
  // zero for layer norm lstm.
  if (use_layer_norm) {
    if (!use_cifg) {
      std::fill_n(input_gate_scratch, n_cell * n_batch, 0.0f);
    }
    std::fill_n(forget_gate_scratch, n_cell * n_batch, 0.0f);
    std::fill_n(cell_gate_scratch, n_cell * n_batch, 0.0f);
    std::fill_n(output_gate_scratch, n_cell * n_batch, 0.0f);
  } else {
    if (!use_cifg) {
      tensor_utils::VectorBatchVectorAssign(input_gate_bias_ptr, n_cell,
                                            n_batch, input_gate_scratch);
    }
    tensor_utils::VectorBatchVectorAssign(forget_gate_bias_ptr, n_cell, n_batch,
                                          forget_gate_scratch);
    tensor_utils::VectorBatchVectorAssign(cell_gate_bias_ptr, n_cell, n_batch,
                                          cell_gate_scratch);
    tensor_utils::VectorBatchVectorAssign(output_gate_bias_ptr, n_cell, n_batch,
                                          output_gate_scratch);
  }

  // For each batch and cell: compute input_weight * input.
  // Skip if input is all zeros.
  if (!is_input_all_zeros) {
    if (!use_cifg) {
      tensor_utils::MatrixBatchVectorMultiplyAccumulate(
          input_to_input_weights_ptr, n_cell, n_input, input_ptr, n_batch,
          input_gate_scratch);
    }

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_to_forget_weights_ptr, n_cell, n_input, input_ptr, n_batch,
        forget_gate_scratch);
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_to_cell_weights_ptr, n_cell, n_input, input_ptr, n_batch,
        cell_gate_scratch);
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_to_output_weights_ptr, n_cell, n_input, input_ptr, n_batch,
        output_gate_scratch);
  }

  // For each batch and cell: compute aux_input_weight * aux_input.
  // Skip if auxiliary input is not available or all zeros.
  if (!is_aux_input_all_zeros) {
    if (!use_cifg) {
      tensor_utils::MatrixBatchVectorMultiplyAccumulate(
          aux_input_to_input_weights_ptr, n_cell, n_aux_input, aux_input_ptr,
          n_batch, input_gate_scratch);
    }

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        aux_input_to_forget_weights_ptr, n_cell, n_aux_input, aux_input_ptr,
        n_batch, forget_gate_scratch);
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        aux_input_to_cell_weights_ptr, n_cell, n_aux_input, aux_input_ptr,
        n_batch, cell_gate_scratch);
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        aux_input_to_output_weights_ptr, n_cell, n_aux_input, aux_input_ptr,
        n_batch, output_gate_scratch);
  }

  // For each batch and cell: compute recurrent_weight * output_state.
  if (!use_cifg) {
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        recurrent_to_input_weights_ptr, n_cell, n_output, output_state_ptr,
        n_batch, input_gate_scratch);
  }
  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      recurrent_to_forget_weights_ptr, n_cell, n_output, output_state_ptr,
      n_batch, forget_gate_scratch);
  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      recurrent_to_cell_weights_ptr, n_cell, n_output, output_state_ptr,
      n_batch, cell_gate_scratch);
  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      recurrent_to_output_weights_ptr, n_cell, n_output, output_state_ptr,
      n_batch, output_gate_scratch);

  // For each batch and cell: update input gate.
  if (!use_cifg) {
    if (use_peephole) {
      tensor_utils::VectorBatchVectorCwiseProductAccumulate(
          cell_to_input_weights_ptr, n_cell, cell_state_ptr, n_batch,
          input_gate_scratch);
    }
    if (use_layer_norm) {
      tensor_utils::MeanStddevNormalization(
          input_gate_scratch, input_gate_scratch, n_cell, n_batch);
      tensor_utils::VectorBatchVectorCwiseProduct(
          input_layer_norm_coefficients_ptr, n_cell, input_gate_scratch,
          n_batch, input_gate_scratch);
      tensor_utils::VectorBatchVectorAdd(input_gate_bias_ptr, n_cell, n_batch,
                                         input_gate_scratch);
    }
    tensor_utils::ApplySigmoidToVector(input_gate_scratch, n_cell * n_batch,
                                       input_gate_scratch);
  }

  // For each batch and cell: update forget gate.
  if (use_peephole) {
    tensor_utils::VectorBatchVectorCwiseProductAccumulate(
        cell_to_forget_weights_ptr, n_cell, cell_state_ptr, n_batch,
        forget_gate_scratch);
  }
  if (use_layer_norm) {
    tensor_utils::MeanStddevNormalization(forget_gate_scratch,
                                          forget_gate_scratch, n_cell, n_batch);
    tensor_utils::VectorBatchVectorCwiseProduct(
        forget_layer_norm_coefficients_ptr, n_cell, forget_gate_scratch,
        n_batch, forget_gate_scratch);
    tensor_utils::VectorBatchVectorAdd(forget_gate_bias_ptr, n_cell, n_batch,
                                       forget_gate_scratch);
  }
  tensor_utils::ApplySigmoidToVector(forget_gate_scratch, n_cell * n_batch,
                                     forget_gate_scratch);

  // For each batch and cell: update the cell.
  if (use_layer_norm) {
    tensor_utils::MeanStddevNormalization(cell_gate_scratch, cell_gate_scratch,
                                          n_cell, n_batch);
    tensor_utils::VectorBatchVectorCwiseProduct(
        cell_layer_norm_coefficients_ptr, n_cell, cell_gate_scratch, n_batch,
        cell_gate_scratch);
    tensor_utils::VectorBatchVectorAdd(cell_gate_bias_ptr, n_cell, n_batch,
                                       cell_gate_scratch);
  }
  tensor_utils::ApplyActivationToVector(cell_gate_scratch, n_batch * n_cell,
                                        params->activation, cell_gate_scratch);

  UpdateLstmCellFloat(n_batch, n_cell, cell_state_ptr, input_gate_scratch,
                      forget_gate_scratch, cell_gate_scratch, use_cifg,
                      params->cell_clip);

  // For each batch and cell: update the output gate.
  if (use_peephole) {
    tensor_utils::VectorBatchVectorCwiseProductAccumulate(
        cell_to_output_weights_ptr, n_cell, cell_state_ptr, n_batch,
        output_gate_scratch);
  }
  if (use_layer_norm) {
    tensor_utils::MeanStddevNormalization(output_gate_scratch,
                                          output_gate_scratch, n_cell, n_batch);
    tensor_utils::VectorBatchVectorCwiseProduct(
        output_layer_norm_coefficients_ptr, n_cell, output_gate_scratch,
        n_batch, output_gate_scratch);
    tensor_utils::VectorBatchVectorAdd(output_gate_bias_ptr, n_cell, n_batch,
                                       output_gate_scratch);
  }
  tensor_utils::ApplySigmoidToVector(output_gate_scratch, n_batch * n_cell,
                                     output_gate_scratch);

  CalculateLstmOutputFloat(n_batch, n_cell, n_output, cell_state_ptr,
                           output_gate_scratch, params->activation,
                           projection_weights_ptr, projection_bias_ptr,
                           params->proj_clip, output_state_ptr, scratch2);

  // Copy output_state to the output. Note that the output batch rows may not be
  // contiguous (output_batch_leading_dim != n_output).
  for (int b = 0; b < n_batch; b++) {
    std::copy_n(output_state_ptr + b * n_output, n_output,
                output_ptr + b * output_batch_leading_dim);
  }
}
// LINT.ThenChange(../tools/optimize/calibration/builtin_logging_ops/lstm.cc,\
//                 ../experimental/kernels/fp16/lstm_eval.cc)

// Same as above but with quantized weight matrices. In detail:
// Input of size 'n_batch * n_input':
//   input_ptr
// Input of size 'n_batch * n_aux_input':
//   aux_input_ptr                     - optional (can be nullptr)
//
// LSTM weights:
// Quantized input weights of size 'n_cell * n_input':
//   input_to_input_weights            - optional
//   input_to_forget_weights
//   input_to_cell_weights
//   input_to_input_weights
// Quantized auxiliary input weights of size 'n_cell * n_aux_input':
//   aux_input_to_input_weights        - optional
//   aux_input_to_forget_weights       - optional
//   aux_input_to_cell_weights         - optional
//   aux_input_to_output_weights       - optional
// Quantized recurrent weights of size 'n_cell * n_output':
//   recurrent_to_input_weights        - optional
//   recurrent_to_forget_weights
//   recurrent_to_cell_weights
//   recurrent_to_input_weights
// Quantized peephole weights of size 'n_cell', representing diagonal matrices.
//   cell_to_input_weights             - optional
//   cell_to_cell_weights              - optional
//   cell_to_output_weights            - optional
// Quantized projection weights of size 'n_output * n_cell'
//   projection_weights_ptr            - optional
// Weight scales (scalars) for each of the weights above.
//   input_to_input_weights_scale      - optional
//   input_to_forget_weights_scale
//   input_to_cell_weights_scale
//   input_to_output_weights_scale
//   aux_input_to_input_weights_scale  - optional
//   aux_input_to_forget_weights_scale - optional
//   aux_input_to_cell_weights_scale   - optional
//   aux_input_to_output_weights_scale - optional
//   recurrent_to_input_weights_scale  - optional
//   recurrent_to_forget_weights_scale
//   recurrent_to_cell_weights_scale
//   recurrent_to_output_weights_scale
//   cell_to_input_weights_scale,
//   cell_to_forget_weights_scale,
//   cell_to_output_weights_scale,
//   projection_weights_scale          - optional
// Gate biases of size 'n_cell':
//   input_gate_bias_ptr               - optional
//   forget_gate_bias_ptr
//   cell_gate_bias_ptr
//   output_gate_bias_ptr
//
// Layer norm coefficients of size 'n_cell', representing diagonal matrices.
//   input_layer_norm_coefficients_ptr  - optional
//   forget_layer_norm_coefficients_ptr - optional
//   cell_layer_norm_coefficients_ptr   - optional
//   output_layer_norm_coefficients_ptr - optional
//
// Temporary pre-allocated storage for quantized values:
//   quantized_input_ptr (same size as input_ptr)
//   quantized_output_state_ptr (same size as output_state_ptr)
//   quantized_output_scratch (same size as cell_state_ptr)
// Temporary pre-allocated storage for recovered values:
//   recovered_cell_weights (same size as cell_to_*_weights)
//
// Outputs:
//   output_state_ptr - size 'n_batch * n_output'
//   cell_state_ptr   - size 'n_batch * n_cell'
//   output_ptr       - size 'n_batch * output_batch_leading_dim'
inline void LstmStepHybrid(
    const float* input_ptr, const int8_t* input_to_input_weights_ptr,
    float input_to_input_weights_scale,
    const int8_t* input_to_forget_weights_ptr,
    float input_to_forget_weights_scale,
    const int8_t* input_to_cell_weights_ptr, float input_to_cell_weights_scale,
    const int8_t* input_to_output_weights_ptr,
    float input_to_output_weights_scale, const float* aux_input_ptr,
    const int8_t* aux_input_to_input_weights_ptr,
    float aux_input_to_input_weights_scale,
    const int8_t* aux_input_to_forget_weights_ptr,
    float aux_input_to_forget_weights_scale,
    const int8_t* aux_input_to_cell_weights_ptr,
    float aux_input_to_cell_weights_scale,
    const int8_t* aux_input_to_output_weights_ptr,
    float aux_input_to_output_weights_scale,
    const int8_t* recurrent_to_input_weights_ptr,
    float recurrent_to_input_weights_scale,
    const int8_t* recurrent_to_forget_weights_ptr,
    float recurrent_to_forget_weights_scale,
    const int8_t* recurrent_to_cell_weights_ptr,
    float recurrent_to_cell_weights_scale,
    const int8_t* recurrent_to_output_weights_ptr,
    float recurrent_to_output_weights_scale,
    const int8_t* cell_to_input_weights_ptr, float cell_to_input_weights_scale,
    const int8_t* cell_to_forget_weights_ptr,
    float cell_to_forget_weights_scale,
    const int8_t* cell_to_output_weights_ptr,
    float cell_to_output_weights_scale,
    const float* input_layer_norm_coefficients_ptr,
    const float* forget_layer_norm_coefficients_ptr,
    const float* cell_layer_norm_coefficients_ptr,
    const float* output_layer_norm_coefficients_ptr,
    const float* input_gate_bias_ptr, const float* forget_gate_bias_ptr,
    const float* cell_gate_bias_ptr, const float* output_gate_bias_ptr,
    const int8_t* projection_weights_ptr, float projection_weights_scale,
    const float* projection_bias_ptr, const TfLiteLSTMParams* params,
    int n_batch, int n_cell, int n_input, int n_aux_input, int n_output,
    int output_batch_leading_dim, float* scratch0, float* scratch1,
    float* scratch2, float* scratch3, float* input_sf, float* aux_input_sf,
    float* output_state_sf, float* scaling_factors_scratch,
    float* recovered_cell_weights, int8_t* quantized_input_ptr,
    int8_t* quantized_aux_input_ptr, int8_t* quantized_output_state_ptr,
    int8_t* quantized_output_scratch, float* output_state_ptr,
    float* cell_state_ptr, int32_t* accum_scratch_ptr, float* output_ptr,
    int32_t* input_zp, int32_t* aux_input_zp, int32_t* output_state_zp,
    int32_t* row_sums, int row_sums_size, bool* compute_row_sums,
    bool asymmetric_quantize_inputs, CpuBackendContext* context) {
  ruy::profiler::ScopeLabel label("LstmStepHybrid");
  // Since we have already checked that weights are all there or none, we
  // can check the existence of only one to the get the condition.
  const bool use_cifg = (input_to_input_weights_ptr == nullptr);
  const bool use_peephole = (cell_to_output_weights_ptr != nullptr);
  const bool use_layer_norm = (forget_layer_norm_coefficients_ptr != nullptr);

  // Make named scratch buffers for the different gates.
  float* input_gate_scratch = scratch0;
  float* forget_gate_scratch = scratch1;
  float* cell_gate_scratch = scratch2;
  float* output_gate_scratch = scratch3;

  int32_t* input_to_input_row_sums = nullptr;
  int32_t* input_to_forget_row_sums = nullptr;
  int32_t* input_to_cell_row_sums = nullptr;
  int32_t* input_to_output_row_sums = nullptr;
  int32_t* aux_input_to_input_row_sums = nullptr;
  int32_t* aux_input_to_forget_row_sums = nullptr;
  int32_t* aux_input_to_cell_row_sums = nullptr;
  int32_t* aux_input_to_output_row_sums = nullptr;
  int32_t* recurrent_to_input_row_sums = nullptr;
  int32_t* recurrent_to_forget_row_sums = nullptr;
  int32_t* recurrent_to_cell_row_sums = nullptr;
  int32_t* recurrent_to_output_row_sums = nullptr;
  int32_t* projection_weights_row_sums = nullptr;

  if (asymmetric_quantize_inputs) {
    int num_row_sums = use_cifg ? 6 : 8;
    if (aux_input_ptr != nullptr) {
      num_row_sums += use_cifg ? 3 : 4;
    }
    if (projection_weights_ptr != nullptr) {
      num_row_sums += ceil(static_cast<float>(n_output) / n_cell);
    }
    TF_LITE_ASSERT(row_sums_size == num_row_sums);
    input_to_input_row_sums = row_sums;
    input_to_forget_row_sums =
        use_cifg ? input_to_input_row_sums : input_to_input_row_sums + n_cell;
    input_to_cell_row_sums = input_to_forget_row_sums + n_cell;
    input_to_output_row_sums = input_to_cell_row_sums + n_cell;
    if (aux_input_ptr != nullptr) {
      aux_input_to_input_row_sums = input_to_output_row_sums + n_cell;
      aux_input_to_forget_row_sums = use_cifg
                                         ? aux_input_to_input_row_sums
                                         : aux_input_to_input_row_sums + n_cell;
      aux_input_to_cell_row_sums = aux_input_to_forget_row_sums + n_cell;
      aux_input_to_output_row_sums = aux_input_to_cell_row_sums + n_cell;
    }
    recurrent_to_input_row_sums = aux_input_ptr
                                      ? aux_input_to_output_row_sums + n_cell
                                      : input_to_output_row_sums + n_cell;
    recurrent_to_forget_row_sums = use_cifg
                                       ? recurrent_to_input_row_sums
                                       : recurrent_to_input_row_sums + n_cell;
    recurrent_to_cell_row_sums = recurrent_to_forget_row_sums + n_cell;
    recurrent_to_output_row_sums = recurrent_to_cell_row_sums + n_cell;
    if (projection_weights_ptr != nullptr) {
      projection_weights_row_sums = recurrent_to_output_row_sums + n_cell;
    }
    if (*compute_row_sums) {
      ComputeRowSums(
          input_to_input_row_sums, input_to_forget_row_sums,
          input_to_cell_row_sums, input_to_output_row_sums,
          aux_input_to_input_row_sums, aux_input_to_forget_row_sums,
          aux_input_to_cell_row_sums, aux_input_to_output_row_sums,
          recurrent_to_input_row_sums, recurrent_to_forget_row_sums,
          recurrent_to_cell_row_sums, recurrent_to_output_row_sums,
          projection_weights_row_sums, row_sums, n_cell, n_input, n_aux_input,
          n_output, input_to_input_weights_ptr, input_to_forget_weights_ptr,
          input_to_cell_weights_ptr, input_to_output_weights_ptr,
          aux_input_to_input_weights_ptr, aux_input_to_forget_weights_ptr,
          aux_input_to_cell_weights_ptr, aux_input_to_output_weights_ptr,
          recurrent_to_input_weights_ptr, recurrent_to_forget_weights_ptr,
          recurrent_to_cell_weights_ptr, recurrent_to_output_weights_ptr,
          projection_weights_ptr, use_cifg, aux_input_ptr);
      *compute_row_sums = false;
    }
  }

  const bool is_input_all_zeros =
      tensor_utils::IsZeroVector(input_ptr, n_batch * n_input);
  const bool is_aux_input_all_zeros =
      (aux_input_ptr == nullptr ||
       tensor_utils::IsZeroVector(aux_input_ptr, n_batch * n_aux_input));
  const bool is_output_state_all_zeros =
      tensor_utils::IsZeroVector(output_state_ptr, n_batch * n_output);

  if (!is_input_all_zeros) {
    tensor_utils::BatchQuantizeFloats(input_ptr, n_batch, n_input,
                                      quantized_input_ptr, input_sf, input_zp,
                                      asymmetric_quantize_inputs);
  }
  if (!is_aux_input_all_zeros) {
    tensor_utils::BatchQuantizeFloats(aux_input_ptr, n_batch, n_aux_input,
                                      quantized_aux_input_ptr, aux_input_sf,
                                      aux_input_zp, asymmetric_quantize_inputs);
  }
  if (!is_output_state_all_zeros) {
    tensor_utils::BatchQuantizeFloats(
        output_state_ptr, n_batch, n_output, quantized_output_state_ptr,
        output_state_sf, output_state_zp, asymmetric_quantize_inputs);
  }

  // Initialize scratch buffers with bias for regular lstm or initialize with
  // zero for layer norm lstm.
  if (use_layer_norm) {
    if (!use_cifg) {
      std::fill_n(input_gate_scratch, n_cell * n_batch, 0.0f);
    }
    std::fill_n(forget_gate_scratch, n_cell * n_batch, 0.0f);
    std::fill_n(cell_gate_scratch, n_cell * n_batch, 0.0f);
    std::fill_n(output_gate_scratch, n_cell * n_batch, 0.0f);
  } else {
    if (!use_cifg) {
      tensor_utils::VectorBatchVectorAssign(input_gate_bias_ptr, n_cell,
                                            n_batch, input_gate_scratch);
    }
    tensor_utils::VectorBatchVectorAssign(forget_gate_bias_ptr, n_cell, n_batch,
                                          forget_gate_scratch);
    tensor_utils::VectorBatchVectorAssign(cell_gate_bias_ptr, n_cell, n_batch,
                                          cell_gate_scratch);
    tensor_utils::VectorBatchVectorAssign(output_gate_bias_ptr, n_cell, n_batch,
                                          output_gate_scratch);
  }

  if (!is_input_all_zeros) {
    if (!use_cifg) {
      tensor_utils::MatrixBatchVectorMultiplyAccumulate(
          input_to_input_weights_ptr, n_cell, n_input, quantized_input_ptr,
          input_to_input_weights_scale, input_sf, n_batch, input_gate_scratch,
          /*per_channel_scale=*/nullptr, input_zp, accum_scratch_ptr,
          input_to_input_row_sums, compute_row_sums, scaling_factors_scratch,
          context);
    }

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_to_forget_weights_ptr, n_cell, n_input, quantized_input_ptr,
        input_to_forget_weights_scale, input_sf, n_batch, forget_gate_scratch,
        /*per_channel_scale=*/nullptr, input_zp, accum_scratch_ptr,
        input_to_forget_row_sums, compute_row_sums, scaling_factors_scratch,
        context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_to_cell_weights_ptr, n_cell, n_input, quantized_input_ptr,
        input_to_cell_weights_scale, input_sf, n_batch, cell_gate_scratch,
        /*per_channel_scale=*/nullptr, input_zp, accum_scratch_ptr,
        input_to_cell_row_sums, compute_row_sums, scaling_factors_scratch,
        context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_to_output_weights_ptr, n_cell, n_input, quantized_input_ptr,
        input_to_output_weights_scale, input_sf, n_batch, output_gate_scratch,
        /*per_channel_scale=*/nullptr, input_zp, accum_scratch_ptr,
        input_to_output_row_sums, compute_row_sums, scaling_factors_scratch,
        context);
  }

  // For each batch and cell: compute aux_input_weight * aux_input.
  // Skip if auxiliary input is not available or all zeros.
  if (!is_aux_input_all_zeros) {
    if (!use_cifg) {
      tensor_utils::MatrixBatchVectorMultiplyAccumulate(
          aux_input_to_input_weights_ptr, n_cell, n_aux_input,
          quantized_aux_input_ptr, aux_input_to_input_weights_scale,
          aux_input_sf, n_batch, input_gate_scratch,
          /*per_channel_scale=*/nullptr, aux_input_zp, accum_scratch_ptr,
          aux_input_to_input_row_sums, compute_row_sums,
          scaling_factors_scratch, context);
    }

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        aux_input_to_forget_weights_ptr, n_cell, n_aux_input,
        quantized_aux_input_ptr, aux_input_to_forget_weights_scale,
        aux_input_sf, n_batch, forget_gate_scratch,
        /*per_channel_scale=*/nullptr, aux_input_zp, accum_scratch_ptr,
        aux_input_to_forget_row_sums, compute_row_sums, scaling_factors_scratch,
        context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        aux_input_to_cell_weights_ptr, n_cell, n_aux_input,
        quantized_aux_input_ptr, aux_input_to_cell_weights_scale, aux_input_sf,
        n_batch, cell_gate_scratch, /*per_channel_scale=*/nullptr, aux_input_zp,
        accum_scratch_ptr, aux_input_to_cell_row_sums, compute_row_sums,
        scaling_factors_scratch, context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        aux_input_to_output_weights_ptr, n_cell, n_aux_input,
        quantized_aux_input_ptr, aux_input_to_output_weights_scale,
        aux_input_sf, n_batch, output_gate_scratch,
        /*per_channel_scale=*/nullptr, aux_input_zp, accum_scratch_ptr,
        aux_input_to_output_row_sums, compute_row_sums, scaling_factors_scratch,
        context);
  }

  if (!is_output_state_all_zeros) {
    // For each batch and cell: compute recurrent_weight * output_state.
    if (!use_cifg) {
      tensor_utils::MatrixBatchVectorMultiplyAccumulate(
          recurrent_to_input_weights_ptr, n_cell, n_output,
          quantized_output_state_ptr, recurrent_to_input_weights_scale,
          output_state_sf, n_batch, input_gate_scratch,
          /*per_channel_scale=*/nullptr, output_state_zp, accum_scratch_ptr,
          recurrent_to_input_row_sums, compute_row_sums,
          scaling_factors_scratch, context);
    }

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        recurrent_to_forget_weights_ptr, n_cell, n_output,
        quantized_output_state_ptr, recurrent_to_forget_weights_scale,
        output_state_sf, n_batch, forget_gate_scratch,
        /*per_channel_scale=*/nullptr, output_state_zp, accum_scratch_ptr,
        recurrent_to_forget_row_sums, compute_row_sums, scaling_factors_scratch,
        context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        recurrent_to_cell_weights_ptr, n_cell, n_output,
        quantized_output_state_ptr, recurrent_to_cell_weights_scale,
        output_state_sf, n_batch, cell_gate_scratch,
        /*per_channel_scale=*/nullptr, output_state_zp, accum_scratch_ptr,
        recurrent_to_cell_row_sums, compute_row_sums, scaling_factors_scratch,
        context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        recurrent_to_output_weights_ptr, n_cell, n_output,
        quantized_output_state_ptr, recurrent_to_output_weights_scale,
        output_state_sf, n_batch, output_gate_scratch,
        /*per_channel_scale=*/nullptr, output_state_zp, accum_scratch_ptr,
        recurrent_to_output_row_sums, compute_row_sums, scaling_factors_scratch,
        context);
  }

  // For each batch and cell: update input gate.
  if (!use_cifg) {
    if (use_peephole) {
      tensor_utils::VectorScalarMultiply(cell_to_input_weights_ptr, n_cell,
                                         cell_to_input_weights_scale,
                                         recovered_cell_weights);
      tensor_utils::VectorBatchVectorCwiseProductAccumulate(
          recovered_cell_weights, n_cell, cell_state_ptr, n_batch,
          input_gate_scratch);
    }
    if (use_layer_norm) {
      tensor_utils::MeanStddevNormalization(
          input_gate_scratch, input_gate_scratch, n_cell, n_batch);
      tensor_utils::VectorBatchVectorCwiseProduct(
          input_layer_norm_coefficients_ptr, n_cell, input_gate_scratch,
          n_batch, input_gate_scratch);
      tensor_utils::VectorBatchVectorAdd(input_gate_bias_ptr, n_cell, n_batch,
                                         input_gate_scratch);
    }
    tensor_utils::ApplySigmoidToVector(input_gate_scratch, n_cell * n_batch,
                                       input_gate_scratch);
  }

  // For each batch and cell: update forget gate.
  if (use_peephole) {
    tensor_utils::VectorScalarMultiply(cell_to_forget_weights_ptr, n_cell,
                                       cell_to_forget_weights_scale,
                                       recovered_cell_weights);
    tensor_utils::VectorBatchVectorCwiseProductAccumulate(
        recovered_cell_weights, n_cell, cell_state_ptr, n_batch,
        forget_gate_scratch);
  }
  if (use_layer_norm) {
    tensor_utils::MeanStddevNormalization(forget_gate_scratch,
                                          forget_gate_scratch, n_cell, n_batch);
    tensor_utils::VectorBatchVectorCwiseProduct(
        forget_layer_norm_coefficients_ptr, n_cell, forget_gate_scratch,
        n_batch, forget_gate_scratch);
    tensor_utils::VectorBatchVectorAdd(forget_gate_bias_ptr, n_cell, n_batch,
                                       forget_gate_scratch);
  }
  tensor_utils::ApplySigmoidToVector(forget_gate_scratch, n_cell * n_batch,
                                     forget_gate_scratch);

  // For each batch and cell: update the cell.
  if (use_layer_norm) {
    tensor_utils::MeanStddevNormalization(cell_gate_scratch, cell_gate_scratch,
                                          n_cell, n_batch);
    tensor_utils::VectorBatchVectorCwiseProduct(
        cell_layer_norm_coefficients_ptr, n_cell, cell_gate_scratch, n_batch,
        cell_gate_scratch);
    tensor_utils::VectorBatchVectorAdd(cell_gate_bias_ptr, n_cell, n_batch,
                                       cell_gate_scratch);
  }
  tensor_utils::ApplyActivationToVector(cell_gate_scratch, n_batch * n_cell,
                                        params->activation, cell_gate_scratch);

  UpdateLstmCellFloat(n_batch, n_cell, cell_state_ptr, input_gate_scratch,
                      forget_gate_scratch, cell_gate_scratch, use_cifg,
                      params->cell_clip);

  // For each batch and cell: update the output gate.
  if (use_peephole) {
    tensor_utils::VectorScalarMultiply(cell_to_output_weights_ptr, n_cell,
                                       cell_to_output_weights_scale,
                                       recovered_cell_weights);
    tensor_utils::VectorBatchVectorCwiseProductAccumulate(
        recovered_cell_weights, n_cell, cell_state_ptr, n_batch,
        output_gate_scratch);
  }
  if (use_layer_norm) {
    tensor_utils::MeanStddevNormalization(output_gate_scratch,
                                          output_gate_scratch, n_cell, n_batch);
    tensor_utils::VectorBatchVectorCwiseProduct(
        output_layer_norm_coefficients_ptr, n_cell, output_gate_scratch,
        n_batch, output_gate_scratch);
    tensor_utils::VectorBatchVectorAdd(output_gate_bias_ptr, n_cell, n_batch,
                                       output_gate_scratch);
  }
  tensor_utils::ApplySigmoidToVector(output_gate_scratch, n_batch * n_cell,
                                     output_gate_scratch);

  CalculateLstmOutputHybrid(
      n_batch, n_cell, n_output, cell_state_ptr, output_gate_scratch,
      params->activation, projection_weights_ptr, projection_weights_scale,
      projection_bias_ptr, params->proj_clip, output_state_ptr,
      asymmetric_quantize_inputs, projection_weights_row_sums, compute_row_sums,
      context, scratch2, quantized_output_scratch, input_sf, input_zp,
      accum_scratch_ptr);

  // Copy output_state_ptr to the output. Note that the output batch rows may
  // not be contiguous (output_batch_leading_dim != n_output).
  for (int b = 0; b < n_batch; b++) {
    std::copy_n(output_state_ptr + b * n_output, n_output,
                output_ptr + b * output_batch_leading_dim);
  }
}

// Fully quantized lstm kernel for 16 bit gate matmul output.
//
// Input tensor of size n_batch * n_input:
//   input_ptr
//
// LSTM weights:
// Quantized input weights of size 'n_cell * n_input':
//   input_to_input_weight_ptr            - optional
//   input_to_forget_weight_ptr           - optional
//   input_to_cell_weight_ptr             - optional
//   input_to_output_weight_ptr           - optional
//
// Quantized recurrent weights of size 'n_cell * n_output':
//   recurrent_to_input_weight_ptr        - optional
//   recurrent_to_forget_weights_ptr
//   recurrent_to_cell_weights_ptr
//   recurrent_to_input_weights_ptr
//
// Quantized peephole weights of size 'n_cell', representing diagonal matrices.
//   cell_to_input_weights               - optional
//   cell_to_cell_weights                - optional
//   cell_to_output_weights              - optional
//
// Quantized projection weights of size 'n_output * n_cell'
//   projection_weight_ptr                     - optional
//
// Weight scales (scalars) for each of the weights above.
//   effective_input_to_input_scale_a    - optional
//   effective_input_to_input_scale_b    - optional
//   effective_input_to_forget_scale_a
//   effective_input_to_forget_scale_b
//   effective_input_to_cell_scale_a
//   effective_input_to_cell_scale_b
//   effective_input_to_output_scale_a
//   effective_input_to_output_scale_b
//   effective_recurrent_to_input_scale_a    - optional
//   effective_recurrent_to_input_scale_b    - optional
//   effective_recurrent_to_forget_scale_a
//   effective_recurrent_to_forget_scale_b
//   effective_recurrent_to_cell_scale_a
//   effective_recurrent_to_cell_scale_b
//   effective_recurrent_to_output_scale_a
//   effective_recurrent_to_output_scale_b
//   effective_proj_scale_a                  - optional
//   effective_proj_scale_b                  - optional
//
// Gate biases of size 'n_cell':
//   input_gate_bias_ptr                 - optional
//   forget_gate_bias_ptr
//   cell_gate_bias_ptr
//   output_gate_bias_ptr
//
// Layer norm coefficients of size 'n_cell', representing diagonal matrices.
//   layer_norm_input_weight_ptr    - optional
//   layer_norm_forget_weight_ptr   - optional
//   layer_norm_cell_weight_ptr     - optional
//   layer_norm_output_weight_ptr   - optional
//
// Layer norm scales of size 'n_cell'.
//   layer_norm_input_scale_a     - optional
//   layer_norm_input_scale_b     - optional
//   layer_norm_forget_scale_a    - optional
//   layer_norm_forget_scale_b    - optional
//   layer_norm_cell_scale_a      - optional
//   layer_norm_cell_scale_b      - optional
//   layer_norm_output_scale_a    - optional
//   layer_norm_output_scale_b    - optional
//
// Scalar values:
//   quantized_cell_clip: quantized clip value for cell.
//   quantized_proj_clip: quantized clip value for projection.
//   cell_state_scale: the power of two scale for cell state.
//
// Zero points:
//   output_state_zp: zero point of output state
//   hidden_zp: zero point for hidden state.
//
// Temporary pre-allocated storage for the calculation. Each is of size n_cell *
// n_batch.
//   scratch0
//   scratch1
//   scratch2
//   scratch3
//   scratch4
//   scratch5: this scratch buffer is created purely for optimizing the
//              MatrixBatchVectorMultiplyAccumulate.
//
// Outputs:
//   output_state_ptr - size 'n_batch * n_output'
//   cell_state_ptr   - size 'n_batch * n_cell'
//   output_ptr       - size 'n_batch * n_output'
// TODO(b/159947023): scratch0 is not used if (!cifg). Don't allocate then.
inline void LstmStepInteger8x8_16(
    const int8_t* input_ptr, const int8_t* input_to_input_weight_ptr,
    int32_t effective_input_to_input_scale_a,
    int32_t effective_input_to_input_scale_b,
    const int8_t* input_to_forget_weight_ptr,
    int32_t effective_input_to_forget_scale_a,
    int32_t effective_input_to_forget_scale_b,
    const int8_t* input_to_cell_weight_ptr,
    int32_t effective_input_to_cell_scale_a,
    int32_t effective_input_to_cell_scale_b,
    const int8_t* input_to_output_weight_ptr,
    int32_t effective_input_to_output_scale_a,
    int32_t effective_input_to_output_scale_b,
    const int8_t* recurrent_to_input_weight_ptr,
    int32_t effective_recurrent_to_input_scale_a,
    int32_t effective_recurrent_to_input_scale_b,
    const int8_t* recurrent_to_forget_weight_ptr,
    int32_t effective_recurrent_to_forget_scale_a,
    int32_t effective_recurrent_to_forget_scale_b,
    const int8_t* recurrent_to_cell_weight_ptr,
    int32_t effective_recurrent_to_cell_scale_a,
    int32_t effective_recurrent_to_cell_scale_b,
    const int8_t* recurrent_to_output_weight_ptr,
    int32_t effective_recurrent_to_output_scale_a,
    int32_t effective_recurrent_to_output_scale_b,
    const int16_t* cell_to_input_weight_ptr,
    int32_t effective_cell_to_input_scale_a,
    int32_t effective_cell_to_input_scale_b,
    const int16_t* cell_to_forget_weight_ptr,
    int32_t effective_cell_to_forget_scale_a,
    int32_t effective_cell_to_forget_scale_b,
    const int16_t* cell_to_output_weight_ptr,
    int32_t effective_cell_to_output_scale_a,
    int32_t effective_cell_to_output_scale_b,
    const int8_t* projection_weight_ptr, int32_t effective_proj_scale_a,
    int32_t effective_proj_scale_b, int32_t hidden_zp,
    int32_t effective_hidden_scale_a, int32_t effective_hidden_scale_b,
    const int16_t* layer_norm_input_weight_ptr,
    int32_t layer_norm_input_scale_a, int32_t layer_norm_input_scale_b,
    const int16_t* layer_norm_forget_weight_ptr,
    int32_t layer_norm_forget_scale_a, int32_t layer_norm_forget_scale_b,
    const int16_t* layer_norm_cell_weight_ptr, int32_t layer_norm_cell_scale_a,
    int32_t layer_norm_cell_scale_b,
    const int16_t* layer_norm_output_weight_ptr,
    int32_t layer_norm_output_scale_a, int32_t layer_norm_output_scale_b,
    const int32_t* input_gate_bias_ptr, const int32_t* forget_gate_bias_ptr,
    const int32_t* cell_gate_bias_ptr, const int32_t* output_gate_bias_ptr,
    int16_t quantized_cell_clip, int8_t quantized_proj_clip,
    int32_t cell_state_scale, int32_t input_variance_guard,
    int32_t forget_variance_guard, int32_t cell_variance_guard,
    int32_t output_variance_guard,
    const int32_t* input_to_forget_effective_bias,
    const int32_t* recurrent_to_forget_effective_bias,
    const int32_t* input_to_cell_effective_bias,
    const int32_t* recurrent_to_cell_effective_bias,
    const int32_t* input_to_output_effective_bias,
    const int32_t* recurrent_to_output_effective_bias,
    const int32_t* input_to_input_effective_bias,
    const int32_t* recurrent_to_input_effective_bias,
    const int32_t* projection_effective_bias, int n_batch, int n_cell,
    int n_input, int n_output, int8_t* output_state_ptr,
    int32_t output_state_zp, int16_t* cell_state_ptr, int8_t* output_ptr,
    int16_t* scratch0, int16_t* scratch1, int16_t* scratch2, int16_t* scratch3,
    int8_t* scratch4, int32_t* scratch5, CpuBackendContext* context) {
  ruy::profiler::ScopeLabel label("LstmStepInteger8x8_16");
  // Make named scratch buffers for the different gates.
  int16_t* input_gate_scratch = scratch0;
  int16_t* forget_gate_scratch = scratch1;
  int16_t* cell_gate_scratch = scratch2;
  int16_t* output_gate_scratch = scratch3;

  // Get hyper parameters.
  const bool use_cifg = (input_to_input_weight_ptr == nullptr);
  const bool use_peephole = (cell_to_output_weight_ptr != nullptr);
  const bool use_layer_norm = (layer_norm_forget_weight_ptr != nullptr);

  // Check for nullptrs.
  TFLITE_DCHECK(input_to_forget_effective_bias);
  TFLITE_DCHECK(recurrent_to_forget_effective_bias);
  TFLITE_DCHECK(input_to_cell_effective_bias);
  TFLITE_DCHECK(recurrent_to_cell_effective_bias);
  TFLITE_DCHECK(input_to_output_effective_bias);
  TFLITE_DCHECK(recurrent_to_output_effective_bias);
  if (!use_cifg) {
    TFLITE_DCHECK(input_to_input_effective_bias);
    TFLITE_DCHECK(recurrent_to_input_effective_bias);
  }
  TFLITE_DCHECK(projection_effective_bias);

  // Set scratch to 0.
  if (!use_cifg) {
    std::fill_n(input_gate_scratch, n_batch * n_cell, 0);
  }
  std::fill_n(forget_gate_scratch, n_batch * n_cell, 0);
  std::fill_n(cell_gate_scratch, n_batch * n_cell, 0);
  std::fill_n(output_gate_scratch, n_batch * n_cell, 0);

  // Forget gate.
  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      input_ptr, input_to_forget_effective_bias, input_to_forget_weight_ptr,
      effective_input_to_forget_scale_a, effective_input_to_forget_scale_b,
      n_batch, n_input, n_cell, 0, scratch5, forget_gate_scratch, context);

  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      output_state_ptr, recurrent_to_forget_effective_bias,
      recurrent_to_forget_weight_ptr, effective_recurrent_to_forget_scale_a,
      effective_recurrent_to_forget_scale_b, n_batch, n_output, n_cell, 0,
      scratch5, forget_gate_scratch, context);
  if (use_peephole) {
    tensor_utils::VectorBatchVectorCwiseProductAccumulate(
        cell_to_forget_weight_ptr, n_output, cell_state_ptr, n_batch,
        effective_cell_to_forget_scale_a, effective_cell_to_forget_scale_b,
        forget_gate_scratch);
  }

  if (use_layer_norm) {
    tensor_utils::ApplyLayerNorm(
        forget_gate_scratch, layer_norm_forget_weight_ptr, forget_gate_bias_ptr,
        layer_norm_forget_scale_a, layer_norm_forget_scale_b,
        forget_variance_guard, n_batch, n_cell, forget_gate_scratch);
  }

  tensor_utils::ApplySigmoid(forget_gate_scratch, n_batch, n_cell,
                             forget_gate_scratch);

  // Cell gate.
  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      input_ptr, input_to_cell_effective_bias, input_to_cell_weight_ptr,
      effective_input_to_cell_scale_a, effective_input_to_cell_scale_b, n_batch,
      n_input, n_cell, 0, scratch5, cell_gate_scratch, context);

  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      output_state_ptr, recurrent_to_cell_effective_bias,
      recurrent_to_cell_weight_ptr, effective_recurrent_to_cell_scale_a,
      effective_recurrent_to_cell_scale_b, n_batch, n_output, n_cell, 0,
      scratch5, cell_gate_scratch, context);

  if (use_layer_norm) {
    tensor_utils::ApplyLayerNorm(cell_gate_scratch, layer_norm_cell_weight_ptr,
                                 cell_gate_bias_ptr, layer_norm_cell_scale_a,
                                 layer_norm_cell_scale_b, cell_variance_guard,
                                 n_batch, n_cell, cell_gate_scratch);
  }

  tensor_utils::ApplyTanh(3, cell_gate_scratch, n_batch, n_cell,
                          cell_gate_scratch);

  // Input gate.
  if (!use_cifg) {
    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        input_ptr, input_to_input_effective_bias, input_to_input_weight_ptr,
        effective_input_to_input_scale_a, effective_input_to_input_scale_b,
        n_batch, n_input, n_cell, 0, scratch5, input_gate_scratch, context);

    tensor_utils::MatrixBatchVectorMultiplyAccumulate(
        output_state_ptr, recurrent_to_input_effective_bias,
        recurrent_to_input_weight_ptr, effective_recurrent_to_input_scale_a,
        effective_recurrent_to_input_scale_b, n_batch, n_output, n_cell, 0,
        scratch5, input_gate_scratch, context);
    if (use_peephole) {
      tensor_utils::VectorBatchVectorCwiseProductAccumulate(
          cell_to_input_weight_ptr, n_output, cell_state_ptr, n_batch,
          effective_cell_to_input_scale_a, effective_cell_to_input_scale_b,
          input_gate_scratch);
    }

    if (use_layer_norm) {
      tensor_utils::ApplyLayerNorm(
          input_gate_scratch, layer_norm_input_weight_ptr, input_gate_bias_ptr,
          layer_norm_input_scale_a, layer_norm_input_scale_b,
          input_variance_guard, n_batch, n_cell, input_gate_scratch);
    }
    tensor_utils::ApplySigmoid(input_gate_scratch, n_batch, n_cell,
                               input_gate_scratch);
  }

  UpdateLstmCellInteger(n_batch, n_cell, cell_state_ptr, cell_state_scale,
                        input_gate_scratch, forget_gate_scratch,
                        cell_gate_scratch, use_cifg, quantized_cell_clip);

  // Ouptut gate.
  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      input_ptr, input_to_output_effective_bias, input_to_output_weight_ptr,
      effective_input_to_output_scale_a, effective_input_to_output_scale_b,
      n_batch, n_input, n_cell, 0, scratch5, output_gate_scratch, context);

  tensor_utils::MatrixBatchVectorMultiplyAccumulate(
      output_state_ptr, recurrent_to_output_effective_bias,
      recurrent_to_output_weight_ptr, effective_recurrent_to_output_scale_a,
      effective_recurrent_to_output_scale_b, n_batch, n_output, n_cell, 0,
      scratch5, output_gate_scratch, context);
  if (use_peephole) {
    tensor_utils::VectorBatchVectorCwiseProductAccumulate(
        cell_to_output_weight_ptr, n_output, cell_state_ptr, n_batch,
        effective_cell_to_output_scale_a, effective_cell_to_output_scale_b,
        output_gate_scratch);
  }

  if (use_layer_norm) {
    tensor_utils::ApplyLayerNorm(
        output_gate_scratch, layer_norm_output_weight_ptr, output_gate_bias_ptr,
        layer_norm_output_scale_a, layer_norm_output_scale_b,
        output_variance_guard, n_batch, n_cell, output_gate_scratch);
  }

  tensor_utils::ApplySigmoid(output_gate_scratch, n_batch, n_cell,
                             output_gate_scratch);

  CalculateLstmOutputInteger8x8_16(
      n_batch, n_cell, n_output, cell_state_ptr, cell_state_scale,
      output_gate_scratch, effective_hidden_scale_a, effective_hidden_scale_b,
      hidden_zp, projection_weight_ptr, effective_proj_scale_a,
      effective_proj_scale_b, projection_effective_bias, output_state_zp,
      quantized_proj_clip, output_state_ptr, context, scratch0, scratch4,
      scratch5);

  // Copy output state to the output. Note that unlike float or hybrid, output
  // is always contigous.
  std::copy_n(output_state_ptr, n_batch * n_output, output_ptr);
}

// Fully quantized lstm kernel for 8 bit gate matmul output.
//
// Input tensor of size n_batch * n_input:
//   input_ptr
//
// LSTM weights:
// Quantized input weights of size 'n_cell * n_input':
//   input_to_input_weight_ptr            - optional
//   input_to_forget_weight_ptr           - optional
//   input_to_cell_weight_ptr             - optional
//   input_to_output_weight_ptr           - optional
//
// Quantized recurrent weights of size 'n_cell * n_output':
//   recurrent_to_input_weight_ptr        - optional
//   recurrent_to_forget_weights_ptr
//   recurrent_to_cell_weights_ptr
//   recurrent_to_input_weights_ptr
//
// Quantized peephole weights of size 'n_cell', representing diagonal matrices.
//   cell_to_input_weights               - optional
//   cell_to_cell_weights                - optional
//   cell_to_output_weights              - optional
//
// Quantized projection weights of size 'n_output * n_cell'
//   projection_weight_ptr                     - optional
//
// Weight scales (scalars) for each of the weights above.
//   effective_input_to_input_scale_a    - optional
//   effective_input_to_input_scale_b    - optional
//   effective_input_to_forget_scale_a
//   effective_input_to_forget_scale_b
//   effective_input_to_cell_scale_a
//   effective_input_to_cell_scale_b
//   effective_input_to_output_scale_a
//   effective_input_to_output_scale_b
//   effective_recurrent_to_input_scale_a    - optional
//   effective_recurrent_to_input_scale_b    - optional
//   effective_recurrent_to_forget_scale_a
//   effective_recurrent_to_forget_scale_b
//   effective_recurrent_to_cell_scale_a
//   effective_recurrent_to_cell_scale_b
//   effective_recurrent_to_output_scale_a
//   effective_recurrent_to_output_scale_b
//   effective_proj_scale_a                  - optional
//   effective_proj_scale_b                  - optional
//
// Gate biases of size 'n_cell':
//   input_gate_bias_ptr                 - optional
//   forget_gate_bias_ptr
//   cell_gate_bias_ptr
//   output_gate_bias_ptr
//
// Layer norm coefficients of size 'n_cell', representing diagonal matrices.
//   layer_norm_input_weight_ptr    - optional
//   layer_norm_forget_weight_ptr   - optional
//   layer_norm_cell_weight_ptr     - optional
//   layer_norm_output_weight_ptr   - optional
//
// Layer norm scales of size 'n_cell'.
//   layer_norm_input_scale_a     - optional
//   layer_norm_input_scale_b     - optional
//   layer_norm_forget_scale_a    - optional
//   layer_norm_forget_scale_b    - optional
//   layer_norm_cell_scale_a      - optional
//   layer_norm_cell_scale_b      - optional
//   layer_norm_output_scale_a    - optional
//   layer_norm_output_scale_b    - optional
//
// Scalar values:
//   quantized_cell_clip: quantized clip value for cell.
//   quantized_proj_clip: quantized clip value for projection.
//   cell_state_scale: the power of two scale for cell state.
//
// Zero points:
//   output_state_zp: zero point of output state.
//   hidden_zp: zero point for hidden state.
//
// Temporary pre-allocated storage for the calculation. Each is of size n_cell *
// n_batch.
//   scratch0
//   scratch1
//   scratch2
//   scratch3
//   scratch4
//   scratch5
//   scratch6
//   scratch7
//
// Outputs:
//   output_state_ptr - size 'n_batch * n_output'
//   cell_state_ptr   - size 'n_batch * n_cell'
//   output_ptr       - size 'n_batch * n_output'
// TODO(b/148688698): Move zero point calculation into Prepare().
// TODO(b/159947023): scratch5 is unused, remove.
inline void LstmStepInteger8x8_8(
    const int8_t* input_ptr, int32_t input_zp,
    const int8_t* input_to_input_weight_ptr,
    int32_t effective_input_to_input_scale_a,
    int32_t effective_input_to_input_scale_b,
    const int8_t* input_to_forget_weight_ptr,
    int32_t effective_input_to_forget_scale_a,
    int32_t effective_input_to_forget_scale_b,
    const int8_t* input_to_cell_weight_ptr,
    int32_t effective_input_to_cell_scale_a,
    int32_t effective_input_to_cell_scale_b,
    const int8_t* input_to_output_weight_ptr,
    int32_t effective_input_to_output_scale_a,
    int32_t effective_input_to_output_scale_b,
    const int8_t* recurrent_to_input_weight_ptr,
    int32_t effective_recurrent_to_input_scale_a,
    int32_t effective_recurrent_to_input_scale_b,
    const int8_t* recurrent_to_forget_weight_ptr,
    int32_t effective_recurrent_to_forget_scale_a,
    int32_t effective_recurrent_to_forget_scale_b,
    const int8_t* recurrent_to_cell_weight_ptr,
    int32_t effective_recurrent_to_cell_scale_a,
    int32_t effective_recurrent_to_cell_scale_b,
    const int8_t* recurrent_to_output_weight_ptr,
    int32_t effective_recurrent_to_output_scale_a,
    int32_t effective_recurrent_to_output_scale_b,
    const int8_t* cell_to_input_weight_ptr,
    int32_t effective_cell_to_input_scale_a,
    int32_t effective_cell_to_input_scale_b,
    const int8_t* cell_to_forget_weight_ptr,
    int32_t effective_cell_to_forget_scale_a,
    int32_t effective_cell_to_forget_scale_b,
    const int8_t* cell_to_output_weight_ptr,
    int32_t effective_cell_to_output_scale_a,
    int32_t effective_cell_to_output_scale_b,
    const int8_t* projection_weight_ptr, int32_t effective_proj_scale_a,
    int32_t effective_proj_scale_b, const int16_t* layer_norm_input_weight_ptr,
    int32_t layer_norm_input_scale_a, int32_t layer_norm_input_scale_b,
    const int16_t* layer_norm_forget_weight_ptr,
    int32_t layer_norm_forget_scale_a, int32_t layer_norm_forget_scale_b,
    const int16_t* layer_norm_cell_weight_ptr, int32_t layer_norm_cell_scale_a,
    int32_t layer_norm_cell_scale_b,
    const int16_t* layer_norm_output_weight_ptr,
    int32_t layer_norm_output_scale_a, int32_t layer_norm_output_scale_b,
    const int32_t* input_gate_bias_ptr, const int32_t* forget_gate_bias_ptr,
    const int32_t* cell_gate_bias_ptr, const int32_t* output_gate_bias_ptr,
    const int32_t* projection_bias_ptr, const TfLiteLSTMParams* params,
    const int32_t* intermediate_scale_a, const int32_t* intermediate_scale_b,
    const int32_t* intermediate_zp, int16_t quantized_cell_clip,
    int8_t quantized_proj_clip, int n_batch, int n_cell, int n_input,
    int n_output, int output_batch_leading_dim, int8_t* output_state_ptr,
    int32_t output_state_zp, int16_t* cell_state_ptr, int8_t* output_ptr,
    int8_t* scratch0, int8_t* scratch1, int16_t* scratch2, int16_t* scratch3,
    int16_t* scratch4, int16_t* scratch5, int16_t* scratch6,
    int16_t* scratch7) {
  ruy::profiler::ScopeLabel label("LstmStepInteger8x8_8");
  // Make named scratch buffers for the different gates.
  int16_t* forget_gate_scratch = scratch2;
  int16_t* cell_gate_scratch = scratch3;
  int16_t* output_gate_scratch = scratch4;

  // Forget gate.
  std::fill_n(scratch0, n_batch * n_cell, 0);
  std::fill_n(scratch1, n_batch * n_cell, 0);
  tensor_utils::MatrixBatchVectorMultiply(
      input_ptr, input_zp, input_to_forget_weight_ptr,
      effective_input_to_forget_scale_a, effective_input_to_forget_scale_b,
      n_batch, n_input, n_cell, scratch0, intermediate_zp[4]);

  tensor_utils::MatrixBatchVectorMultiply(
      output_state_ptr, output_state_zp, recurrent_to_forget_weight_ptr,
      effective_recurrent_to_forget_scale_a,
      effective_recurrent_to_forget_scale_b, n_batch, n_output, n_cell,
      scratch1, intermediate_zp[5]);

  tensor_utils::TwoGateSaturatingAdd(
      scratch0, intermediate_zp[4], scratch1, intermediate_zp[5],
      intermediate_scale_a[2], intermediate_scale_b[2], intermediate_scale_a[3],
      intermediate_scale_b[3], n_batch, n_cell, forget_gate_scratch);

  // Forget gate layer norm.
  tensor_utils::ApplyLayerNormFloat(
      forget_gate_scratch, layer_norm_forget_weight_ptr,
      layer_norm_forget_scale_a, layer_norm_forget_scale_b,
      forget_gate_bias_ptr, n_batch, n_cell, forget_gate_scratch);

  // Forget gate sigmoid.
  tensor_utils::ApplySigmoidFloat(forget_gate_scratch, n_batch, n_cell,
                                  forget_gate_scratch);

  // Cell gate.
  std::fill_n(scratch0, n_batch * n_cell, 0);
  std::fill_n(scratch1, n_batch * n_cell, 0);
  tensor_utils::MatrixBatchVectorMultiply(
      input_ptr, input_zp, input_to_cell_weight_ptr,
      effective_input_to_cell_scale_a, effective_input_to_cell_scale_b, n_batch,
      n_input, n_cell, scratch0, intermediate_zp[7]);

  tensor_utils::MatrixBatchVectorMultiply(
      output_state_ptr, output_state_zp, recurrent_to_cell_weight_ptr,
      effective_recurrent_to_cell_scale_a, effective_recurrent_to_cell_scale_b,
      n_batch, n_output, n_cell, scratch1, intermediate_zp[8]);

  tensor_utils::TwoGateSaturatingAdd(
      scratch0, intermediate_zp[7], scratch1, intermediate_zp[8],
      intermediate_scale_a[4], intermediate_scale_b[4], intermediate_scale_a[5],
      intermediate_scale_b[5], n_batch, n_cell, cell_gate_scratch);

  // Cell gate layer norm.
  tensor_utils::ApplyLayerNormFloat(
      cell_gate_scratch, layer_norm_cell_weight_ptr, layer_norm_cell_scale_a,
      layer_norm_cell_scale_b, cell_gate_bias_ptr, n_batch, n_cell,
      cell_gate_scratch);

  // Cell gate tanh.
  tensor_utils::ApplyTanhFloat(cell_gate_scratch, n_batch, n_cell, -12,
                               cell_gate_scratch);

  // Output gate.
  std::fill_n(scratch0, n_batch * n_cell, 0);
  std::fill_n(scratch1, n_batch * n_cell, 0);
  tensor_utils::MatrixBatchVectorMultiply(
      input_ptr, input_zp, input_to_output_weight_ptr,
      effective_input_to_output_scale_a, effective_input_to_output_scale_b,
      n_batch, n_input, n_cell, scratch0, intermediate_zp[10]);

  tensor_utils::MatrixBatchVectorMultiply(
      output_state_ptr, output_state_zp, recurrent_to_output_weight_ptr,
      effective_recurrent_to_output_scale_a,
      effective_recurrent_to_output_scale_b, n_batch, n_output, n_cell,
      scratch1, intermediate_zp[11]);

  tensor_utils::TwoGateSaturatingAdd(
      scratch0, intermediate_zp[10], scratch1, intermediate_zp[11],
      intermediate_scale_a[6], intermediate_scale_b[6], intermediate_scale_a[7],
      intermediate_scale_b[7], n_batch, n_cell, output_gate_scratch);

  // Output gate with layer norm.
  tensor_utils::ApplyLayerNormFloat(
      output_gate_scratch, layer_norm_output_weight_ptr,
      layer_norm_output_scale_a, layer_norm_output_scale_b,
      output_gate_bias_ptr, n_batch, n_cell, output_gate_scratch);

  // Output gate sigmoid.
  tensor_utils::ApplySigmoidFloat(output_gate_scratch, n_batch, n_cell,
                                  output_gate_scratch);

  UpdateLstmCellInteger(n_batch, n_cell, cell_state_ptr,
                        /*cell_state_scale=*/-15, /*input_gate=*/nullptr,
                        forget_gate_scratch, cell_gate_scratch,
                        /*use_cifg=*/true, quantized_cell_clip);

  CalculateLstmOutputInteger8x8_8(
      n_batch, n_cell, n_output, cell_state_ptr, output_gate_scratch,
      projection_weight_ptr, effective_proj_scale_a, effective_proj_scale_b,
      projection_bias_ptr, output_state_zp, quantized_proj_clip,
      output_state_ptr, scratch2);

  // Copy output state to the output. Note that unlike float or hybrid, output
  // is always contigous.
  std::copy_n(output_state_ptr, n_batch * n_output, output_ptr);
}

}  // namespace

// LINT.IfChange
TfLiteStatus EvalFloat(
    const TfLiteTensor* input, const TfLiteTensor* input_to_input_weights,
    const TfLiteTensor* input_to_forget_weights,
    const TfLiteTensor* input_to_cell_weights,
    const TfLiteTensor* input_to_output_weights,
    const TfLiteTensor* recurrent_to_input_weights,
    const TfLiteTensor* recurrent_to_forget_weights,
    const TfLiteTensor* recurrent_to_cell_weights,
    const TfLiteTensor* recurrent_to_output_weights,
    const TfLiteTensor* cell_to_input_weights,
    const TfLiteTensor* cell_to_forget_weights,
    const TfLiteTensor* cell_to_output_weights,
    const TfLiteTensor* input_layer_norm_coefficients,
    const TfLiteTensor* forget_layer_norm_coefficients,
    const TfLiteTensor* cell_layer_norm_coefficients,
    const TfLiteTensor* output_layer_norm_coefficients,
    const TfLiteTensor* aux_input,
    const TfLiteTensor* aux_input_to_input_weights,
    const TfLiteTensor* aux_input_to_forget_weights,
    const TfLiteTensor* aux_input_to_cell_weights,
    const TfLiteTensor* aux_input_to_output_weights,
    const TfLiteTensor* input_gate_bias, const TfLiteTensor* forget_gate_bias,
    const TfLiteTensor* cell_gate_bias, const TfLiteTensor* output_gate_bias,
    const TfLiteTensor* projection_weights, const TfLiteTensor* projection_bias,
    const TfLiteLSTMParams* params, bool forward_sequence, bool time_major,
    int output_offset, TfLiteTensor* scratch_buffer, TfLiteTensor* output_state,
    TfLiteTensor* cell_state, TfLiteTensor* output) {
  TF_LITE_ASSERT(input->dims->size >= 2 && input->dims->size <= 3);
  int max_time, n_batch;
  if (input->dims->size == 3) {
    max_time = (time_major) ? input->dims->data[0] : input->dims->data[1];
    n_batch = (time_major) ? input->dims->data[1] : input->dims->data[0];
  } else {
    max_time = 1;
    n_batch = input->dims->data[0];
  }
  const int n_input = input->dims->data[input->dims->size - 1];
  const int aux_input_size =
      (aux_input) ? aux_input->dims->data[aux_input->dims->size - 1] : 0;

  // n_cell and n_output will be the same size when there is no projection.
  const int n_cell = input_to_output_weights->dims->data[0];
  const int n_output = recurrent_to_output_weights->dims->data[1];

  // Since we have already checked that weights are all there or none, we can
  // check the existence of only one to the get the condition.
  const bool use_cifg = (input_to_input_weights == nullptr);

  // Index the scratch buffers pointers to the global scratch buffer.
  float* scratch_buffer_ptr = GetTensorData<float>(scratch_buffer);
  float* input_gate_scratch = nullptr;
  float* cell_gate_scratch = nullptr;
  float* forget_gate_scratch = nullptr;
  float* output_gate_scratch = nullptr;
  if (use_cifg) {
    cell_gate_scratch = scratch_buffer_ptr;
    forget_gate_scratch = scratch_buffer_ptr + n_cell * n_batch;
    output_gate_scratch = scratch_buffer_ptr + 2 * n_cell * n_batch;
  } else {
    input_gate_scratch = scratch_buffer_ptr;
    cell_gate_scratch = scratch_buffer_ptr + n_cell * n_batch;
    forget_gate_scratch = scratch_buffer_ptr + 2 * n_cell * n_batch;
    output_gate_scratch = scratch_buffer_ptr + 3 * n_cell * n_batch;
  }

  const int output_batch_leading_dim =
      output->dims->data[output->dims->size - 1];
  if (time_major) {
    // Loop through the sequence.
    const int input_step = n_batch * n_input;
    const int output_step = n_batch * output_batch_leading_dim;
    for (int t = 0; t < max_time; t++) {
      // If this is the forward_sequence, step forward, otherwise step
      // backwards.
      const int t_rel = forward_sequence ? t : max_time - t - 1;
      const float* input_ptr = GetTensorData<float>(input) + t_rel * input_step;
      const float* aux_input_ptr = nullptr;
      if (aux_input) {
        aux_input_ptr = GetTensorData<float>(aux_input) + t_rel * input_step;
      }
      float* output_ptr =
          GetTensorData<float>(output) + t_rel * output_step + output_offset;

      LstmStepFloat(
          input_ptr, GetTensorData<float>(input_to_input_weights),
          GetTensorData<float>(input_to_forget_weights),
          GetTensorData<float>(input_to_cell_weights),
          GetTensorData<float>(input_to_output_weights), aux_input_ptr,
          GetTensorData<float>(aux_input_to_input_weights),
          GetTensorData<float>(aux_input_to_forget_weights),
          GetTensorData<float>(aux_input_to_cell_weights),
          GetTensorData<float>(aux_input_to_output_weights),
          GetTensorData<float>(recurrent_to_input_weights),
          GetTensorData<float>(recurrent_to_forget_weights),
          GetTensorData<float>(recurrent_to_cell_weights),
          GetTensorData<float>(recurrent_to_output_weights),
          GetTensorData<float>(cell_to_input_weights),
          GetTensorData<float>(cell_to_forget_weights),
          GetTensorData<float>(cell_to_output_weights),
          GetTensorData<float>(input_layer_norm_coefficients),
          GetTensorData<float>(forget_layer_norm_coefficients),
          GetTensorData<float>(cell_layer_norm_coefficients),
          GetTensorData<float>(output_layer_norm_coefficients),
          GetTensorData<float>(input_gate_bias),
          GetTensorData<float>(forget_gate_bias),
          GetTensorData<float>(cell_gate_bias),
          GetTensorData<float>(output_gate_bias),
          GetTensorData<float>(projection_weights),
          GetTensorData<float>(projection_bias), params, n_batch, n_cell,
          n_input, aux_input_size, n_output, output_batch_leading_dim,
          GetTensorData<float>(output_state), GetTensorData<float>(cell_state),
          input_gate_scratch, forget_gate_scratch, cell_gate_scratch,
          output_gate_scratch, output_ptr);
    }
  } else {
    for (int b = 0; b < n_batch; b++) {
      const int input_step = n_input;
      const int output_step = output_batch_leading_dim;
      for (int t = 0; t < max_time; t++) {
        // If this is the forward_sequence, step forward, otherwise step
        // backwards.
        const int t_rel = forward_sequence ? t : max_time - t - 1;
        const int time_offset = b * max_time + t_rel;
        const float* input_ptr =
            GetTensorData<float>(input) + time_offset * input_step;
        const float* aux_input_ptr = nullptr;
        if (aux_input) {
          aux_input_ptr =
              GetTensorData<float>(aux_input) + time_offset * input_step;
        }
        float* output_ptr = GetTensorData<float>(output) +
                            time_offset * output_step + output_offset;

        // Offset the {output,cell}_state pointers to the right batch.
        float* output_state_ptr =
            GetTensorData<float>(output_state) + b * output_batch_leading_dim;
        float* cell_state_ptr = GetTensorData<float>(cell_state) + b * n_cell;
        // Offset the scratch pointers to the right batch.
        float* input_gate_scratch_ptr =
            input_gate_scratch ? input_gate_scratch + b * n_cell : nullptr;
        float* forget_gate_scratch_ptr = forget_gate_scratch + b * n_cell;
        float* cell_gate_scratch_ptr = cell_gate_scratch + b * n_cell;
        float* output_gate_scratch_ptr = output_gate_scratch + b * n_cell;

        LstmStepFloat(
            input_ptr, GetTensorData<float>(input_to_input_weights),
            GetTensorData<float>(input_to_forget_weights),
            GetTensorData<float>(input_to_cell_weights),
            GetTensorData<float>(input_to_output_weights), aux_input_ptr,
            GetTensorData<float>(aux_input_to_input_weights),
            GetTensorData<float>(aux_input_to_forget_weights),
            GetTensorData<float>(aux_input_to_cell_weights),
            GetTensorData<float>(aux_input_to_output_weights),
            GetTensorData<float>(recurrent_to_input_weights),
            GetTensorData<float>(recurrent_to_forget_weights),
            GetTensorData<float>(recurrent_to_cell_weights),
            GetTensorData<float>(recurrent_to_output_weights),
            GetTensorData<float>(cell_to_input_weights),
            GetTensorData<float>(cell_to_forget_weights),
            GetTensorData<float>(cell_to_output_weights),
            GetTensorData<float>(input_layer_norm_coefficients),
            GetTensorData<float>(forget_layer_norm_coefficients),
            GetTensorData<float>(cell_layer_norm_coefficients),
            GetTensorData<float>(output_layer_norm_coefficients),
            GetTensorData<float>(input_gate_bias),
            GetTensorData<float>(forget_gate_bias),
            GetTensorData<float>(cell_gate_bias),
            GetTensorData<float>(output_gate_bias),
            GetTensorData<float>(projection_weights),
            GetTensorData<float>(projection_bias), params, /*n_batch=*/1,
            n_cell, n_input, aux_input_size, n_output, output_batch_leading_dim,
            output_state_ptr, cell_state_ptr, input_gate_scratch_ptr,
            forget_gate_scratch_ptr, cell_gate_scratch_ptr,
            output_gate_scratch_ptr, output_ptr);
      }
    }
  }
  return kTfLiteOk;
}
// LINT.ThenChange(//tensorflow/lite/tools/optimize/calibration/builtin_logging_ops/lstm.cc)

TfLiteStatus EvalHybrid(
    const TfLiteTensor* input, const TfLiteTensor* input_to_input_weights,
    const TfLiteTensor* input_to_forget_weights,
    const TfLiteTensor* input_to_cell_weights,
    const TfLiteTensor* input_to_output_weights,
    const TfLiteTensor* recurrent_to_input_weights,
    const TfLiteTensor* recurrent_to_forget_weights,
    const TfLiteTensor* recurrent_to_cell_weights,
    const TfLiteTensor* recurrent_to_output_weights,
    const TfLiteTensor* cell_to_input_weights,
    const TfLiteTensor* cell_to_forget_weights,
    const TfLiteTensor* cell_to_output_weights,
    const TfLiteTensor* input_layer_norm_coefficients,
    const TfLiteTensor* forget_layer_norm_coefficients,
    const TfLiteTensor* cell_layer_norm_coefficients,
    const TfLiteTensor* output_layer_norm_coefficients,
    const TfLiteTensor* aux_input,
    const TfLiteTensor* aux_input_to_input_weights,
    const TfLiteTensor* aux_input_to_forget_weights,
    const TfLiteTensor* aux_input_to_cell_weights,
    const TfLiteTensor* aux_input_to_output_weights,
    const TfLiteTensor* input_gate_bias, const TfLiteTensor* forget_gate_bias,
    const TfLiteTensor* cell_gate_bias, const TfLiteTensor* output_gate_bias,
    const TfLiteTensor* projection_weights, const TfLiteTensor* projection_bias,
    const TfLiteLSTMParams* params, bool forward_sequence, bool time_major,
    int output_offset, TfLiteTensor* scratch_buffer, TfLiteTensor* input_sf,
    TfLiteTensor* aux_input_sf, TfLiteTensor* output_state_sf,
    TfLiteTensor* prod_scaling_factors, TfLiteTensor* recovered_cell_weights,
    TfLiteTensor* input_quantized, TfLiteTensor* aux_input_quantized,
    TfLiteTensor* output_state_quantized, TfLiteTensor* cell_state_quantized,
    TfLiteTensor* output_state, TfLiteTensor* cell_state,
    TfLiteTensor* output_scratch_buffer, TfLiteTensor* output,
    TfLiteTensor* input_zp, TfLiteTensor* aux_input_zp,
    TfLiteTensor* output_state_zp, TfLiteTensor* row_sums, int row_sums_size,
    bool* compute_row_sums, CpuBackendContext* context) {
  TF_LITE_ASSERT(input->dims->size >= 2 && input->dims->size <= 3);
  const int n_input = input->dims->data[input->dims->size - 1];
  int max_time, n_batch;
  if (input->dims->size == 2) {
    max_time = 1;
    n_batch = input->dims->data[0];
  } else {
    max_time = (time_major) ? input->dims->data[0] : input->dims->data[1];
    n_batch = (time_major) ? input->dims->data[1] : input->dims->data[0];
  }
  const int aux_input_size =
      (aux_input) ? aux_input->dims->data[aux_input->dims->size - 1] : 0;
  // n_cell and n_output will be the same size when there is no projection.
  const int n_cell = input_to_output_weights->dims->data[0];
  const int n_output = recurrent_to_output_weights->dims->data[1];

  // Since we have already checked that weights are all there or none, we can
  // check the existence of only one to get the condition.
  const bool use_cifg = (input_to_input_weights == nullptr);

  float* scratch_buffer_ptr = GetTensorData<float>(scratch_buffer);
  float* input_gate_scratch = nullptr;
  float* cell_gate_scratch = nullptr;
  float* forget_gate_scratch = nullptr;
  float* output_gate_scratch = nullptr;
  if (use_cifg) {
    cell_gate_scratch = scratch_buffer_ptr;
    forget_gate_scratch = scratch_buffer_ptr + n_cell * n_batch;
    output_gate_scratch = scratch_buffer_ptr + 2 * n_cell * n_batch;
  } else {
    input_gate_scratch = scratch_buffer_ptr;
    cell_gate_scratch = scratch_buffer_ptr + n_cell * n_batch;
    forget_gate_scratch = scratch_buffer_ptr + 2 * n_cell * n_batch;
    output_gate_scratch = scratch_buffer_ptr + 3 * n_cell * n_batch;
  }

  const int output_batch_leading_dim =
      output->dims->data[output->dims->size - 1];

  int32_t* input_zp_ptr = nullptr;
  int32_t* aux_input_zp_ptr = nullptr;
  int32_t* output_state_zp_ptr = nullptr;
  int32_t* row_sums_ptr = nullptr;
  if (params->asymmetric_quantize_inputs) {
    input_zp_ptr = GetTensorData<int32_t>(input_zp);
    aux_input_zp_ptr = GetTensorData<int32_t>(aux_input_zp);
    output_state_zp_ptr = GetTensorData<int32_t>(output_state_zp);
    row_sums_ptr = GetTensorData<int32_t>(row_sums);
  }

  if (time_major) {
    // Feed the sequence into the LSTM step-by-step.
    const int input_step = n_batch * n_input;
    const int output_step = n_batch * output_batch_leading_dim;
    for (int t = 0; t < max_time; t++) {
      // If this is the forward_sequence, step forward, otherwise step
      // backwards.
      const int t_rel = forward_sequence ? t : max_time - t - 1;
      const float* input_ptr = GetTensorData<float>(input) + t_rel * input_step;
      const float* aux_input_ptr = nullptr;
      if (aux_input) {
        aux_input_ptr = GetTensorData<float>(aux_input) + t_rel * input_step;
      }
      float* output_ptr =
          GetTensorData<float>(output) + t_rel * output_step + output_offset;
      LstmStepHybrid(
          input_ptr, GetTensorData<int8_t>(input_to_input_weights),
          GetTensorScale(input_to_input_weights),
          GetTensorData<int8_t>(input_to_forget_weights),
          GetTensorScale(input_to_forget_weights),
          GetTensorData<int8_t>(input_to_cell_weights),
          GetTensorScale(input_to_cell_weights),
          GetTensorData<int8_t>(input_to_output_weights),
          GetTensorScale(input_to_output_weights), aux_input_ptr,
          GetTensorData<int8_t>(aux_input_to_input_weights),
          GetTensorScale(aux_input_to_input_weights),
          GetTensorData<int8_t>(aux_input_to_forget_weights),
          GetTensorScale(aux_input_to_forget_weights),
          GetTensorData<int8_t>(aux_input_to_cell_weights),
          GetTensorScale(aux_input_to_cell_weights),
          GetTensorData<int8_t>(aux_input_to_output_weights),
          GetTensorScale(aux_input_to_output_weights),
          GetTensorData<int8_t>(recurrent_to_input_weights),
          GetTensorScale(recurrent_to_input_weights),
          GetTensorData<int8_t>(recurrent_to_forget_weights),
          GetTensorScale(recurrent_to_forget_weights),
          GetTensorData<int8_t>(recurrent_to_cell_weights),
          GetTensorScale(recurrent_to_cell_weights),
          GetTensorData<int8_t>(recurrent_to_output_weights),
          GetTensorScale(recurrent_to_output_weights),
          GetTensorData<int8_t>(cell_to_input_weights),
          GetTensorScale(cell_to_input_weights),
          GetTensorData<int8_t>(cell_to_forget_weights),
          GetTensorScale(cell_to_forget_weights),
          GetTensorData<int8_t>(cell_to_output_weights),
          GetTensorScale(cell_to_output_weights),
          GetTensorData<float>(input_layer_norm_coefficients),
          GetTensorData<float>(forget_layer_norm_coefficients),
          GetTensorData<float>(cell_layer_norm_coefficients),
          GetTensorData<float>(output_layer_norm_coefficients),
          GetTensorData<float>(input_gate_bias),
          GetTensorData<float>(forget_gate_bias),
          GetTensorData<float>(cell_gate_bias),
          GetTensorData<float>(output_gate_bias),
          GetTensorData<int8_t>(projection_weights),
          GetTensorScale(projection_weights),
          GetTensorData<float>(projection_bias), params, n_batch, n_cell,
          n_input, aux_input_size, n_output, output_batch_leading_dim,
          input_gate_scratch, forget_gate_scratch, cell_gate_scratch,
          output_gate_scratch, GetTensorData<float>(input_sf),
          GetTensorData<float>(aux_input_sf),
          GetTensorData<float>(output_state_sf),
          GetTensorData<float>(prod_scaling_factors),
          GetTensorData<float>(recovered_cell_weights),
          GetTensorData<int8_t>(input_quantized),
          GetTensorData<int8_t>(aux_input_quantized),
          GetTensorData<int8_t>(output_state_quantized),
          GetTensorData<int8_t>(cell_state_quantized),
          GetTensorData<float>(output_state), GetTensorData<float>(cell_state),
          GetTensorData<int32_t>(output_scratch_buffer), output_ptr,
          input_zp_ptr, aux_input_zp_ptr, output_state_zp_ptr, row_sums_ptr,
          row_sums_size, compute_row_sums, params->asymmetric_quantize_inputs,
          context);
    }
  } else {
    for (int b = 0; b < n_batch; b++) {
      const int input_step = n_input;
      const int output_step = output_batch_leading_dim;
      for (int t = 0; t < max_time; t++) {
        // If this is the forward_sequence, step forward, otherwise step
        // backwards.
        const int t_rel = forward_sequence ? t : max_time - t - 1;
        const int time_offset = b * max_time + t_rel;
        const float* input_ptr =
            GetTensorData<float>(input) + time_offset * input_step;
        const float* aux_input_ptr = nullptr;
        if (aux_input) {
          aux_input_ptr =
              GetTensorData<float>(aux_input) + time_offset * input_step;
        }
        float* output_ptr = GetTensorData<float>(output) +
                            time_offset * output_step + output_offset;

        // Offset the {output,cell}_state pointers to the right batch.
        float* output_state_ptr =
            GetTensorData<float>(output_state) + b * output_batch_leading_dim;
        float* cell_state_ptr = GetTensorData<float>(cell_state) + b * n_cell;
        // Offset the scratch pointers to the right batch.
        float* input_gate_scratch_ptr =
            input_gate_scratch ? input_gate_scratch + b * n_cell : nullptr;
        float* forget_gate_scratch_ptr = forget_gate_scratch + b * n_cell;
        float* cell_gate_scratch_ptr = cell_gate_scratch + b * n_cell;
        float* output_gate_scratch_ptr = output_gate_scratch + b * n_cell;

        LstmStepHybrid(
            input_ptr, GetTensorData<int8_t>(input_to_input_weights),
            GetTensorScale(input_to_input_weights),
            GetTensorData<int8_t>(input_to_forget_weights),
            GetTensorScale(input_to_forget_weights),
            GetTensorData<int8_t>(input_to_cell_weights),
            GetTensorScale(input_to_cell_weights),
            GetTensorData<int8_t>(input_to_output_weights),
            GetTensorScale(input_to_output_weights), aux_input_ptr,
            GetTensorData<int8_t>(aux_input_to_input_weights),
            GetTensorScale(aux_input_to_input_weights),
            GetTensorData<int8_t>(aux_input_to_forget_weights),
            GetTensorScale(aux_input_to_forget_weights),
            GetTensorData<int8_t>(aux_input_to_cell_weights),
            GetTensorScale(aux_input_to_cell_weights),
            GetTensorData<int8_t>(aux_input_to_output_weights),
            GetTensorScale(aux_input_to_output_weights),
            GetTensorData<int8_t>(recurrent_to_input_weights),
            GetTensorScale(recurrent_to_input_weights),
            GetTensorData<int8_t>(recurrent_to_forget_weights),
            GetTensorScale(recurrent_to_forget_weights),
            GetTensorData<int8_t>(recurrent_to_cell_weights),
            GetTensorScale(recurrent_to_cell_weights),
            GetTensorData<int8_t>(recurrent_to_output_weights),
            GetTensorScale(recurrent_to_output_weights),
            GetTensorData<int8_t>(cell_to_input_weights),
            GetTensorScale(cell_to_input_weights),
            GetTensorData<int8_t>(cell_to_forget_weights),
            GetTensorScale(cell_to_forget_weights),
            GetTensorData<int8_t>(cell_to_output_weights),
            GetTensorScale(cell_to_output_weights),
            GetTensorData<float>(input_layer_norm_coefficients),
            GetTensorData<float>(forget_layer_norm_coefficients),
            GetTensorData<float>(cell_layer_norm_coefficients),
            GetTensorData<float>(output_layer_norm_coefficients),
            GetTensorData<float>(input_gate_bias),
            GetTensorData<float>(forget_gate_bias),
            GetTensorData<float>(cell_gate_bias),
            GetTensorData<float>(output_gate_bias),
            GetTensorData<int8_t>(projection_weights),
            GetTensorScale(projection_weights),
            GetTensorData<float>(projection_bias), params,
            /*n_batch=*/1, n_cell, n_input, aux_input_size, n_output,
            output_batch_leading_dim, input_gate_scratch_ptr,
            forget_gate_scratch_ptr, cell_gate_scratch_ptr,
            output_gate_scratch_ptr, GetTensorData<float>(input_sf),
            GetTensorData<float>(aux_input_sf),
            GetTensorData<float>(output_state_sf),
            GetTensorData<float>(prod_scaling_factors),
            GetTensorData<float>(recovered_cell_weights),
            GetTensorData<int8_t>(input_quantized),
            GetTensorData<int8_t>(aux_input_quantized),
            GetTensorData<int8_t>(output_state_quantized),
            GetTensorData<int8_t>(cell_state_quantized), output_state_ptr,
            cell_state_ptr, GetTensorData<int32_t>(output_scratch_buffer),
            output_ptr, input_zp_ptr, aux_input_zp_ptr, output_state_zp_ptr,
            row_sums_ptr, row_sums_size, compute_row_sums,
            params->asymmetric_quantize_inputs, context);
      }
    }
  }

  return kTfLiteOk;
}

TfLiteStatus EvalInteger8x8_16(
    const TfLiteTensor* input, const TfLiteTensor* input_to_input_weights,
    const TfLiteTensor* input_to_forget_weights,
    const TfLiteTensor* input_to_cell_weights,
    const TfLiteTensor* input_to_output_weights,
    const TfLiteTensor* recurrent_to_input_weights,
    const TfLiteTensor* recurrent_to_forget_weights,
    const TfLiteTensor* recurrent_to_cell_weights,
    const TfLiteTensor* recurrent_to_output_weights,
    const TfLiteTensor* cell_to_input_weights,
    const TfLiteTensor* cell_to_forget_weights,
    const TfLiteTensor* cell_to_output_weights,
    const TfLiteTensor* input_layer_norm_coefficients,
    const TfLiteTensor* forget_layer_norm_coefficients,
    const TfLiteTensor* cell_layer_norm_coefficients,
    const TfLiteTensor* output_layer_norm_coefficients,
    const TfLiteTensor* input_gate_bias, const TfLiteTensor* forget_gate_bias,
    const TfLiteTensor* cell_gate_bias, const TfLiteTensor* output_gate_bias,
    const TfLiteTensor* projection_weights, const TfLiteTensor* projection_bias,
    const TfLiteLSTMParams* params,
    const lstm_eval::IntegerLstmParameter* integer_lstm_param,
    TfLiteTensor* output_state, TfLiteTensor* cell_state, TfLiteTensor* output,
    TfLiteTensor* scratch0, TfLiteTensor* scratch1, TfLiteTensor* scratch2,
    TfLiteTensor* scratch3, TfLiteTensor* scratch4, TfLiteTensor* scratch5,
    CpuBackendContext* context) {
  TF_LITE_ASSERT(input->dims->size >= 2 && input->dims->size <= 3);
  const int n_input = input->dims->data[input->dims->size - 1];
  int max_time, n_batch;
  if (input->dims->size == 2) {
    max_time = 1;
    n_batch = input->dims->data[0];
  } else {
    max_time = input->dims->data[0];
    n_batch = input->dims->data[1];
  }

  // n_cell and n_output will be the same size when there is no projection.
  const int n_cell = input_to_output_weights->dims->data[0];
  const int n_output = recurrent_to_output_weights->dims->data[1];

  // Activation zero point
  int output_state_zp = output_state->params.zero_point;

  // Get params for time/batch/sequence.
  const int output_batch_leading_dim =
      output->dims->data[output->dims->size - 1];
  const int input_step = n_batch * n_input;
  const int output_step = n_batch * output_batch_leading_dim;

  for (int t = 0; t < max_time; t++) {
    const int t_rel = t;
    int8_t* output_ptr = GetTensorData<int8_t>(output) + t_rel * output_step;
    const int8_t* input_ptr = GetTensorData<int8_t>(input) + t_rel * input_step;
    LstmStepInteger8x8_16(
        input_ptr, GetTensorData<int8_t>(input_to_input_weights),
        integer_lstm_param->effective_input_to_input_scale_a,
        integer_lstm_param->effective_input_to_input_scale_b,
        GetTensorData<int8_t>(input_to_forget_weights),
        integer_lstm_param->effective_input_to_forget_scale_a,
        integer_lstm_param->effective_input_to_forget_scale_b,
        GetTensorData<int8_t>(input_to_cell_weights),
        integer_lstm_param->effective_input_to_cell_scale_a,
        integer_lstm_param->effective_input_to_cell_scale_b,
        GetTensorData<int8_t>(input_to_output_weights),
        integer_lstm_param->effective_input_to_output_scale_a,
        integer_lstm_param->effective_input_to_output_scale_b,
        GetTensorData<int8_t>(recurrent_to_input_weights),
        integer_lstm_param->effective_recurrent_to_input_scale_a,
        integer_lstm_param->effective_recurrent_to_input_scale_b,
        GetTensorData<int8_t>(recurrent_to_forget_weights),
        integer_lstm_param->effective_recurrent_to_forget_scale_a,
        integer_lstm_param->effective_recurrent_to_forget_scale_b,
        GetTensorData<int8_t>(recurrent_to_cell_weights),
        integer_lstm_param->effective_recurrent_to_cell_scale_a,
        integer_lstm_param->effective_recurrent_to_cell_scale_b,
        GetTensorData<int8_t>(recurrent_to_output_weights),
        integer_lstm_param->effective_recurrent_to_output_scale_a,
        integer_lstm_param->effective_recurrent_to_output_scale_b,
        GetTensorData<int16_t>(cell_to_input_weights),
        integer_lstm_param->effective_cell_to_input_scale_a,
        integer_lstm_param->effective_cell_to_input_scale_b,
        GetTensorData<int16_t>(cell_to_forget_weights),
        integer_lstm_param->effective_cell_to_forget_scale_a,
        integer_lstm_param->effective_cell_to_forget_scale_b,
        GetTensorData<int16_t>(cell_to_output_weights),
        integer_lstm_param->effective_cell_to_output_scale_a,
        integer_lstm_param->effective_cell_to_output_scale_b,
        GetTensorData<int8_t>(projection_weights),
        integer_lstm_param->effective_proj_scale_a,
        integer_lstm_param->effective_proj_scale_b,
        integer_lstm_param->hidden_zp,
        integer_lstm_param->effective_hidden_scale_a,
        integer_lstm_param->effective_hidden_scale_b,
        GetTensorData<int16_t>(input_layer_norm_coefficients),
        integer_lstm_param->layer_norm_input_scale_a,
        integer_lstm_param->layer_norm_input_scale_b,
        GetTensorData<int16_t>(forget_layer_norm_coefficients),
        integer_lstm_param->layer_norm_forget_scale_a,
        integer_lstm_param->layer_norm_forget_scale_b,
        GetTensorData<int16_t>(cell_layer_norm_coefficients),
        integer_lstm_param->layer_norm_cell_scale_a,
        integer_lstm_param->layer_norm_cell_scale_b,
        GetTensorData<int16_t>(output_layer_norm_coefficients),
        integer_lstm_param->layer_norm_output_scale_a,
        integer_lstm_param->layer_norm_output_scale_b,
        GetTensorData<int32_t>(input_gate_bias),
        GetTensorData<int32_t>(forget_gate_bias),
        GetTensorData<int32_t>(cell_gate_bias),
        GetTensorData<int32_t>(output_gate_bias),
        integer_lstm_param->quantized_cell_clip,
        integer_lstm_param->quantized_proj_clip, integer_lstm_param->cell_scale,
        integer_lstm_param->input_variance_guard,
        integer_lstm_param->forget_variance_guard,
        integer_lstm_param->cell_variance_guard,
        integer_lstm_param->output_variance_guard,
        integer_lstm_param->input_to_forget_effective_bias.get(),
        integer_lstm_param->recurrent_to_forget_effective_bias.get(),
        integer_lstm_param->input_to_cell_effective_bias.get(),
        integer_lstm_param->recurrent_to_cell_effective_bias.get(),
        integer_lstm_param->input_to_output_effective_bias.get(),
        integer_lstm_param->recurrent_to_output_effective_bias.get(),
        integer_lstm_param->input_to_input_effective_bias.get(),
        integer_lstm_param->recurrent_to_input_effective_bias.get(),
        integer_lstm_param->projection_effective_bias.get(), n_batch, n_cell,
        n_input, n_output, GetTensorData<int8_t>(output_state), output_state_zp,
        GetTensorData<int16_t>(cell_state), output_ptr,
        GetTensorData<int16_t>(scratch0), GetTensorData<int16_t>(scratch1),
        GetTensorData<int16_t>(scratch2), GetTensorData<int16_t>(scratch3),
        GetTensorData<int8_t>(scratch4), GetTensorData<int32_t>(scratch5),
        context);
  }

  return kTfLiteOk;
}

TfLiteStatus EvalInteger8x8_8(
    const TfLiteTensor* input, const TfLiteTensor* input_to_input_weights,
    const TfLiteTensor* input_to_forget_weights,
    const TfLiteTensor* input_to_cell_weights,
    const TfLiteTensor* input_to_output_weights,
    const TfLiteTensor* recurrent_to_input_weights,
    const TfLiteTensor* recurrent_to_forget_weights,
    const TfLiteTensor* recurrent_to_cell_weights,
    const TfLiteTensor* recurrent_to_output_weights,
    const TfLiteTensor* cell_to_input_weights,
    const TfLiteTensor* cell_to_forget_weights,
    const TfLiteTensor* cell_to_output_weights,
    const TfLiteTensor* input_layer_norm_coefficients,
    const TfLiteTensor* forget_layer_norm_coefficients,
    const TfLiteTensor* cell_layer_norm_coefficients,
    const TfLiteTensor* output_layer_norm_coefficients,
    const TfLiteTensor* input_gate_bias, const TfLiteTensor* forget_gate_bias,
    const TfLiteTensor* cell_gate_bias, const TfLiteTensor* output_gate_bias,
    const TfLiteTensor* projection_weights, const TfLiteTensor* projection_bias,
    const TfLiteLSTMParams* params, TfLiteTensor* output_state,
    TfLiteTensor* cell_state, TfLiteTensor* output,
    const lstm_eval::IntegerLstmParameter* integer_lstm_param,
    TfLiteTensor* scratch0, TfLiteTensor* scratch1, TfLiteTensor* scratch2,
    TfLiteTensor* scratch3, TfLiteTensor* scratch4, TfLiteTensor* scratch5,
    TfLiteTensor* scratch6, TfLiteTensor* scratch7) {
  TF_LITE_ASSERT(input->dims->size >= 2 && input->dims->size <= 3);
  const int n_input = input->dims->data[input->dims->size - 1];
  int max_time, n_batch;
  if (input->dims->size == 2) {
    max_time = 1;
    n_batch = input->dims->data[0];
  } else {
    max_time = input->dims->data[0];
    n_batch = input->dims->data[1];
  }

  // n_cell and n_output will be the same size when there is no projection.
  const int n_cell = input_to_output_weights->dims->data[0];
  const int n_output = recurrent_to_output_weights->dims->data[1];

  const int32_t input_zp = input->params.zero_point;
  const int32_t output_state_zp = output_state->params.zero_point;

  // Get params for time/batch/sequence.
  const int output_batch_leading_dim =
      output->dims->data[output->dims->size - 1];
  const int input_step = n_batch * n_input;
  const int output_step = n_batch * output_batch_leading_dim;

  for (int t = 0; t < max_time; t++) {
    const int t_rel = t;
    int8_t* output_ptr = GetTensorData<int8_t>(output) + t_rel * output_step;
    // Input can be int8 asymmetric or int16 symmetric.
    const int8_t* input_ptr = GetTensorData<int8_t>(input) + t_rel * input_step;
    lstm_eval::LstmStepInteger8x8_8(
        input_ptr, input_zp,

        GetTensorData<int8_t>(input_to_input_weights),
        integer_lstm_param->effective_input_to_input_scale_a,
        integer_lstm_param->effective_input_to_input_scale_b,

        GetTensorData<int8_t>(input_to_forget_weights),
        integer_lstm_param->effective_input_to_forget_scale_a,
        integer_lstm_param->effective_input_to_forget_scale_b,

        GetTensorData<int8_t>(input_to_cell_weights),
        integer_lstm_param->effective_input_to_cell_scale_a,
        integer_lstm_param->effective_input_to_cell_scale_b,

        GetTensorData<int8_t>(input_to_output_weights),
        integer_lstm_param->effective_input_to_output_scale_a,
        integer_lstm_param->effective_input_to_output_scale_b,

        GetTensorData<int8_t>(recurrent_to_input_weights),
        integer_lstm_param->effective_recurrent_to_input_scale_a,
        integer_lstm_param->effective_recurrent_to_input_scale_b,

        GetTensorData<int8_t>(recurrent_to_forget_weights),
        integer_lstm_param->effective_recurrent_to_forget_scale_a,
        integer_lstm_param->effective_recurrent_to_forget_scale_b,

        GetTensorData<int8_t>(recurrent_to_cell_weights),
        integer_lstm_param->effective_recurrent_to_cell_scale_a,
        integer_lstm_param->effective_recurrent_to_cell_scale_b,

        GetTensorData<int8_t>(recurrent_to_output_weights),
        integer_lstm_param->effective_recurrent_to_output_scale_a,
        integer_lstm_param->effective_recurrent_to_output_scale_b,

        GetTensorData<int8_t>(cell_to_input_weights),
        integer_lstm_param->effective_cell_to_input_scale_a,
        integer_lstm_param->effective_cell_to_input_scale_b,

        GetTensorData<int8_t>(cell_to_forget_weights),
        integer_lstm_param->effective_cell_to_forget_scale_a,
        integer_lstm_param->effective_cell_to_forget_scale_b,

        GetTensorData<int8_t>(cell_to_output_weights),
        integer_lstm_param->effective_cell_to_output_scale_a,
        integer_lstm_param->effective_cell_to_output_scale_b,

        GetTensorData<int8_t>(projection_weights),
        integer_lstm_param->effective_proj_scale_a,
        integer_lstm_param->effective_proj_scale_b,

        GetTensorData<int16_t>(input_layer_norm_coefficients),
        integer_lstm_param->layer_norm_input_scale_a,
        integer_lstm_param->layer_norm_input_scale_b,

        GetTensorData<int16_t>(forget_layer_norm_coefficients),
        integer_lstm_param->layer_norm_forget_scale_a,
        integer_lstm_param->layer_norm_forget_scale_b,

        GetTensorData<int16_t>(cell_layer_norm_coefficients),
        integer_lstm_param->layer_norm_cell_scale_a,
        integer_lstm_param->layer_norm_cell_scale_b,

        GetTensorData<int16_t>(output_layer_norm_coefficients),
        integer_lstm_param->layer_norm_output_scale_a,
        integer_lstm_param->layer_norm_output_scale_b,

        GetTensorData<int32_t>(input_gate_bias),
        GetTensorData<int32_t>(forget_gate_bias),
        GetTensorData<int32_t>(cell_gate_bias),
        GetTensorData<int32_t>(output_gate_bias),
        GetTensorData<int32_t>(projection_bias),

        params, integer_lstm_param->intermediate_scale_a,
        integer_lstm_param->intermediate_scale_b,
        integer_lstm_param->intermediate_zp,
        integer_lstm_param->quantized_cell_clip,
        integer_lstm_param->quantized_proj_clip, n_batch, n_cell, n_input,
        n_output, output_batch_leading_dim, GetTensorData<int8_t>(output_state),
        output_state_zp, GetTensorData<int16_t>(cell_state), output_ptr,
        GetTensorData<int8_t>(scratch0), GetTensorData<int8_t>(scratch1),
        GetTensorData<int16_t>(scratch2), GetTensorData<int16_t>(scratch3),
        GetTensorData<int16_t>(scratch4), GetTensorData<int16_t>(scratch5),
        GetTensorData<int16_t>(scratch6), GetTensorData<int16_t>(scratch7));
  }

  return kTfLiteOk;
}

}  // namespace lstm_eval
}  // namespace builtin
}  // namespace ops
}  // namespace tflite
