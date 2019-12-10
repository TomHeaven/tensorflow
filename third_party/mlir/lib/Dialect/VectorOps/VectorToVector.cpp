//===- VectorToLoops.cpp - Conversion within the Vector dialect -----------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements target-independent rewrites as 1->N patterns.
//
//===----------------------------------------------------------------------===//

#include <type_traits>

#include "mlir/Dialect/VectorOps/Utils.h"
#include "mlir/Dialect/VectorOps/VectorOps.h"
#include "mlir/Dialect/VectorOps/VectorTransforms.h"
#include "mlir/EDSC/Builders.h"
#include "mlir/EDSC/Helpers.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/Functional.h"
#include "mlir/Support/STLExtras.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "vector-to-vector"

using namespace mlir;
using llvm::dbgs;
using mlir::functional::zipMap;

/// Given a shape with sizes greater than 0 along all dimensions,
/// returns the distance, in number of elements, between a slice in a dimension
/// and the next slice in the same dimension.
///   e.g. shape[3, 4, 5] -> linearization_basis[20, 5, 1]
static SmallVector<int64_t, 8> computeStrides(ArrayRef<int64_t> shape) {
  if (shape.empty())
    return {};
  SmallVector<int64_t, 8> tmp;
  tmp.reserve(shape.size());
  int64_t running = 1;
  for (auto size : llvm::reverse(shape)) {
    assert(size > 0 && "size must be nonnegative");
    tmp.push_back(running);
    running *= size;
  }
  return SmallVector<int64_t, 8>(tmp.rbegin(), tmp.rend());
}

static int64_t computeMaxLinearIndex(ArrayRef<int64_t> basis) {
  if (basis.empty())
    return 0;
  int64_t res = 1;
  for (auto b : basis)
    res *= b;
  return res;
}

/// Computes and returns the linearized index of 'offsets' w.r.t. 'basis'.
static int64_t linearize(ArrayRef<int64_t> offsets, ArrayRef<int64_t> basis) {
  assert(offsets.size() == basis.size());
  int64_t linearIndex = 0;
  for (unsigned idx = 0, e = basis.size(); idx < e; ++idx)
    linearIndex += offsets[idx] * basis[idx];
  return linearIndex;
}

/// Given a shape with sizes greater than 0 along all dimensions, returns the
/// delinearized components of linearIndex along shape.
static SmallVector<int64_t, 8> delinearize(int64_t linearIndex,
                                           ArrayRef<int64_t> basis) {
  SmallVector<int64_t, 8> res;
  res.reserve(basis.size());
  for (unsigned idx = 0, e = basis.size(); idx < e; ++idx) {
    assert(basis[idx] > 0);
    res.push_back(linearIndex / basis[idx]);
    linearIndex %= basis[idx];
  }
  // Sanity check.
  assert(linearIndex == 0 && "linear index remainder must be 0");
  return res;
}

// Clones `op` into a new operations that takes `operands` and returns
// `resultTypes`.
static Operation *cloneOpWithOperandsAndTypes(PatternRewriter &builder,
                                              Location loc, Operation *op,
                                              ArrayRef<Value *> operands,
                                              ArrayRef<Type> resultTypes) {
  OperationState res(loc, op->getName().getStringRef(), operands, resultTypes,
                     op->getAttrs());
  return builder.createOperation(res);
}

// Helper function for Tablegen.
static bool hasShape(Value *v, ArrayRef<int64_t> shape) {
  auto t = v->getType().dyn_cast<ShapedType>();
  if (!t)
    return false;
  return std::equal(t.getShape().begin(), t.getShape().end(), shape.begin());
}

static Value *makeSplatZero(Location loc, PatternRewriter &rewriter,
                            VectorType vt) {
  auto t = vt.getElementType();
  Value *f = nullptr;
  if (t.isBF16() || t.isF16())
    f = rewriter.create<ConstantOp>(loc, t, rewriter.getF64FloatAttr(0.0f));
  else if (t.isF32())
    f = rewriter.create<ConstantOp>(loc, t, rewriter.getF32FloatAttr(0.0f));
  else if (t.isF64())
    f = rewriter.create<ConstantOp>(loc, t, rewriter.getF64FloatAttr(0.0f));
  if (f)
    return rewriter.create<SplatOp>(loc, vt, f);
  llvm_unreachable("Unsupported type in `makeSplatZero`");
}

