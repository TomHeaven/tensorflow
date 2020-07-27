# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for TPUStrategy."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os

from absl.testing import parameterized

from tensorflow.core.protobuf import config_pb2
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.distribute import distribute_lib
from tensorflow.python.distribute import distribution_strategy_context
from tensorflow.python.distribute import reduce_util
from tensorflow.python.distribute import strategy_test_lib
from tensorflow.python.distribute import tpu_strategy as tpu_lib
from tensorflow.python.distribute.cluster_resolver import tpu_cluster_resolver
from tensorflow.python.eager import def_function
from tensorflow.python.eager import function
from tensorflow.python.eager import remote
from tensorflow.python.eager import test
from tensorflow.python.framework import config
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import device as tf_device
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.framework import tensor_spec
from tensorflow.python.module import module
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import embedding_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.ops import variables
from tensorflow.python.ops.ragged import ragged_tensor
from tensorflow.python.platform import flags
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.tpu import device_assignment as device_assignment_lib
from tensorflow.python.tpu import tpu
from tensorflow.python.tpu import tpu_strategy_util
from tensorflow.python.training import checkpoint_management
from tensorflow.python.training import server_lib
from tensorflow.python.training.tracking import util
from tensorflow.python.util import nest


FLAGS = flags.FLAGS
flags.DEFINE_string("tpu", "", "Name of TPU to connect to.")
flags.DEFINE_string("project", None, "Name of GCP project with TPU.")
flags.DEFINE_string("zone", None, "Name of GCP zone with TPU.")


def get_tpu_cluster_resolver():
  resolver = tpu_cluster_resolver.TPUClusterResolver(
      tpu=FLAGS.tpu,
      zone=FLAGS.zone,
      project=FLAGS.project,
  )
  return resolver


def get_tpu_strategy(enable_packed_var=False):
  resolver = get_tpu_cluster_resolver()
  remote.connect_to_cluster(resolver)
  tpu_strategy_util.initialize_tpu_system(resolver)
  strategy = tpu_lib.TPUStrategyV2(resolver)
  strategy._enable_packed_variable_in_eager_mode = enable_packed_var
  return strategy


