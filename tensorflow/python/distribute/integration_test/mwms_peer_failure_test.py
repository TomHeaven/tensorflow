# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""This file contains tests that simulate peer failures.

When a peer fails during MultiWorkerMirroredStrategy training. All workers
should get Unavailable error.
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os

import tensorflow as tf

from tensorflow.python.distribute import combinations
from tensorflow.python.distribute import multi_process_runner
from tensorflow.python.distribute import multi_worker_test_base
from tensorflow.python.eager import test


def get_attempt(strategy, attempts):
  task_type = strategy.cluster_resolver.task_type
  task_id = strategy.cluster_resolver.task_id
  attempts[(task_type, task_id)] = attempts.get((task_type, task_id), 0) + 1
  return task_id, attempts[(task_type, task_id)]


quick_exit = os._exit  # pylint: disable=protected-access


class PeerFailureTest(test.TestCase):
  # Note that all the tests use auto_restart=True. Currently we rely on the
  # assumption that an external system restarts failed tasks. If the assumption
  # is not true, the remaining tasks may still hang instead of fail.
  #
  # In these tests we leverage the auto restart feature of MultiProcessRunner.
  # Failed workers are restarted automatically. In reality there needs to be
  # some job management system that does the restart, e.g. Kubernetes.
  #
  # Worker failures may cause problems if there're more than one collective, and
  # the failure happens after the first collective. In this case the recovered
  # worker will be running a different collective with the rest, which causes a
  # deadlock. Note that collectives are common, e.g. when creating variables the
  # initial values are broadcasted from the first worker.
  #
  # We use a multiprocessing.Manager().dict() object to track the attempts of
  # each worker. We take different actions in different attempts to simuate the
  # events in real world. E.g. some tests make a worker fail on the first
  # attempt only, and asserts that it should recovery.

  def test_creating_variable_broken(self):
    # This test simulates the case when a worker fails before or during creating
    # a variable. Creating variables involve broadcasting the initial value from
    # the first replica to all replicas.

    def worker_fn(attempts):
      strategy = tf.distribute.experimental.MultiWorkerMirroredStrategy()
      task_id, attempt = get_attempt(strategy, attempts)
      with strategy.scope():
        tf.Variable(1.)
        # worker-1 dies here.
        if attempt == 1 and task_id == 1:
          quick_exit(1)
        v = tf.Variable(tf.random.uniform(()))
        return v.read_value().numpy()

    cluster_spec = multi_worker_test_base.create_cluster_spec(num_workers=2)
    attempts = multi_process_runner.manager().dict()
    mpr = multi_process_runner.MultiProcessRunner(
        worker_fn, cluster_spec, args=(attempts,), auto_restart=True)
    mpr.start()
    # TODO(b/151232436): worker-0 should raises Unavailable instead of hanging.
    # Now after worker-1 fails, worker-0 waits on the second variable creation;
    # after worker-1 recovers, worker-1 waits on the first variable creation.
    with self.assertRaises(multi_process_runner.SubprocessTimeoutError):
      mpr.join(timeout=30)

  def test_reduce_small_tensor_broken(self):
    # This test simulates the case when a worker fails before or during reducing
    # a small tensors, e.g. reading a metric.
    #
    # Note that this is a rather corner case and only happens when all of the
    # following conditions are met:
    #   - There're two workers.
    #   - They're reducing a small tensor. The definition of small varies
    #     per platform.
    #   - They're reducing a single tensor. Batched all-reduce are not affected.
    #   - It must be worker-1 that fails.

    def worker_fn(attempts):
      strategy = tf.distribute.experimental.MultiWorkerMirroredStrategy()
      task_id, attempt = get_attempt(strategy, attempts)
      value = tf.identity([1.])
      strategy.reduce("sum", value, axis=None)
      # worker-1 dies here.
      if attempt == 1 and task_id == 1:
        quick_exit(1)
      strategy.reduce("sum", value, axis=None)

    cluster_spec = multi_worker_test_base.create_cluster_spec(num_workers=2)
    attempts = multi_process_runner.manager().dict()
    mpr = multi_process_runner.MultiProcessRunner(
        worker_fn, cluster_spec, args=(attempts,), auto_restart=True)
    mpr.start()
    # TODO(b/151232436): worker-0 should raises Unavailable instead of hanging.
    # Now after worker-1 fails, worker-0 waits on the second reduce; after
    # worker-1 recovers, worker-1 waits on the first reduce.
    with self.assertRaises(multi_process_runner.SubprocessTimeoutError):
      mpr.join(timeout=30)

  def test_quick_recover(self):
    # This test simulates the case when a worker fails but recovers quickly
    # before the next collective.
    #
    # It's not guaranteed that the cluster only restarts once when one worker
    # fails. The external job management system is expected to keep restarting
    # failed workers.

    def worker_fn(attempts):
      strategy = tf.distribute.experimental.MultiWorkerMirroredStrategy()
      task_id, attempt = get_attempt(strategy, attempts)

      if attempt == 2 and task_id == 1:
        multi_process_runner.barrier().wait()

      @tf.function
      def replica_fn():
        ctx = tf.distribute.get_replica_context()
        # Use a large tensor because small tensor may hang regardless when the
        # worker recovers.
        value = tf.ones((64, 64))
        ctx.all_reduce(tf.distribute.ReduceOp.SUM, [value, value])

      strategy.run(replica_fn)
      # worker-1 dies here.
      if attempt == 1 and task_id == 1:
        quick_exit(1)
      # Make worker-0 waits for worker-1 to restart before entering the next
      # collective to simulate a quick recovery of worker-1.
      if attempt == 1 and task_id == 0:
        multi_process_runner.barrier().wait()
      strategy.run(replica_fn)

    cluster_spec = multi_worker_test_base.create_cluster_spec(num_workers=2)
    attempts = multi_process_runner.manager().dict()
    mpr = multi_process_runner.MultiProcessRunner(
        worker_fn, cluster_spec, args=(attempts,), auto_restart=True)
    mpr.start()
    mpr.join(timeout=90)


if __name__ == "__main__":
  combinations.main()