// Populates 'resultElements[indexMap[i]]' with elements from 'inputElements[i]'
// for each index 'i' in inputElements with a valid mapping in 'indexMap'.
static void getMappedElements(const DenseMap<int64_t, int64_t> &indexMap,
                              ArrayRef<int64_t> inputElements,
                              SmallVectorImpl<int64_t> &resultElements) {
  assert(indexMap.size() == resultElements.size());
  assert(inputElements.size() >= resultElements.size());
  for (unsigned i = 0, e = inputElements.size(); i < e; ++i) {
    auto it = indexMap.find(i);
    if (it != indexMap.end())
      resultElements[it->second] = inputElements[i];
  }
}

// UnrolledVectorState aggregates per-operand/result vector state required for
// unrolling.
struct UnrolledVectorState {
  SmallVector<int64_t, 4> unrolledShape;
  SmallVector<int64_t, 4> unrollFactors;
  SmallVector<int64_t, 8> basis;
  int64_t numInstances;
};

// Populates 'state' with unrolled shape, unroll factors, basis and
// num unrolled instances for 'vectorType'.
static void initUnrolledVectorState(VectorType vectorType,
                                    const DenseMap<int64_t, int64_t> &indexMap,
                                    ArrayRef<int64_t> targetShape,
                                    UnrolledVectorState &state) {
  // Compute unrolled shape of 'vectorType'.
  state.unrolledShape.resize(vectorType.getRank());
  getMappedElements(indexMap, targetShape, state.unrolledShape);
  // Compute unroll factors for unrolled shape.
  auto maybeUnrollFactors =
      shapeRatio(vectorType.getShape(), state.unrolledShape);
  assert(maybeUnrollFactors.hasValue());
  state.unrollFactors = *maybeUnrollFactors;
  // Compute 'basis' and 'numInstances' based on 'state.unrollFactors'.
  state.basis = computeStrides(state.unrollFactors);
  state.numInstances = computeMaxLinearIndex(state.unrollFactors);
}

// Computes and returns the linear index of the unrolled vector at
// 'vectorOffsets' within the vector represented by 'state'.
static int64_t
getUnrolledVectorLinearIndex(UnrolledVectorState &state,
                             ArrayRef<int64_t> vectorOffsets,
                             DenseMap<int64_t, int64_t> &indexMap) {
  // Compute vector offsets.
  SmallVector<int64_t, 4> sliceOffsets(state.unrolledShape.size());
  getMappedElements(indexMap, vectorOffsets, sliceOffsets);
  // Compute and return linear index of 'sliceOffsets' w.r.t 'state.basis'.
  return linearize(sliceOffsets, state.basis);
}

// Returns an unrolled vector at 'vectorOffsets' within the vector
// represented by 'state'. The vector is created from a slice of 'initValue'
// if not present in 'cache'.
static Value *getOrCreateUnrolledVectorSlice(
    Location loc, UnrolledVectorState &state, ArrayRef<int64_t> vectorOffsets,
    ArrayRef<int64_t> offsets, DenseMap<int64_t, int64_t> &indexMap,
    Value *initValue, SmallVectorImpl<Value *> &cache,
    PatternRewriter &builder) {
  // Compute slice offsets.
  SmallVector<int64_t, 4> sliceOffsets(state.unrolledShape.size());
  getMappedElements(indexMap, offsets, sliceOffsets);
  // TODO(b/144845578) Support non-1 strides.
  SmallVector<int64_t, 4> sliceStrides(state.unrolledShape.size(), 1);
  // Compute linear index of 'sliceOffsets' w.r.t 'state.basis'.
  int64_t sliceLinearIndex =
      getUnrolledVectorLinearIndex(state, vectorOffsets, indexMap);
  assert(sliceLinearIndex < static_cast<int64_t>(cache.size()));
  auto *valueSlice = cache[sliceLinearIndex];
  if (valueSlice == nullptr) {
    assert(initValue != nullptr);
    // Initialize 'cache' with slice from 'state.value'.
    valueSlice = builder.create<vector::StridedSliceOp>(
        loc, initValue, sliceOffsets, state.unrolledShape, sliceStrides);
    // Store value back to 'cache'.
    cache[sliceLinearIndex] = valueSlice;
  }
  return valueSlice;
}

