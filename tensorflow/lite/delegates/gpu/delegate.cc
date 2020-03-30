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

#include "tensorflow/lite/delegates/gpu/delegate.h"

#include <cstdint>
#include <memory>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "absl/memory/memory.h"
#include "absl/types/span.h"
#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/delegates/gpu/api.h"
#include "tensorflow/lite/delegates/gpu/cl/api.h"
#include "tensorflow/lite/delegates/gpu/cl/opencl_wrapper.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type_util.h"
#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/model_builder.h"
#include "tensorflow/lite/delegates/gpu/common/model_transformer.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/gl/api2.h"
#include "tensorflow/lite/minimal_logging.h"

namespace tflite {
namespace gpu {
namespace {

InferencePriority ToPriority(int32_t priority) {
  switch (priority) {
    case TFLITE_GPU_INFERENCE_PRIORITY_AUTO:
      return InferencePriority::AUTO;
    case TFLITE_GPU_INFERENCE_PRIORITY_MAX_PRECISION:
      return InferencePriority::MAX_PRECISION;
    case TFLITE_GPU_INFERENCE_PRIORITY_MIN_LATENCY:
      return InferencePriority::MIN_LATENCY;
    case TFLITE_GPU_INFERENCE_PRIORITY_MIN_MEMORY_USAGE:
      return InferencePriority::MIN_MEMORY_USAGE;
  }
  return InferencePriority::UNKNOWN;
}

InferenceUsage ToUsage(int32_t usage) {
  switch (usage) {
    case TFLITE_GPU_INFERENCE_PREFERENCE_FAST_SINGLE_ANSWER:
      return InferenceUsage::FAST_SINGLE_ANSWER;
    case TFLITE_GPU_INFERENCE_PREFERENCE_SUSTAINED_SPEED:
      return InferenceUsage::SUSTAINED_SPEED;
  }
  return InferenceUsage::UNKNOWN;
}

// Forward declarations.
TfLiteStatus DelegatePrepare(TfLiteContext* context, TfLiteDelegate* delegate);

class Delegate {
 public:
  explicit Delegate(const TfLiteGpuDelegateOptionsV2* options) {
    options_ = options ? *options : TfLiteGpuDelegateOptionsV2Default();
  }

  TfLiteDelegate* tflite_delegate() { return &delegate_; }
  const TfLiteGpuDelegateOptionsV2& options() const { return options_; }

 private:
  TfLiteDelegate delegate_ = {
      .data_ = reinterpret_cast<void*>(this),
      .Prepare = DelegatePrepare,
      .CopyFromBufferHandle = nullptr,
      .CopyToBufferHandle = nullptr,
      .FreeBufferHandle = nullptr,
      .flags = kTfLiteDelegateFlagsNone,
  };

  TfLiteGpuDelegateOptionsV2 options_;
};

// Represent the execution of a subset of nodes on GPU.
class DelegateKernel {
 public:
  explicit DelegateKernel(const TfLiteGpuDelegateOptionsV2& options)
      : options_(options) {}

  absl::Status Prepare(TfLiteContext* context,
                       const TfLiteDelegateParams* delegate_params) {
    thread_id_prepare_ = std::this_thread::get_id();

    // Extract TFLite delegate execution plan from the context and convert it
    // into FlowGraph32.
    GraphFloat32 graph;
    RETURN_IF_ERROR(BuildFinalModel(context, delegate_params, &graph));

    std::vector<uint32_t> input_refs;
    {
      const auto& inputs = graph.inputs();
      input_refs.reserve(inputs.size());
      for (auto input : inputs) {
        input_refs.push_back(input->tensor.ref);
      }
    }
    std::vector<uint32_t> output_refs;
    {
      const auto& outputs = graph.outputs();
      output_refs.reserve(outputs.size());
      for (auto output : outputs) {
        output_refs.push_back(output->tensor.ref);
      }
    }

    std::unique_ptr<InferenceBuilder> builder;
    bool graph_is_destroyed;
    absl::Status status =
        InitializeOpenClApi(&graph, &builder, &graph_is_destroyed);
    if (!status.ok()) {
      TF_LITE_KERNEL_LOG(context, std::string(status.message()).c_str());
      context->ReportError(context, "Falling back to OpenGL");

      // Graph need to be re-created because it is moved above.
      GraphFloat32 graph2;
      if (graph_is_destroyed) {
        RETURN_IF_ERROR(BuildFinalModel(context, delegate_params, &graph2));
      }
      RETURN_IF_ERROR(
          InitializeOpenGlApi(graph_is_destroyed ? &graph2 : &graph, &builder));
    }

    // At this point tflite didn't allocate tensors yet, therefore, collect
    // indices and set all input and output tensors from tflite later.
    input_indices_.reserve(input_refs.size());
    for (uint32_t tensor_index : input_refs) {
      const int64_t object_index = input_indices_.size();
      input_indices_.push_back(tensor_index);
      RETURN_IF_ERROR(
          builder->SetInputObjectDef(object_index, GetObjectDef(tensor_index)));
    }
    output_indices_.reserve(output_refs.size());
    for (uint32_t tensor_index : output_refs) {
      const int64_t object_index = output_indices_.size();
      output_indices_.push_back(tensor_index);
      RETURN_IF_ERROR(builder->SetOutputObjectDef(object_index,
                                                  GetObjectDef(tensor_index)));
    }

    return builder->Build(&runner_);
  }

