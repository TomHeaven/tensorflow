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

#include <cstdint>
#include <iterator>
#include <numeric>
#include <string>

#include "absl/memory/memory.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Quant/QuantOps.h"  // from @llvm-project
#include "mlir/Dialect/SCF/SCF.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/Module.h"  // from @llvm-project
#include "mlir/IR/OperationSupport.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/SymbolTable.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "mlir/Transforms/InliningUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tfr/ir/tfr_ops.h"
#include "tensorflow/compiler/mlir/tfr/ir/tfr_types.h"
#include "tensorflow/compiler/mlir/tfr/passes/passes.h"
#include "tensorflow/compiler/mlir/tfr/utils/utils.h"

//===----------------------------------------------------------------------===//
// The pass to rewrite the TFR function call ops by TF ops. The callee of the
// TFR function call defines the signatures of the TF ops.
//
namespace mlir {
namespace TFR {

namespace {

// This pattern is to rewrite the "tfr.call" op and the "tfr.cast" ops on the
// operands by a TF op with "tfr.cast" ops on the results. The result type of
// the new TF op is an unranked tensor with element type derived.
class RewriteTFRCallOp : public OpRewritePattern<CallOp> {
  using OpRewritePattern<CallOp>::OpRewritePattern;

 public:
  explicit RewriteTFRCallOp(MLIRContext* context, const SymbolTable& table,
                            bool materialize_derived_attrs)
      : OpRewritePattern<CallOp>(context),
        symbol_table_(table),
        materialize_derived_attrs_(materialize_derived_attrs) {}

  LogicalResult matchAndRewrite(CallOp call_op,
                                PatternRewriter& rewriter) const override;

 private:
  // Derives the attribute values for the attributes attached to the
  // `input_tfr_type`. These attributes are only for the element type of the
  // inputs, and these type information has been collected in the `input_types`.
  // The result is stored in `derived_attrs` as the named attributes. Returns
  // failure if the attributes stored in the `input_tfr_type` violates the
  // assumptions.
  LogicalResult AddDerivedAttrs(
      PatternRewriter& rewriter, Type input_tfr_type,
      ArrayRef<Attribute> input_types,
      llvm::StringMap<Attribute>* derived_attrs) const;

  // Collects the operands and attributes for the TF op. At the same time, it
  // collects all the derived attribute values to derive the output types of the
  // TF op.
  LogicalResult CollectInputsAndAttributes(
      PatternRewriter& rewriter, TFRFuncOp signature, CallOp call_op,
      SmallVectorImpl<Value>* inputs, NamedAttrList* arg_attrs,
      llvm::StringMap<Attribute>* derived_attrs) const;

  // Uses the collected attribute values to derive all the output types.
  LogicalResult DeriveOutputTypes(FunctionType signature,
                                  const llvm::StringMap<Attribute>& attrs,
                                  SmallVectorImpl<Type>* output_types) const;

  // Creates the TF op and also the necessary tfr.cast ops to replace the
  // original TFR call op.
  LogicalResult CreateAndReplaceOp(
      PatternRewriter& rewriter, CallOp call_op,
      const SmallVectorImpl<Type>& output_types,
      const SmallVectorImpl<Value>& inputs, const NamedAttrList& attr_list,
      const llvm::StringMap<Attribute>& derived_attrs) const;

  // Adds a tf.Cast op if the tfr.tensor attribute indicated a fixed element
  // type.
  // TODO(fengliuai): This method is required when the operand types are not set
  // by the frontend correctly.
  Value CastToNonDerivedType(PatternRewriter& rewriter, Location loc,
                             CastOp cast_op, Type input_tfr_type) const {
    auto tensor_type = input_tfr_type.dyn_cast<TFRTensorType>();
    if (!tensor_type) return cast_op.arg();

    auto attr_names = tensor_type.getAttrKeys();
    if (attr_names.empty() || attr_names.size() > 1) return cast_op.arg();
    StringRef tfr_type_attr = attr_names[0].getValue();
    if (!fixed_elt_type_attrs_.contains(tfr_type_attr)) return cast_op.arg();

    Type result_elt_type;
    if (tfr_type_attr == "i32_") {
      result_elt_type = rewriter.getI32Type();
    } else if (tfr_type_attr == "i64_") {
      result_elt_type = rewriter.getI64Type();
    } else if (tfr_type_attr == "f32_") {
      result_elt_type = rewriter.getF32Type();
    } else if (tfr_type_attr == "i1_") {
      result_elt_type = rewriter.getI1Type();
    } else {
      return cast_op.arg();
    }

    Type original_input_type =
        cast_op.getInputElementType().cast<TypeAttr>().getValue();
    if (result_elt_type != original_input_type) {
      UnrankedTensorType result_type = UnrankedTensorType::get(result_elt_type);
      return rewriter.create<TF::CastOp>(loc, result_type, cast_op.arg());
    }
    return cast_op.arg();
  }

