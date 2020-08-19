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

#include <memory>
#include <vector>

#include "absl/strings/str_cat.h"
#include "tensorflow/c/c_api.h"
#include "tensorflow/c/eager/abstract_context.h"
#include "tensorflow/c/eager/c_api_internal.h"
#include "tensorflow/c/eager/c_api_unified_experimental.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/tf_datatype.h"
#include "tensorflow/c/tf_status.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/llvm_rtti/llvm_rtti.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/strcat.h"
#include "tensorflow/core/platform/types.h"

using tensorflow::dyn_cast;
using tensorflow::string;
using tensorflow::gtl::ArraySlice;

namespace tensorflow {
namespace tracing {
namespace graph {

class GraphContext;
class GraphOperation;
class GraphTensor;

// GraphTensor wraps a `TF_Output`, i.e. a pointer to TF_Operation and the index
// into the list of outputs for the operation.
class GraphTensor : public TracingTensorHandle {
 public:
  explicit GraphTensor(TF_Output output)
      : TracingTensorHandle(kGraph), output_(output) {}

  tensorflow::DataType DataType() const override {
    return static_cast<tensorflow::DataType>(TF_OperationOutputType(output_));
  }
  TF_Output output_;

  // For LLVM style RTTI.
  static bool classof(const AbstractTensorHandle* ptr) {
    return ptr->getKind() == kGraph;
  }
};

// GraphOperation wraps and populates a TF_OperationDescription.
class GraphOperation : public TracingOperation {
 public:
  explicit GraphOperation(TF_Graph* g) : TracingOperation(kGraph), g_(g) {}
  void Release() override { delete this; }
  Status Reset(const char* op, const char* raw_device_name) override {
    if (op_) {
      return errors::FailedPrecondition("Reset called on already built op.");
    }
    if (raw_device_name) {
      device_name_ = raw_device_name;
    }
    op_type_ = op;
    return Status::OK();
  }
  Status SetOpName(const char* const op_name) override {
    if (op_) {
      return errors::FailedPrecondition(
          "SetOpName called on already built op.");
    }
    if (op_type_.empty()) {
      return errors::FailedPrecondition(
          "GraphOperation::Reset must be called before calling SetOpName.");
    }
    // TODO(b/145674566): We use Graph::NewName to get a unique name here but
    // this may not be consistent with python's naming policy.
    mutex_lock l(g_->mu);
    op_.reset(new TF_OperationDescription(g_, op_type_.c_str(),
                                          g_->graph.NewName(op_name).c_str()));
    return Status::OK();
  }
  const string& Name() const override { return op_type_; }
  const string& DeviceName() const override { return device_name_; }

  Status SetDeviceName(const char* name) override {
    // TODO(srbs): Implement this.
    device_name_ = name;
    return Status::OK();
  }

  Status AddInput(AbstractTensorHandle* input) override {
    GraphTensor* t = dyn_cast<GraphTensor>(input);
    if (!t) {
      return tensorflow::errors::InvalidArgument(
          "Unable to cast input to GraphTensor");
    }
    TF_AddInput(op_.get(), t->output_);
    return Status::OK();
  }
  Status AddInputList(absl::Span<AbstractTensorHandle* const> inputs) override {
    std::vector<TF_Output> tf_outputs(inputs.size());
    for (int i = 0; i < inputs.size(); i++) {
      GraphTensor* t = dyn_cast<GraphTensor>(inputs[i]);
      if (!t) {
        return tensorflow::errors::InvalidArgument(
            "Unable to cast input to GraphTensor");
      }
      tf_outputs[i] = t->output_;
    }
    TF_AddInputList(op_.get(), tf_outputs.data(), tf_outputs.size());
    return Status::OK();
  }
  Status Execute(absl::Span<AbstractTensorHandle*> retvals,
                 int* num_retvals) override {
    auto* tf_opdesc = op_.release();
    if (tf_opdesc == nullptr) {
      return errors::InvalidArgument("AbstractOp is incomplete.");
    }
    TF_Status* s = TF_NewStatus();
    auto* operation = TF_FinishOperation(tf_opdesc, s);
    TF_RETURN_IF_ERROR(StatusFromTF_Status(s));
    TF_DeleteStatus(s);
    *num_retvals = TF_OperationNumOutputs(operation);
    for (int i = 0; i < *num_retvals; ++i) {
      retvals[i] = new GraphTensor({operation, i});
    }
    return Status::OK();
  }