// VectorState aggregates per-operand/result vector state required for
// creating slices of vector operands, and clones of the operation being
// unrolled.
struct VectorState {
  // The type of this vector.
  VectorType type;
  // Map from iteration space index to vector dimension index.
  DenseMap<int64_t, int64_t> indexMap;
  // Index of this value in operation's operand list (-1 if not an operand).
  int64_t operandIndex = -1;
  // Accumulator iterator flag.
  bool isAcc = false;
};

//
// unrollSingleResultStructuredOp
//
// Returns a value representing the result of structured operation 'op'
// with iteration bounds 'iterationBounds' unrolled to 'targetShape'.
// A list of VectorState objects must be specified in 'vectors', where
// each VectorState in the list represents a vector operand or vector result
// (if the operation does not have an accumulator operand).
// The VectorState at index 'resultIndex' in the list must be the state
// associated with the operations single result (i.e. either its accumulator
// operand or vector result value).
//
// Example:
//
//  // Before unrolling
//
//   operand0                operand1                operand2
//       \                      |                      /
//        -------------------- opA --------------------
//
//  // After unrolling by 2
//
//   operand0                operand1                operand2
//   /      \                /      \                /      \
// slice00  slice01       slice10  slice11        slice20  slice21
//   \         |            |          |            /          |
//    -------------------- opA0 --------------------           |
//             |            |          |                       |
//              \           |          |                      /
//               -------------------- opA1 -------------------
//                          |          |
//                           \        /
//                           insertslice
//                                |

// TODO(andydavis) Add the following canonicalization/simplifcation patterns:
// *) Add pattern which matches InsertStridedSlice -> StridedSlice and forwards
//    InsertStridedSlice operand to StridedSlice.
// *) Add pattern which matches SourceOp -> StridedSlice -> UserOp which checks
//    if there are duplicate identical StridedSlice ops from SourceOp, and
//    rewrites itself to use the first duplicate. This transformation should
//    cause users of identifical StridedSlice ops to reuse the same StridedSlice
//    operation, and leave the duplicate StridedSlice ops with no users
//    (removable with DCE).

