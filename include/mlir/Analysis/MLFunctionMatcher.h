//===- MLFunctionMacher.h - Recursive matcher for MLFunction ----*- C++ -*-===//
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

#ifndef MLIR_ANALYSIS_MLFUNCTIONMATCHER_H_
#define MLIR_ANALYSIS_MLFUNCTIONMATCHER_H_

#include "mlir/IR/InstVisitor.h"
#include "llvm/Support/Allocator.h"
#include <utility>

namespace mlir {

struct MLFunctionMatcherStorage;
struct MLFunctionMatchesStorage;
class Instruction;

/// An MLFunctionMatcher is a recursive matcher that captures nested patterns in
/// an ML Function. It is used in conjunction with a scoped
/// MLFunctionMatcherContext that handles the memory allocations efficiently.
///
/// In order to use MLFunctionMatchers creates a scoped context and uses
/// matchers. When the context goes out of scope, everything is freed.
/// This design simplifies the API by avoiding references to the context and
/// makes it clear that references to matchers must not escape.
///
/// Example:
///   {
///      MLFunctionMatcherContext context;
///      auto gemmLike = Doall(Doall(Red(LoadStores())));
///      auto matches = gemmLike.match(f);
///      // do work on matches
///   }  // everything is freed
///

/// Recursive abstraction for matching results.
/// Provides iteration over the Instruction* captured by a Matcher.
///
/// Implemented as a POD value-type with underlying storage pointer.
/// The underlying storage lives in a scoped bumper allocator whose lifetime
/// is managed by an RAII MLFunctionMatcherContext.
/// This should be used by value everywhere.
struct MLFunctionMatches {
  using EntryType = std::pair<Instruction *, MLFunctionMatches>;
  using iterator = EntryType *;

  MLFunctionMatches() : storage(nullptr) {}

  explicit operator bool() { return storage; }

  iterator begin();
  iterator end();
  EntryType &front();
  EntryType &back();
  unsigned size() { return end() - begin(); }
  unsigned empty() { return size() == 0; }

  /// Appends the pair <inst, children> to the current matches.
  void append(Instruction *inst, MLFunctionMatches children);

private:
  friend class MLFunctionMatcher;
  friend class MLFunctionMatcherContext;

  /// Underlying global bump allocator managed by an MLFunctionMatcherContext.
  static llvm::BumpPtrAllocator *&allocator();

  MLFunctionMatchesStorage *storage;
};

/// A MLFunctionMatcher is a special type of InstWalker that:
///   1. recursively matches a substructure in the tree;
///   2. uses a filter function to refine matches with extra semantic
///      constraints (passed via a lambda of type FilterFunctionType);
///   3. TODO(ntv) Optionally applies actions (lambda).
///
/// Implemented as a POD value-type with underlying storage pointer.
/// The underlying storage lives in a scoped bumper allocator whose lifetime
/// is managed by an RAII MLFunctionMatcherContext.
/// This should be used by value everywhere.
using FilterFunctionType = std::function<bool(const Instruction &)>;
static bool defaultFilterFunction(const Instruction &) { return true; };
struct MLFunctionMatcher : public InstWalker<MLFunctionMatcher> {
  MLFunctionMatcher(Instruction::Kind k, MLFunctionMatcher child,
                    FilterFunctionType filter = defaultFilterFunction);
  MLFunctionMatcher(Instruction::Kind k,
                    MutableArrayRef<MLFunctionMatcher> children,
                    FilterFunctionType filter = defaultFilterFunction);

  /// Returns all the matches in `function`.
  MLFunctionMatches match(Function *function);

  /// Returns all the matches nested under `instruction`.
  MLFunctionMatches match(Instruction *instruction);

  unsigned getDepth();

private:
  friend class MLFunctionMatcherContext;
  friend InstWalker<MLFunctionMatcher>;

  Instruction::Kind getKind();
  MutableArrayRef<MLFunctionMatcher> getChildrenMLFunctionMatchers();
  FilterFunctionType getFilterFunction();

  MLFunctionMatcher forkMLFunctionMatcherAt(MLFunctionMatcher tmpl,
                                            Instruction *inst);

  void matchOne(Instruction *elem);

  void visitForInst(ForInst *forInst) { matchOne(forInst); }
  void visitIfInst(IfInst *ifInst) { matchOne(ifInst); }
  void visitOperationInst(OperationInst *opInst) { matchOne(opInst); }

  /// Underlying global bump allocator managed by an MLFunctionMatcherContext.
  static llvm::BumpPtrAllocator *&allocator();

  MLFunctionMatcherStorage *storage;

  // By-value POD wrapper to underlying storage pointer.
  MLFunctionMatches matches;
};

/// RAII structure to transparently manage the bump allocator for
/// MLFunctionMatcher and MLFunctionMatches classes.
struct MLFunctionMatcherContext {
  MLFunctionMatcherContext() {
    MLFunctionMatcher::allocator() = &allocator;
    MLFunctionMatches::allocator() = &allocator;
  }
  ~MLFunctionMatcherContext() {
    MLFunctionMatcher::allocator() = nullptr;
    MLFunctionMatches::allocator() = nullptr;
  }
  llvm::BumpPtrAllocator allocator;
};

namespace matcher {
// Syntactic sugar MLFunctionMatcher builder functions.
MLFunctionMatcher Op(FilterFunctionType filter = defaultFilterFunction);
MLFunctionMatcher If(MLFunctionMatcher child);
MLFunctionMatcher If(FilterFunctionType filter, MLFunctionMatcher child);
MLFunctionMatcher If(MutableArrayRef<MLFunctionMatcher> children = {});
MLFunctionMatcher If(FilterFunctionType filter,
                     MutableArrayRef<MLFunctionMatcher> children = {});
MLFunctionMatcher For(MLFunctionMatcher child);
MLFunctionMatcher For(FilterFunctionType filter, MLFunctionMatcher child);
MLFunctionMatcher For(MutableArrayRef<MLFunctionMatcher> children = {});
MLFunctionMatcher For(FilterFunctionType filter,
                      MutableArrayRef<MLFunctionMatcher> children = {});

bool isParallelLoop(const Instruction &inst);
bool isReductionLoop(const Instruction &inst);
bool isLoadOrStore(const Instruction &inst);

} // end namespace matcher
} // end namespace mlir

#endif // MLIR_ANALYSIS_MLFUNCTIONMATCHER_H_