  absl::Status Invoke(TfLiteContext* context) {
    if (thread_id_prepare_ != std::this_thread::get_id()) {
      TFLITE_LOG(tflite::TFLITE_LOG_WARNING,
                 "GpuDelegate invoke thread != prepare thread");
      if (enforce_same_thread_) {
        return absl::FailedPreconditionError(
            "GpuDelegate must run on the same thread where it was "
            "initialized.");
      }
    }

    RETURN_IF_ERROR(SetInputsAndOutputs(context));
    return runner_->Run();
  }

 private:
  absl::Status SetInputsAndOutputs(TfLiteContext* context) {
    for (int i = 0; i < input_indices_.size(); ++i) {
      RETURN_IF_ERROR(runner_->SetInputObject(
          i, GetTensorObject(input_indices_[i], context)));
    }
    for (int i = 0; i < output_indices_.size(); ++i) {
      RETURN_IF_ERROR(runner_->SetOutputObject(
          i, GetTensorObject(output_indices_[i], context)));
    }
    return absl::OkStatus();
  }

  ObjectDef GetObjectDef(int index) const {
    ObjectDef default_object_def;
    default_object_def.data_type = DataType::FLOAT32;
    default_object_def.data_layout = DataLayout::BHWC;
    default_object_def.object_type = ObjectType::CPU_MEMORY;
    default_object_def.user_provided = true;
    return default_object_def;
  }

  TensorObject GetTensorObject(int index, TfLiteContext* context) const {
    auto& tensor = context->tensors[index];
    return MakeCpuMemory(absl::MakeSpan(tensor.data.raw, tensor.bytes));
  }

  absl::Status InitializeOpenClApi(GraphFloat32* graph,
                                   std::unique_ptr<InferenceBuilder>* builder,
                                   bool* graph_is_destroyed) {
    *graph_is_destroyed = false;
    cl::InferenceEnvironmentOptions env_options;
    cl::InferenceEnvironmentProperties properties;
    RETURN_IF_ERROR(cl::NewInferenceEnvironment(env_options, &cl_environment_,
                                                &properties));
    cl::InferenceOptions options;
    // If is_precision_loss_allowed == -1, then just use priorities instead
    // of paying attention to is_precision_loss_allowed value.
    if (options_.is_precision_loss_allowed == -1) {
      options.priority1 = ToPriority(options_.inference_priority1);
      options.priority2 = ToPriority(options_.inference_priority2);
      options.priority3 = ToPriority(options_.inference_priority3);
    } else {
      // Users set is_precision_loss_allowed explicitly, thus use it explicitly.
      if (options_.is_precision_loss_allowed == 0) {
        options.priority1 = InferencePriority::MAX_PRECISION;
      } else {
        options.priority1 = InferencePriority::MIN_LATENCY;
      }
    }
    options.usage = ToUsage(options_.inference_preference);
    *graph_is_destroyed = true;
    RETURN_IF_ERROR(cl_environment_->NewInferenceBuilder(
        options, std::move(*graph), builder));
    TFLITE_LOG_PROD_ONCE(tflite::TFLITE_LOG_INFO,
                         "Initialized OpenCL-based API.");
    return absl::OkStatus();
  }

  absl::Status InitializeOpenGlApi(GraphFloat32* graph,
                                   std::unique_ptr<InferenceBuilder>* builder) {
    gl::InferenceEnvironmentOptions env_options;
    gl::InferenceEnvironmentProperties properties;
    RETURN_IF_ERROR(
        NewInferenceEnvironment(env_options, &gl_environment_, &properties));
    gl::InferenceOptions options;
    options.usage = ToUsage(options_.inference_preference);
    options.priority1 = ToPriority(options_.inference_priority1);
    options.priority2 = ToPriority(options_.inference_priority2);
    options.priority3 = ToPriority(options_.inference_priority3);
    RETURN_IF_ERROR(gl_environment_->NewInferenceBuilder(std::move(*graph),
                                                         options, builder));
    enforce_same_thread_ = true;
    TFLITE_LOG_PROD_ONCE(tflite::TFLITE_LOG_INFO,
                         "Initialized OpenGL-based API.");
    return absl::OkStatus();
  }

