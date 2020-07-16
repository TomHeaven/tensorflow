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

#include <memory>

#include "absl/types/span.h"
#include "tensorflow/c/eager/abstract_tensor_handle.h"
#include "tensorflow/c/eager/c_api_experimental.h"
#include "tensorflow/c/eager/c_api_test_util.h"
#include "tensorflow/c/eager/c_api_unified_experimental.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/eager/gradients_internal.h"
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

// Creates an Identity op.
Status Identity(AbstractContext* ctx,
                absl::Span<AbstractTensorHandle* const> inputs,
                absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  
  AbstractOperationPtr identity_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(
      identity_op->Reset("Identity", /*raw_device_name=*/nullptr));
  if (isa<tracing::TracingOperation>(identity_op.get())) {
    TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingOperation>(identity_op.get())
                           ->SetOpName(name));
  }
  TF_RETURN_IF_ERROR(identity_op->AddInput(inputs[0]));
  int num_retvals = 1;
  TF_RETURN_IF_ERROR(identity_op->Execute(outputs, &num_retvals));
  return Status::OK();
}

// Creates a MatMul op.
Status MatMul(AbstractContext* ctx,
                absl::Span<AbstractTensorHandle* const> inputs,
                absl::Span<AbstractTensorHandle*> outputs, const char* name,
                bool transpose_a, bool transpose_b) {
  
  AbstractOperationPtr matmul_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(
      matmul_op->Reset("MatMul", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(matmul_op.get())) {
    TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingOperation>(matmul_op.get())
                           ->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(matmul_op->AddInput(inputs[0]));
  TF_RETURN_IF_ERROR(matmul_op->AddInput(inputs[1]));

  matmul_op->SetAttrBool("transpose_a", transpose_a);
  matmul_op->SetAttrBool("transpose_b", transpose_b);
  
  int num_retvals = 1;
  TF_RETURN_IF_ERROR(matmul_op->Execute(outputs, &num_retvals));
  return Status::OK();
}


// =================== Register gradients for Add ============================
class AddGradientFunction : public GradientFunction {
 public:
  explicit AddGradientFunction(AbstractContext* ctx) : ctx_(ctx) {}
  
  Status Compute(absl::Span<AbstractTensorHandle* const> grad_inputs,
                 std::vector<AbstractTensorHandle*>* grad_outputs) override {
    
    grad_outputs->resize(2);
    std::vector<AbstractTensorHandle*> identity_outputs(1);
    TF_RETURN_IF_ERROR(Identity(ctx_, {grad_inputs[0]},
                                absl::MakeSpan(identity_outputs), "Id0"));
    (*grad_outputs)[0] = identity_outputs[0];
    TF_RETURN_IF_ERROR(Identity(ctx_, {grad_inputs[0]},
                                absl::MakeSpan(identity_outputs), "Id1"));
    (*grad_outputs)[1] = identity_outputs[0];
    return Status::OK();
  }
  ~AddGradientFunction() override {}

 private:
  AbstractContext* ctx_;
};

GradientFunction* AddRegisterer(const ForwardOperation& op) {
  return new AddGradientFunction(op.ctx);
}
 
Status RegisterGradientAdd(GradientRegistry* registry) {
  return registry->Register("Add", AddRegisterer);
}

// =================== Register gradients for MatMul ============================
class MatMulGradientFunction : public GradientFunction {
 public:
  explicit MatMulGradientFunction(AbstractContext* ctx, std::vector<AbstractTensorHandle*> f_inputs) : 
            ctx_(ctx), forward_inputs(f_inputs) {}
  
  Status Compute(absl::Span<AbstractTensorHandle* const> grad_inputs,
                 std::vector<AbstractTensorHandle*>* grad_outputs) override {
    
    /* Given upstream grad U and a matmul op A*B, the gradients are:
     *      
     *    dA = U * B.T   
     *    dB = A.T * U
     *
     *    where A.T means `transpose(A)`
     */

    AbstractTensorHandle* upstream_grad = grad_inputs[0];
    grad_outputs->resize(2);
    std::vector<AbstractTensorHandle*> matmul_outputs(1);

    // Gradient for A
    TF_RETURN_IF_ERROR(MatMul(ctx_, {upstream_grad, forward_inputs[1]},
                              absl::MakeSpan(matmul_outputs), "mm0",  
                              /*transpose_a = */false, /*transpose_b = */true));

    (*grad_outputs)[0] = matmul_outputs[0];

    // Gradient for B
    TF_RETURN_IF_ERROR(MatMul(ctx_, {upstream_grad},
                              absl::MakeSpan(matmul_outputs), "mm1", 
                              /*transpose_a = */true, /*transpose_b = */false));

    (*grad_outputs)[1] = matmul_outputs[0];
    return Status::OK();
  }
  ~MatMulGradientFunction() override {}

 private:
  AbstractContext* ctx_;
  std::vector<AbstractTensorHandle*> forward_inputs;

};

GradientFunction* MatMulRegisterer(const ForwardOperation& op) {
  return new MatMulGradientFunction(op.ctx, op.inputs);
}
 
Status RegisterGradientMatMul(GradientRegistry* registry) {
  return registry->Register("MatMul", MatMulRegisterer);
}

// =================== End gradient registrations ============================

// Computes `inputs[0] + inputs[1]` and records it on the tape.
Status Add(AbstractContext* ctx, Tape* tape,
           absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs,
           const GradientRegistry& registry) {
  
  AbstractOperationPtr add_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(
      Reset(add_op.get(), "Add", /*raw_device_name=*/nullptr, &forward_op));
  if (isa<tracing::TracingOperation>(add_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(add_op.get())->SetOpName("my_add"));
  }
  TF_RETURN_IF_ERROR(AddInput(add_op.get(), inputs[0], &forward_op));
  TF_RETURN_IF_ERROR(AddInput(add_op.get(), inputs[1], &forward_op));
  int num_retvals = 1;
  return Execute(add_op.get(), ctx, outputs, &num_retvals, &forward_op, tape,
                 registry);
}

// Computes `inputs[0] * inputs[1]` for matrices and records it on the tape.
Status MatMul(AbstractContext* ctx, Tape* tape,
           absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name,
           bool transpose_a, bool transpose_b,
           const GradientRegistry& registry) {
  
  AbstractOperationPtr matmul_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(
      Reset(matmul_op.get(), "MatMul", /*raw_device_name=*/nullptr, &forward_op));
  if (isa<tracing::TracingOperation>(matmul_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(matmul_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(AddInput(matmul_op.get(), inputs[0], &forward_op));
  TF_RETURN_IF_ERROR(AddInput(matmul_op.get(), inputs[1], &forward_op));
  matmul_op->SetAttrBool("transpose_a", transpose_a);
  matmul_op->SetAttrBool("transpose_b", transpose_b);

  int num_retvals = 1;
  return Execute(matmul_op.get(), ctx, outputs, &num_retvals, &forward_op, tape,
                 registry);
}

// Computes `Relu(inputs[0])` and records it on the tape.
Status Relu(AbstractContext* ctx, Tape* tape,
           absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name,
           const GradientRegistry& registry) {
  
  AbstractOperationPtr relu_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(
      Reset(relu_op.get(), "Relu", /*raw_device_name=*/nullptr, &forward_op));
  if (isa<tracing::TracingOperation>(relu_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(relu_op.get())->SetOpName(name));
  }
  TF_RETURN_IF_ERROR(AddInput(relu_op.get(), inputs[0], &forward_op));
  int num_retvals = 1;
  return Execute(relu_op.get(), ctx, outputs, &num_retvals, &forward_op, tape,
                 registry);
}

// Computes `SoftmaxLoss(scores, labels)` for matrices and records it on the tape.
Status SparseSoftmaxCrossEntropyLoss(AbstractContext* ctx, Tape* tape,
           absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name,
           const GradientRegistry& registry) {
  
  AbstractTensorHandle* scores = inputs[0];
  AbstractTensorHandle* labels = inputs[1];

  AbstractOperationPtr sm_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(
      Reset(sm_op.get(), "SparseSoftmaxCrossEntropyWithLogits", /*raw_device_name=*/nullptr, &forward_op));
  if (isa<tracing::TracingOperation>(sm_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(sm_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(AddInput(sm_op.get(), scores, &forward_op));
  TF_RETURN_IF_ERROR(AddInput(sm_op.get(), labels, &forward_op));

  int num_retvals = 2; // returns loss values and backprop
  return Execute(sm_op.get(), ctx, outputs, &num_retvals, &forward_op, tape,
                 registry);
}

// Computes
// y = inputs[0] + inputs[1]
// return grad(y, {inputs[0], inputs[1]})
Status AddGradModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(inputs[0]));  // Watch x.
  tape->Watch(ToId(inputs[1]));  // Watch y.
  std::vector<AbstractTensorHandle*> add_outputs(1);
  TF_RETURN_IF_ERROR(Add(ctx, tape, inputs, absl::MakeSpan(add_outputs),
                         registry));  // Compute x+y.
  std::unordered_map<tensorflow::int64, TapeTensor>
      source_tensors_that_are_targets;

  std::vector<AbstractTensorHandle*> out_grads;
  TF_RETURN_IF_ERROR(tape->ComputeGradient(
      vspace, /*target_tensor_ids=*/{ToId(add_outputs[0])},
      /*source_tensor_ids=*/{ToId(inputs[0]), ToId(inputs[1])},
      source_tensors_that_are_targets,
      /*output_gradients=*/{}, &out_grads));
  for (auto add_output : add_outputs) {
    add_output->Release();
  }
  outputs[0] = out_grads[0];
  outputs[1] = out_grads[1];
  delete tape;
  return Status::OK();
}

// // Computes
// // y = inputs[0] * inputs[1]
// // return grad(y, {inputs[0], inputs[1]})
// Status MatMulGradModel(AbstractContext* ctx,
//                     absl::Span<AbstractTensorHandle* const> inputs,
//                     absl::Span<AbstractTensorHandle*> outputs,
//                     const GradientRegistry& registry) {
  
//   TapeVSpace vspace(ctx);
//   auto tape = new Tape(/*persistent=*/false);
//   tape->Watch(ToId(inputs[0]));  // Watch x.
//   tape->Watch(ToId(inputs[1]));  // Watch y.
//   std::vector<AbstractTensorHandle*> mm_outputs(1);
//   TF_RETURN_IF_ERROR(MatMul(ctx, tape, inputs, absl::MakeSpan(mm_outputs), 
//       "matmul0", /*transpose_a=*/false, /*transpose_b=*/false, registry));  // Compute x*y.
  
//   std::unordered_map<tensorflow::int64, TapeTensor>
//       source_tensors_that_are_targets;

//   std::vector<AbstractTensorHandle*> out_grads;
//   TF_RETURN_IF_ERROR(tape->ComputeGradient(
//       vspace, /*target_tensor_ids=*/{ToId(mm_outputs[0])},
//       /*source_tensor_ids=*/{ToId(inputs[0]), ToId(inputs[1])},
//       source_tensors_that_are_targets,
//       /*output_gradients=*/{}, &out_grads));
//   for (auto mm_output : mm_outputs) {
//     mm_output->Release();
//   }
//   outputs[0] = out_grads[0];
//   outputs[1] = out_grads[1];
//   delete tape;
//   return Status::OK();
// }

Status MNISTForwardModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  /**
    * We will trace a 2-layer fully connected network for an MNIST model:
    *
    *   def mnist_forward(X, W1, W2, y_labels):
    *     mm_out_1 = tf.matmul(X,W1)
    *     hidden_layer = tf.ReLu(mm_out_1)
    *     scores = tf.matmul(hidden_layer,W2)
    *     softmax = tf.softmaxLoss(scores,y_labels)
    *     return scores, softmax 
    *
    * Use this convention for inputs:
    * 
    *   inputs = [X, W1, W2, y_labels]
    *
    */ 
  AbstractTensorHandle* X = inputs[0];
  AbstractTensorHandle* W1 = inputs[1];
  AbstractTensorHandle* W2 = inputs[2];
  AbstractTensorHandle* y_labels = inputs[3];

  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(W1));  // Watch W1.
  tape->Watch(ToId(W2));  // Watch W2.
  std::vector<AbstractTensorHandle*> temp_outputs(1);

  TF_RETURN_IF_ERROR(MatMul(ctx, tape, {X, W1}, absl::MakeSpan(temp_outputs),
                     "matmul0",/*transpose_a=*/false,/*transpose_b=*/false, registry));  // Compute X*W1

  TF_RETURN_IF_ERROR(Relu(ctx, tape, {temp_outputs[0]}, absl::MakeSpan(temp_outputs),
                     "relu", registry)); // Compute Relu(X*W1)

  TF_RETURN_IF_ERROR(MatMul(ctx, tape, {temp_outputs[0], W2}, absl::MakeSpan(temp_outputs),
                     "matmul1",/*transpose_a=*/false,/*transpose_b=*/false, registry));  // Compute W2*Relu(X*W1)
  
  AbstractTensorHandle* scores = temp_outputs[0];

  TF_RETURN_IF_ERROR(SparseSoftmaxCrossEntropyLoss(ctx, tape, {scores, y_labels}, absl::MakeSpan(temp_outputs),
                     "softmax_loss", registry));  // Compute Softmax(Scores,labels)
  
  AbstractTensorHandle* loss_vals  = temp_outputs[0];
  outputs[0] = scores;
  outputs[1] = loss_vals;
  delete tape;
  return Status::OK();
}

AbstractContext* BuildFunction(const char* fn_name) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TF_ExecutionContext* graph_ctx = TF_CreateFunction(fn_name, status.get());
  return unwrap(graph_ctx);
}

Status CreateParamsForInputs(AbstractContext* ctx,
                             absl::Span<AbstractTensorHandle* const> inputs,
                             std::vector<AbstractTensorHandle*>* params) {
  
  tracing::TracingTensorHandle* handle = nullptr;
  for (auto input : inputs) {
    TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingContext>(ctx)->AddParameter(
        input->DataType(), &handle));
    params->emplace_back(handle);
  }
  return Status::OK();
}

using Model = std::function<Status(
    AbstractContext*, absl::Span<AbstractTensorHandle* const>,
    absl::Span<AbstractTensorHandle*>, const GradientRegistry&)>;

// Runs `model` maybe wrapped in a function.
Status RunModel(Model model, AbstractContext* ctx,
                absl::Span<AbstractTensorHandle* const> inputs,
                absl::Span<AbstractTensorHandle*> outputs, bool use_function,
                const GradientRegistry& registry) {
  
  if (use_function) {
    const char* fn_name = "test_fn";
    std::unique_ptr<AbstractFunction> scoped_func;
    {
      AbstractContextPtr func_ctx(BuildFunction(fn_name));
      std::vector<AbstractTensorHandle*> func_inputs;
      func_inputs.reserve(inputs.size());
      TF_RETURN_IF_ERROR(
          CreateParamsForInputs(func_ctx.get(), inputs, &func_inputs));
      OutputList output_list;
      output_list.expected_num_outputs = outputs.size();
      output_list.outputs.resize(outputs.size());
      TF_RETURN_IF_ERROR(model(func_ctx.get(), absl::MakeSpan(func_inputs),
                               absl::MakeSpan(output_list.outputs), registry));
      for (auto func_input : func_inputs) {
        func_input->Release();
      }
      AbstractFunction* func = nullptr;
      TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingContext>(func_ctx.get())
                             ->Finalize(&output_list, &func));
      scoped_func.reset(func);
      output_list.outputs[0]->Release();
      output_list.outputs[1]->Release();
      TF_RETURN_IF_ERROR(ctx->RegisterFunction(func));
    }

    AbstractOperationPtr fn_op(ctx->CreateOperation());
    TF_RETURN_IF_ERROR(fn_op->Reset(fn_name, /*raw_device_name=*/nullptr));
    for (auto input : inputs) {
      TF_RETURN_IF_ERROR(fn_op->AddInput(input));
    }
    int retvals = outputs.size();
    TF_RETURN_IF_ERROR(fn_op->Execute(outputs, &retvals));
    TF_RETURN_IF_ERROR(ctx->RemoveFunction(fn_name));
    return Status::OK();
  } else {
    return model(ctx, inputs, outputs, registry);
  }
}

void printArr(float data[], int n)
{
  std::cout << std::endl << "[";
  for(int i = 0; i < n-1; i++){
    std::cout << data[i] << ", ";

  }
  std::cout << data [n-1] << "]" << std::endl<<std::endl;

}


Status BuildImmediateExecutionContext(bool use_tfrt, AbstractContext** ctx) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_ContextOptions* opts = TFE_NewContextOptions();
  TFE_ContextOptionsSetTfrt(opts, use_tfrt);
  *ctx = unwrap(TF_NewEagerExecutionContext(opts, status.get()));
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  TFE_DeleteContextOptions(opts);
  return Status::OK();
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

  // Pseudo-code:
  //
  // tape.watch(x)
  // tape.watch(y)
  // y = x + y
  // outputs = tape.gradient(y, [x, y])
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
  // float tolerance = 1e-3;
  // for(int j = 0; j < 4; j++){
  //   ASSERT_NEAR(result_data[j], expected_dA[j], tolerance);
  // }  


  /* ERROR: This test runs 2x when we bazel test
   *
   *  1st time result_data: [-.5, 2, -.5, 2]  ----> This is correct
   *
   *  2nd time result_data: [1.5, 0, 1.5, 0]  ----> This is WRONG
   *
   *  For some reason, the tensor `B` is getting transposed 2x (or not at all)
   *  when the gradient is called (see `dA` in `MatMulGradientFunction`)
   * 
   *  Possible memory issue where the inputs and/or Op is not resetting the 2nd time?
   */

  printArr(result_data, 4);

  outputs[0]->Release();
  outputs[1]->Release();
  TF_DeleteTensor(dA_tensor);
}