// TODO(andydavis) Generalize this to support structured ops beyond
// vector ContractionOp, and merge it with 'unrollSingleResultOpMatchingType'
static Value *unrollSingleResultStructuredOp(Operation *op,
                                             ArrayRef<int64_t> iterationBounds,
                                             std::vector<VectorState> &vectors,
                                             unsigned resultIndex,
                                             ArrayRef<int64_t> targetShape,
                                             PatternRewriter &builder) {
  auto shapedType = op->getResult(0)->getType().dyn_cast_or_null<ShapedType>();
  if (!shapedType || !shapedType.hasStaticShape())
    assert(false && "Expected a statically shaped result type");

  // Compute unroll factors for 'iterationBounds' based on 'targetShape'
  auto maybeUnrollFactors = shapeRatio(iterationBounds, targetShape);
  if (!maybeUnrollFactors.hasValue())
    assert(false && "Failed to compute unroll factors for target shape");
  auto unrollFactors = *maybeUnrollFactors;

  // Compute unrolled vector state for each vector in 'vectors'.
  unsigned numVectors = vectors.size();
  SmallVector<UnrolledVectorState, 3> unrolledVectorState(numVectors);
  for (unsigned i = 0; i < numVectors; ++i) {
    initUnrolledVectorState(vectors[i].type, vectors[i].indexMap, targetShape,
                            unrolledVectorState[i]);
  }
  // Compute number of total unrolled instances.
  auto numUnrolledInstances = computeMaxLinearIndex(unrollFactors);
  auto basis = computeStrides(unrollFactors);

  auto &resultValueState = unrolledVectorState[resultIndex];
  auto unrolledResultType = VectorType::get(resultValueState.unrolledShape,
                                            shapedType.getElementType());

  // Initialize caches for intermediate vector results.
  std::vector<SmallVector<Value *, 4>> caches(numVectors);
  for (unsigned i = 0; i < numVectors; ++i)
    caches[i].resize(unrolledVectorState[i].numInstances);

  // Unroll 'numUnrolledInstances' of 'op', storing results in 'caches'.
  for (unsigned i = 0; i < numUnrolledInstances; ++i) {
    // De-linearize w.r.t. 'basis'.
    auto vectorOffsets = delinearize(i, basis);
    // Convert from unrolled vector-space offsets to element-space offsets.
    auto offsets = zipMap([](int64_t v1, int64_t v2) { return v1 * v2; },
                          vectorOffsets, targetShape);
    // Get cached slice (or create slice) for each operand at 'offsets'.
    SmallVector<Value *, 3> operands;
    operands.resize(op->getNumOperands());
    for (unsigned i = 0; i < numVectors; ++i) {
      int64_t operandIndex = vectors[i].operandIndex;
      if (operandIndex < 0)
        continue; // Output
      auto *operand = op->getOperand(operandIndex);
      operands[operandIndex] = getOrCreateUnrolledVectorSlice(
          op->getLoc(), unrolledVectorState[i], vectorOffsets, offsets,
          vectors[i].indexMap, operand, caches[i], builder);
    }
    // Create op on sliced vector arguments.
    auto resultVector =
        cloneOpWithOperandsAndTypes(builder, op->getLoc(), op, operands,
                                    unrolledResultType)
            ->getResult(0);

    // Compute linear result index.
    int64_t linearIndex = getUnrolledVectorLinearIndex(
        resultValueState, vectorOffsets, vectors[resultIndex].indexMap);
    // Update result cache at 'linearIndex'.
    caches[resultIndex][linearIndex] = resultVector;
  }

  // Make zero splat into which we will insert results from
  // 'cache[resultIndex]'
  auto resultVectorType = op->getResult(0)->getType().cast<VectorType>();
  auto *res = makeSplatZero(op->getLoc(), builder, resultVectorType);
  SmallVector<int64_t, 4> strides(resultValueState.unrollFactors.size(), 1);
  // Insert vector accumulators into output.
  for (unsigned i = 0; i < resultValueState.numInstances; ++i) {
    auto vectorOffsets = delinearize(i, resultValueState.basis);
    // Convert from unrolled vector-space offsets to element-space offsets.
    auto offsets = zipMap([](int64_t v1, int64_t v2) { return v1 * v2; },
                          vectorOffsets, resultValueState.unrolledShape);
    res = builder.create<vector::InsertStridedSliceOp>(
        op->getLoc(), caches[resultIndex][i], res, offsets, strides);
  }
  return res;
}

static void getVectorContractionOpUnrollState(
    vector::ContractionOp contractionOp, ArrayRef<int64_t> targetShape,
    SmallVectorImpl<int64_t> &iterationBounds,
    std::vector<VectorState> &vectors, unsigned &resultIndex) {
  // Get contraction op iteration bounds.
  contractionOp.getIterationBounds(iterationBounds);
  assert(iterationBounds.size() == targetShape.size());
  // Get map from iteration space index to lhs/rhs/result shape index.
  std::vector<DenseMap<int64_t, int64_t>> iterationIndexMapList;
  contractionOp.getIterationIndexMap(iterationIndexMapList);
  unsigned numIterators = iterationIndexMapList.size();
  vectors.resize(numIterators);
  unsigned accOperandIndex = vector::ContractionOp::getAccOperandIndex();
  for (unsigned i = 0; i < numIterators; ++i) {
    vectors[i].type = contractionOp.getOperand(i)->getType().cast<VectorType>();
    vectors[i].indexMap = iterationIndexMapList[i];
    vectors[i].operandIndex = i;
    vectors[i].isAcc = i == accOperandIndex ? true : false;
  }

  if (llvm::size(contractionOp.masks()) == 2) {
    // Add vectors for lhs/rhs vector mask arguments. Masks have the
    // same vector shape lhs/rhs args, so copy their index maps.
    vectors.push_back(
        {vectors[0].type, vectors[0].indexMap, accOperandIndex + 1, false});
    vectors.push_back(
        {vectors[1].type, vectors[1].indexMap, accOperandIndex + 2, false});
  }
  // Unroll 'op' 'iterationBounds' to 'targetShape'.
  // TODO(andydavis) Use linalg style 'args_in'/'args_out' to partition
  // 'vectors' instead of 'resultIndex'.
  resultIndex = accOperandIndex;
}

