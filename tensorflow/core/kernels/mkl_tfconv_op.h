/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CORE_KERNELS_MKL_TFCONV_OP_H_
#define TENSORFLOW_CORE_KERNELS_MKL_TFCONV_OP_H_

#ifdef INTEL_MKL

#include <algorithm>
#include <vector>
#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/platform/byte_order.h"
#include "tensorflow/core/platform/cpu_info.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/tensor_format.h"

#include "tensorflow/core/util/mkl_util.h"

using mkldnn::stream;

namespace tensorflow {
#ifdef ENABLE_MKLDNN_V1
#define ENGINE_CPU engine::kind::cpu
#define OUTPUT_TF_MD output_tf_md
#else
#define ENGINE_CPU engine::cpu
#define OUTPUT_TF_MD output_tf_pd
#endif  // ENABLE_MKLDNN_V1
typedef Eigen::ThreadPoolDevice CPUDevice;

///////////////////////////////////////////////////////////
//               Op kernel
///////////////////////////////////////////////////////////

template <typename Device, typename T>
class MklToTfOp : public OpKernel {
 public:
  explicit MklToTfOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &data_format_str));
    OP_REQUIRES_OK(context, context->GetAttr("T", &op_data_type));
    has_avx512f_ = port::TestCPUFeature(port::CPUFeature::AVX512F);
  }

  void Compute(OpKernelContext* context) override {
    ConvertMklToTf(this, context, data_format_str, op_data_type, has_avx512f_,
                   0);
    VLOG(1) << "MKLToTFConversion complete successfully.";
  }

  static void ConvertMklToTf(OpKernel* op_kernel, OpKernelContext* context,
                             string data_format_str, DataType op_data_type,
                             bool has_avx512f, uint input_number) {
    try {
      // Check that input tensor is in MKL format.
      const Tensor& input_tensor = MklGetInput(context, input_number);
      MklDnnShape input_shape;
      GetMklShape(context, input_number, &input_shape);

      // if input is already in Tf format, then copy input tensor to output.
      if (!input_shape.IsMklTensor()) {
        context->set_output(input_number, input_tensor);
        VLOG(1) << "MKLToTFConversion: No conversion needed, "
                << "copying input to output";
        return;
      }

      // Check that input data type is same as operator data type and that it
      // is same as output data type.
      DataType input_data_type = op_kernel->input_type(input_number);
      DataType output_data_type = op_kernel->output_type(input_number);
      CHECK_EQ(op_data_type, input_data_type);
      CHECK_EQ(op_data_type, output_data_type);

      auto cpu_engine = engine(ENGINE_CPU, 0);
      MklDnnData<T> input(&cpu_engine);

      // Get MKL layout of input tensor.
      auto input_mkl_md = input_shape.GetMklLayout();
      // Get TensorFlow layout of input tensor. Expected output of conversion
      // has same layout as Tensorflow layout of input tensor.
      auto output_tf_md = input_shape.GetTfLayout();
#ifndef ENABLE_MKLDNN_V1
      auto output_tf_pd = memory::primitive_desc(output_tf_md, cpu_engine);
#endif  // !ENABLE_MKLDNN_V1
      // Set input MKL layout as the user layout.
      input.SetUsrMem(input_mkl_md, &input_tensor);

      // Allocate output tensor.
      TensorShape output_shape = input_shape.GetTfShape();
      Tensor* output_tensor = NULL;
      OP_REQUIRES_OK(context, context->allocate_output(
                                  input_number, output_shape, &output_tensor));
      CHECK_NOTNULL(output_tensor);

      // Do we need to reorder MKL layout into TensorFlow layout?
      if (input.IsReorderNeeded(OUTPUT_TF_MD)) {
#ifdef ENABLE_MKLDNN_V1
        std::vector<primitive> net;
        std::vector<MemoryArgsMap> net_args;
        // Insert reorder between MKL layout and TensorFlow layout.
        // TODO(bhavanis): Refactor CheckReorderToOpMem() to directly insert
        // the reorder primitive
        bool status = input.CheckReorderToOpMem(output_tf_md, output_tensor,
                                                net, net_args, cpu_engine);
        OP_REQUIRES(
            context, status,
            errors::Internal("MklToTfOp: Failed to create input reorder"));

        ExecutePrimitive(net, &net_args, cpu_engine);
#else
        // Insert reorder between MKL layout and TensorFlow layout.
        bool status = input.CheckReorderToOpMem(output_tf_pd, output_tensor);
        OP_REQUIRES(
            context, status,
            errors::Internal("MklToTfOp: Failed to create input reorder"));
#endif  // ENABLE_MKLDNN_V1
      } else {
        // If not, just forward input tensor to output tensor.
        bool status = output_tensor->CopyFrom(input_tensor, output_shape);
        OP_REQUIRES(context, status,
                    errors::Internal(
                        "MklToTfOp: Failed to forward input tensor to output"));
      }
    } catch (mkldnn::error& e) {
      OP_REQUIRES_OK(
          context,
          errors::Aborted("Operation received an exception: Status: ", e.status,
                          ", message: ", StringPiece(e.message), ", in file ",
                          __FILE__, ":", __LINE__));
    }
  }

 private:
  /// Data format of the operation
  string data_format_str;

  /// Data type of the operation
  DataType op_data_type;

  /// CPUIDInfo
  bool has_avx512f_ = false;
};

///////////////////////////////////////////////////////////
//               Register kernel
///////////////////////////////////////////////////////////

#define REGISTER_CPU(T)                                        \
  REGISTER_KERNEL_BUILDER(                                     \
      Name("_MklToTf")                                         \
          .Device(DEVICE_CPU)                                  \
          .TypeConstraint<T>("T")                              \
          .Label(mkl_op_registry::kMklLayoutDependentOpLabel), \
      MklToTfOp<CPUDevice, T>);

TF_CALL_NUMBER_TYPES(REGISTER_CPU);
TF_CALL_QUANTIZED_TYPES(REGISTER_CPU);

#undef REGISTER_CPU
#undef ENGINE_CPU
#undef OUTPUT_TF_MD

}  // namespace tensorflow
#endif  // INTEL_MKL
#endif  // TENSORFLOW_CORE_KERNELS_MKL_TFCONV_OP_H_