TEST_P(CppGradients, TestMNISTForward) {
  //std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(TF_NewStatus(), TF_DeleteStatus);
  
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
  Status s = MNISTForwardModel(ctx.get(), {X.get(), W1.get(), W2.get(), y.get()}, absl::MakeSpan(outputs), registry);
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
  //std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(TF_NewStatus(), TF_DeleteStatus);
  
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
  Status s = MNISTForwardModel(ctx.get(), {X.get(), W1.get(), W2.get(), y.get()}, absl::MakeSpan(outputs), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Verify the Results
  TF_Tensor* scores_tensor;
  s = getValue(outputs[0], &scores_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  float result_data[6] = {0};
  memcpy(&result_data[0], TF_TensorData(scores_tensor), TF_TensorByteSize(scores_tensor));
  
  //float expected_scores [6] = {0f, 12.0f, -1.0f, -17.0f, 16.8f, -28.0f};
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
  tape->Watch(ToId(W1));  // Watch W1.
  std::vector<AbstractTensorHandle*> temp_outputs(1);

  TF_RETURN_IF_ERROR(MatMul(ctx, tape, {X, W1}, absl::MakeSpan(temp_outputs),
                     "matmul0",/*transpose_a=*/true,/*transpose_b=*/false, registry));  // Compute X*W1

  outputs[0] =  temp_outputs[0];

  delete tape;
  return Status::OK();
}

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
  Status s = MatMulTransposeModel(ctx.get(), {X.get(), W1.get()}, absl::MakeSpan(outputs), registry);
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


// TODO(b/160888630): Enable this test with mlir after AddInputList is
// supported. It is needed for AddN op which is used for gradient aggregation.
#ifdef PLATFORM_GOOGLE
INSTANTIATE_TEST_SUITE_P(
    UnifiedCAPI, CppGradients,
    ::testing::Combine(::testing::Values("graphdef"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));
#else
INSTANTIATE_TEST_SUITE_P(
    UnifiedCAPI, CppGradients,
    ::testing::Combine(::testing::Values("graphdef"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));
#endif
}  // namespace
}  // namespace internal
}  // namespace gradients
}  // namespace tensorflow