static void
getVectorElementwiseOpUnrollState(Operation *op, ArrayRef<int64_t> targetShape,
                                  SmallVectorImpl<int64_t> &iterationBounds,
                                  std::vector<VectorState> &vectors,
                                  unsigned &resultIndex) {
  // Verify that operation and operands all have the same vector shape.
  auto resultType = op->getResult(0)->getType().dyn_cast_or_null<VectorType>();
  assert(resultType && "Expected op with vector result type");
  auto resultShape = resultType.getShape();
  // Verify that all operands have the same vector type as result.
  assert(llvm::all_of(op->getOperandTypes(),
                      [=](Type type) { return type == resultType; }));
  // Populate 'iterationBounds' with 'resultShape' for elementwise operations.
  iterationBounds.assign(resultShape.begin(), resultShape.end());

  // Create trivial elementwise identity index map based on 'resultShape'.
  DenseMap<int64_t, int64_t> indexMap;
  indexMap.reserve(resultShape.size());
  for (unsigned i = 0; i < resultShape.size(); ++i)
    indexMap[i] = i;

  // Create VectorState each operand and single result.
  unsigned numVectors = op->getNumOperands() + op->getNumResults();
  vectors.resize(numVectors);
  for (unsigned i = 0; i < op->getNumOperands(); ++i)
    vectors[i] = {resultType, indexMap, i, false};
  vectors[numVectors - 1] = {resultType, indexMap, -1, false};
  resultIndex = numVectors - 1;
}

// Entry point for unrolling declarative pattern rewrites.
Value *mlir::vector::unrollSingleResultOpMatchingType(
    PatternRewriter &builder, Operation *op, ArrayRef<int64_t> targetShape) {
  assert(op->getNumResults() == 1 && "Expected single result operation");

  // Populate 'iterationBounds', 'vectors' and 'resultIndex' to unroll 'op'.
  SmallVector<int64_t, 6> iterationBounds;
  std::vector<VectorState> vectors;
  unsigned resultIndex;

  if (auto contractionOp = dyn_cast<vector::ContractionOp>(op)) {
    // Popultate state for vector ContractionOp.
    getVectorContractionOpUnrollState(contractionOp, targetShape,
                                      iterationBounds, vectors, resultIndex);
  } else {
    // Populate state for vector elementwise op.
    getVectorElementwiseOpUnrollState(op, targetShape, iterationBounds, vectors,
                                      resultIndex);
  }

  // Unroll 'op' with 'iterationBounds' to 'targetShape'.
  return unrollSingleResultStructuredOp(op, iterationBounds, vectors,
                                        resultIndex, targetShape, builder);
}

namespace mlir {
namespace vector {
namespace {
#include "mlir/Dialect/VectorOps/VectorTransformPatterns.h.inc"
} // end namespace
} // end namespace vector
} // end namespace mlir

void mlir::populateVectorToVectorConversionPatterns(
    MLIRContext *context, OwningRewritePatternList &patterns,
    ArrayRef<int64_t> coarseVectorShape, ArrayRef<int64_t> fineVectorShape) {
  vector::populateWithGenerated(context, &patterns);
  vector::populateVectorToVectorCanonicalizationPatterns(patterns, context);
}
