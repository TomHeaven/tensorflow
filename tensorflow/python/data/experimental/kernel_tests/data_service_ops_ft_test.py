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
"""Tests for tf.data service ops where servers are started late or preempted."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import threading
import time

from absl.testing import parameterized

from tensorflow.python.data.experimental.kernel_tests import data_service_test_base
from tensorflow.python.data.experimental.ops import data_service_ops
from tensorflow.python.data.experimental.service import server_lib
from tensorflow.python.data.kernel_tests import test_base
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.framework import combinations
from tensorflow.python.framework import dtypes
from tensorflow.python.ops import array_ops
from tensorflow.python.platform import test

TMP_WORK_DIR = data_service_test_base.TMP_WORK_DIR
NO_WORK_DIR = data_service_test_base.NO_WORK_DIR


def _all_cluster_configurations():
  with_work_dir = combinations.combine(
      work_dir=TMP_WORK_DIR, fault_tolerant_mode=[True, False])
  without_work_dir = combinations.combine(
      work_dir=NO_WORK_DIR, fault_tolerant_mode=False)
  return with_work_dir + without_work_dir


class DataServiceOpsTest(data_service_test_base.TestBase,
                         parameterized.TestCase):

  @combinations.generate(test_base.eager_only_combinations())
  def testDispatcherStop(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    ds = self.make_distributed_range_dataset(num_elements, dispatcher)
    iterator = iter(ds)
    results = []
    results.append(next(iterator).numpy())
    dispatcher._stop()
    # After the dispatcher dies, the worker should continue providing the rest
    # of the dataset's elements.
    for _ in range(num_elements - 1):
      results.append(next(iterator).numpy())
    self.assertEqual(results, list(range(num_elements)))

  @combinations.generate(test_base.eager_only_combinations())
  def testDispatcherRestartBeforeReading(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    ds = self.make_distributed_range_dataset(num_elements, dispatcher)
    dispatcher = self.restart_dispatcher(dispatcher)

    self.assertDatasetProduces(ds, list(range(num_elements)))

  @combinations.generate(test_base.eager_only_combinations())
  def testDispatcherRestartDuringReading(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    ds = self.make_distributed_range_dataset(num_elements, dispatcher)
    iterator = iter(ds)
    results = []
    for _ in range(num_elements // 2):
      results.append(next(iterator).numpy())
    dispatcher = self.restart_dispatcher(dispatcher)
    for elem in iterator:
      results.append(elem.numpy())

    self.assertEqual(list(range(num_elements)), results)

  @combinations.generate(test_base.eager_only_combinations())
  def testDispatcherRestartBetweenIterations(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    ds = self.make_distributed_range_dataset(100, dispatcher)
    self.assertDatasetProduces(ds, list(range(num_elements)))
    dispatcher = self.restart_dispatcher(dispatcher)
    self.assertDatasetProduces(ds, list(range(num_elements)))

  @combinations.generate(test_base.eager_only_combinations())
  def testDispatcherManyRestarts(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements_start = 10
    num_elements_end = 15
    datasets = []
    for num_elements in range(num_elements_start, num_elements_end):
      datasets.append(
          self.make_distributed_range_dataset(num_elements, dispatcher))
      dispatcher = self.restart_dispatcher(dispatcher)
    for ds, num_elements in zip(datasets,
                                range(num_elements_start, num_elements_end)):
      self.assertDatasetProduces(ds, list(range(num_elements)))

  @combinations.generate(test_base.eager_only_combinations())
  def testDispatcherAndWorkerRestart(self):
    dispatcher, [worker] = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    ds = dataset_ops.Dataset.range(num_elements)

    def restart():
      return (self.restart_dispatcher(dispatcher),
              self.restart_worker(worker, dispatcher))

    ds = self.make_distributed_dataset(ds, dispatcher)
    dispatcher, worker = restart()
    self.assertDatasetProduces(ds, list(range(num_elements)))
    dispatcher, worker = restart()
    self.assertDatasetProduces(ds, list(range(num_elements)))

  @combinations.generate(test_base.eager_only_combinations())
  def testStartServersLate(self):
    # Test that the data service client performs retries instead of failing when
    # the dataset is created before the master and worker are started.
    try:
      import portpicker  # pylint: disable=g-import-not-at-top
      dispatcher_port = portpicker.pick_unused_port()
    except:
      raise self.skipTest("Flakes in portpicker library do not represent "
                          "TensorFlow errors.")
    dispatcher = server_lib.DispatchServer(
        server_lib.DispatcherConfig(port=dispatcher_port), start=False)
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(
            dispatcher_address=self.dispatcher_address(dispatcher), port=0),
        start=False)

    def start_servers():
      time.sleep(1)
      dispatcher.start()
      worker.start()

    start_servers_thread = threading.Thread(target=start_servers, daemon=True)
    start_servers_thread.start()

    num_elements = 10
    ds = self.make_distributed_range_dataset(num_elements, dispatcher)
    results = [elem.numpy() for elem in ds]
    self.assertEqual(list(range(num_elements)), results)
    start_servers_thread.join()

  @combinations.generate(test_base.eager_only_combinations())
  def testAddWorkerMidJob(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    ds = self.make_distributed_range_dataset(num_elements, dispatcher)
    iterator = iter(ds)
    results = []
    # Read halfway through the dataset.
    for _ in range(num_elements // 2):
      results.append(next(iterator).numpy())

    new_worker = self.start_worker_server(dispatcher)  # to avoid gcing workers, pylint: disable=unused-variable
    # Wait for the new worker to register with the dispatcher.
    while dispatcher._num_workers() < 2:
      time.sleep(10 / 1000)  # 10ms

    for elem in iterator:
      results.append(elem.numpy())

    self.assertCountEqual(2 * list(range(num_elements)), results)

  @combinations.generate(
      combinations.times(test_base.eager_only_combinations(),
                         combinations.combine(use_same_port=[True, False]),
                         _all_cluster_configurations()))
  def testRestartWorker(self, use_same_port, work_dir, fault_tolerant_mode):
    dispatcher, [worker] = self.start_cluster(
        1, work_dir=work_dir, fault_tolerant_mode=fault_tolerant_mode)
    num_elements = 100
    ds = self.make_distributed_range_dataset(num_elements, dispatcher)
    iterator = iter(ds)
    # Read halfway through the dataset.
    midpoint = num_elements // 2
    for i in range(midpoint):
      self.assertEqual(i, next(iterator).numpy())

    # Stop the original worker and start a new one.
    worker = self.restart_worker(worker, dispatcher, use_same_port)

    # There may have been some elements prefetched from the first worker
    # before it was stopped.
    while True:
      val = next(iterator).numpy()
      if val == 0:
        break

    # The dataset starts over now that we read from the new worker.
    # TODO(b/157086991): Iterate until end of sequence when we support
    # detecting lost workers.
    for i in range(1, num_elements // 2):
      val = next(iterator).numpy()
      self.assertEqual(i, val)

  @combinations.generate(test_base.eager_only_combinations())
  def testChangeProcessingModeAfterRestart(self):
    dispatcher, workers = self.start_cluster(1)  # to avoid gcing workers, pylint: disable=unused-variable
    num_elements = 100
    range_dataset = dataset_ops.Dataset.range(num_elements)
    ds = range_dataset.apply(
        data_service_ops.distribute(
            processing_mode="parallel_epochs",
            service=dispatcher.target,
            job_name="test"))
    iterator = iter(ds)
    for i in range(num_elements // 2):
      self.assertEqual(i, next(iterator).numpy())
    dispatcher = self.restart_dispatcher(dispatcher)
    ds = range_dataset.apply(
        data_service_ops.distribute(
            processing_mode="distributed_epoch",
            service=dispatcher.target,
            job_name="test"))
    with self.assertRaisesOpError("already an existing job with that name "
                                  "using processing mode <parallel_epochs>"):
      next(iter(ds)).numpy()

  @combinations.generate(
      combinations.times(
          test_base.eager_only_combinations(),
          combinations.combine(work_dir=[TMP_WORK_DIR, NO_WORK_DIR])))
  def testDistributeLargeGraphThenRegisterWorker(self, work_dir):
    dispatcher = self.start_dispatch_server(
        work_dir=work_dir, fault_tolerant_mode=False)
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(
            dispatcher_address=self.dispatcher_address(dispatcher), port=0),
        start=False)
    # Larger than default OSS grpc message size limit of 4MB.
    tensor = array_ops.ones((2, 1000, 1000), dtype=dtypes.float32)
    ds = dataset_ops.Dataset.from_tensors(tensor)
    ds = self.make_distributed_dataset(ds, dispatcher)
    it = iter(ds)
    worker.start()
    self.assertAllEqual(next(it), tensor)


if __name__ == "__main__":
  test.main()
