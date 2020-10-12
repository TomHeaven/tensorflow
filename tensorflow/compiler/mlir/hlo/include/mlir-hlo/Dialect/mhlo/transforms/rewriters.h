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

#ifndef TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_MHLO_TRANSFORMS_REWRITERS_H_
#define TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_MHLO_TRANSFORMS_REWRITERS_H_

#include <memory>

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/Bufferize.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
class LLVMTypeConverter;
class LowerToLLVMOptions;
class OwningRewritePatternList;
class BufferAssignmentPlacer;

// Populates a collection of rewrite patterns to realize element-wise operations
// on ranked tensors where possible.
void PopulateTransformUnrankedHloPatterns(MLIRContext *context,
                                          OwningRewritePatternList *patterns);

namespace mhlo {

// Collection of rewrite patterns for lowering a general dot product.
void PopulateGeneralDotOpLoweringPatterns(OwningRewritePatternList *patterns,
                                          MLIRContext *ctx);

// Collection of rewrite patterns for lowering complex operations to equivalent
// float operations.
void PopulateComplexLoweringPatterns(MLIRContext *context,
                                     OwningRewritePatternList *patterns);

void PopulateOptimizeMHLOPatterns(MLIRContext *context,
                                  OwningRewritePatternList *patterns);

// Rewrite patterns for gather to equivalent torch index select legalization.
void PopulateGatherToTorchIndexSelectPatterns(
    mlir::MLIRContext *context, OwningRewritePatternList *patterns);

void PopulateMhloToStdPatterns(OwningRewritePatternList *patterns,
                               MLIRContext *ctx);

// Collection of rewrite patterns for lowering of HLO to LHLO dialect.
void populateHLOToLHLOConversionPattern(
    MLIRContext *context, BufferAssignmentTypeConverter *converter,
    OwningRewritePatternList *patterns);

// Collection of rewrite patterns for lowering of HLO to Linalg dialect.
void populateHLOToLinalgConversionPattern(MLIRContext *context,
                                          OwningRewritePatternList *patterns);

// Sets up legality definitions for materializing broadcasts.
void SetupMaterializeBroadcastsLegality(MLIRContext *context,
                                        ConversionTarget *conversionTarget);

// Populates a collection of rewrite patterns for materializing broadcast
// attributes to equivalent sequences of ops.
void PopulateMaterializeBroadcastsPatterns(MLIRContext *context,
                                           OwningRewritePatternList *patterns);

// Sets up legality definitions for element-wise operations on ranked tensors.
void SetupTransformUnrankedHloLegality(MLIRContext *context,
                                       ConversionTarget *conversionTarget);

// Populates a collection of rewrite patterns to realize element-wise operations
// on ranked tensors where possible.
void PopulateTransformUnrankedHloPatterns(MLIRContext *context,
                                          OwningRewritePatternList *patterns);

// Populate a collection of conversion patterns for un-fusing
// batch_norm_inference and batch_norm_training into constituent HLO ops.
// TODO(laurenzo): Implement un-fusing of batch_norm_training.
void PopulateUnfuseBatchNormPatterns(MLIRContext *context,
                                     OwningRewritePatternList *patterns);

// Populates patterns that translate the trigonometric operations from the
// standard dialect to approximations that do not use intrinsics.
void PopulateTrigonometricToApproximationPatterns(
    MLIRContext *context, OwningRewritePatternList *patterns);

}  // namespace mhlo

namespace lmhlo {

/// Collect a set of patterns to convert from the LHLO dialect to LLVM.
void PopulateLhloToLLVMConversionPatterns(LLVMTypeConverter *converter,
                                          OwningRewritePatternList *patterns);

}  // namespace lmhlo

namespace chlo {

// Populates a collection of conversion patterns for legalizing client-HLO to
// HLO.
void PopulateLegalizeChloToHloPatterns(MLIRContext *context,
                                       OwningRewritePatternList *patterns);

}  // namespace chlo

}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_MHLO_TRANSFORMS_REWRITERS_H_