  Status SetAttrString(const char* attr_name, const char* data,
                       size_t length) override {
    tensorflow::StringPiece s(data, length);
    op_->node_builder.Attr(attr_name, s);
    return Status::OK();
  }
  Status SetAttrInt(const char* attr_name, int64_t value) override {
    static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                  "64-bit int types should match in size");
    op_->node_builder.Attr(attr_name, static_cast<tensorflow::int64>(value));
    return Status::OK();
  }
  Status SetAttrFloat(const char* attr_name, float value) override {
    op_->node_builder.Attr(attr_name, value);
    return Status::OK();
  }
  Status SetAttrBool(const char* attr_name, bool value) override {
    op_->node_builder.Attr(attr_name, value);
    return Status::OK();
  }
  Status SetAttrType(const char* const attr_name, DataType value) override {
    if (!op_) {
      return Status(
          error::Code::FAILED_PRECONDITION,
          "op_type and op_name must be specified before specifying attrs.");
    }
    op_->node_builder.Attr(attr_name, value);
    return Status::OK();
  }
  Status SetAttrShape(const char* attr_name, const int64_t* dims,
                      const int num_dims) override {
    PartialTensorShape shape;
    if (num_dims >= 0) {
      static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                    "64-bit int types should match in size");
      shape = PartialTensorShape(ArraySlice<tensorflow::int64>(
          reinterpret_cast<const tensorflow::int64*>(dims), num_dims));
    }
    op_->node_builder.Attr(attr_name, shape);
    return Status::OK();
  }
  Status SetAttrFunction(const char* attr_name,
                         const AbstractOperation* value) override {
    return tensorflow::errors::Unimplemented(
        "SetAttrFunction has not been implemented yet.");
  }
  Status SetAttrFunctionName(const char* attr_name, const char* value,
                             size_t length) override {
    tensorflow::NameAttrList func_name;
    func_name.set_name(string(value, value + length));
    op_->node_builder.Attr(attr_name, func_name);
    return Status::OK();
  }
  Status SetAttrTensor(const char* attr_name,
                       AbstractTensorInterface* tensor) override {
    return tensorflow::errors::Unimplemented(
        "SetAttrTensor has not been implemented yet.");
  }
  Status SetAttrStringList(const char* attr_name, const void* const* values,
                           const size_t* lengths, int num_values) override {
    if (strcmp(attr_name, tensorflow::kColocationAttrName) == 0) {
      op_->colocation_constraints.clear();
      for (int i = 0; i < num_values; ++i) {
        op_->colocation_constraints.emplace(static_cast<const char*>(values[i]),
                                            lengths[i]);
      }
    } else {
      std::vector<tensorflow::StringPiece> v;
      v.reserve(num_values);
      for (int i = 0; i < num_values; ++i) {
        v.emplace_back(static_cast<const char*>(values[i]), lengths[i]);
      }
      op_->node_builder.Attr(attr_name, v);
    }
    return Status::OK();
  }
  Status SetAttrFloatList(const char* attr_name, const float* values,
                          int num_values) override {
    op_->node_builder.Attr(attr_name,
                           ArraySlice<const float>(values, num_values));
    return Status::OK();
  }
  Status SetAttrIntList(const char* attr_name, const int64_t* values,
                        int num_values) override {
    static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                  "64-bit int types should match in size");
    op_->node_builder.Attr(
        attr_name,
        ArraySlice<const tensorflow::int64>(
            reinterpret_cast<const tensorflow::int64*>(values), num_values));
    return Status::OK();
  }
  Status SetAttrTypeList(const char* attr_name, const DataType* values,
                         int num_values) override {
    op_->node_builder.Attr(attr_name,
                           ArraySlice<const DataType>(values, num_values));
    return Status::OK();
  }
  Status SetAttrBoolList(const char* attr_name, const unsigned char* values,
                         int num_values) override {
    std::unique_ptr<bool[]> b(new bool[num_values]);
    for (int i = 0; i < num_values; ++i) {
      b[i] = values[i];
    }
    op_->node_builder.Attr(attr_name,
                           ArraySlice<const bool>(b.get(), num_values));

    return Status::OK();
  }
  Status SetAttrShapeList(const char* attr_name, const int64_t** dims,
                          const int* num_dims, int num_values) override {
    std::vector<PartialTensorShape> shapes;
    shapes.reserve(num_values);
    for (int i = 0; i < num_values; ++i) {
      if (num_dims[i] < 0) {
        shapes.emplace_back();
      } else {
        static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                      "64-bit int types should match in size");
        shapes.emplace_back(ArraySlice<tensorflow::int64>(
            reinterpret_cast<const tensorflow::int64*>(dims[i]), num_dims[i]));
      }
    }
    op_->node_builder.Attr(attr_name, shapes);
    return Status::OK();
  }
  Status SetAttrFunctionList(
      const char* attr_name,
      absl::Span<const AbstractOperation*> values) override {
    return tensorflow::errors::Unimplemented(
        "SetAttrFunctionList has not been implemented yet.");
  }
  // For LLVM style RTTI.
  static bool classof(const AbstractOperation* ptr) {
    return ptr->getKind() == kGraph;
  }
  ~GraphOperation() override {}

 private:
  friend class GraphContext;  // For access to op_.
  TF_Graph* g_;
  std::unique_ptr<TF_OperationDescription> op_;
  // Hold `op_type` and `op_name` till both are available since we need both
  // to build a graph operation.
  string op_type_;
  const char* op_name_ = nullptr;
  // TODO(srbs): Use this.
  string device_name_;
};