# TPU tests which don't use TPUStrategy.
class TPUTest(test.TestCase):

  def test_multiple_initialize_system(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    tpu_strategy_util.initialize_tpu_system(resolver)

    with test.mock.patch.object(logging, "warning") as mock_log:
      tpu_strategy_util.initialize_tpu_system(resolver)
      self.assertRegex(str(mock_log.call_args), "already been initialized")

  def test_tpu_tf_function_same_device(self):
    with ops.device("/device:TPU:0"):
      a = variables.Variable(1)

    @function.defun_with_attributes(attributes={"_noinline": True})
    def get_a_plus_one():
      return a + 1

    @def_function.function(
        input_signature=[tensor_spec.TensorSpec([], dtypes.int32)])
    def foo(x):
      with ops.device("/device:TPU:0"):
        b = x + get_a_plus_one()
      return b + 1

    result = foo(a)
    self.assertAllEqual(4, result)

  def test_tpu_return_int32(self):
    with ops.device("/device:TPU:0"):
      a = variables.Variable(0)

    @def_function.function
    def foo():
      return a + 1

    @def_function.function
    def bar():
      with ops.device("/device:TPU:1"):
        return foo()

    with ops.device("/device:CPU:0"):
      result = bar() + 1
      self.assertAllEqual(result, 2)

  def test_on_demand_op_with_dynamic_output(self):
    with ops.device("/device:TPU:0"):
      where_output = array_ops.where([True, False, True])
    self.assertAllEqual(where_output, [[0], [2]])

    with ops.device("/device:TPU:0"):
      repeat_output = array_ops.repeat(math_ops.range(2), [1, 4])
    self.assertAllEqual(repeat_output, [0, 1, 1, 1, 1])


@parameterized.named_parameters([("PackedVar", True), ("", False)])
class TPUStrategyTest(test.TestCase, parameterized.TestCase):

  def test_function_compile_with_xla(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    with strategy.scope():
      v = variables.Variable(1.0)

    @def_function.function
    def func():
      return v.read_value() + 1.0

    with ops.device("/device:TPU:0"):
      self.assertAllEqual(func(), 2.0)

  def test_sequential_experimental_runs(self, enable_packed_var):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    # Computation replicated to all cores.
    device_assignment = device_assignment_lib.DeviceAssignment.build(
        topology, num_replicas=2)
    strategy = tpu_lib.TPUStrategyV2(
        resolver, experimental_device_assignment=device_assignment)
    strategy._enable_packed_variable_in_eager_mode = enable_packed_var

    # Computation on the 1st core.
    device_assignment2 = device_assignment_lib.DeviceAssignment.build(
        topology, num_replicas=1)
    strategy2 = tpu_lib.TPUStrategyV2(
        resolver, experimental_device_assignment=device_assignment2)

    def computation(x):
      return math_ops.square(x)

    @def_function.function
    def train_step():
      outputs = strategy.experimental_local_results(
          strategy.run(computation, args=([2., 2.],)))
      outputs2 = strategy2.run(
          computation, args=([outputs[0]],))
      return outputs2

    self.assertAllEqual([[16., 16.]], train_step())

  def test_device_switch_case(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    with strategy.scope():
      a = variables.Variable(1)

    inference_iteration = variables.Variable(-1)

    def inference_fn(x, i):
      return a + x + i

    @def_function.function
    def run_inference(x):

      def do_inference(device, inference_fn, i):
        with ops.device(device):
          return inference_fn(x, i)

      branch_fns = {
          0: (lambda: do_inference("/device:TPU:0", inference_fn, 0)),
          1: (lambda: do_inference("/device:TPU:1", inference_fn, 1)),
      }
      branch_index = inference_iteration.assign_add(1, use_locking=True) % 2
      return control_flow_ops.switch_case(branch_index, branch_fns)

    self.assertAllEqual(2., run_inference(1))  # Use TPU core 0.
    self.assertAllEqual(3., run_inference(1))  # Use TPU core 1.

  def test_recover_from_compilation_failures(self, enable_packed_var):
    # TODO(b/148150981): Stop skipping this test once recovery works
    # for non-local TPU.
    if FLAGS.tpu:
      self.skipTest("Recovery fails for non-local TPU, see b/148150981")

    # Disable automatic outside compilation.
    config.set_soft_device_placement(False)
    strategy = get_tpu_strategy(enable_packed_var)

    @def_function.function
    def compilation_failure_run():

      def computation():
        return random_ops.random_gamma([10], [0.5, 1.5])

      return strategy.run(computation)

    with self.assertRaisesRegex(errors.InvalidArgumentError,
                                "TPU compilation failed"):
      compilation_failure_run()

    @def_function.function
    def good_run():

      def computation():
        return random_ops.random_normal([10])

      return strategy.run(computation)

    good_run()

  def test_dynamic_shape_with_outside_compilation_failure(
      self, enable_packed_var):
    # Enable automatic outside compilation.
    config.set_soft_device_placement(True)
    strategy = get_tpu_strategy(enable_packed_var)
    dataset = dataset_ops.Dataset.from_tensors(("string", 1.0)).repeat().batch(
        2, drop_remainder=False)
    dataset = strategy.experimental_distribute_dataset(dataset)
    iterator = iter(dataset)

    @def_function.function
    def train_fn(iterator):

      def step_fn(inputs):
        _, inputs = inputs
        return math_ops.reduce_sum(inputs)

      return strategy.experimental_local_results(
          strategy.run(step_fn, args=(next(iterator),)))

    with self.assertRaisesRegex(errors.InternalError, "Compilation failure"):
      logging.info(train_fn(iterator))

  def test_computation_on_subset_cores(self, enable_packed_var):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    all_core_strategy = tpu_lib.TPUStrategyV2(resolver)
    all_core_strategy._enable_packed_variable_in_eager_mode = enable_packed_var

    with all_core_strategy.scope():
      v = variables.Variable(0.0,
                             aggregation=variables.VariableAggregation.MEAN)

    # Computation on the 1st core.
    device_assignment = device_assignment_lib.DeviceAssignment.build(
        topology, num_replicas=1)
    first_core_strategy = tpu_lib.TPUStrategyV2(
        resolver, experimental_device_assignment=device_assignment)
    first_core_strategy._enable_packed_variable_in_eager_mode = (
        enable_packed_var)

    # Computation on the 2nd core.
    device_assignment2 = device_assignment_lib.DeviceAssignment(
        topology, [[[0, 0, 0, 1]]])
    second_core_strategy = tpu_lib.TPUStrategyV2(
        resolver, experimental_device_assignment=device_assignment2)
    second_core_strategy._enable_packed_variable_in_eager_mode = (
        enable_packed_var)

    @def_function.function
    def train_step():

      def step_fn():
        return v + 1.0

      all_core_strategy.run(step_fn)
      r1 = first_core_strategy.run(step_fn)
      r2 = second_core_strategy.run(step_fn)
      return r1 + r2

    train_step()
    self.assertAllEqual(2., train_step())

  def test_worker_devices_on_subset_cores(self, enable_packed_var):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)

    # Strategy for the 1st core.
    device_assignment = device_assignment_lib.DeviceAssignment.build(
        topology, num_replicas=1)
    first_core_strategy = tpu_lib.TPUStrategyV2(
        resolver, experimental_device_assignment=device_assignment)
    first_core_strategy._enable_packed_variable_in_eager_mode = (
        enable_packed_var)

    # Strategy for the 2nd core.
    device_assignment2 = device_assignment_lib.DeviceAssignment(
        topology, [[[0, 0, 0, 1]]])
    second_core_strategy = tpu_lib.TPUStrategyV2(
        resolver, experimental_device_assignment=device_assignment2)
    second_core_strategy._enable_packed_variable_in_eager_mode = (
        enable_packed_var)

    self.assertLen(first_core_strategy.extended.worker_devices, 1)
    self.assertEndsWith(first_core_strategy.extended.worker_devices[0],
                        "device:TPU:0")

    self.assertLen(second_core_strategy.extended.worker_devices, 1)
    self.assertEndsWith(second_core_strategy.extended.worker_devices[0],
                        "device:TPU:1")

  def test_control_output_in_while_body_fn(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)

    with strategy.scope():
      v = variables.Variable(
          0.0, aggregation=variables.VariableAggregation.MEAN)

    @def_function.function
    def train_step():

      def step_fn():
        v.assign_add(1)

      for _ in math_ops.range(2):
        strategy.run(step_fn)

    train_step()
    self.assertEqual(2.0, v.numpy())

  def test_cluster_in_graph_and_while_body_fn(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)

    @def_function.function
    def train_step():

      def step_fn(prev):
        s = prev + 1
        return s

      def init_fn():
        return array_ops.zeros(shape=())

      prev = strategy.run(init_fn)
      for _ in math_ops.range(10):
        prev = strategy.run(step_fn, args=(prev,))
      return strategy.reduce(reduce_util.ReduceOp.SUM, prev, axis=None)

    sum_val = train_step().numpy().astype(float)
    self.assertEqual(sum_val, strategy.num_replicas_in_sync * 10)

  def test_two_clusters_with_same_fn(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)

    @def_function.function
    def foo(x):
      return strategy.run(lambda x: x + 1, (x,))

    @def_function.function
    def bar(x):
      foo(x)
      return foo(x)

    bar(1)

  def test_using_external_variable_inside_tf_function(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    dataset = dataset_ops.Dataset.range(
        strategy.num_replicas_in_sync * 2,
        output_type=dtypes.float32).batch(strategy.num_replicas_in_sync)
    input_iterator = iter(strategy.experimental_distribute_dataset(dataset))

    v = variables.Variable(2.0)

    @def_function.function
    def train_step(data):
      def computation(inputs):
        return inputs + v
      return strategy.run(computation, args=(data,))

    expected_result = [[x + 2.] for x in range(0, strategy.num_replicas_in_sync)
                      ]
    self.assertAllEqual(
        expected_result,
        strategy.experimental_local_results(train_step(next(input_iterator))))

  # TODO(b/145574622): Remove this test once it is re-enabled in values_test.py.
  def test_all_reduce_on_sync_on_read_variable(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    dataset = dataset_ops.Dataset.range(
        strategy.num_replicas_in_sync, output_type=dtypes.float32).batch(
            strategy.num_replicas_in_sync, drop_remainder=True)
    input_iterator = iter(strategy.experimental_distribute_dataset(dataset))

    with strategy.scope():
      w = variables.Variable(
          (0.,),
          shape=(1,),
          trainable=False,
          synchronization=variables.VariableSynchronization.ON_READ,
          aggregation=variables.VariableAggregation.ONLY_FIRST_REPLICA)

    @def_function.function
    def run(iterator):

      def computation(x):
        w.assign(x + w)
        return w

      def all_reduce(x):
        ctx = distribution_strategy_context.get_replica_context()
        return ctx.all_reduce("SUM", w) + x

      outputs = strategy.run(computation, args=(next(iterator),))
      outputs2 = strategy.experimental_local_results(
          strategy.run(all_reduce, args=(outputs,)))
      return outputs2

    data = range(0, strategy.num_replicas_in_sync)
    data_sum = sum(data)
    expected_result = [
        [x + data_sum] for x in range(0, strategy.num_replicas_in_sync)
    ]
    self.assertAllEqual(expected_result, run(input_iterator))
    self.assertAllEqual((0.,), w.read_value())

  # TODO(b/140633529): Re-enable the test.
  def disable_test_experimental_run_output_on_device(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)

    def computation(x):
      return math_ops.square(x)

    @def_function.function
    def train_step():
      outputs = strategy.experimental_local_results(
          strategy.run(computation, args=(2,)))
      return outputs

    results = train_step()
    self.assertAllEqual([4., 4.], results)
    self.assertAllEqual("/job:localhost/replica:0/task:0/device:TPU:0",
                        results[0].backing_device)
    self.assertAllEqual("/job:localhost/replica:0/task:0/device:TPU:1",
                        results[1].backing_device)

  def test_composite_input_output(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    if strategy.num_replicas_in_sync != 2:
      self.skipTest("Test assumes two replicas.")

    with strategy.scope():
      table = variables.Variable(
          initial_value=[[0.0, 1.0], [3.0, 7.0]], dtype=dtypes.float32)

    @def_function.function
    def sparse_lookup(iterator):

      def tpu_function(sparse):
        # Assumes dense_shape is (2, *)
        looked_up = array_ops.gather(table, sparse.values)
        segment_sum = math_ops.unsorted_segment_sum(
            looked_up, sparse.indices[:, 0], 2)
        return sparse, segment_sum

      return nest.map_structure(
          strategy.experimental_local_results,
          strategy.run(tpu_function, args=(next(iterator),)))

    def dataset_fn(_):
      dataset = dataset_ops.Dataset.range(2)

      def make_sparse(_):
        return sparse_tensor.SparseTensor(
            indices=array_ops.constant([[0, 0], [1, 0], [1, 1]],
                                       dtype=dtypes.int64),
            values=array_ops.constant([0, 0, 1], dtype=dtypes.int32),
            dense_shape=array_ops.constant([2, 2], dtype=dtypes.int64))

      return dataset.map(make_sparse)

    dataset = iter(
        strategy.experimental_distribute_datasets_from_function(
            dataset_fn,
            distribute_lib.InputOptions(
                experimental_prefetch_to_device=False)))

    sparse, result = sparse_lookup(dataset)

    # All replicas return identical reults.
    for replica in range(strategy.num_replicas_in_sync):
      self.assertIsInstance(sparse[replica], sparse_tensor.SparseTensor)
      self.assertAllEqual(sparse[replica].indices, [[0, 0], [1, 0], [1, 1]])
      self.assertAllEqual(sparse[replica].values, [0, 0, 1])
      self.assertAllEqual(sparse[replica].dense_shape, [2, 2])
      self.assertAllEqual(result[replica], [[0.0, 1.0], [3.0, 8.0]])

  def test_composite_input_non_flat_output(self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    if strategy.num_replicas_in_sync != 2:
      self.skipTest("Test assumes two replicas.")

    with strategy.scope():
      table = variables.Variable(
          initial_value=[[0.0, 1.0], [3.0, 7.0]], dtype=dtypes.float32)

    @def_function.function
    def sparse_lookup(iterator):

      def tpu_function(sparse):
        # Assumes dense_shape is (2, *)
        looked_up = array_ops.gather(table, sparse.values)
        segment_sum = math_ops.unsorted_segment_sum(
            looked_up, sparse.indices[:, 0], 2)
        return {"sparse": sparse, "segment_sum": segment_sum}

      return nest.map_structure(
          strategy.experimental_local_results,
          strategy.run(tpu_function, args=(next(iterator),)))

    def dataset_fn(_):
      dataset = dataset_ops.Dataset.range(2)

      def make_sparse(_):
        return sparse_tensor.SparseTensor(
            indices=array_ops.constant([[0, 0], [1, 0], [1, 1]],
                                       dtype=dtypes.int64),
            values=array_ops.constant([0, 0, 1], dtype=dtypes.int32),
            dense_shape=array_ops.constant([2, 2], dtype=dtypes.int64))

      return dataset.map(make_sparse)

    dataset = iter(
        strategy.experimental_distribute_datasets_from_function(
            dataset_fn,
            distribute_lib.InputOptions(
                experimental_prefetch_to_device=False)))

    output = sparse_lookup(dataset)

    # All replicas return identical reults.
    for replica in range(strategy.num_replicas_in_sync):
      self.assertIsInstance(output["sparse"][replica],
                            sparse_tensor.SparseTensor)
      self.assertAllEqual(output["sparse"][replica].indices,
                          [[0, 0], [1, 0], [1, 1]])
      self.assertAllEqual(output["sparse"][replica].values, [0, 0, 1])
      self.assertAllEqual(output["sparse"][replica].dense_shape, [2, 2])
      self.assertAllEqual(output["segment_sum"][replica],
                          [[0.0, 1.0], [3.0, 8.0]])

  def test_composite_input_dynamic_shapes_outside_compilation(
      self, enable_packed_var):
    strategy = get_tpu_strategy(enable_packed_var)
    if strategy.num_replicas_in_sync != 2:
      self.skipTest("Test assumes two replicas.")

    table = variables.Variable(
        initial_value=[[0.0, 1.0], [3.0, 7.0]], dtype=dtypes.float32)

    @def_function.function
    def sparse_lookup(iterator):

      def tpu_function(sparse):
        lookup = tpu.outside_compilation(
            embedding_ops.safe_embedding_lookup_sparse, table, sparse)
        return math_ops.reduce_sum(lookup, axis=0)

      return strategy.experimental_local_results(
          strategy.run(tpu_function, args=(next(iterator),)))

    def dataset_fn(_):
      dataset = dataset_ops.Dataset.range(2)

      def make_sparse(i):
        indices = array_ops.constant([[0, 0], [1, 0], [1, 1]],
                                     dtype=dtypes.int64)[0:2 + i]
        values = array_ops.constant([0, 0, 1], dtype=dtypes.int32)[0:2 + i]
        shape = [
            array_ops.constant([2], dtype=dtypes.int64),
            array_ops.expand_dims(1 + i, axis=0)
        ]
        dense_shape = array_ops.concat(shape, axis=0)
        return sparse_tensor.SparseTensor(
            indices=indices, values=values, dense_shape=dense_shape)

      return dataset.map(make_sparse)

    dataset = iter(
        strategy.experimental_distribute_datasets_from_function(
            dataset_fn,
            options=distribute_lib.InputOptions(
                experimental_prefetch_to_device=False)))

    result = sparse_lookup(dataset)
    self.assertAllEqual(result, [[0.0, 2.0], [1.5, 5.0]])

  def test_per_device_tracing_of_mirrored_variables(self, enable_packed_var):
    # Define trace_count as a list to avoid python scoping error
    trace_count = [0]

    strategy = get_tpu_strategy(enable_packed_var)
    with strategy.scope():
      variable = variables.Variable(0.0)

    @def_function.function
    def add_one():
      trace_count[0] = trace_count[0] + 1
      return math_ops.add(variable, constant_op.constant(1.0))

    @def_function.function
    def update_variable():
      for device in set(strategy.extended.worker_devices):
        with ops.device(device):
          add_one()

    with strategy.scope():
      update_variable.get_concrete_function()
      self.assertLen(strategy.extended.worker_devices, trace_count[0])


class TPUStrategyDataPrefetchTest(test.TestCase):

  def test_prefetch_to_device_default(self):
    strategy = get_tpu_strategy()
    dataset = dataset_ops.Dataset.range(
        strategy.num_replicas_in_sync * 2,
        output_type=dtypes.float32).batch(strategy.num_replicas_in_sync)

    # Check default, should prefetch to TPU.
    dataset_item = next(iter(strategy.experimental_distribute_dataset(dataset)))
    dataset_location = tf_device.DeviceSpec.from_string(
        dataset_item.values[0].device)
    self.assertEqual(dataset_location.device_type, "TPU")

  def test_prefetch_to_device_tpu(self):
    strategy = get_tpu_strategy()
    dataset = dataset_ops.Dataset.range(
        strategy.num_replicas_in_sync * 2,
        output_type=dtypes.float32).batch(strategy.num_replicas_in_sync)

    input_options = distribute_lib.InputOptions(
        experimental_prefetch_to_device=True)
    dataset_item = next(iter(strategy.experimental_distribute_dataset(
        dataset, options=input_options)))
    dataset_location = tf_device.DeviceSpec.from_string(
        dataset_item.values[0].device)
    self.assertEqual(dataset_location.device_type, "TPU")

  def test_prefetch_to_device_cpu(self):
    strategy = get_tpu_strategy()
    dataset = dataset_ops.Dataset.range(
        strategy.num_replicas_in_sync * 2,
        output_type=dtypes.float32).batch(strategy.num_replicas_in_sync)

    # Should be CPU when prefetch_to_device is False.
    input_options = distribute_lib.InputOptions(
        experimental_prefetch_to_device=False)
    dataset_item = next(iter(strategy.experimental_distribute_dataset(
        dataset, options=input_options)))
    dataset_location = tf_device.DeviceSpec.from_string(
        dataset_item.values[0].device)
    self.assertEqual(dataset_location.device_type, "CPU")

  def test_prefetch_to_device_sparse_dataset(self):
    strategy = get_tpu_strategy()
    # Values here aren't important.
    dataset = dataset_ops.Dataset.from_tensors(
        sparse_tensor.SparseTensor(indices=[[0, 0], [0, 1], [1, 0]],
                                   values=[1, 2, 3],
                                   dense_shape=[2, 2]))
    dataset = dataset.repeat()
    dataset = dataset.batch(strategy.num_replicas_in_sync)

    with self.assertRaisesRegex(ValueError, "TPUStrategy does not support"):
      iter(strategy.experimental_distribute_dataset(dataset))

  def test_prefetch_to_device_ragged_dataset(self):
    strategy = get_tpu_strategy()
    # Values here aren't important.
    dataset = dataset_ops.Dataset.from_tensors(
        ragged_tensor.RaggedTensor.from_row_splits(
            values=[1, 2, 3],
            row_splits=[0, 2, 3]))
    dataset = dataset.repeat()
    dataset = dataset.batch(strategy.num_replicas_in_sync)

    with self.assertRaisesRegex(ValueError, "TPUStrategy does not support"):
      iter(strategy.experimental_distribute_dataset(dataset))

  def test_prefetch_to_device_sparse_dataset_fn(self):
    strategy = get_tpu_strategy()
    def dataset_fn(ctx):
      del ctx
      # Values here aren't important.
      dataset = dataset_ops.Dataset.from_tensors(
          sparse_tensor.SparseTensor(indices=[[0, 0], [0, 1], [1, 0]],
                                     values=[1, 2, 3],
                                     dense_shape=[2, 2]))
      dataset = dataset.repeat()
      return dataset.batch(strategy.num_replicas_in_sync)

    with self.assertRaisesRegex(ValueError, "TPUStrategy does not support"):
      iter(strategy.experimental_distribute_datasets_from_function(dataset_fn))

  def test_prefetch_to_device_ragged_dataset_fn(self):
    strategy = get_tpu_strategy()
    def dataset_fn(ctx):
      del ctx
      # Values here aren't important.
      dataset = dataset_ops.Dataset.from_tensors(
          ragged_tensor.RaggedTensor.from_row_splits(
              values=[1, 2, 3],
              row_splits=[0, 2, 3]))
      dataset = dataset.repeat()
      return dataset.batch(strategy.num_replicas_in_sync)

    with self.assertRaisesRegex(ValueError, "TPUStrategy does not support"):
      iter(strategy.experimental_distribute_datasets_from_function(dataset_fn))


class TPUStrategyDistributionTest(
    strategy_test_lib.DistributionTestBase,
    strategy_test_lib.TwoDeviceDistributionTestBase):

  def test_update_config_proto(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    tpu_strategy_util.initialize_tpu_system(resolver)
    strategy = tpu_lib.TPUStrategyV2(resolver)

    config_proto = config_pb2.ConfigProto()
    cluster_spec = server_lib.ClusterSpec({"worker": ["fake1", "fake2"]})
    with test.mock.patch.object(
        resolver, "cluster_spec", return_value=cluster_spec):
      new_config = strategy.update_config_proto(config_proto)

    # Verify cluster_def.
    self.assertProtoEquals(cluster_spec.as_cluster_def(),
                           new_config.cluster_def)

    # Verify isolate_session_state
    self.assertTrue(new_config.isolate_session_state)

  # TODO(b/158110684): enable this test.
  def disable_test_numpy_dataset(self):
    strategy = get_tpu_strategy()
    self._test_numpy_dataset(strategy)

  def test_make_input_fn_iterable(self):
    dataset_fn = lambda: dataset_ops.Dataset.range(10)
    expected_values = [[i, i+1] for i in range(0, 10, 2)]
    distribution = get_tpu_strategy()
    input_fn = self._input_fn_to_test_input_context(
        dataset_fn,
        expected_num_replicas_in_sync=2,
        expected_num_input_pipelines=1,
        expected_input_pipeline_id=0)
    self._test_input_fn_iterable(distribution, input_fn, expected_values)

  def test_make_input_fn_iterator(self):
    dataset_fn = lambda: dataset_ops.Dataset.range(10)
    expected_values = [[i, i+1] for i in range(0, 10, 2)]
    distribution = get_tpu_strategy()
    input_fn = self._input_fn_to_test_input_context(
        dataset_fn,
        expected_num_replicas_in_sync=2,
        expected_num_input_pipelines=1,
        expected_input_pipeline_id=0)
    iterator = distribution.make_input_fn_iterator(input_fn)
    self._test_input_fn_iterator(
        iterator,
        distribution.extended.worker_devices,
        expected_values)

  def test_run(self):
    strategy = get_tpu_strategy()
    self._test_run(strategy, run_in_function=True)

  def test_all_reduce_sum(self):
    strategy = get_tpu_strategy()
    self._test_all_reduce_sum(strategy, run_in_function=True)

  def test_all_reduce_sum_gradients(self):
    strategy = get_tpu_strategy()
    self._test_all_reduce_sum_gradients(strategy, run_in_function=True)

  def test_all_reduce_sum_gradient_tape(self):
    strategy = get_tpu_strategy()
    self._test_all_reduce_sum_gradient_tape(strategy, run_in_function=True)

  def test_all_reduce_mean(self):
    strategy = get_tpu_strategy()
    self._test_all_reduce_mean(strategy, run_in_function=True)

  def test_all_reduce_mean_gradients(self):
    strategy = get_tpu_strategy()
    self._test_all_reduce_mean_gradients(strategy, run_in_function=True)

  def test_all_reduce_mean_gradient_tape(self):
    strategy = get_tpu_strategy()
    self._test_all_reduce_mean_gradient_tape(strategy, run_in_function=True)

  def test_reduce(self):
    strategy = get_tpu_strategy()

    inputs = strategy.make_input_fn_iterator(
        lambda _: dataset_ops.Dataset.from_tensor_slices([2., 3.]))

    self.evaluate(inputs.initialize())
    per_replica_outputs = strategy.run(
        def_function.function(math_ops.square), args=(next(inputs),))

    with strategy.scope():
      mean = strategy.reduce(reduce_util.ReduceOp.MEAN, per_replica_outputs,
                             axis=None)
      self.assertEqual(6.5, self.evaluate(mean))

  def test_constraint(self):
    strategy = get_tpu_strategy()

    with strategy.scope():
      variable = variables.Variable(initial_value=2.,
                                    constraint=lambda x: 0. * x + 1.)
    self.assertEqual(variable.value().numpy(), 2)

    @def_function.function
    def update_variable():
      variable.assign_add(1)
      variable.assign(variable.constraint(variable))

    update_variable()
    self.assertEqual(variable.value().numpy(), 1)

  def test_trainable_variables(self):
    strategy = get_tpu_strategy()
    self._test_trainable_variable(strategy)

  def test_model_parallelism(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    device_assignment = device_assignment_lib.DeviceAssignment(
        topology, core_assignment=[[[0, 0, 0, 0], [0, 0, 0, 1]]])
    strategy = tpu_lib.TPUStrategyV2(
        resolver,
        experimental_device_assignment=device_assignment)

    with strategy.scope():
      v = variables.Variable(2.)
      with strategy.extended.experimental_logical_device(1):
        w = variables.Variable(3.)

    self.assertLen(strategy.experimental_local_results(v), 1)
    self.assertLen(strategy.experimental_local_results(w), 1)
    self.assertEqual("/job:localhost/replica:0/task:0/device:TPU:0",
                     strategy.experimental_local_results(v)[0].device)
    self.assertEqual("/job:localhost/replica:0/task:0/device:TPU:1",
                     strategy.experimental_local_results(w)[0].device)

    logical_devices = []
    @def_function.function
    def f(x):
      replica_ctx = distribution_strategy_context.get_replica_context()
      with replica_ctx.experimental_logical_device(0):
        y = v * x
      with replica_ctx.experimental_logical_device(1):
        z = w * y
      logical_devices.append((y.device, z.device))
      return z

    result = strategy.run(f, args=(5.,))

    self.assertEqual(
        [("/device:TPU_REPLICATED_CORE:0", "/device:TPU_REPLICATED_CORE:1")],
        logical_devices)

    with self.cached_session():
      self.evaluate(variables.global_variables_initializer())
      self.assertEqual(30., self.evaluate(result))

  def test_model_parallelism_checkpointing(self):

    class PartitionedModel(module.Module):

      def __init__(self, v, w):
        super(PartitionedModel, self).__init__()

        assert distribution_strategy_context.has_strategy()
        strategy = distribution_strategy_context.get_strategy()

        with strategy.extended.experimental_logical_device(0):
          self.v = variables.Variable(v)
        with strategy.extended.experimental_logical_device(1):
          self.w = variables.Variable(w)

      def __call__(self, x):
        replica_ctx = distribution_strategy_context.get_replica_context()
        with replica_ctx.experimental_logical_device(0):
          y = self.v * x
        with replica_ctx.experimental_logical_device(1):
          z = self.w * y
        return z

      def change_weights_op(self, v_new, w_new):
        return control_flow_ops.group([self.v.assign(v_new),
                                       self.w.assign(w_new)])

    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    device_assignment = device_assignment_lib.DeviceAssignment(
        topology, core_assignment=[[[0, 0, 0, 0], [0, 0, 0, 1]]])
    strategy = tpu_lib.TPUStrategyV2(
        resolver,
        experimental_device_assignment=device_assignment)

    with strategy.scope():
      model = PartitionedModel(2., 3.)

    checkpoint_dir = self.get_temp_dir()
    checkpoint_prefix = os.path.join(checkpoint_dir, "ckpt")
    checkpoint = util.Checkpoint(model=model)

    with self.cached_session() as sess:
      self.evaluate(variables.global_variables_initializer())
      checkpoint.save(file_prefix=checkpoint_prefix)

      self.evaluate(model.change_weights_op(1., 4.))
      result = strategy.run(def_function.function(model), args=(5.0,))
      self.assertEqual(20., self.evaluate(result))

      status = checkpoint.restore(
          checkpoint_management.latest_checkpoint(checkpoint_dir))
      status.run_restore_ops(sess)  # must run restore op in non-eager mode.
      status.assert_consumed()
      status.assert_existing_objects_matched()
      result = strategy.run(def_function.function(model), args=(5.0,))
      self.assertEqual(30., self.evaluate(result))


class DeviceAssignmentTest(test.TestCase):

  def test_core_assignment(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    device_assignment = device_assignment_lib.DeviceAssignment(
        topology, core_assignment=[[[0, 0, 0, 0]]])
    self.assertAllEqual([[[0, 0, 0, 0]]], device_assignment.core_assignment)
    self.assertEqual(1, device_assignment.num_cores_per_replica)
    self.assertEqual(1, device_assignment.num_replicas)
    self.assertEqual("/task:0/device:TPU:0", device_assignment.tpu_device())
    self.assertEqual("/task:0/device:CPU:0", device_assignment.host_device())

  def test_device_assignment_strategy_properties(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    device_assignment = device_assignment_lib.DeviceAssignment(
        topology, core_assignment=[[[0, 0, 0, 0]]])
    strategy = tpu_lib.TPUStrategyV2(
        resolver,
        experimental_device_assignment=device_assignment)
    self.assertEqual(strategy.extended.num_hosts, 1)
    self.assertEqual(strategy.num_replicas_in_sync, 1)
    self.assertEqual(strategy.extended.num_replicas_per_host, 1)  # pylint: disable=protected-access

  def test_device_assignment_constants(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)
    device_assignment = device_assignment_lib.DeviceAssignment(
        topology,
        core_assignment=device_assignment_lib.SINGLE_CORE_ASSIGNMENT)
    self.assertAllEqual([[[0, 0, 0, 0]]], device_assignment.core_assignment)
    self.assertEqual(1, device_assignment.num_cores_per_replica)
    self.assertEqual(1, device_assignment.num_replicas)
    self.assertEqual("/task:0/device:TPU:0", device_assignment.tpu_device())
    self.assertEqual("/task:0/device:CPU:0", device_assignment.host_device())

  def test_variables_mismatched_device_assignment(self):
    resolver = get_tpu_cluster_resolver()
    remote.connect_to_cluster(resolver)
    topology = tpu_strategy_util.initialize_tpu_system(resolver)

    strategy0 = tpu_lib.TPUStrategyV2(resolver)
    self.assertEqual(
        ("/job:localhost/replica:0/task:0/device:TPU:0",
         "/job:localhost/replica:0/task:0/device:TPU:1"),
        strategy0.extended.worker_devices)

    with strategy0.scope():
      v = variables.Variable(1.)

    v1_assign_op = strategy0.experimental_local_results(v)[1].assign(42.)

    with self.cached_session():
      self.evaluate(variables.global_variables_initializer())
      self.evaluate(v1_assign_op)
      self.assertAllEqual([1., 42.],
                          self.evaluate(
                              strategy0.experimental_local_results(v)))

    # Second strategy has devices reversed relative to the first.
    device_assignment = device_assignment_lib.DeviceAssignment(
        topology, core_assignment=[[[0, 0, 0, 1]], [[0, 0, 0, 0]]])
    strategy1 = tpu_lib.TPUStrategyV2(
        resolver,
        experimental_device_assignment=device_assignment)
    self.assertEqual(
        ("/job:localhost/replica:0/task:0/device:TPU:1",
         "/job:localhost/replica:0/task:0/device:TPU:0"),
        strategy1.extended.worker_devices)

    v_read = strategy1.run(def_function.function(v.read_value))

    with self.cached_session():
      self.assertAllEqual([42., 1.],
                          self.evaluate(
                              strategy0.experimental_local_results(v_read)))


if __name__ == "__main__":
  test.main()
