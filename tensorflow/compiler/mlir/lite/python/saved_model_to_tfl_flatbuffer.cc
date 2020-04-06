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
#include "tensorflow/compiler/mlir/lite/python/saved_model_to_tfl_flatbuffer.h"

#include <utility>

#include "llvm/Support/ToolOutputFile.h"
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Module.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/FileUtilities.h"  // from @llvm-project
#include "mlir/Transforms/ViewOpGraph.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/common/tfl_pass_config.h"
#include "tensorflow/compiler/mlir/lite/python/tf_tfl_flatbuffer_helpers.h"
#include "tensorflow/compiler/mlir/lite/tf_tfl_passes.h"
#include "tensorflow/compiler/mlir/lite/tf_to_tfl_flatbuffer.h"
#include "tensorflow/compiler/mlir/lite/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/import_model.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/mlir_roundtrip_flags.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/protobuf/graph_debug_info.pb.h"
#include "tensorflow/lite/toco/model_flags.pb.h"
#include "tensorflow/lite/toco/toco_flags.pb.h"
#include "tensorflow/lite/toco/types.pb.h"
#include "tensorflow/stream_executor/lib/statusor.h"

namespace tensorflow {

Status ConvertSavedModelToTFLiteFlatBuffer(
    const toco::ModelFlags& model_flags, const toco::TocoFlags& toco_flags,
    string* result) {
  mlir::MLIRContext context;
  mlir::TFL::QuantizationSpecs quant_specs;

  // Parse input arrays.
  std::vector<string> node_names;
  std::vector<string> node_dtypes;
  std::vector<std::vector<int>> node_shapes;
  std::vector<double> node_mins;
  std::vector<double> node_maxs;

  // Populate quantization specs.
  TF_RETURN_IF_ERROR(internal::PopulateQuantizationSpecs(
      model_flags, toco_flags, &quant_specs, &node_names, &node_dtypes,
      &node_shapes, &node_mins, &node_maxs));

  internal::WarningUnusedFlags(model_flags, toco_flags);

  // Register all custom ops, including user-specified custom ops.
  TF_RETURN_IF_ERROR(internal::RegisterAllCustomOps(toco_flags));

  auto& saved_model_tags = model_flags.saved_model_tags();
  auto& saved_model_exported_names = model_flags.saved_model_exported_names();
  std::unordered_set<std::string> tags(saved_model_tags.begin(),
                                       saved_model_tags.end());
  auto exported_names_in_vector = std::vector<std::string>(
      saved_model_exported_names.begin(), saved_model_exported_names.end());
  absl::Span<std::string> exported_names(exported_names_in_vector);

  TF_ASSIGN_OR_RETURN(auto module,
                      ImportSavedModel(model_flags.saved_model_dir(),
                                       model_flags.saved_model_version(), tags,
                                       exported_names, &context));

  mlir::TFL::PassConfig pass_config(quant_specs);
  bool emit_builtin_tflite_ops = !toco_flags.force_select_tf_ops();
  pass_config.emit_builtin_tflite_ops = emit_builtin_tflite_ops;
  pass_config.lower_tensor_list_ops = true;
  pass_config.shape_inference = true;

  auto status = internal::ConvertMLIRToTFLiteFlatBuffer(
      toco_flags, std::move(module), pass_config, result);
  return status;
}

}  // namespace tensorflow
