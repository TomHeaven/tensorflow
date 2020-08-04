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
"""Utilities for collectives."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.util.tf_export import tf_export


@tf_export("distribute.experimental.CollectiveHints")
class Hints(object):
  """Hints for collective operations like AllReduce.

  This can be passed to methods like
  `tf.distribute.get_replica_context().all_reduce()` to optimize collective
  operation performance. Note that these are only hints, which may or may not
  change the actual behavior. Some options only apply to certain strategy and
  are ignored by others.

  One common optimization is to break gradients all-reduce into multiple packs
  so that weight updates can overlap with gradient all-reduce.

  Examples:

  - bytes_per_pack

  ```python
  hints = tf.distribute.experimental.CollectiveHints(
      bytes_per_pack=50 * 1024 * 1024)
  grads = tf.distribute.get_replica_context().all_reduce(
      'sum', grads, experimental_hints=hints)
  optimizer.apply_gradients(zip(grads, vars),
      experimental_aggregate_gradients=False)
  ```

  - timeout_seconds

  ```python
  strategy = tf.distribute.MirroredStrategy()
  hints = tf.distribute.experimental.CollectiveHints(
      timeout_seconds=120)
  try:
    strategy.reduce("sum", v, axis=None, experimental_hints=hints)
  except tf.errors.DeadlineExceededError:
    do_something()
  ```

  """

  def __init__(self, bytes_per_pack=0, timeout_seconds=None):
    """Creates a CollectiveHints.

    Args:
      bytes_per_pack: a non-negative integer. Breaks collective operations into
        packs of certain size. If it's zero, the value is determined
        automatically. This only applies to all-reduce with
        `MultiWorkerMirroredStrategy` currently.
      timeout_seconds: a float or None, timeout in seconds. If not None, the
        collective raises `tf.errors.DeadlineExceededError` if it takes longer
        than this timeout. This can be useful when debugging hanging issues.
        This should only be used for debugging since it creates a new thread for
        each collective, i.e. an overhead of `timeout_seconds *
        num_collectives_per_second` more threads.  This only works for
        `tf.distribute.experimental.MultiWorkerMirroredStrategy`.

    Raises:
      ValueError: When arguments have invalid value.
    """
    if bytes_per_pack < 0:
      raise ValueError("bytes_per_pack must be non-negative")
    self.bytes_per_pack = bytes_per_pack
    self.timeout_seconds = timeout_seconds
