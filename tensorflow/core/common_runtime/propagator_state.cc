/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/common_runtime/propagator_state.h"

#include "tensorflow/core/common_runtime/graph_view.h"
#include "tensorflow/core/common_runtime/propagator_debug_utils.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/profiler/lib/traceme.h"

namespace tensorflow {

PropagatorState::PropagatorState(const ImmutableExecutorState& immutable_state,
                                 int64 step_id)
    : immutable_state_(immutable_state),
      step_id_(step_id),
      vlog_(VLOG_IS_ON(1)) {
  // We start the entire execution in iteration 0 of the root frame
  // so let us create the root frame and the state for iteration 0.
  // We assume root_frame_->frame_name.empty().
  root_frame_ = new FrameState(immutable_state_, 1);
  root_frame_->frame_id = 0;  // must be 0
  root_frame_->InitializeFrameInfo(root_frame_->frame_name);

  // Initialize iteration 0.
  root_frame_->SetIteration(
      0, new PropagatorState::IterationState(0, root_frame_->pending_counts,
                                             root_frame_->total_input_tensors));

  outstanding_frames_.insert({root_frame_->frame_name, root_frame_});
}

PropagatorState::~PropagatorState() {
  for (auto name_frame : outstanding_frames_) {
    delete name_frame.second;
  }
}

void PropagatorState::ActivateRoots(gtl::ArraySlice<const NodeItem*> roots,
                                    TaggedNodeSeq* ready) {
  mutex_lock l(root_frame_->mu);
  IterationState* root_iter = root_frame_->GetIteration(0);
  for (const NodeItem* item : roots) {
    DCHECK_EQ(item->num_inputs, 0);
    ready->emplace_back(item, root_frame_, root_iter, false);
  }
  root_iter->outstanding_ops = ready->size();
}

void PropagatorState::PropagateOutputs(const TaggedNode& tagged_node,
                                       EntryVector* outputs,
                                       TaggedNodeSeq* ready) {
  profiler::TraceMe activity(
      [&]() {
        return strings::StrCat(
            "ExecutorPropagateOutputs#", "id=", step_id_,
            ",kernel_name=", tagged_node.node_item->kernel->name_view(),
            ",num_output_edges=", tagged_node.node_item->num_output_edges,
            ",num_output_control_edges=",
            tagged_node.node_item->num_output_control_edges, "#");
      },
      profiler::GetTFTraceMeLevel(/*is_expensive=*/false));

  const NodeItem* const item = tagged_node.node_item;
  FrameState* const input_frame = tagged_node.input_frame;
  IterationState* const input_iter = tagged_node.input_iter;
  const bool is_dead = tagged_node.is_dead;

  // Propagates outputs along out edges, and puts newly ready nodes
  // into the ready queue.
  DCHECK(ready->empty());
  bool is_frame_done = false;
  FrameState* output_frame = input_frame;
  IterationState* output_iter = input_iter;

  if (!item->is_enter_exit_or_next_iter) {
    // Fast path for nodes types that don't need special handling
    DCHECK_EQ(input_frame, output_frame);
    // Normal path for most nodes
    mutex_lock l(input_frame->mu);
    output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
    is_frame_done =
        input_frame->DecrementOutstandingOpsLocked(input_iter, ready);
  } else if (item->is_enter) {
    FindOrCreateChildFrame(input_frame, input_iter, *item, &output_frame);
    {
      mutex_lock l(output_frame->mu);
      output_iter = output_frame->GetIteration(0);
      if (item->is_constant_enter) {
        // Propagate to all active iterations if this is a loop invariant.
        output_frame->AddLoopInv(item, (*outputs)[0], ready);
      } else {
        output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
      }
      output_frame->num_pending_inputs--;
    }
    is_frame_done = input_frame->DecrementOutstandingOps(input_iter, ready);
  } else if (item->is_exit) {
    if (is_dead) {
      mutex_lock l(input_frame->mu);
      // Stop and remember this node if it is a dead exit.
      if (input_iter->iter_num == input_frame->iteration_count) {
        input_frame->dead_exits.push_back(item);
      }
      is_frame_done =
          input_frame->DecrementOutstandingOpsLocked(input_iter, ready);
    } else {
      output_frame = input_frame->parent_frame;
      output_iter = input_frame->parent_iter;
      {
        mutex_lock l(output_frame->mu);
        output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
      }
      is_frame_done = input_frame->DecrementOutstandingOps(input_iter, ready);
    }
  } else {
    DCHECK(item->is_next_iteration);
    mutex_lock l(input_frame->mu);
    if (is_dead) {
      // Stop the deadness propagation.
      output_frame = nullptr;
    } else {
      if (input_iter->iter_num == input_frame->iteration_count &&
          input_frame->num_outstanding_iterations ==
              input_frame->max_parallel_iterations) {
        // Reached the maximum for parallel iterations.
        input_frame->next_iter_roots.push_back({item, (*outputs)[0]});
        output_frame = nullptr;
      } else {
        // If this is a new iteration, start it.
        if (input_iter->iter_num == input_frame->iteration_count) {
          output_iter = input_frame->IncrementIteration(ready);
        } else {
          output_iter = input_frame->GetIteration(input_iter->iter_num + 1);
        }
      }
    }
    if (output_frame != nullptr) {
      // This is the case when node is not Enter, Exit, or NextIteration.
      DCHECK(input_frame == output_frame);
      output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
    }
    is_frame_done =
        input_frame->DecrementOutstandingOpsLocked(input_iter, ready);
  }

  // At this point, this node is completely done. We also know if the
  // completion of this node makes its frame completed.
  if (is_frame_done) {
    FrameState* parent_frame = input_frame->parent_frame;
    IterationState* parent_iter = input_frame->parent_iter;
    DeleteFrame(input_frame, ready);
    if (parent_frame != nullptr) {
      // The completion of frame may cause completions in its parent frame.
      // So clean things up recursively.
      CleanupFramesIterations(parent_frame, parent_iter, ready);
    }
  }
}

void PropagatorState::DumpIterationState(const FrameState* frame,
                                         IterationState* iteration) {
  const std::vector<const NodeItem*>* nodes = frame->nodes;
  // Dump any waiting nodes that are holding on to tensors.
  for (const NodeItem* node : *nodes) {
    PendingCounts::Handle pending_id =
        immutable_state_.pending_ids()[node->node_id];
    if (iteration->node_state(pending_id) == PendingCounts::PENDING_NOTREADY ||
        iteration->node_state(pending_id) == PendingCounts::PENDING_READY) {
      DumpPendingNodeState(*node, iteration->input_tensors, false);
    }
  }
  // Then the active nodes.
  for (const NodeItem* node : *nodes) {
    PendingCounts::Handle pending_id =
        immutable_state_.pending_ids()[node->node_id];
    if (iteration->node_state(pending_id) == PendingCounts::STARTED) {
      DumpActiveNodeState(*node, iteration->input_tensors);
    }
  }
  // Show all input tensors in use.
  const int total_input_tensors = frame->total_input_tensors;
  size_t total_bytes = 0;
  for (int i = 0; i < total_input_tensors; ++i) {
    const Entry& input = iteration->input_tensors[i];
    const Tensor* tensor = GetTensorValueForDump(input);
    if (tensor->IsInitialized()) {
      LOG(WARNING) << "    Input " << i << ": "
                   << strings::StrCat(
                          "Tensor<type: ", DataTypeString(tensor->dtype()),
                          " shape: ", tensor->shape().DebugString(),
                          ", bytes: ", tensor->TotalBytes(), ">");
      total_bytes += tensor->TotalBytes();
    }
  }
  LOG(WARNING) << "    Total bytes " << total_bytes;
}

void PropagatorState::DumpState() {
  mutex_lock l(mu_);
  LOG(WARNING) << "Dumping state";
  for (auto& frame : outstanding_frames_) {
    LOG(WARNING) << frame.first;
    FrameState* frame_state = frame.second;
    frame_state->DumpIterationState(this);
  }
}

void PropagatorState::FindOrCreateChildFrame(FrameState* frame,
                                             IterationState* iter_state,
                                             const NodeItem& node_item,
                                             FrameState** child) {
  // Get the child frame name.
  AttrSlice attrs(node_item.kernel->def());
  const string& enter_name = GetNodeAttrString(attrs, "frame_name");
  DCHECK(!enter_name.empty()) << "Could not find \"frame_name\" attr in node "
                              << node_item.kernel->name();
  const string child_name = strings::StrCat(
      frame->frame_name, ";", iter_state->iter_num, ";", enter_name);

  {
    mutex_lock executor_lock(mu_);
    auto it = outstanding_frames_.find(child_name);
    if (it != outstanding_frames_.end()) {
      *child = it->second;
      return;
    }
  }

  // Need to create a new frame instance.
  // Note that this new frame instance is created without any locks.
  if (vlog_) VLOG(2) << "Create frame: " << child_name;

  int parallel_iters;
  bool found_parallel_iters =
      TryGetNodeAttr(attrs, "parallel_iterations", &parallel_iters);
  DCHECK(found_parallel_iters)
      << "Could not find \"parallel_iterations\" attr in node "
      << node_item.kernel->name();
  FrameState* temp = new FrameState(immutable_state_, parallel_iters);
  temp->frame_name = child_name;
  temp->frame_id = Hash64(child_name);
  temp->parent_frame = frame;
  temp->parent_iter = iter_state;
  temp->InitializeFrameInfo(enter_name);

  // Initialize iteration 0.
  {
    mutex_lock l(temp->mu);
    temp->SetIteration(0, new IterationState(0, temp->pending_counts,
                                             temp->total_input_tensors));
  }

  {
    mutex_lock executor_lock(mu_);
    auto it = outstanding_frames_.find(child_name);
    if (it != outstanding_frames_.end()) {
      *child = it->second;
    } else {
      mutex_lock frame_lock(frame->mu);
      iter_state->outstanding_frame_count++;
      outstanding_frames_[child_name] = temp;
      *child = temp;
      temp = nullptr;
    }
  }
  delete temp;  // Not used so delete it.
}

void PropagatorState::DeleteFrame(FrameState* frame, TaggedNodeSeq* ready) {
  // First, propagate dead_exits (if any) to the parent frame.
  FrameState* parent_frame = frame->parent_frame;
  IterationState* parent_iter_state = frame->parent_iter;
  if (parent_frame != nullptr) {
    mutex_lock parent_frame_lock(parent_frame->mu);
    // Propagate all the dead exits to the parent frame.
    mutex_lock this_frame_lock(frame->mu);

    for (const NodeItem* item : frame->dead_exits) {
      auto maybe_add_to_ready = [&](const NodeItem& dst_item, bool dst_ready,
                                    bool dst_dead) {
        if (dst_ready) {
          if (dst_item.is_control_trigger) dst_dead = false;
          ready->emplace_back(&dst_item, parent_frame, parent_iter_state,
                              dst_dead);
          parent_iter_state->outstanding_ops++;
        }
      };

      auto propagate_to_non_merge = [&](PendingCounts::Handle dst_pending_id) {
        parent_iter_state->increment_dead_count(dst_pending_id);
        return parent_iter_state->decrement_pending(dst_pending_id, 1) == 0;
      };

      for (const EdgeInfo& e : item->output_edges()) {
        const NodeItem& dst_item =
            immutable_state_.graph_view().node_ref(e.dst_id);
        const auto dst_pending_id = immutable_state_.pending_ids()[e.dst_id];

        bool dst_dead = true;
        bool dst_ready;
        // We know this is a dead input to dst.
        if (dst_item.is_merge) {
          parent_iter_state->increment_dead_count(dst_pending_id);
          const int dead_cnt = parent_iter_state->dead_count(dst_pending_id);
          dst_dead = (dead_cnt == dst_item.num_inputs);
          dst_ready =
              (parent_iter_state->pending(dst_pending_id) == 1) && dst_dead;
        } else {
          dst_ready = propagate_to_non_merge(dst_pending_id);
        }
        maybe_add_to_ready(dst_item, dst_ready, dst_dead);
      }

      for (const ControlEdgeInfo& e : item->output_control_edges()) {
        const NodeItem& dst_item =
            immutable_state_.graph_view().node_ref(e.dst_id);
        const auto dst_pending_id = immutable_state_.pending_ids()[e.dst_id];

        bool dst_dead;
        bool dst_ready;
        // We know this is a dead input to dst.
        if (dst_item.is_merge) {
          parent_iter_state->decrement_pending(dst_pending_id, 2);
          int count = parent_iter_state->pending(dst_pending_id);
          int dead_cnt = parent_iter_state->dead_count(dst_pending_id);
          dst_dead = (dead_cnt == dst_item.num_inputs);
          dst_ready = (count == 0) || ((count == 1) && dst_dead);
        } else {
          dst_dead = true;
          dst_ready = propagate_to_non_merge(dst_pending_id);
        }
        maybe_add_to_ready(dst_item, dst_ready, dst_dead);
      }
    }
  }

  // Delete the frame.
  const string& frame_name = frame->frame_name;
  if (vlog_) VLOG(2) << "Delete frame " << frame_name;
  {
    mutex_lock executor_lock(mu_);
    outstanding_frames_.erase(frame_name);
  }
  delete frame;
}

void PropagatorState::CleanupFramesIterations(FrameState* frame,
                                              IterationState* iter_state,
                                              TaggedNodeSeq* ready) {
  bool is_frame_done = false;
  {
    mutex_lock frame_lock(frame->mu);
    iter_state->outstanding_frame_count--;
    is_frame_done = frame->CleanupIterations(iter_state, ready);
  }
  if (is_frame_done) {
    FrameState* parent_frame = frame->parent_frame;
    IterationState* parent_iter = frame->parent_iter;
    DeleteFrame(frame, ready);
    if (parent_frame != nullptr) {
      // The completion of frame may cause completions in its parent frame.
      // So clean things up recursively.
      CleanupFramesIterations(parent_frame, parent_iter, ready);
    }
  }
}

void PropagatorState::FrameState::ActivateNodesFastPath(
    const NodeItem* item, const bool is_dead, IterationState* iter_state,
    EntryVector* outputs, TaggedNodeSeq* ready) {
  // If we know that none of the item's edge destinations require special
  // handling (i.e. none of the nodes is a merge or control trigger node), we
  // can take a fast path that avoids accessing the destination NodeItem.
  const GraphView& gview = immutable_state.graph_view();

// Add dst to the ready queue if it's ready
//
// NOTE(mrry): Use a macro here instead of a lambda, because this method is
// performance-critical and we need to ensure that the code is inlined.
#define MAYBE_ADD_TO_READY(dst_id, adjust_result)         \
  do {                                                    \
    if (!adjust_result.any_pending) {                     \
      const NodeItem* dst_item = &gview.node_ref(dst_id); \
      TaggedNode& t = ready->emplace_back();              \
      t.node_item = dst_item;                             \
      t.input_frame = this;                               \
      t.input_iter = iter_state;                          \
      t.is_dead = adjust_result.any_dead;                 \
      iter_state->outstanding_ops++;                      \
    }                                                     \
  } while (0);

  Entry* input_tensors = iter_state->input_tensors;

  for (const EdgeInfo& e : item->output_edges()) {
    const int dst_id = e.dst_id;
    const PendingCounts::Handle dst_pending_id =
        immutable_state.pending_ids()[dst_id];
    const int src_slot = e.output_slot;

    const bool increment_dead =
        (is_dead || ((*outputs)[src_slot].state == Entry::State::NO_VALUE));
    const PendingCounts::AdjustResult adjust_result =
        iter_state->adjust_for_activation(dst_pending_id, increment_dead);
    const int dst_loc = e.input_slot;
    if (e.is_last) {
      input_tensors[dst_loc] = std::move((*outputs)[src_slot]);
    } else {
      input_tensors[dst_loc] = (*outputs)[src_slot];
    }
    MAYBE_ADD_TO_READY(dst_id, adjust_result);
  }

  for (const ControlEdgeInfo& e : item->output_control_edges()) {
    const int dst_id = e.dst_id;
    const PendingCounts::Handle dst_pending_id =
        immutable_state.pending_ids()[dst_id];
    const PendingCounts::AdjustResult adjust_result =
        iter_state->adjust_for_activation(dst_pending_id, is_dead);
    MAYBE_ADD_TO_READY(dst_id, adjust_result);
  }
#undef MAYBE_ADD_TO_READY
}

void PropagatorState::FrameState::ActivateNodesSlowPath(
    const NodeItem* item, const bool is_dead, IterationState* iter_state,
    EntryVector* outputs, TaggedNodeSeq* ready) {
  // If any of the edge destinations is a merge or a control trigger node,
  // we need to read each destination NodeItem to determine what action
  // to take.
  const GraphView& gview = immutable_state.graph_view();

  auto maybe_add_to_ready = [&](int dst_id, const NodeItem* dst_item,
                                bool dst_ready, bool dst_dead) {
    // Add dst to the ready queue if it's ready
    if (dst_ready) {
      if (dst_item->is_control_trigger) dst_dead = false;
      ready->emplace_back(dst_item, this, iter_state, dst_dead);
      iter_state->outstanding_ops++;
    }
  };

  Entry* input_tensors = iter_state->input_tensors;

  for (const EdgeInfo& e : item->output_edges()) {
    const int dst_id = e.dst_id;
    const NodeItem* dst_item = &gview.node_ref(dst_id);
    const PendingCounts::Handle dst_pending_id =
        immutable_state.pending_ids()[dst_id];
    const int src_slot = e.output_slot;

    bool dst_dead = false;
    bool dst_ready = false;
    bool dst_need_input = true;

    if (dst_item->is_merge) {
      // A merge node is ready if all control inputs have arrived and either
      // a) a live data input becomes available or b) all data inputs are
      // dead. For Merge, pending's LSB is set iff a live data input has
      // arrived.
      if ((*outputs)[src_slot].state != Entry::State::NO_VALUE) {
        // This is a live data input.
        int count = iter_state->pending(dst_pending_id);
        iter_state->mark_live(dst_pending_id);
        // Only the first live edge sets the input and (potentially)
        // triggers execution. The low bit of count is set if and
        // only if no live input has been used yet (mark_live clears
        // it). The node should be started if and only if this is
        // the first live input and there are no pending control
        // edges, i.e. count == 1.
        dst_ready = (count == 1);
        dst_need_input = ((count & 0x1) == 1);
      } else {
        // This is a dead data input. Note that dst_node is dead if node is
        // a dead enter. We need this to handle properly a while loop on
        // the untaken branch of a conditional.
        // TODO(yuanbyu): This is a bit hacky, but a good solution for
        // now.
        iter_state->increment_dead_count(dst_pending_id);
        const int dead_cnt = iter_state->dead_count(dst_pending_id);
        dst_dead = (dead_cnt == dst_item->num_inputs) || item->is_enter;
        dst_ready = (iter_state->pending(dst_pending_id) == 1) && dst_dead;
        dst_need_input = false;
      }
    } else {
      // Handle all other (non-merge) nodes.
      const bool increment_dead =
          (is_dead || ((*outputs)[src_slot].state == Entry::State::NO_VALUE));
      const PendingCounts::AdjustResult adjust_result =
          iter_state->adjust_for_activation(dst_pending_id, increment_dead);
      dst_dead = adjust_result.any_dead;
      dst_ready = !adjust_result.any_pending;
    }

    if (dst_need_input) {
      const int dst_loc = e.input_slot;
      if (e.is_last) {
        input_tensors[dst_loc] = std::move((*outputs)[src_slot]);
      } else {
        input_tensors[dst_loc] = (*outputs)[src_slot];
      }
    }

    maybe_add_to_ready(dst_id, dst_item, dst_ready, dst_dead);
  }

  for (const ControlEdgeInfo& e : item->output_control_edges()) {
    const int dst_id = e.dst_id;
    const NodeItem* dst_item = &gview.node_ref(dst_id);
    const PendingCounts::Handle dst_pending_id =
        immutable_state.pending_ids()[dst_id];

    bool dst_dead;
    bool dst_ready;
    if (dst_item->is_merge) {
      // A merge node is ready if all control inputs have arrived and either
      // a) a live data input becomes available or b) all data inputs are
      // dead. For Merge, pending's LSB is set iff a live data input has
      // arrived.
      iter_state->decrement_pending(dst_pending_id, 2);
      int count = iter_state->pending(dst_pending_id);
      int dead_cnt = iter_state->dead_count(dst_pending_id);
      dst_dead = (dead_cnt == dst_item->num_inputs);
      dst_ready = (count == 0) || ((count == 1) && dst_dead);
    } else {
      // Handle all other (non-merge) nodes.
      const PendingCounts::AdjustResult adjust_result =
          iter_state->adjust_for_activation(dst_pending_id, is_dead);
      dst_dead = adjust_result.any_dead;
      dst_ready = !adjust_result.any_pending;
    }
    maybe_add_to_ready(dst_id, dst_item, dst_ready, dst_dead);
  }
}

void PropagatorState::FrameState::ActivateNodes(const NodeItem* item,
                                                const bool is_dead,
                                                IterationState* iter_state,
                                                EntryVector* outputs,
                                                TaggedNodeSeq* ready) {
  if (TF_PREDICT_FALSE(item->is_any_consumer_merge_or_control_trigger)) {
    ActivateNodesSlowPath(item, is_dead, iter_state, outputs, ready);
  } else {
    ActivateNodesFastPath(item, is_dead, iter_state, outputs, ready);
  }
}

void PropagatorState::FrameState::ActivateNexts(IterationState* iter_state,
                                                TaggedNodeSeq* ready) {
  // Propagate the deferred NextIteration nodes to the new iteration.
  for (auto& node_entry : next_iter_roots) {
    const NodeItem* item = node_entry.first;
    const Entry& entry = node_entry.second;
    const bool is_dead = entry.state == Entry::State::NO_VALUE;
    EntryVector outputs{entry};
    ActivateNodes(item, is_dead, iter_state, &outputs, ready);
  }
  next_iter_roots.clear();
}

void PropagatorState::FrameState::ActivateLoopInvs(IterationState* iter_state,
                                                   TaggedNodeSeq* ready) {
  // Propagate loop invariants to the new iteration.
  for (auto& node_entry : inv_values) {
    const NodeItem* item = node_entry.first;
    const Entry& entry = node_entry.second;
    const bool is_dead = entry.state == Entry::State::NO_VALUE;
    EntryVector outputs{entry};
    ActivateNodes(item, is_dead, iter_state, &outputs, ready);
  }
}

void PropagatorState::FrameState::AddLoopInv(const NodeItem* item,
                                             const Entry& entry,
                                             TaggedNodeSeq* ready) {
  // Store this value.
  inv_values.push_back({item, entry});

  // Make this value available to all iterations.
  const bool is_dead = entry.state == Entry::State::NO_VALUE;
  for (int i = 0; i <= iteration_count; ++i) {
    EntryVector outputs{entry};
    ActivateNodes(item, is_dead, GetIteration(i), &outputs, ready);
  }
}

bool PropagatorState::FrameState::IsIterationDone(IterationState* iter_state) {
  if (iter_state->outstanding_ops == 0 &&
      iter_state->outstanding_frame_count == 0) {
    if (iter_state->iter_num == 0) {
      // The enclosing frame has no pending input.
      return num_pending_inputs == 0;
    } else {
      // The preceding iteration is deleted (and therefore done).
      return (GetIteration(iter_state->iter_num - 1) == nullptr);
    }
  }
  return false;
}

PropagatorState::IterationState*
PropagatorState::FrameState::IncrementIteration(TaggedNodeSeq* ready) {
  iteration_count++;

  // Initialize the next iteration.
  IterationState* next_iter =
      new IterationState(iteration_count, pending_counts, total_input_tensors);
  SetIteration(iteration_count, next_iter);
  num_outstanding_iterations++;
  dead_exits.clear();

  // Activate the successors of the deferred roots in the new iteration.
  ActivateNexts(next_iter, ready);

  // Activate the loop invariants in the new iteration.
  ActivateLoopInvs(next_iter, ready);

  return next_iter;
}

bool PropagatorState::FrameState::CleanupIterations(IterationState* iter_state,
                                                    TaggedNodeSeq* ready) {
  int64 curr_iter = iter_state->iter_num;
  while (curr_iter <= iteration_count && IsIterationDone(iter_state)) {
    delete iter_state;
    SetIteration(curr_iter, nullptr);
    --num_outstanding_iterations;
    ++curr_iter;

    // When one iteration is completed, we check for deferred iteration,
    // and start it if there is one.
    if (!next_iter_roots.empty()) {
      IncrementIteration(ready);
    }

    if (curr_iter <= iteration_count) {
      iter_state = GetIteration(curr_iter);
    }
  }
  return IsFrameDone();
}

void PropagatorState::FrameState::InitializeFrameInfo(
    const string& enter_name) {
  const ImmutableExecutorState::FrameInfo* finfo =
      immutable_state.get_frame_info(enter_name);
  DCHECK_NE(finfo, nullptr);
  pending_counts = finfo->pending_counts.get();
  total_input_tensors = finfo->total_inputs;
  num_pending_inputs = finfo->input_count;
  nodes = finfo->nodes.get();
}

void PropagatorState::FrameState::SetIteration(int64 iter,
                                               IterationState* state)
    TF_EXCLUSIVE_LOCKS_REQUIRED(mu) {
  size_t index = iter % (max_parallel_iterations + 1);
  DCHECK(state == nullptr || iterations[index] == nullptr);
  iterations_raw[index] = state;
  if (index == 0) {
    iterations_first = state;
  }
}

// Decrement the outstanding op count and clean up the iterations in the
// frame. Return true iff the execution of the frame is done.
bool PropagatorState::FrameState::DecrementOutstandingOps(
    IterationState* iter_state, TaggedNodeSeq* ready) {
  mutex_lock l(mu);
  return DecrementOutstandingOpsLocked(iter_state, ready);
}

// Decrement the outstanding op count and clean up the iterations in the
// frame. Return true iff the execution of the frame is done.
bool PropagatorState::FrameState::DecrementOutstandingOpsLocked(
    IterationState* iter_state, TaggedNodeSeq* ready)
    TF_EXCLUSIVE_LOCKS_REQUIRED(mu) {
  iter_state->outstanding_ops--;
  if (iter_state->outstanding_ops != 0) {
    return false;
  } else {
    return CleanupIterations(iter_state, ready);
  }
}

// Returns true if the computation in the frame is completed.
bool PropagatorState::FrameState::IsFrameDone()
    TF_EXCLUSIVE_LOCKS_REQUIRED(mu) {
  return (num_pending_inputs == 0 && num_outstanding_iterations == 0);
}

}  // namespace tensorflow
