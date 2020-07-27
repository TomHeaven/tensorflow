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
#include "tensorflow/c/eager/gradients.h"
#include "tensorflow/c/eager/mnist_gradients_util.h"


#include <memory>

#include "absl/types/span.h"
#include "tensorflow/c/eager/abstract_tensor_handle.h"
#include "tensorflow/c/eager/c_api_experimental.h"
#include "tensorflow/c/eager/c_api_test_util.h"
#include "tensorflow/c/eager/c_api_unified_experimental.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/eager/gradients_internal.h"
#include "tensorflow/c/experimental/gradients/math_grad.h"
#include "tensorflow/c/experimental/ops/array_ops.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/c/tf_tensor.h"
#include "tensorflow/core/lib/llvm_rtti/llvm_rtti.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/test.h"


namespace tensorflow {
namespace gradients {
namespace internal {
namespace {

class CppGradients
    : public ::testing::TestWithParam<std::tuple<const char*, bool, bool>> {
 protected:
  void SetUp() override {
    TF_SetTracingImplementation(std::get<0>(GetParam()));
  }
};


// ========================= Test Util Functions ==============================
void printArr(float data[], int n)
{
  std::cout << std::endl << "[";
  for(int i = 0; i < n-1; i++){
    std::cout << data[i] << ", ";
  }
  std::cout << data [n-1] << "]" << std::endl;

}

float sumArr(float data [], int n)
{
  float sum = 0;
  for(int i = 0; i < n; i++) {
    sum += data[i]; 
  }
  return sum;
}

// Get a scalar TensorHandle woth given value
Status TestScalarTensorHandle(AbstractContext* ctx, float value,
                              AbstractTensorHandle** tensor) {
  
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_Context* eager_ctx =
      TF_ExecutionContextGetTFEContext(wrap(ctx), status.get());
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  TFE_TensorHandle* input_eager = TestScalarTensorHandle(eager_ctx, value);
  *tensor =
      unwrap(TF_CreateAbstractTensorFromEagerTensor(input_eager, status.get()));
  return Status::OK();
}

// Get a Matrix TensorHandle with given float values and dimensions
Status TestMatrixTensorHandleFloat(AbstractContext* ctx, float data[], int64_t dims[], 
                                   int num_dims, AbstractTensorHandle** tensor) {
  
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_Context* eager_ctx =
      TF_ExecutionContextGetTFEContext(wrap(ctx), status.get());
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  TFE_TensorHandle* input_eager = 
      TestMatrixTensorHandleFloat(eager_ctx, data, dims, num_dims);
  *tensor = 
      unwrap(TF_CreateAbstractTensorFromEagerTensor(input_eager, status.get()));
  return Status::OK();
}

// Get a Matrix TensorHandle with given int values and dimensions
Status TestMatrixTensorHandleInt(AbstractContext* ctx, int data[], int64_t dims[], 
                                 int num_dims, AbstractTensorHandle** tensor) {
  
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_Context* eager_ctx =
      TF_ExecutionContextGetTFEContext(wrap(ctx), status.get());
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  TFE_TensorHandle* input_eager = 
      TestMatrixTensorHandleInt(eager_ctx, data, dims, num_dims);
  *tensor = 
      unwrap(TF_CreateAbstractTensorFromEagerTensor(input_eager, status.get()));
  return Status::OK();
}
 
Status getValue(AbstractTensorHandle* t, TF_Tensor** result_tensor) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_TensorHandle* result_t =
      TF_AbstractTensorGetEagerTensor(wrap(t), status.get());
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  *result_tensor = TFE_TensorHandleResolve(result_t, status.get());
  return Status::OK();
}

AbstractTensorHandlePtr getMatrixTensorHandleUtilFloat(AbstractContext* ctx, float vals[], int64_t dims[], int num_dims){

  AbstractTensorHandlePtr A;
  AbstractTensorHandle* a_raw = nullptr;
  Status s = TestMatrixTensorHandleFloat(ctx, vals, dims, num_dims, &a_raw);
  A.reset(a_raw);
  return A;
}

AbstractTensorHandlePtr getMatrixTensorHandleUtilInt(AbstractContext* ctx, int vals[], int64_t dims[], int num_dims){

  AbstractTensorHandlePtr A;
  AbstractTensorHandle* a_raw = nullptr;
  Status s = TestMatrixTensorHandleInt(ctx, vals, dims, num_dims, &a_raw);
  A.reset(a_raw);
  return A;
}

void printTensor(AbstractTensorHandle* t, int size){

  TF_Tensor* tensor;
  Status s = getValue(t, &tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  float result_data[size] = {0};
  memcpy(&result_data[0], TF_TensorData(tensor), TF_TensorByteSize(tensor));
  printArr(result_data, size);

  TF_DeleteTensor(tensor);
}

// ============================== Start Tests =================================================

TEST_P(CppGradients, TestAddGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  AbstractTensorHandlePtr x;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 2.0f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    x.reset(x_raw);
  }

  AbstractTensorHandlePtr y;
  {
    AbstractTensorHandle* y_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 2.0f, &y_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    y.reset(y_raw);
  }

  GradientRegistry registry;
  Status s = RegisterGradientAdd(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  /* Pseudo-code:
   *
   * tape.watch(x)
   * tape.watch(y)
   * y = x + y
   * outputs = tape.gradient(y, [x, y])
   */

  std::vector<AbstractTensorHandle*> outputs(2);
  s = RunModel(AddGradModel, ctx.get(), {x.get(), y.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* result_tensor;
  s = getValue(outputs[0], &result_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  auto result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_EQ(*result_value, 1.0);
  outputs[0]->Release();
  TF_DeleteTensor(result_tensor);
  result_tensor = nullptr;

  s = getValue(outputs[1], &result_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_EQ(*result_value, 1.0);
  outputs[1]->Release();
  TF_DeleteTensor(result_tensor);
}


TEST_P(CppGradients, TestMatMulGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  float A_vals [] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t A_dims [] = {2, 2};
  float B_vals [] = {.5f, -1.0f, 1.0f, 1.0f}; 
  int64_t B_dims [] = {2, 2};
  int num_dims = 2;
  
  AbstractTensorHandlePtr A = getMatrixTensorHandleUtilFloat(ctx.get(), A_vals, A_dims, num_dims);
  AbstractTensorHandlePtr B = getMatrixTensorHandleUtilFloat(ctx.get(), B_vals, B_dims, num_dims);
  
  GradientRegistry registry;
  Status s = RegisterGradientMatMul(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  /* Pseudo-code:
   *
   * tape.watch(A)
   * tape.watch(B)
   * Y = AB
   * outputs = tape.gradient(Y, [A, B])
   */

  std::vector<AbstractTensorHandle*> outputs(2);
  s = RunModel(MatMulGradModel, ctx.get(), {A.get(), B.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* dA_tensor;
  s = getValue(outputs[0], &dA_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  float result_data[4] = {0};
  memcpy(&result_data[0], TF_TensorData(dA_tensor), TF_TensorByteSize(dA_tensor));
  
  float expected_dA [4] =  {-.5f, 2.0f, -.5f, 2.0f}; 
  float tolerance = 1e-3;
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_dA[j], tolerance);
  }  

  TF_Tensor* dB_tensor;
  s = getValue(outputs[1], &dB_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  memcpy(&result_data[0], TF_TensorData(dB_tensor), TF_TensorByteSize(dB_tensor));
  
  float expected_dB [4] =  {4.0f, 4.0f, 6.0f, 6.0f}; 
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_dB[j], tolerance);
  }  

  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(dA_tensor);
  TF_DeleteTensor(dB_tensor);
}

// Computes
// y = inputs[0] * inputs[1]
// return grad(y, {inputs[0], inputs[1]})
Status MatMulGradModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(inputs[0]));  // Watch x.
  tape->Watch(ToId(inputs[1]));  // Watch y.
  std::vector<AbstractTensorHandle*> mm_outputs(1);
  TF_RETURN_IF_ERROR(MatMul(ctx, tape, inputs, absl::MakeSpan(mm_outputs), 
      "matmul0", /*transpose_a=*/false, /*transpose_b=*/false, registry));  // Compute x*y.
  
  std::unordered_map<tensorflow::int64, TapeTensor>
      source_tensors_that_are_targets;

  std::vector<AbstractTensorHandle*> out_grads;
  TF_RETURN_IF_ERROR(tape->ComputeGradient(
      vspace, /*target_tensor_ids=*/{ToId(mm_outputs[0])},
      /*source_tensor_ids=*/{ToId(inputs[0]), ToId(inputs[1])},
      source_tensors_that_are_targets,
      /*output_gradients=*/{}, &out_grads));
  for (auto mm_output : mm_outputs) {
    mm_output->Release();
  }
  outputs[0] = out_grads[0];
  outputs[1] = out_grads[1];
  delete tape;
  return Status::OK();
}


// TODO: fix graph mode test by using RunModel to verify
TEST_P(CppGradients, TestMatMulGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s = BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  float A_vals [] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t A_dims [] = {2, 2};
  float B_vals [] = {.5f, -1.0f, 1.0f, 1.0f}; 
  int64_t B_dims [] = {2, 2};
  int num_dims = 2;
  
  AbstractTensorHandlePtr A = getMatrixTensorHandleUtilFloat(ctx.get(), A_vals, A_dims, num_dims);
  AbstractTensorHandlePtr B = getMatrixTensorHandleUtilFloat(ctx.get(), B_vals, B_dims, num_dims);
  
  GradientRegistry registry;
  Status s = RegisterGradientMatMul(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  /* Pseudo-code:
   *
   * tape.watch(A)
   * tape.watch(B)
   * Y = AB
   * outputs = tape.gradient(Y, [A, B])
   */

  std::vector<AbstractTensorHandle*> outputs(2);
  s = RunModel(MatMulGradModel, ctx.get(), {A.get(), B.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* dA_tensor;
  s = getValue(outputs[0], &dA_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  float result_data[4] = {0};
  memcpy(&result_data[0], TF_TensorData(dA_tensor), TF_TensorByteSize(dA_tensor));
  
  float expected_dA [4] =  {-.5f, 2.0f, -.5f, 2.0f}; 
  float tolerance = 1e-3;
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_dA[j], tolerance);
  }  

  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(dA_tensor);
}

// Computes
// y = inputs[0] * inputs[1]
// return grad(y, {inputs[0], inputs[1]})
Status MatMulGradModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(inputs[0]));  // Watch x.
  tape->Watch(ToId(inputs[1]));  // Watch y.
  std::vector<AbstractTensorHandle*> mm_outputs(1);
  TF_RETURN_IF_ERROR(MatMul(ctx, tape, inputs, absl::MakeSpan(mm_outputs), 
      "matmul0", /*transpose_a=*/false, /*transpose_b=*/false, registry));  // Compute x*y.
  
  std::unordered_map<tensorflow::int64, TapeTensor>
      source_tensors_that_are_targets;

  std::vector<AbstractTensorHandle*> out_grads;
  TF_RETURN_IF_ERROR(tape->ComputeGradient(
      vspace, /*target_tensor_ids=*/{ToId(mm_outputs[0])},
      /*source_tensor_ids=*/{ToId(inputs[0]), ToId(inputs[1])},
      source_tensors_that_are_targets,
      /*output_gradients=*/{}, &out_grads));
  for (auto mm_output : mm_outputs) {
    mm_output->Release();
  }
  outputs[0] = out_grads[0];
  outputs[1] = out_grads[1];
  delete tape;
  return Status::OK();
}


// TODO: fix graph mode test by using RunModel to verify
TEST_P(CppGradients, TestMatMulGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  float A_vals [] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t A_dims [] = {2, 2};
  float B_vals [] = {.5f, -1.0f, 1.0f, 1.0f}; 
  int64_t B_dims [] = {2, 2};
  int num_dims = 2;
  
  AbstractTensorHandlePtr A = getMatrixTensorHandleUtilFloat(ctx.get(), A_vals, A_dims, num_dims);
  AbstractTensorHandlePtr B = getMatrixTensorHandleUtilFloat(ctx.get(), B_vals, B_dims, num_dims);
  
  GradientRegistry registry;
  Status s = RegisterGradientMatMul(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Pseudo-code:
  //
  // tape.watch(A)
  // tape.watch(B)
  // Y = AB
  // outputs = tape.gradient(Y, [A, B])
  std::vector<AbstractTensorHandle*> outputs(2);
  // s = RunModel(MatMulGradModel, ctx.get(), {A.get(), B.get()},
  //              absl::MakeSpan(outputs),
  //              /*use_function=*/!std::get<2>(GetParam()), registry);
  // ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  s = MatMulGradModel(ctx.get(), {A.get(), B.get()}, absl::MakeSpan(outputs), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // s = MatMulGradModel(ctx.get(), {A.get(), B.get()}, absl::MakeSpan(outputs), registry);
  // ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* dA_tensor;
  s = getValue(outputs[0], &dA_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  float result_data[4] = {0};
  memcpy(&result_data[0], TF_TensorData(dA_tensor), TF_TensorByteSize(dA_tensor));
  
  float expected_dA [4] =  {-.5f, 2.0f, -.5f, 2.0f}; 
  float tolerance = 1e-3;
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_dA[j], tolerance);
  }  

  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(dA_tensor);
}

TEST_P(CppGradients, TestMNISTForward) {
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s = BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = data
  float X_vals [] = {1.0f,2.0f,3.0f,4.0f};
  int64_t dims [] = {2,2};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, dims, num_dims);
 
  // W1 = first weights
  float W1_vals [] = {-1.0f,10.0f,.5f,1.0f};
  AbstractTensorHandlePtr W1 = getMatrixTensorHandleUtilFloat(ctx.get(), W1_vals, dims, num_dims);
 
  // W2 = second weights
  float W2_vals [] = {.1f,.2f,.3f,-.5f};
  AbstractTensorHandlePtr W2 = getMatrixTensorHandleUtilFloat(ctx.get(), W2_vals, dims, num_dims);

  // y = labels
  int y_vals [] = {1,1};
  int64_t dims_y [] = {2};
  num_dims = sizeof(dims_y)/sizeof(dims_y[0]);
  AbstractTensorHandlePtr y = getMatrixTensorHandleUtilInt(ctx.get(), y_vals, dims, num_dims);

  GradientRegistry registry;
 
  // Run the Forward Pass
  std::vector<AbstractTensorHandle*> outputs(2);
  Status s = RunModel(MNISTForwardModel, ctx.get(), {X.get(), W1.get(), W2.get(), y.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Verify the Results
  TF_Tensor* scores_tensor;
  s = getValue(outputs[0], &scores_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  float result_data[4] = {0};
  memcpy(&result_data[0], TF_TensorData(scores_tensor), TF_TensorByteSize(scores_tensor));
  
  float expected_scores [4] = {3.6f, -6.0f, 10.2f, -17.0f};
  float tolerance = 1e-3;
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_scores[j], tolerance);
  }

  TF_Tensor* loss_vals_tensor;
  s = getValue(outputs[1], &loss_vals_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  memcpy(&result_data[0], TF_TensorData(loss_vals_tensor), TF_TensorByteSize(loss_vals_tensor));
  float expected_losses [2] = {9.6f, 27.2f};
  for(int j = 0; j < 2; j++){
    ASSERT_NEAR(result_data[j], expected_losses[j], tolerance);
  }
  
  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(scores_tensor);
  TF_DeleteTensor(loss_vals_tensor);
}

TEST_P(CppGradients, TestMNISTForward2) {
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s = BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = data
  float X_vals [] = {1.0f,2.0f,3.0f,4.0f, 5.0f, 6.0f};
  int64_t X_dims [] = {3,2};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, X_dims, num_dims);
 
  // W1 = first weights
  float W1_vals [] = {-1.0f,10.0f,.5f,1.0f};
  int64_t dims [] = {2,2};
  AbstractTensorHandlePtr W1 = getMatrixTensorHandleUtilFloat(ctx.get(), W1_vals, dims, num_dims);
 
  // W2 = second weights
  float W2_vals [] = {.1f,.2f,.3f,-.5f};
  AbstractTensorHandlePtr W2 = getMatrixTensorHandleUtilFloat(ctx.get(), W2_vals, dims, num_dims);

  // y = labels
  int y_vals [] = {1, 1, 1};
  int64_t y_dims [] = {3};
  num_dims = sizeof(y_dims)/sizeof(y_dims[0]);
  AbstractTensorHandlePtr y = getMatrixTensorHandleUtilInt(ctx.get(), y_vals, y_dims, num_dims);

  GradientRegistry registry;
 
  // Run the Forward Pass
  std::vector<AbstractTensorHandle*> outputs(2);
  Status s = RunModel(MNISTForwardModel, ctx.get(), {X.get(), W1.get(), W2.get(), y.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Verify the Results
  TF_Tensor* scores_tensor;
  s = getValue(outputs[0], &scores_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  float result_data[6] = {0};
  memcpy(&result_data[0], TF_TensorData(scores_tensor), TF_TensorByteSize(scores_tensor));
  
  float expected_scores [6] = {3.6f, -6.0f, 10.2f, -17.0f, 16.8f, -28.0f};
  float tolerance = 1e-3;
  for(int j = 0; j < 6; j++){
    ASSERT_NEAR(result_data[j], expected_scores[j], tolerance);
  }

  TF_Tensor* loss_vals_tensor;
  s = getValue(outputs[1], &loss_vals_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  memcpy(&result_data[0], TF_TensorData(loss_vals_tensor), TF_TensorByteSize(loss_vals_tensor));
  float expected_losses [3] = {9.6f, 27.2f, 44.8f};
  for(int j = 0; j < 3; j++){
    ASSERT_NEAR(result_data[j], expected_losses[j], tolerance);
  }
  
  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(scores_tensor);
  TF_DeleteTensor(loss_vals_tensor);
}

// Test Model to see if transpose attributes are working
Status MatMulTransposeModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  
  AbstractTensorHandle* X = inputs[0];
  AbstractTensorHandle* W1 = inputs[1];
 
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(X));
  tape->Watch(ToId(W1));  // Watch W1.
  std::vector<AbstractTensorHandle*> temp_outputs(1);

  TF_RETURN_IF_ERROR(MatMul(ctx, tape, {X, W1}, absl::MakeSpan(temp_outputs),
                     "matmul0",/*transpose_a=*/true,/*transpose_b=*/false, registry));  // Compute X*W1

  outputs[0] =  temp_outputs[0];

  delete tape;
  return Status::OK();
}

// TODO: fix graph mode test by using RunModel to verify
TEST_P(CppGradients, TestMatMulTranspose) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s = BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = data
  float X_vals [] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  int64_t X_dims [] = {2,3};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, X_dims, num_dims);
 
  // W1 = first weights
  float W1_vals [] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t dims [] = {2,2};
  AbstractTensorHandlePtr W1 = getMatrixTensorHandleUtilFloat(ctx.get(), W1_vals, dims, num_dims);
 
  GradientRegistry registry;
  
  // Run the MatMul Op
  std::vector<AbstractTensorHandle*> outputs(1);
  
  Status s = RunModel(MatMulTransposeModel, ctx.get(), {X.get(), W1.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);

  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  // Verify the Results
  TF_Tensor* scores_tensor;
  s = getValue(outputs[0], &scores_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  float result_data[6] = {0};
  memcpy(&result_data[0], TF_TensorData(scores_tensor), TF_TensorByteSize(scores_tensor));
  
  float expected_scores [6] = {13.0f, 18.0f, 17.0f, 24.0f, 21.0f, 30.0f};
  float tolerance = 1e-3;

  for(int j = 0; j < 6; j++){
    ASSERT_NEAR(result_data[j], expected_scores[j], tolerance);
  }
  
}

TEST_P(CppGradients, TestReluGrad) {

  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s = BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = data
  float X_vals [] = {1.0f, 2.0f, 3.0f, -5.0f, -4.0f, -3.0f, 2.0f, 0.0f, -1.0f};
  int64_t X_dims [] = {3,3};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, X_dims, num_dims);
 
  GradientRegistry registry;
  Status s = RegisterGradientRelu(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  /* Pseudo-code:
   *
   * tape.watch(X)
   * Y = Relu(X)
   * outputs = tape.gradient(Y, [X])
   */
  std::vector<AbstractTensorHandle*> outputs(1);
  s = RunModel(ReluGradModel, ctx.get(), {X.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* dX_tensor;
  s = getValue(outputs[0], &dX_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  float result_data[9] = {0};
  memcpy(&result_data[0], TF_TensorData(dX_tensor), TF_TensorByteSize(dX_tensor));
  
  float expected_dX [9] =  {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}; 
  float tolerance = 1e-3;
  for(int j = 0; j < 9; j++){
    ASSERT_NEAR(result_data[j], expected_dX[j], tolerance);
  }  

  outputs[0]->Release();
  TF_DeleteTensor(dX_tensor);
}


TEST_P(CppGradients, TestSoftmaxLossGrad) {

  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s = BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = scores
  float X_vals [] = {1.0f, 2.0f, 3.0f, -5.0f, -4.0f, -3.0f, 2.0f, 0.0f, -1.0f};
  int64_t X_dims [] = {3,3};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, X_dims, num_dims);

  // y = labels
  int y_vals [] = {1, 0, 1};
  int64_t y_dims [] = {3};
  num_dims = sizeof(y_dims)/sizeof(y_dims[0]);
  AbstractTensorHandlePtr y = getMatrixTensorHandleUtilInt(ctx.get(), y_vals, y_dims, num_dims);
 
  GradientRegistry registry;
  Status s = RegisterGradientSparseSoftmaxCrossEntropyLoss(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  /* Pseudo-code:
   *
   * tape.watch(X)
   * tape.watch(labels)
   * loss = SoftmaxLoss(X, labels)
   * outputs = tape.gradient(loss, [X, labels])
   * 
   *
   */ 

  std::vector<AbstractTensorHandle*> outputs(2);
  s = RunModel(SoftmaxLossGradModel, ctx.get(), {X.get(), y.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);

  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* dX_tensor;
  s = getValue(outputs[0], &dX_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  float result_data[9] = {0};
  memcpy(&result_data[0], TF_TensorData(dX_tensor), TF_TensorByteSize(dX_tensor));
  
  float expected_dX [9] =  {0.090f, -0.7553f, 0.6652f,
                            -0.9099f, 0.2447f, 0.6652f,
                            0.8437f, -0.8858f, 0.0420f}; 
  float tolerance = 1e-3;
  for(int j = 0; j < 9; j++){
    ASSERT_NEAR(result_data[j], expected_dX[j], tolerance);
  }  

  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(dX_tensor);
}


TEST_P(CppGradients, TestMNISTGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = data
  float X_vals [] = {1.0f, 2.0f, 3.0f, 4.0f}; 
  int64_t X_dims [] = {2,2};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, X_dims, num_dims);
 
  // W1 = first weights
  float W1_vals [] = {-1.0f, 10.0f, .5f, 1.0f};
  int64_t dims [] = {2,2};
  AbstractTensorHandlePtr W1 = getMatrixTensorHandleUtilFloat(ctx.get(), W1_vals, dims, num_dims);
 
  // W2 = second weights
  float W2_vals [] = {.1f, .2f, .3f, -.5f};
  AbstractTensorHandlePtr W2 = getMatrixTensorHandleUtilFloat(ctx.get(), W2_vals, dims, num_dims);

  // y = labels
  int y_vals [] = {1, 1};
  int64_t y_dims [] = {2};
  num_dims = sizeof(y_dims)/sizeof(y_dims[0]);
  AbstractTensorHandlePtr y = getMatrixTensorHandleUtilInt(ctx.get(), y_vals, y_dims, num_dims);

  // Register Grads 
  GradientRegistry registry;
  Status s = RegisterGradientMatMul(&registry);
  s = RegisterGradientRelu(&registry);
  s = RegisterGradientSparseSoftmaxCrossEntropyLoss(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  /* Pseudo-code:
   *
   *
   * tape.watch(W1)
   * tape.watch(W2)
   * mm = X*W1
   * hidden = Relu(mm)
   * scores = W2*hidden
   * loss = SoftmaxLoss(scores, y)
   * outputs = tape.gradient(loss, [A, B])
   *
   */

  std::vector<AbstractTensorHandle*> outputs(3);
  s = RunModel(MNISTGradModel, ctx.get(), {X.get(), W1.get(), W2.get(), y.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  float tolerance = 1e-3;
  TF_Tensor* dW1_tensor;
  s = getValue(outputs[0], &dW1_tensor); 
  ASSERT_EQ(errors::OK, s.code()) << s.error_message(); 
  
  float result_data[4] = {0};
  memcpy(&result_data[0], TF_TensorData(dW1_tensor), TF_TensorByteSize(dW1_tensor));
  
  float expected_dW1 [4] = {0.0f, 3.2f, 0.0f, 4.8f}; ;        //dLoss      
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_dW1[j], tolerance);
  }  

  TF_Tensor* dW2_tensor;
  s = getValue(outputs[1], &dW2_tensor); 
  ASSERT_EQ(errors::OK, s.code()) << s.error_message(); 

  memcpy(&result_data[0], TF_TensorData(dW2_tensor), TF_TensorByteSize(dW2_tensor));
     
  float expected_dW2 [4] = {0.0f, 0.0f, 46.0f, -46.0f};   //dLoss
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], expected_dW2[j], tolerance);
  }  

  outputs[0]->Release();
  outputs[1]->Release();
  outputs[2]->Release();
  TF_DeleteTensor(dW1_tensor);
  TF_DeleteTensor(dW2_tensor);
}

TEST_P(CppGradients, TestScalarMul) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

   AbstractTensorHandlePtr eta;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 1.5f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    eta.reset(x_raw);
  }

  float A_vals [] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t A_dims [] = {2, 2};
  int num_dims = 2;
  
  AbstractTensorHandlePtr A = getMatrixTensorHandleUtilFloat(ctx.get(), A_vals, A_dims, num_dims);

  GradientRegistry registry;
  std::vector<AbstractTensorHandle*> outputs(1);
  Status s = RunModel(ScalarMulModel, ctx.get(), {eta.get(), A.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* dA_tensor;
  s = getValue(outputs[0], &dA_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  
  float result_data[4] = {0};
  memcpy(&result_data[0], TF_TensorData(dA_tensor), TF_TensorByteSize(dA_tensor));
  
  float tolerance = 1e-3;
  float eta_val = 1.5f;
  for(int j = 0; j < 4; j++){
    ASSERT_NEAR(result_data[j], eta_val*A_vals[j], tolerance);
  }  

  outputs[0]->Release();
  TF_DeleteTensor(dA_tensor);
}

TEST_P(CppGradients, TestMNIST_Training) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  // X = data
  float X_vals [] = {1.0f, 2.0f, 3.0f, 4.0f}; 
  int64_t X_dims [] = {2,2};
  int num_dims = 2;
  AbstractTensorHandlePtr X = getMatrixTensorHandleUtilFloat(ctx.get(), X_vals, X_dims, num_dims);
 
  // W1 = first weights
  float W1_vals [] = {-.01f, 0.4f, 0.5f, -.2f};
  int64_t dims [] = {2,2};
  AbstractTensorHandlePtr W1 = getMatrixTensorHandleUtilFloat(ctx.get(), W1_vals, dims, num_dims);
 
  // W2 = second weights
  float W2_vals [] = {.1f, .2f, .3f, -.5f};
  AbstractTensorHandlePtr W2 = getMatrixTensorHandleUtilFloat(ctx.get(), W2_vals, dims, num_dims);

  // y = labels
  int y_vals [] = {1, 1};
  int64_t y_dims [] = {2};
  num_dims = sizeof(y_dims)/sizeof(y_dims[0]);
  AbstractTensorHandlePtr y = getMatrixTensorHandleUtilInt(ctx.get(), y_vals, y_dims, num_dims);

  // Register Grads 
  GradientRegistry registry;
  Status s = RegisterGradientMatMul(&registry);
  s = RegisterGradientRelu(&registry);
  s = RegisterGradientSparseSoftmaxCrossEntropyLoss(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Prepare for training
  std::vector<AbstractTensorHandle*> weights;
  weights.push_back(W1.get());
  weights.push_back(W2.get());

  // Set learning rate to be 1e-3
  AbstractTensorHandle* learning_rate = nullptr;
  s = TestScalarTensorHandle(ctx.get(), -1e-2, &learning_rate);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
 
  // Train
  int num_iters = 100;
  std::vector<AbstractTensorHandle*> mnist_outputs(3);
  std::vector<AbstractTensorHandle*> grads(2);
  for(int i = 0; i < num_iters; i++) {
    
    std::cout << "iter " << i << ": " << std::endl; 

    // Run Forward Pass
    s = RunModel(MNISTGradModel, ctx.get(), {X.get(), weights[0], weights[1], y.get()},
               absl::MakeSpan(mnist_outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();

    // Fill grads
    grads[0] = mnist_outputs[0];
    grads[1] = mnist_outputs[1];

    // Gradient Update
    s = UpdateWeights(ctx.get(), grads, weights, learning_rate);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();

    // Print Loss
    AbstractTensorHandle* loss_vals = mnist_outputs[2];
    TF_Tensor* loss_tensor;
    s = getValue(loss_vals, &loss_tensor); 
    ASSERT_EQ(errors::OK, s.code()) << s.error_message(); 
  
    float result_data[2] = {0};
    memcpy(&result_data[0], TF_TensorData(loss_tensor), TF_TensorByteSize(loss_tensor));
    std::cout << "     loss = " << sumArr(result_data, 2) << std::endl;
    std::cout << "-----------------" << std::endl;
    TF_DeleteTensor(loss_tensor);   
  }

  grads[0]->Release();
  grads[1]->Release();
  mnist_outputs[2]->Release();
}


// TODO(b/160888630): Enable this test with mlir after AddInputList is
// supported. It is needed for AddN op which is used for gradient aggregation.
#ifdef PLATFORM_GOOGLE
INSTANTIATE_TEST_SUITE_P(
    UnifiedCAPI, CppGradients,
    ::testing::Combine(::testing::Values("graphdef"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));  // change back to (true,false)
#else
INSTANTIATE_TEST_SUITE_P(
    UnifiedCAPI, CppGradients,
    ::testing::Combine(::testing::Values("graphdef"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false))); // change back to (true,false)
#endif
}  // namespace
}  // namespace internal
}  // namespace gradients
}  // namespace tensorflow