  // Shared across all DelegateKernel instances, passed by the Delegate
  // instance.
  const TfLiteGpuDelegateOptionsV2& options_;
  std::unique_ptr<cl::InferenceEnvironment> cl_environment_;
  std::unique_ptr<gl::InferenceEnvironment> gl_environment_;
  std::unique_ptr<InferenceRunner> runner_;
  std::vector<int64_t> input_indices_;
  std::vector<int64_t> output_indices_;
  std::thread::id thread_id_prepare_;  // thread id used for Prapare()
  bool enforce_same_thread_ = false;   // flag to enforce same thread for Invoke
};

inline DelegateKernel* GetDelegateKernel(TfLiteNode* node) {
  return reinterpret_cast<DelegateKernel*>(node->user_data);
}

inline Delegate* GetDelegate(TfLiteDelegate* delegate) {
  return reinterpret_cast<Delegate*>(delegate->data_);
}

TfLiteStatus DelegatePrepare(TfLiteContext* context, TfLiteDelegate* delegate) {
  const TfLiteRegistration kRegistration = {
      // .init
      [](TfLiteContext* context, const char* buffer, size_t) -> void* {
        const auto* params =
            reinterpret_cast<const TfLiteDelegateParams*>(buffer);
        auto* gpu_delegate = GetDelegate(params->delegate);
        // Everything below should happen in prepare function call, but TFLite
        // for whatever reason forbids that.
        auto gpu_delegate_kernel =
            absl::make_unique<DelegateKernel>(gpu_delegate->options());
        const auto status = gpu_delegate_kernel->Prepare(context, params);
        if (!status.ok()) {
          context->ReportError(context, "TfLiteGpuDelegate Init: %s",
                               std::string(status.message()).c_str());
          return nullptr;
        }
        return gpu_delegate_kernel.release();
      },
      // .free
      [](TfLiteContext*, void* buffer) -> void {
        delete reinterpret_cast<DelegateKernel*>(buffer);
      },
      // .prepare
      [](TfLiteContext* context, TfLiteNode* node) -> TfLiteStatus {
        if (!node->user_data) {
          context->ReportError(
              context,
              "TfLiteGpuDelegate Prepare: delegate is not initialized");
          return kTfLiteError;
        }
        // TODO(akulik): tflite tensors are not allocated here either. It would
        // be good to set inputs and outputs only once here instead of setting
        // them every time in .invoke.
        return kTfLiteOk;
      },
      // .invoke
      [](TfLiteContext* context, TfLiteNode* node) -> TfLiteStatus {
        const auto status = GetDelegateKernel(node)->Invoke(context);
        if (!status.ok()) {
          context->ReportError(context, "TfLiteGpuDelegate Invoke: %s",
                               std::string(status.message()).c_str());
          return kTfLiteError;
        }
        return kTfLiteOk;
      },
      nullptr,                // .profiling_string
      0,                      // .builtin_code
      "TfLiteGpuDelegateV2",  // .custom_name
      1,                      // .version
  };
  TfLiteIntArray* ops_to_replace = GetOpsToReplace(context);
  const auto status = context->ReplaceNodeSubsetsWithDelegateKernels(
      context, kRegistration, ops_to_replace, delegate);
  TfLiteIntArrayFree(ops_to_replace);
  return status;
}

}  // namespace
}  // namespace gpu
}  // namespace tflite

TfLiteGpuDelegateOptionsV2 TfLiteGpuDelegateOptionsV2Default() {
  TfLiteGpuDelegateOptionsV2 options;
  // set it to -1 to detect whether it was later adjusted.
  options.is_precision_loss_allowed = -1;
  options.inference_preference =
      TFLITE_GPU_INFERENCE_PREFERENCE_FAST_SINGLE_ANSWER;
  options.inference_priority1 = TFLITE_GPU_INFERENCE_PRIORITY_MAX_PRECISION;
  options.inference_priority2 = TFLITE_GPU_INFERENCE_PRIORITY_AUTO;
  options.inference_priority3 = TFLITE_GPU_INFERENCE_PRIORITY_AUTO;
  return options;
}

TfLiteDelegate* TfLiteGpuDelegateV2Create(
    const TfLiteGpuDelegateOptionsV2* options) {
  auto* gpu_delegate = new tflite::gpu::Delegate(options);
  TFLITE_LOG_PROD_ONCE(tflite::TFLITE_LOG_INFO,
                       "Created TensorFlow Lite delegate for GPU.");
  return gpu_delegate ? gpu_delegate->tflite_delegate() : nullptr;
}

void TfLiteGpuDelegateV2Delete(TfLiteDelegate* delegate) {
  delete tflite::gpu::GetDelegate(delegate);
}