// GraphFunction is a thin wrapper over a TF_Function.
struct GraphFunction : public AbstractFunction {
  TF_Function* func = nullptr;
  GraphFunction() : AbstractFunction(kGraph) {}
  explicit GraphFunction(TF_Function* func)
      : AbstractFunction(kGraph), func(func) {}
  ~GraphFunction() override {
    if (func) TF_DeleteFunction(func);
  }

  Status GetFunctionDef(FunctionDef** fdef) override {
    *fdef = &func->fdef;
    return Status::OK();
  }

  // For LLVM style RTTI.
  static bool classof(const AbstractFunction* ptr) {
    return ptr->getKind() == kGraph;
  }
};

// GraphContext wraps a TF_Graph modeling a single function and manages the
// "execution" of operation, i.e. adding them to the function.
class GraphContext : public TracingContext {
 public:
  explicit GraphContext(const char* name)
      : TracingContext(kGraph),
        graph_(new TF_Graph(), TF_DeleteGraph),
        name_(name) {}

  void Release() override { delete this; }

  TracingOperation* CreateOperation() override {
    return new GraphOperation(graph_.get());
  }

  Status AddParameter(DataType dtype, TracingTensorHandle** output) override {
    TracingOperationPtr operation(CreateOperation());
    TF_RETURN_IF_ERROR(operation->Reset("Placeholder", nullptr));
    TF_RETURN_IF_ERROR(
        operation->SetOpName(absl::StrCat("_input_", inputs_.size()).c_str()));
    TF_RETURN_IF_ERROR(operation->SetAttrType("dtype", dtype));
    int num_outputs = 1;
    std::vector<AbstractTensorHandle*> outputs(num_outputs);
    TF_RETURN_IF_ERROR(operation->Execute(
        absl::Span<AbstractTensorHandle*>(outputs), &num_outputs));

    if (num_outputs != 1) {
      return errors::Internal("Expected 1 output but found ", num_outputs);
    }
    auto* t = dyn_cast<GraphTensor>(outputs[0]);
    if (!t) {
      return tensorflow::errors::InvalidArgument(
          "Unable to cast input to GraphTensor");
    }
    inputs_.push_back(t->output_);
    *output = tensorflow::down_cast<TracingTensorHandle*>(outputs[0]);
    return Status::OK();
  }

  Status Finalize(OutputList* outputs, AbstractFunction** f) override {
    std::unique_ptr<GraphFunction> func(new GraphFunction);
    std::vector<TF_Output> graph_outputs;
    graph_outputs.reserve(outputs->outputs.size());
    for (auto* abstract_output : outputs->outputs) {
      GraphTensor* output = dyn_cast<GraphTensor>(abstract_output);
      if (!output) {
        return errors::Unimplemented(
            "Returning a non-graph tensor from a function has not "
            "been implemented yet.");
      }
      graph_outputs.push_back(output->output_);
    }

    auto s = TF_NewStatus();
    func->func = TF_GraphToFunction(
        graph_.get(), name_, 0, -1, nullptr, inputs_.size(), inputs_.data(),
        graph_outputs.size(), graph_outputs.data(), nullptr, nullptr, name_, s);
    TF_RETURN_IF_ERROR(StatusFromTF_Status(s));
    TF_DeleteStatus(s);
    *f = func.release();
    return Status::OK();
  }

  Status RegisterFunction(AbstractFunction* func) override {
    return errors::Unimplemented(
        "Registering graph functions has not been implemented yet.");
  }

  Status RemoveFunction(const string& func) override {
    return errors::Unimplemented(
        "GraphContext::RemoveFunction has not been implemented yet.");
  }
  // For LLVM style RTTI.
  static bool classof(const AbstractContext* ptr) {
    return ptr->getKind() == kGraph;
  }

 private:
  std::unique_ptr<TF_Graph, decltype(&TF_DeleteGraph)> graph_;
  std::vector<TF_Output> inputs_;
  const char* name_;
};

static TracingContext* GraphTracingFactory(const char* name, TF_Status* s) {
  return new GraphContext(name);
}

// Register the tracing implemented in this file as the default tracing engine.
static bool register_tracing = [] {
  RegisterTracingEngineFactory("graphdef", GraphTracingFactory);
  SetDefaultTracingEngine("graphdef");
  return true;
}();

}  // namespace graph
}  // namespace tracing
}  // namespace tensorflow