  // For variadic operands, we have to enforce them to use the same types.
  // TODO(fengliuai): This method is required when the operand types are not set
  // by the frontend correctly.
  void CastValuesToSameType(PatternRewriter& rewriter, Location loc,
                            const llvm::SmallVectorImpl<Attribute>& input_types,
                            llvm::SmallVectorImpl<Value>& input_values) const {
    if (input_types.size() <= 1) return;

    Type target_input_type = input_types[0].cast<TypeAttr>().getValue();
    auto result_type = UnrankedTensorType::get(target_input_type);
    for (auto i = 1; i < input_types.size(); ++i) {
      Type current_input_type = input_types[i].cast<TypeAttr>().getValue();
      if (current_input_type != target_input_type) {
        input_values[i] =
            rewriter.create<TF::CastOp>(loc, result_type, input_values[i]);
      }
    }
  }

  const SymbolTable& symbol_table_;
  const bool materialize_derived_attrs_;
  const llvm::SmallDenseSet<StringRef, 4> fixed_elt_type_attrs_{"i32_", "i64_",
                                                                "f32_", "i1_"};
};

LogicalResult RewriteTFRCallOp::AddDerivedAttrs(
    PatternRewriter& rewriter, Type input_tfr_type,
    ArrayRef<Attribute> input_types,
    llvm::StringMap<Attribute>* derived_attrs) const {
  // If there is an attribute associated to the input in the signature, we
  // store it as an derived attribute.
  if (auto tensor_type = input_tfr_type.dyn_cast<TFRTensorType>()) {
    auto attr_names = tensor_type.getAttrKeys();
    if (attr_names.empty()) return success();

    if (attr_names.size() == 1) {
      derived_attrs->insert({attr_names[0].getValue(), input_types[0]});
      return success();
    }
  }

  // If there is an attribute associated to the input in the signature,
  // we store it as an derived attribute.
  if (auto list_type = input_tfr_type.dyn_cast<TFRTensorListType>()) {
    auto attr_names = list_type.getAttrKeys();
    if (attr_names.empty()) return success();

    // N*T case
    if (attr_names.size() == 2) {
      derived_attrs->insert({attr_names[0].getValue(),
                             rewriter.getI32IntegerAttr(input_types.size())});
      // Note that this uses the first element of the list to infer the T value.
      // A tf.Cast is required to cast the other inputs to the same type.
      derived_attrs->insert({attr_names[1].getValue(), input_types[0]});
      return success();
    }

    // list(dtype) case
    if (attr_names.size() == 1) {
      derived_attrs->insert(
          {attr_names[0].getValue(), rewriter.getArrayAttr(input_types)});
      return success();
    }
  }

  return failure();
}

LogicalResult RewriteTFRCallOp::CollectInputsAndAttributes(
    PatternRewriter& rewriter, TFRFuncOp signature, CallOp call_op,
    SmallVectorImpl<Value>* inputs, NamedAttrList* arg_attrs,
    llvm::StringMap<Attribute>* derived_attrs) const {
  for (const auto& operand : llvm::enumerate(signature.getType().getInputs())) {
    // If the index is larger than the operand number of the call_op, the
    // default value of the operand needs to be used.
    if (operand.index() >= call_op.getNumOperands()) {
      auto attr_name = signature.getArgAttrOfType<StringAttr>(
          operand.index(), kAttrArgumentNameAttr);
      auto attr_value =
          signature.getArgAttr(operand.index(), kAttrArgumentDefaultAttr);
      arg_attrs->push_back(
          rewriter.getNamedAttr(attr_name.getValue(), attr_value));
      continue;
    }

    // The index is valid for the call_op.
    Value input = call_op.getOperand(operand.index());
    Operation* input_op = input.getDefiningOp();
    auto input_tfr_type = signature.getType().getInputs()[operand.index()];

    // There are three cases for the preceding input_op:

    // 1. The preceding op can be a tfr.cast op, which will be fused to the
    // current op, so the result op has input with tensor type.
    if (auto cast_op = dyn_cast_or_null<CastOp>(input_op)) {
      Value input_to_cast = CastToNonDerivedType(rewriter, call_op.getLoc(),
                                                 cast_op, input_tfr_type);
      inputs->push_back(input_to_cast);
      if (failed(AddDerivedAttrs(rewriter, input_tfr_type,
                                 {cast_op.getInputElementType()},
                                 derived_attrs))) {
        return failure();
      }
      continue;
    }

    // 2. The preceding op is a tfr.build_list op, which collects multiple
    // values with tensor types via the tfr.cast ops. These ops will be fused
    // to the current op as well, so all the tfr.cast op inputs will be inputs
    // to the result op.
    if (auto list_op = dyn_cast_or_null<BuildListOp>(input_op)) {
      // Find out all the inputs to the build list op
      // TODO(fengliuai): make build_list op only take tensor argument
      llvm::SmallVector<Attribute, 4> list_input_types;
      llvm::SmallVector<Value, 4> list_inputs;
      for (auto list_input : list_op.getOperands()) {
        auto cast_op = dyn_cast_or_null<CastOp>(list_input.getDefiningOp());
        if (!cast_op) return failure();
        list_inputs.push_back(cast_op.arg());
        list_input_types.push_back(cast_op.getInputElementType());
      }
      CastValuesToSameType(rewriter, call_op.getLoc(), list_input_types,
                           list_inputs);
      inputs->append(list_inputs.begin(), list_inputs.end());
      if (failed(AddDerivedAttrs(rewriter, input_tfr_type, list_input_types,
                                 derived_attrs))) {
        return failure();
      }
      continue;
    }

    // 3. The preceding op is a constant, thus the value of this constant is
    // used to create an attribute of the result op, according to the signature.
    Attribute arg_value;
    // A failure indicates the argument isn't a constant value, so we should
    // not use it as an attribute.
    if (!matchPattern(input, m_Constant(&arg_value))) {
      return failure();
    }
    auto attr_name = signature.getArgAttrOfType<StringAttr>(
        operand.index(), kAttrArgumentNameAttr);
    arg_attrs->push_back(
        rewriter.getNamedAttr(attr_name.getValue(), arg_value));
  }
  return success();
}

// For each output, uses the attribute name associated to the tfr types to find
// out the attribute value from the collected `attrs` and create the output type
// of the result op by using the attribute value as the element type.
LogicalResult RewriteTFRCallOp::DeriveOutputTypes(
    FunctionType signature, const llvm::StringMap<Attribute>& attrs,
    SmallVectorImpl<Type>* output_types) const {
  for (auto res : llvm::enumerate(signature.getResults())) {
    if (auto tensor_type = res.value().dyn_cast<TFRTensorType>()) {
      // tfr.tensor should only have one attribute attached.
      auto attr_key = tensor_type.getAttrKeys().front();
      output_types->push_back(UnrankedTensorType::get(
          attrs.lookup(attr_key.getValue()).cast<TypeAttr>().getValue()));
      continue;
    }

    if (auto list_type = res.value().dyn_cast<TFRTensorListType>()) {
      // There are two cases: N*T or list(dtype)
      auto attr_keys = list_type.getAttrKeys();
      // N*T case
      if (attr_keys.size() == 2) {
        // The first one is N, and the second one is T
        int list_size =
            attrs.lookup(attr_keys[0].getValue()).cast<IntegerAttr>().getInt();
        Type list_type =
            attrs.lookup(attr_keys[1].getValue()).cast<TypeAttr>().getValue();
        for (int i = 0; i < list_size; ++i) {
          output_types->push_back(UnrankedTensorType::get(list_type));
        }
        continue;
      }
      // TODO(fengliuai): list(dtype) case
    }
    return failure();
  }
  return success();
}

LogicalResult RewriteTFRCallOp::CreateAndReplaceOp(
    PatternRewriter& rewriter, CallOp call_op,
    const SmallVectorImpl<Type>& output_types,
    const SmallVectorImpl<Value>& inputs, const NamedAttrList& attr_list,
    const llvm::StringMap<Attribute>& derived_attrs) const {
  // Create the new op
  Location loc = call_op.getLoc();
  rewriter.setInsertionPointAfter(call_op);
  std::string tf_op_name = GetTFOpName(call_op.callee());
  OperationState new_state(loc, tf_op_name, inputs, output_types, attr_list);
  Operation* new_op = rewriter.createOperation(new_state);
  if (materialize_derived_attrs_) {
    for (const auto& attr : derived_attrs) {
      // Add or update the derived attribute with the value. Skip the fixed
      // element type attributes, in case they are present in the NodeDef.
      if (!fixed_elt_type_attrs_.contains(attr.first())) {
        new_op->setAttr(attr.first(), attr.second);
      }
    }
  }

  // Create the tfr.cast ops on the results and replace the uses of the
  // original call op.
  TFRTensorType unconstrainted_type = rewriter.getType<TFRTensorType>();
  SmallVector<Value, 4> new_results;
  for (auto res : llvm::enumerate(call_op.getResultTypes())) {
    Type res_type = res.value();
    if (res_type.dyn_cast<TFRTensorType>()) {
      Value new_res = new_op->getResult(res.index());
      auto casted = rewriter.create<CastOp>(loc, res_type, new_res);
      new_results.push_back(casted.out());
    } else if (auto list_type = res.value().dyn_cast<TFRTensorListType>()) {
      SmallVector<Value, 4> tensor_list;
      for (int i = res.index(); i < new_op->getNumResults(); i++) {
        Value new_res = new_op->getResult(i);
        auto casted =
            rewriter.create<CastOp>(loc, unconstrainted_type, new_res);
        tensor_list.push_back(casted.out());
      }
      auto list_op = rewriter.create<BuildListOp>(loc, res_type, tensor_list);
      new_results.push_back(list_op.out());
    }
  }
  rewriter.replaceOp(call_op, new_results);
  return success();
}

LogicalResult RewriteTFRCallOp::matchAndRewrite(
    CallOp call_op, PatternRewriter& rewriter) const {
  // Get the func op and verify that it is external. The type of this external
  // func op is used as the signature of the corresponding TF ops. All the
  // external func ops have the trailing underscore.
  std::string external_callee_name = call_op.callee().str().append("_");
  TFRFuncOp func = symbol_table_.lookup<TFRFuncOp>(external_callee_name);
  if (!func || !func.isExternal()) return failure();
  // Get the inputs and attributes. The attributes include these from the
  // argument list and also these derived from the inputs.
  SmallVector<Value, 4> inputs;
  NamedAttrList argument_attrs;
  llvm::StringMap<Attribute> derived_attrs;
  if (failed(CollectInputsAndAttributes(rewriter, func, call_op, &inputs,
                                        &argument_attrs, &derived_attrs))) {
    return failure();
  }

  // Derive the output types. The result type is derived by using the
  // attributes attched to the result type of the signature. The attribute
  // value should be either in the attribute argument list or the derived
  // attribute from the input tensors. All the result type
  // are unranked, and shape inference should be applied afterwards.
  SmallVector<Type, 4> output_types;

  // Merge the attributes from the argument list to the derived ones.
  for (auto& attr : argument_attrs) {
    derived_attrs.insert({attr.first, attr.second});
  }

  // Derive the output types by using the attributes attached to the tfr
  // types.
  if (failed(DeriveOutputTypes(func.getType(), derived_attrs, &output_types))) {
    return failure();
  }

  // Create the new op and replace the old TFR call op.
  return CreateAndReplaceOp(rewriter, call_op, output_types, inputs,
                            argument_attrs, derived_attrs);
}

// Raise TFR call ops to the TF ops.
struct RaiseToTFOpsPass : public PassWrapper<RaiseToTFOpsPass, FunctionPass> {
  void getDependentDialects(DialectRegistry& registry) const override {
    registry.insert<TFRDialect, TF::TensorFlowDialect, scf::SCFDialect,
                    StandardOpsDialect>();
  }

