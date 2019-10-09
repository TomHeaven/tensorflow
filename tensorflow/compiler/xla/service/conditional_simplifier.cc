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

#include "tensorflow/compiler/xla/service/conditional_simplifier.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/str_cat.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/call_graph.h"
#include "tensorflow/compiler/xla/service/call_inliner.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"

namespace xla {

namespace {
// Tries to replace a conditional with a call operation of the corresponding
// computation. If the given conditional has a constant branch_index, tries to
// replace it with a call to its corresponding branch computation and then
// inline that computation.
//
// Returns true if it made a change to the graph.
StatusOr<bool> TryRemoveConditional(HloInstruction* conditional) {
  CHECK_EQ(conditional->opcode(), HloOpcode::kConditional);
  // Do not remove conditionals that contain side-effecting instructions or
  // have control predecessors/successors in either true/false computation.
  if (!conditional->parent()->IsSafelyRemovable(conditional) ||
      conditional->HasSideEffect()) {
    VLOG(2) << "Not attempting to remove conditional as it is not removable or "
               "has side effect: "
            << conditional->ToShortString();
    return false;
  }

  // We can always inline a 1-branch conditional due to default branch fallback.
  auto computation = conditional->parent();
  auto create_call = [&](int64 branch) {
    auto call = computation->AddInstruction(HloInstruction::CreateCall(
        conditional->shape(), {conditional->mutable_operand(1 + branch)},
        conditional->branch_computation(branch)));
    conditional->SetupDerivedInstruction(call);
    return call;
  };

  if (conditional->branch_count() == 1) {
    HloInstruction* call_op = create_call(0);
    TF_RETURN_IF_ERROR(computation->ReplaceInstruction(conditional, call_op));
    TF_RETURN_IF_ERROR(CallInliner::Inline(call_op).status());
    return true;
  }

  if (conditional->operand(0)->opcode() == HloOpcode::kConstant) {
    int branch_index = 0;
    if (conditional->operand(0)->shape().element_type() == PRED) {
      branch_index = conditional->operand(0)->literal().Get<bool>({}) ? 0 : 1;
    } else {
      branch_index = conditional->operand(0)->literal().Get<int32>({});
      if (branch_index < 0 || branch_index >= conditional->branch_count()) {
        branch_index = conditional->branch_count() - 1;
      }
    }
    HloInstruction* call_op = create_call(branch_index);
    TF_RETURN_IF_ERROR(computation->ReplaceInstruction(conditional, call_op));
    TF_RETURN_IF_ERROR(CallInliner::Inline(call_op).status());

    return true;
  }

  auto instruction_is_expensive = [](const HloInstruction* hlo) {
    switch (hlo->opcode()) {
      case HloOpcode::kBroadcast:
      case HloOpcode::kConcatenate:
      case HloOpcode::kDynamicSlice:
      case HloOpcode::kDynamicUpdateSlice:
      case HloOpcode::kGetTupleElement:
      case HloOpcode::kReduce:
      case HloOpcode::kReshape:
      case HloOpcode::kPad:
      case HloOpcode::kParameter:
      case HloOpcode::kSlice:
      case HloOpcode::kTuple:
        return false;
      default:
        return !hlo->IsElementwise();
    }
  };

  if (conditional->branch_count() != 2 ||
      conditional->operand(0)->shape().element_type() != PRED ||
      absl::c_any_of(conditional->branch_computation(0)->instructions(),
                     instruction_is_expensive) ||
      absl::c_any_of(conditional->branch_computation(1)->instructions(),
                     instruction_is_expensive)) {
    VLOG(2)
        << "Not attempting  to remove conditional as its branch_index is not a "
           "compile-time constant or contains expensive instructions: "
        << conditional->ToShortString();
    return false;
  }

  HloInstruction* true_call_op = create_call(0);
  HloInstruction* false_call_op = create_call(1);
  auto condition_broadcast = [&](const Shape& shape) {
    if (ShapeUtil::IsScalar(shape)) {
      return conditional->mutable_operand(0);
    }
    return computation->AddInstruction(HloInstruction::CreateBroadcast(
        ShapeUtil::ChangeElementType(shape, PRED),
        conditional->mutable_operand(0), {}));
  };

  auto gte = [&](HloInstruction* hlo, int64 i) {
    return computation->AddInstruction(HloInstruction::CreateGetTupleElement(
        hlo->shape().tuple_shapes(i), hlo, i));
  };
  std::function<HloInstruction*(HloInstruction*, HloInstruction*)> select =
      [&](HloInstruction* t, HloInstruction* f) {
        if (f->shape().IsArray()) {
          return computation->AddInstruction(HloInstruction::CreateTernary(
              f->shape(), HloOpcode::kSelect, condition_broadcast(f->shape()),
              t, f));
        }
        std::vector<HloInstruction*> selects;
        const int64 tuple_element_count =
            ShapeUtil::TupleElementCount(f->shape());
        selects.reserve(tuple_element_count);
        for (int64 i = 0; i < tuple_element_count; ++i) {
          selects.push_back(select(gte(t, i), gte(f, i)));
        }
        return computation->AddInstruction(
            HloInstruction::CreateTuple(selects));
      };

  TF_RETURN_IF_ERROR(computation->ReplaceInstruction(
      conditional, select(true_call_op, false_call_op)));

  TF_RETURN_IF_ERROR(CallInliner::Inline(false_call_op).status());
  TF_RETURN_IF_ERROR(CallInliner::Inline(true_call_op).status());
  return true;
}
StatusOr<bool> TryRemoveUnusedConditionalOperands(
    HloInstruction* conditional,
    std::map<HloComputation*, std::set<int64>>* changed_computations) {
  // Avoid dealing with sharding.
  if (conditional->has_sharding()) {
    return false;
  }
  std::vector<std::set<int64>> tuple_indices_to_keep(
      conditional->branch_count());
  bool will_change = false;
  for (int64 i = 0; i < conditional->branch_count(); ++i) {
    HloComputation* computation = conditional->branch_computation(i);
    if (changed_computations->count(computation) > 0) {
      will_change = true;
      break;
    }
    HloInstruction* param = computation->parameter_instruction(0);
    // Do not remove the root instruction.
    if (param == computation->root_instruction()) {
      return false;
    }
    // There is nothing to be removed for non-tuple operands.
    if (!param->shape().IsTuple()) {
      return false;
    }
    for (HloInstruction* user : param->users()) {
      // If the user is not a get tuple element, assume it is unsafe to remove
      // elemnts from the tuple.
      if (user->opcode() != HloOpcode::kGetTupleElement) {
        return false;
      }
      tuple_indices_to_keep[i].insert(user->tuple_index());
    }
    // If not all tuple elements are used in this conditional branch, some can
    // removed from the computation.
    if (tuple_indices_to_keep[i].size() !=
        ShapeUtil::TupleElementCount(param->shape())) {
      will_change = true;
    }
  }

  if (!will_change) {
    return false;
  }

  for (int64 branch = 0; branch < conditional->branch_count(); ++branch) {
    const Shape& old_shape = conditional->operand(branch + 1)->shape();
    int64 old_tuple_element_count = ShapeUtil::TupleElementCount(old_shape);
    // Clone the computation in case it is called by another instruction.
    HloComputation* computation = conditional->branch_computation(branch);
    if (changed_computations
            ->insert({computation, tuple_indices_to_keep[branch]})
            .second) {
      HloInstruction* param = computation->parameter_instruction(0);

      // Create a new tuple shape based on the indices actually used by this
      // branch.
      std::vector<Shape> new_tuple_shapes;
      new_tuple_shapes.reserve(tuple_indices_to_keep[branch].size());
      std::vector<int64> map(old_tuple_element_count, -1);
      for (int64 i : tuple_indices_to_keep[branch]) {
        map[i] = new_tuple_shapes.size();
        new_tuple_shapes.push_back(old_shape.tuple_shapes(i));
      }
      Shape tuple_shape = ShapeUtil::MakeTupleShape(new_tuple_shapes);
      // Reset the parameter shape of the computation.
      *param->mutable_shape() = tuple_shape;

      // Reroute the GTE instructions to new tuple indices.
      for (HloInstruction* user : param->users()) {
        user->set_tuple_index(map[user->tuple_index()]);
      }
    }

    // Reroute the operand tuple through a tuple of gte instructions of the
    // original operand tuple.
    const auto& to_keep = (*changed_computations)[computation];
    std::vector<HloInstruction*> new_tuple_operands;
    new_tuple_operands.reserve(to_keep.size());
    for (int64 i : to_keep) {
      new_tuple_operands.push_back(conditional->parent()->AddInstruction(
          HloInstruction::CreateGetTupleElement(
              old_shape.tuple_shapes(i),
              conditional->mutable_operand(branch + 1), i)));
    }
    HloInstruction* new_tuple = conditional->parent()->AddInstruction(
        HloInstruction::CreateTuple(new_tuple_operands));
    TF_RETURN_IF_ERROR(
        conditional->ReplaceOperandWithDifferentShape(branch + 1, new_tuple));
  }
  return true;
}

// Replaces the roots of all branches with an empty tuple if the conditional op
// has no users. Returns true if anything is changed.
bool ReplaceRootWithEmptyTupleIfNoUsers(HloInstruction* conditional_op) {
  const Shape empty_tuple = ShapeUtil::MakeTupleShape({});
  if (conditional_op->user_count() == 0 &&
      conditional_op != conditional_op->parent()->root_instruction() &&
      !ShapeUtil::Compatible(empty_tuple, conditional_op->shape())) {
    for (int64 branch_id = 0; branch_id < conditional_op->branch_count();
         ++branch_id) {
      auto branch_computation =
          conditional_op->GetModule()->AddEmbeddedComputation(
              conditional_op->branch_computation(branch_id)->Clone());
      conditional_op->set_branch_computation(branch_id, branch_computation);
      auto new_empty_root =
          branch_computation->AddInstruction(HloInstruction::CreateTuple({}));
      branch_computation->set_root_instruction(new_empty_root,
                                               /*accept_different_shape=*/true);
    }
    *conditional_op->mutable_shape() = empty_tuple;
    return true;
  }
  return false;
}

// Removes all unused elements from result tuple. Returns true if anything is
// changed.
//
// Computes and only keeps a subset of result tuple indices which are actually
// being used. This simplification frees up some data-dependencies in branches'
// sub-computations and enables further optimizations.
//
// *) It is considered the whole tuple is used, and there will be no removal for
//    this case:
//
//        kTuple-result
//              |
//              |
//           kWhile
//
// *) Only index=0 is used, so change (f32[10,10], f32[20,20]) to (f32[10,10])
//    and drop f32[20,20].
//
//        kTuple-result (f32[10,10], f32[20,20])
//              |
//              |
//        get-tuple-element, index=0
//
bool RemoveUnusedTupleElements(HloInstruction* conditional_op) {
  if (conditional_op->user_count() == 0 ||
      conditional_op == conditional_op->parent()->root_instruction() ||
      !conditional_op->shape().IsTuple()) {
    VLOG(3) << "Skip RemoveUnusedTupleElements due to non-tuple result:\n"
            << conditional_op->ToShortString();
    return false;
  }

  const int old_tuple_shapes_size = conditional_op->shape().tuple_shapes_size();

  // Select indices that are actually used by some GTE instructions.
  std::vector<bool> used_indices(old_tuple_shapes_size, false);
  for (const HloInstruction* user : conditional_op->users()) {
    // We only deal with the case where all users are GTE instructions.
    if (user->opcode() != HloOpcode::kGetTupleElement) {
      VLOG(3) << "Skip RemoveUnusedTupleElements due to non-GTE user:\n"
              << user->ToShortString();
      return false;
    }
    used_indices[user->tuple_index()] = true;
  }

  const int new_tuple_shapes_size =
      std::count(used_indices.begin(), used_indices.end(), true);
  if (new_tuple_shapes_size == old_tuple_shapes_size) {
    VLOG(3) << "Skip RemoveUnusedTupleElements due to every index is in use.";
    return false;
  }

  // Compute old-to-new (old-to-new) indices mapping.
  std::map<int, int> new_to_old_mapping, old_to_new_mapping;
  auto old_iter = used_indices.begin();
  for (int new_index = 0; new_index < new_tuple_shapes_size; ++new_index) {
    old_iter = std::find(old_iter, used_indices.end(), true);
    const int old_index = std::distance(used_indices.begin(), old_iter);
    new_to_old_mapping[new_index] = old_index;
    old_to_new_mapping[old_index] = new_index;
    ++old_iter;
  }

  // Create new tuple shape, only keep active indices.
  const Shape old_shape = conditional_op->shape();
  std::vector<Shape> new_tuple_shapes;
  new_tuple_shapes.reserve(new_tuple_shapes_size);
  for (int new_index = 0; new_index < new_tuple_shapes_size; ++new_index) {
    new_tuple_shapes.push_back(
        old_shape.tuple_shapes(new_to_old_mapping[new_index]));
  }
  const Shape new_shape = ShapeUtil::MakeTupleShape(new_tuple_shapes);

  // Double-check the old branch root shape is compatible (tuple-like).
  for (HloComputation* branch : conditional_op->branch_computations()) {
    const HloInstruction* root = branch->root_instruction();
    if (!root->shape().IsTuple() ||
        !ShapeUtil::Compatible(branch->root_instruction()->shape(),
                               old_shape)) {
      VLOG(3) << "Skip RemoveUnusedTupleElements due to some branch "
              << branch->name() << " has in-compatible root shape, expect "
              << old_shape.ToString() << ", but got "
              << root->shape().ToString() << "\n"
              << conditional_op->ToString();
      return false;
    }
  }

  // Replace all branches with new tuple shape. Add 'gtes' for active indices
  // and create a new root gathering them.
  //
  //  non-kTuple-root
  //    |      |
  //   gte   gte
  //     \    /
  //    new_root
  for (int branch_id = 0; branch_id < conditional_op->branch_count();
       ++branch_id) {
    HloComputation* old_branch = conditional_op->branch_computation(branch_id);
    HloComputation* cloned_branch =
        conditional_op->GetModule()->AddEmbeddedComputation(
            old_branch->Clone());
    conditional_op->set_branch_computation(branch_id, cloned_branch);

    HloInstruction* old_root = cloned_branch->root_instruction();
    std::vector<HloInstruction*> new_tuple_root_operands;
    for (int old_index = 0; old_index < old_tuple_shapes_size; ++old_index) {
      if (used_indices[old_index]) {
        new_tuple_root_operands.push_back(
            cloned_branch->AddInstruction(HloInstruction::CreateGetTupleElement(
                old_shape.tuple_shapes(old_index), old_root, old_index)));
      }
    }
    HloInstruction* new_tuple_root = cloned_branch->AddInstruction(
        HloInstruction::CreateTuple(new_tuple_root_operands));
    cloned_branch->set_root_instruction(new_tuple_root,
                                        /*accept_different_shape=*/true);
  }

  // Replace the conditional instruction itself.
  *conditional_op->mutable_shape() = new_shape;

  // Reroute all user GTE instructions to new tuple indices.
  for (HloInstruction* user : conditional_op->users()) {
    const int old_index = user->tuple_index();
    const int new_index = old_to_new_mapping[old_index];
    user->set_tuple_index(new_index);
  }
  return true;
}

// Merges duplicate(identical) elements in result tuple.
//
// Two tuple elements(indices) are duplicate if they return identical value
// (from the same HloInstruction source) in every branch. In other words, if
// replacing j-th with i-th tuple index results in an invariant, i-th/j-th are
// identical and we can safely replace all GTE j-th (users this conditional
// instruction) with GTE i-th.
//
// Afterwards, any unused j-th tuple index will be removed by
// RemoveUnusedTupleElements and the size of tuple shape will be reduced.
// E.g.
//
// Before:
//       gte          add
//      /   \        /   \
//      |   |        |   |
//     on_true      on_false
//    (f32, f32)   (f32, f32)
//         |           |
//          \         /
//          conditional
//          (f32, f32)
//            |    |
//           gte  gte
//            \    /
//            tuple
//          (f32, f32)
//
// After:
//       gte          add
//        |            |
//     on_true      on_false
//      (f32)        (f32)
//         |           |
//          \         /
//          conditional
//             (f32)
//               |
//              gte
//              |  \
//              |   |
//              tuple
//            (f32, f32)
bool MergeDuplicateTupleElements(HloInstruction* conditional) {
  if (conditional->user_count() == 0 ||
      conditional == conditional->parent()->root_instruction() ||
      !conditional->shape().IsTuple()) {
    VLOG(3) << "Skip MergeDuplicateTupleElements due not tuple shape nor root "
               "instruction:\n"
            << conditional->ToShortString();
    return false;
  }

  for (const HloInstruction* user : conditional->users()) {
    if (user->opcode() != HloOpcode::kGetTupleElement) {
      VLOG(3) << "Skip MergeDuplicateTupleElements due not all users are "
                 "kGetTupleElement:\n"
              << conditional->ToShortString();
      return false;
    }
  }

  for (const HloComputation* branch : conditional->branch_computations()) {
    if (branch->root_instruction()->opcode() != HloOpcode::kTuple) {
      VLOG(3) << "Skip MergeDuplicateTupleElements due not all branch roots "
                 "are kTuple:\n"
              << conditional->ToShortString();
      return false;
    }
  }

  // For example,
  //
  //    tuple index   |         0      1      2
  //    ------------------------------------------
  //    branch #0 root: tuple(gte-0, add-0, add-0)
  //    branch #1 root: tuple(rng-1, add-1, add-1)
  //    branch #2 root: tuple(add-2, add-2, add-2)
  //
  // vectorize(0) will be [gte-0, rng-1, add-2]
  // vectorize(1) will be [add-0, add-1, add-2]
  // vectorize(2) will be [add-0, add-1, add-2]
  //
  // In this case, vectorize(1), vectorize(2) are equal and index 1, 2 are
  // identical.
  auto vectorize_branches_root_tuple_ith_operand = [conditional](int64 i) {
    std::vector<const HloInstruction*> operands;
    absl::c_transform(conditional->branch_computations(),
                      std::back_inserter(operands),
                      [i](const HloComputation* branch) {
                        return branch->root_instruction()->operand(i);
                      });
    return operands;
  };

  auto replace_root_user_gte_jth_with_gte_ith = [conditional](int64 i,
                                                              int64 j) {
    bool changed = false;
    for (HloInstruction* user : conditional->users()) {
      if (user->tuple_index() == j) {
        user->set_tuple_index(i);
        changed |= true;
      }
    }
    return changed;
  };

  bool changed = false;
  std::map<std::vector<const HloInstruction*>, int64> index_collision_table;
  for (int i = 0; i < conditional->shape().tuple_shapes_size(); ++i) {
    const std::vector<const HloInstruction*> ith_operands_vector =
        vectorize_branches_root_tuple_ith_operand(i);
    const auto emplace_res =
        index_collision_table.emplace(ith_operands_vector, i);
    if (!emplace_res.second) {
      changed |=
          replace_root_user_gte_jth_with_gte_ith(emplace_res.first->second, i);
    }
  }
  return changed;
}

// If a conditional is unbalanced, with trivial computation in one side and
// expensive in the other, we swap true/false to always make trivial computation
// in true-branch.
//
// Background:
// The live range interference analysis in CopyRemover is biased and favorites
// removing 'copies' from true-branch over false-branch. This is because we have
// pre-defined instruction execute order (see HloOrdering::ExecutesBefore,
// copy_insertion.cc) where conditional's (i)th-branch executed before
// (i+1)th-branch. So by making trivial computation true-branch, we might
// potentially save copies from true-branch (a.k.a frequent side) and improve
// performance overall.
//
// The transformation invariant is based on:
//   cond(pred, true_fn, false_fn) == cond(not pred, false_fn, true_fn)
StatusOr<bool> TrySwapTrueFalse(HloInstruction* conditional) {
  if (conditional->user_count() == 0 &&
      conditional != conditional->parent()->root_instruction()) {
    VLOG(2) << "Skip TrySwapTrueFalse, dangling conditional instruction:\n"
            << conditional->ToString();
    return false;
  }
  if (conditional->branch_count() != 2 ||
      conditional->operand(0)->shape().element_type() != PRED) {
    VLOG(2) << "Skip TrySwapTrueFalse, non-binary conditional instruction:\n"
            << conditional->ToString();
    return false;
  }

  // Returns true if given branch computation is trivial (e.g. just paramter
  // forward).
  auto is_trivial = [](const HloComputation* branch) {
    return absl::c_all_of(branch->instructions(),
                          [](const HloInstruction* hlo) {
                            switch (hlo->opcode()) {
                              case HloOpcode::kCopy:
                              case HloOpcode::kGetTupleElement:
                              case HloOpcode::kParameter:
                              case HloOpcode::kTuple:
                              case HloOpcode::kAfterAll:
                                return true;
                              default:
                                return false;
                            }
                          });
  };

  HloComputation* true_fn = conditional->true_computation();
  HloComputation* false_fn = conditional->false_computation();

  const bool do_swap = !is_trivial(true_fn) && is_trivial(false_fn);
  if (!do_swap) {
    VLOG(2) << "Skip TrySwapTrueFalse due to conditional instruction is not "
               "satisfied:\n"
            << conditional->ToString();
    return false;
  }

  VLOG(2) << "Swapping True/False for " << conditional->ToShortString()
          << " to elide data copy from frequent branch.";

  HloInstruction* new_inverted_pred =
      conditional->parent()->AddInstruction(HloInstruction::CreateUnary(
          conditional->operand(0)->shape(), HloOpcode::kNot,
          conditional->mutable_operand(0)));
  HloComputation* new_true_fn =
      conditional->GetModule()->AddEmbeddedComputation(
          false_fn->Clone(/*suffix=*/"true_false_swapped"));
  HloComputation* new_false_fn =
      conditional->GetModule()->AddEmbeddedComputation(
          true_fn->Clone(/*suffix=*/"true_false_swapped"));
  HloInstruction* new_true_fn_args = conditional->mutable_operand(2);
  HloInstruction* new_false_fn_args = conditional->mutable_operand(1);

  conditional->set_branch_computation(0, new_true_fn);
  conditional->set_branch_computation(1, new_false_fn);
  TF_RETURN_IF_ERROR(
      conditional->ReplaceOperandWithDifferentShape(0, new_inverted_pred));
  TF_RETURN_IF_ERROR(
      conditional->ReplaceOperandWithDifferentShape(1, new_true_fn_args));
  TF_RETURN_IF_ERROR(
      conditional->ReplaceOperandWithDifferentShape(2, new_false_fn_args));
  return true;
}
}  // namespace

StatusOr<bool> ConditionalSimplifier::Run(HloModule* module) {
  XLA_VLOG_LINES(
      3, "ConditionalSimplifier::Run(), before:\n" + module->ToString());
  bool changed = false;

  // Gather all the conditional ops in our module. We do this ahead of time so
  // we don't have to worry about mutating the lists of computations or
  // instructions as we iterate.
  std::vector<HloInstruction*> conditional_ops;
  for (auto* comp : module->computations()) {
    for (auto* instr : comp->MakeInstructionPostOrder()) {
      if (instr->opcode() == HloOpcode::kConditional) {
        conditional_ops.push_back(instr);
      }
    }
  }

  std::map<HloComputation*, std::set<int64>> changed_computations;
  for (HloInstruction* conditional_op : conditional_ops) {
    changed |= MergeDuplicateTupleElements(conditional_op);
    changed |= RemoveUnusedTupleElements(conditional_op);
    changed |= ReplaceRootWithEmptyTupleIfNoUsers(conditional_op);
    TF_ASSIGN_OR_RETURN(bool result, TryRemoveConditional(conditional_op));
    if (!result) {
      TF_ASSIGN_OR_RETURN(bool swapped, TrySwapTrueFalse(conditional_op));
      TF_ASSIGN_OR_RETURN(bool removed,
                          TryRemoveUnusedConditionalOperands(
                              conditional_op, &changed_computations));
      result |= swapped || removed;
    }
    changed |= result;
  }

  XLA_VLOG_LINES(3,
                 "ConditionalSimplifier::Run(), after:\n" + module->ToString());
  return changed;
}

}  // namespace xla
