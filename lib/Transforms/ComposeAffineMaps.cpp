//===- ComposeAffineMaps.cpp - MLIR Affine Transform Class-----*- C++ -*-===//
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
// This file implements a testing pass which composes affine maps from
// AffineApplyOps in a Function, by forward subtituting results from an
// AffineApplyOp into any of its users which are also AffineApplyOps.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/InstVisitor.h"
#include "mlir/Pass.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/Support/CommandLine.h"

using namespace mlir;

namespace {

// ComposeAffineMaps walks inst blocks in a Function, and for each
// AffineApplyOp, forward substitutes its results into any users which are
// also AffineApplyOps. After forward subtituting its results, AffineApplyOps
// with no remaining uses are collected and erased after the walk.
// TODO(andydavis) Remove this when Chris adds instruction combiner pass.
struct ComposeAffineMaps : public FunctionPass, InstWalker<ComposeAffineMaps> {
  std::vector<OperationInst *> affineApplyOpsToErase;

  explicit ComposeAffineMaps() : FunctionPass(&ComposeAffineMaps::passID) {}
  using InstListType = llvm::iplist<Instruction>;
  void walk(InstListType::iterator Start, InstListType::iterator End);
  void visitOperationInst(OperationInst *inst);
  PassResult runOnFunction(Function *f) override;
  using InstWalker<ComposeAffineMaps>::walk;

  static char passID;
};

} // end anonymous namespace

char ComposeAffineMaps::passID = 0;

FunctionPass *mlir::createComposeAffineMapsPass() {
  return new ComposeAffineMaps();
}

void ComposeAffineMaps::walk(InstListType::iterator Start,
                             InstListType::iterator End) {
  while (Start != End) {
    walk(&(*Start));
    // Increment iterator after walk as visit function can mutate inst list
    // ahead of 'Start'.
    ++Start;
  }
}

void ComposeAffineMaps::visitOperationInst(OperationInst *opInst) {
  if (auto affineApplyOp = opInst->dyn_cast<AffineApplyOp>()) {
    forwardSubstitute(affineApplyOp);
    bool allUsesEmpty = true;
    for (auto *result : affineApplyOp->getInstruction()->getResults()) {
      if (!result->use_empty()) {
        allUsesEmpty = false;
        break;
      }
    }
    if (allUsesEmpty) {
      affineApplyOpsToErase.push_back(opInst);
    }
  }
}

PassResult ComposeAffineMaps::runOnFunction(Function *f) {
  affineApplyOpsToErase.clear();
  walk(f);
  for (auto *opInst : affineApplyOpsToErase) {
    opInst->erase();
  }
  return success();
}

static PassRegistration<ComposeAffineMaps> pass("compose-affine-maps",
                                                "Compose affine maps");