  explicit RaiseToTFOpsPass(llvm::Optional<ModuleOp> tfr_module,
                            bool materialize_derived_attrs)
      : external_tfr_module(tfr_module),
        materialize_derived_attrs(materialize_derived_attrs) {}

  void runOnFunction() override;

 private:
  llvm::Optional<ModuleOp> external_tfr_module;
  const bool materialize_derived_attrs;
};

void RaiseToTFOpsPass::runOnFunction() {
  FuncOp func = getFunction();
  MLIRContext* ctx = &getContext();
  SymbolTable table(external_tfr_module.hasValue()
                        ? *external_tfr_module
                        : func.getParentOfType<ModuleOp>());

  OwningRewritePatternList patterns;
  patterns.insert<RewriteTFRCallOp>(ctx, table, materialize_derived_attrs);
  for (auto* op : ctx->getRegisteredOperations()) {
    op->getCanonicalizationPatterns(patterns, ctx);
  }

  applyPatternsAndFoldGreedily(func, patterns);
}
}  // namespace

// Creates an instance of the pass to raise TFR call ops to the TF ops.
std::unique_ptr<OperationPass<FuncOp>> CreateRaiseToTFOpsPass(
    llvm::Optional<ModuleOp> tfr_module, bool materialize_derived_attrs) {
  return std::make_unique<RaiseToTFOpsPass>(tfr_module,
                                            materialize_derived_attrs);
}

static PassRegistration<RaiseToTFOpsPass> pass(
    "tfr-raise-to-tf", "Raise all the TFR call ops to TF ops.",
    [] { return CreateRaiseToTFOpsPass(); });

}  // namespace TFR
}  // namespace mlir
