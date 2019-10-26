# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for Collective Operations."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.core.protobuf import config_pb2
from tensorflow.core.protobuf import rewriter_config_pb2
from tensorflow.python.eager import context
from tensorflow.python.eager import def_function
from tensorflow.python.framework import config
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import kernels
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import collective_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.platform import tf_logging as logging


class CollectiveOpTest(test.TestCase):

  def _testCollectiveReduce(self, inputs, expected, set_graph_key,
                            communication_hint='auto', fp16=False,
                            instance_key=1, merge_op='Add', final_op='Div'):
    group_key = 1
    group_size = len(inputs)
    device_type = 'CPU'
    config = config_pb2.ConfigProto(device_count={device_type: group_size})
    devices = ['/{}:{}'.format(device_type, i) for i in range(group_size)]

    with self.session(config=config) as sess:
      colred = []
      for i in range(group_size):
        with ops.device(devices[i]):
          tensor = constant_op.constant(inputs[i], dtype=(
              dtypes.float16 if fp16 else dtypes.float32))
          colred.append(collective_ops.all_reduce(
              tensor, group_size, group_key, instance_key, merge_op, final_op,
              communication_hint=communication_hint))
      run_options = config_pb2.RunOptions()
      if set_graph_key:
        run_options.experimental.collective_graph_key = 1
      results = sess.run(colred, options=run_options)
    tolerance = 1e-3 if fp16 else 1e-5
    for i in range(group_size):
      logging.info('i {} result {} expected {}'.format(i, results[i], expected))
      self.assertAllClose(results[i], expected, rtol=tolerance, atol=tolerance)

  def _testMultipleConcurrentCollectiveReduce(self, t0, t1, expected):
    group_key = 1
    group_size = 2
    num_instances = 2
    all_reduces = []
    config = config_pb2.ConfigProto(device_count={'CPU': group_size})
    config.experimental.collective_deterministic_sequential_execution = True
    with self.session(config=config) as sess:
      for cpu in range(group_size):
        with ops.device('/CPU:%d' % cpu):
          in_tensor = constant_op.constant(t0 if cpu == 0 else t1)
          for instance in range(num_instances):
            all_reduces.append(collective_ops.all_reduce(
                in_tensor, group_size, group_key, instance, 'Add', 'Div'))
      results = sess.run(all_reduces)
    for i in range(group_size * num_instances):
      self.assertAllClose(results[i], expected, rtol=1e-5, atol=1e-5)

  @test_util.run_deprecated_v1
  def testCollectiveReduce(self):
    self._testCollectiveReduce(
        inputs=[[0.1, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1],
                [0.3, 1.3, 2.3, 3.3, 4.3, 5.3, 6.3, 7.3]],
        expected=[0.2, 1.2, 2.2, 3.2, 4.2, 5.2, 6.2, 7.2],
        set_graph_key=True)

  @test_util.run_deprecated_v1
  def testCollectiveAutoGraphKey(self):
    self._testCollectiveReduce(
        inputs=[[0.1, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1],
                [0.3, 1.3, 2.3, 3.3, 4.3, 5.3, 6.3, 7.3]],
        expected=[0.2, 1.2, 2.2, 3.2, 4.2, 5.2, 6.2, 7.2],
        set_graph_key=False)

  @test_util.run_deprecated_v1
  def testFp16Reduce(self):
    self._testCollectiveReduce(
        inputs=[[0.1, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1],
                [0.3, 1.3, 2.3, 3.3, 4.3, 5.3, 6.3, 7.3]],
        expected=[0.2, 1.2, 2.2, 3.2, 4.2, 5.2, 6.2, 7.2],
        set_graph_key=True,
        fp16=True)

  @test_util.run_deprecated_v1
  def testCollectiveMultipleConcurrentReduce(self):
    self._testMultipleConcurrentCollectiveReduce(
        [0.1, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1],
        [0.3, 1.3, 2.3, 3.3, 4.3, 5.3, 6.3, 7.3],
        [0.2, 1.2, 2.2, 3.2, 4.2, 5.2, 6.2, 7.2])

  @test_util.run_deprecated_v1
  def testNcclHintFallbackToRingReduce(self):
    """Tests that setting `communication_hint=nccl` works on non-GPU builds."""
    if kernels.get_registered_kernels_for_op('NcclAllReduce'):
      self.skipTest('Run only on non-GPU environments')
    self._testCollectiveReduce(
        inputs=[[0.1, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1],
                [0.3, 1.3, 2.3, 3.3, 4.3, 5.3, 6.3, 7.3]],
        expected=[0.2, 1.2, 2.2, 3.2, 4.2, 5.2, 6.2, 7.2],
        set_graph_key=False,
        communication_hint='nccl')

  def _testWhile(self, num_vars, num_iterations, key_base):
    group_size = 2
    group_key = 1
    instances = [(key_base + i) for i in range(num_vars)]
    devices = ['CPU:{}'.format(i) for i in range(group_size)]

    config = config_pb2.ConfigProto(device_count={'CPU': group_size})
    rewrite_options = config.graph_options.rewrite_options
    rewrite_options.scoped_allocator_optimization = (
        rewriter_config_pb2.RewriterConfig.ON)
    del rewrite_options.scoped_allocator_opts.enable_op[:]
    rewrite_options.scoped_allocator_opts.enable_op.append('CollectiveReduce')

    with self.session(config=config) as sess:
      loop_vars = []
      for device in devices:
        with ops.device(device):
          loop_vars.append(
              [variables.VariableV1((1 << i) * 1.) for i in range(num_vars)])
      # This variable controls number of iterations.
      loop_vars.append(variables.VariableV1(0.))
      def loop_body(dev0_tensors, dev1_tensors, loop_tensor):
        return_ops = []
        for i in range(len(devices)):
          device = devices[i]
          device_tensors = dev0_tensors if i == 0 else dev1_tensors
          with ops.device(device):
            device_collectives = []
            for j in range(num_vars):
              # NOTE(ayushd): we need the `cast` here to ensure that the input
              # to `all_reduce` has an explicit device string.  We don't use
              # `identity` because `cast` is more resilient to getting optimized
              # away by various optimization passes.
              input_tensor = math_ops.cast(device_tensors[j], dtypes.float16)
              collective_op = collective_ops.all_reduce(
                  input_tensor, group_size, group_key, instances[j],
                  'Add', 'Id')
              output_tensor = math_ops.cast(collective_op, dtypes.float32)
              device_collectives.append(output_tensor)
            return_ops.append(device_collectives)
        return_ops.append(math_ops.add(loop_tensor, 1.))
        return return_ops
      # Run until last variable exceeds number of iterations.
      loop_cond = lambda d0, d1, i: math_ops.less(i, num_iterations)
      sess.run(variables.global_variables_initializer())
      results = sess.run(control_flow_ops.while_loop(loop_cond, loop_body,
                                                     loop_vars))
      self.assertEqual(results[:-1], [
          [((1 << (num_iterations + v)) * 1.) for v in range(num_vars)]
          for _ in range(group_size)])

  @test_util.run_deprecated_v1
  def testSimpleWhile(self):
    self._testWhile(num_vars=1, num_iterations=4, key_base=20)

  @test_util.run_deprecated_v1
  def testWhileMultipleAllReduce(self):
    self._testWhile(num_vars=2, num_iterations=4, key_base=20)

  @test_util.run_deprecated_v1
  def testWhileWithScopedAllocator(self):
    group_size = 2
    group_key = 1
    instance_key0 = 1
    instance_key1 = 2

    config = config_pb2.ConfigProto(device_count={'CPU': group_size})
    rewrite_options = config.graph_options.rewrite_options
    rewrite_options.scoped_allocator_optimization = (
        rewriter_config_pb2.RewriterConfig.ON)
    del rewrite_options.scoped_allocator_opts.enable_op[:]
    rewrite_options.scoped_allocator_opts.enable_op.append('CollectiveReduce')

    with self.session(config=config) as sess:
      run_ops = []
      for i in range(group_size):
        with ops.device('CPU:%d' % i):
          constant = constant_op.constant(0.)
          cond = lambda i: math_ops.less(i, 10.)
          body = lambda i: math_ops.add(i, 1.)
          input0 = control_flow_ops.while_loop(cond, body, [constant])
          input1 = math_ops.add(constant, 5)
          colred0 = collective_ops.all_reduce(input0, group_size, group_key,
                                              instance_key0, 'Add', 'Id')
          colred1 = collective_ops.all_reduce(input1, group_size, group_key,
                                              instance_key1, 'Add', 'Id')
          run_ops.append(math_ops.add_n([colred0, colred1]))
      results = sess.run(run_ops)
      self.assertEqual(results, [30., 30.])

  @test_util.run_deprecated_v1
  def testCollectiveReduceScalar(self):
    self._testCollectiveReduce(inputs=[0.1, 0.3], expected=0.2,
                               set_graph_key=True)

  @test_util.run_deprecated_v1
  def testCollectiveReduceMaximum(self):
    self._testCollectiveReduce(
        inputs=[[1., 20., 3., 40., 5.], [10., 2., 30., 4., 50.]],
        expected=[10., 20., 30., 40., 50.],
        set_graph_key=True,
        instance_key=30,
        merge_op='Max',
        final_op='Id')

  @test_util.run_deprecated_v1
  def testCollectiveReduceMinimum(self):
    self._testCollectiveReduce(
        inputs=[[1., 20., 3., 40., 5.], [10., 2., 30., 4., 50.]],
        expected=[1., 2., 3., 4., 5.],
        set_graph_key=True,
        instance_key=40,
        merge_op='Min',
        final_op='Id')

  def _testCollectiveBroadcast(self, t0):
    group_key = 1
    instance_key = 1
    with self.session(
        config=config_pb2.ConfigProto(device_count={'CPU': 2})) as sess:
      with ops.device('/CPU:0'):
        in0 = constant_op.constant(t0)
        out0 = collective_ops.broadcast_send(in0, in0.shape, in0.dtype,
                                             2, group_key, instance_key)
      with ops.device('/CPU:1'):
        c1 = constant_op.constant(t0)
        out1 = collective_ops.broadcast_recv(c1.shape, c1.dtype,
                                             2, group_key, instance_key)
      run_options = config_pb2.RunOptions()
      run_options.experimental.collective_graph_key = 1
      results = sess.run([out0, out1], options=run_options)
    self.assertAllClose(results[0], t0, rtol=1e-5, atol=1e-5)
    self.assertAllClose(results[1], t0, rtol=1e-5, atol=1e-5)

  @test_util.run_deprecated_v1
  def testCollectiveBroadcast(self):
    self._testCollectiveBroadcast([0.1, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1])

  def _testCollectiveGather(self, t0, t1, expected, set_graph_key):
    group_key = 1
    instance_key = 1
    with self.session(
        config=config_pb2.ConfigProto(device_count={'CPU': 2})) as sess:
      with ops.device('/CPU:0'):
        in0 = constant_op.constant(t0)
        c0 = collective_ops.all_gather(in0, 2, group_key, instance_key)
      with ops.device('/CPU:1'):
        in1 = constant_op.constant(t1)
        c1 = collective_ops.all_gather(in1, 2, group_key, instance_key)
      run_options = config_pb2.RunOptions()
      if set_graph_key:
        run_options.experimental.collective_graph_key = 1
      results = sess.run([c0, c1], options=run_options)
    self.assertAllClose(results[0], expected, rtol=1e-5, atol=1e-5)
    self.assertAllClose(results[1], expected, rtol=1e-5, atol=1e-5)

  @test_util.run_deprecated_v1
  def testCollectiveGather(self):
    self._testCollectiveGather([0, 1, 2, 3, 4, 5, 6, 7],
                               [10, 11, 12, 13, 14, 15, 16, 17],
                               [0, 1, 2, 3, 4, 5, 6, 7,
                                10, 11, 12, 13, 14, 15, 16, 17],
                               True)
    self._testCollectiveGather([[0, 1, 2, 3], [4, 5, 6, 7]],
                               [[10, 11, 12, 13], [14, 15, 16, 17]],
                               [[0, 1, 2, 3], [4, 5, 6, 7],
                                [10, 11, 12, 13], [14, 15, 16, 17]],
                               True)
    self._testCollectiveGather([[[0, 1], [2, 3]], [[4, 5], [6, 7]]],
                               [[[10, 11], [12, 13]], [[14, 15], [16, 17]]],
                               [[[0, 1], [2, 3]], [[4, 5], [6, 7]],
                                [[10, 11], [12, 13]], [[14, 15], [16, 17]]],
                               True)

  @test_util.run_deprecated_v1
  def testCollectiveGatherShapeMismatch(self):
    group_key = 1
    instance_key = 1
    t0 = [1, 2, 3, 4]
    t1 = [5, 6, 7, 8]
    t2 = [9, 10]
    with self.session(
        config=config_pb2.ConfigProto(device_count={'CPU': 2})) as sess:
      with ops.device('/CPU:0'):
        in0 = constant_op.constant(t0)
        c0 = collective_ops.all_gather(in0, 2, group_key, instance_key)
      with ops.device('/CPU:1'):
        in1 = constant_op.constant(t1)
        in2 = constant_op.constant(t2)
        c1 = collective_ops.all_gather(in1, 2, group_key, instance_key)
        c2 = collective_ops.all_gather(in2, 2, group_key, instance_key)
      run_options = config_pb2.RunOptions()
      run_options.experimental.collective_graph_key = 1
      sess.run([c0, c1], options=run_options)
      with self.assertRaisesRegexp(errors.InvalidArgumentError,
                                   'Shape mismatch'):
        sess.run([c0, c2], options=run_options)

  @test_util.run_deprecated_v1
  def testCollectiveGatherShapeMismatchAcrossDevices(self):
    group_key = 1
    instance_key = 1
    t0 = [1, 2, 3, 4]
    t1 = [5, 6]
    with self.session(
        config=config_pb2.ConfigProto(device_count={'CPU': 2})) as sess:
      with ops.device('/CPU:0'):
        in0 = constant_op.constant(t0)
        c0 = collective_ops.all_gather(in0, 2, group_key, instance_key)
      with ops.device('/CPU:1'):
        in1 = constant_op.constant(t1)
        c1 = collective_ops.all_gather(in1, 2, group_key, instance_key)
      run_options = config_pb2.RunOptions()
      run_options.experimental.collective_graph_key = 1
      with self.assertRaisesRegexp(errors.InvalidArgumentError,
                                   'Shape mismatch'):
        sess.run([c0, c1], options=run_options)

  @test_util.run_v2_only
  def testCollectiveGroupSizeMismatch(self):
    cpus = config.list_physical_devices('CPU')
    self.assertEqual(len(cpus), 1)
    config.set_virtual_device_configuration(cpus[0], [
        context.VirtualDeviceConfiguration(),
        context.VirtualDeviceConfiguration()
    ])
    context.ensure_initialized()

    @def_function.function
    def run_all_reduce():
      group_key = 10
      instance_key = 20
      t0 = [1, 2, 3, 4]
      t1 = [5, 6, 7, 8]
      with ops.device('/CPU:0'):
        in0 = constant_op.constant(t0)
        c0 = collective_ops.all_reduce(
            in0, group_size=2, group_key=group_key, instance_key=instance_key,
            merge_op='Add', final_op='Id')
      with ops.device('/CPU:1'):
        in1 = constant_op.constant(t1)
        c1 = collective_ops.all_reduce(
            in1, group_size=3, group_key=group_key, instance_key=instance_key,
            merge_op='Add', final_op='Id')
      return c0, c1

    with self.assertRaisesRegexp(errors.InternalError,
                                 'but that group has size'):
      run_all_reduce()

  @test_util.run_deprecated_v1
  def testCollectiveTensorsHaveNoDeviceSpecified(self):
    group_size = 2
    group_key = 1
    instance_key = 1

    @def_function.function
    def fn(all_args):
      results = []
      # The inputs have no devices set. This is expected to be a trace-time
      # check only.
      self.assertEqual(all_args[0].device, '')
      self.assertEqual(all_args[1].device, '')

      with ops.device('/CPU:0'):
        results.append(
            collective_ops.all_reduce(all_args[0], group_size, group_key,
                                      instance_key, 'Add', 'Div'))
      with ops.device('/CPU:1'):
        results.append(
            collective_ops.all_reduce(all_args[1], group_size, group_key,
                                      instance_key, 'Add', 'Div'))

      return results

    with self.session(config=config_pb2.ConfigProto(
        device_count={'CPU': 2})) as sess:
      with ops.device('/CPU:0'):
        in0 = constant_op.constant(1)
      with ops.device('/CPU:1'):
        in1 = constant_op.constant(3)

      result_op = fn([in0, in1])

      run_options = config_pb2.RunOptions()
      run_options.experimental.collective_graph_key = 1
      result = sess.run(result_op, options=run_options)

      self.assertAllClose(result, [2, 2])

  @test_util.run_v2_only
  def testCollectiveGroupSizeOne(self):
    group_size = 1
    group_key = 100
    instance_key = 100
    in_value = [1, 2, 3, 4]
    in_tensor = constant_op.constant(in_value)

    reduced_tensor = collective_ops.all_reduce(
        in_tensor, group_size, group_key, instance_key, 'Add', 'Id')
    self.assertAllEqual(in_value, reduced_tensor.numpy())

    gathered_tensor = collective_ops.all_gather(
        in_tensor, group_size, group_key, instance_key)
    self.assertAllEqual(in_value, gathered_tensor.numpy())


if __name__ == '__main__':
  test.main()
