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
"""Tests for timeseries."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.compat import v2_compat
from tensorflow.python.keras.preprocessing import timeseries
from tensorflow.python.platform import test


class TimeseriesDatasetTest(test.TestCase):

  def test_basics(self):
    # Test ordering, targets, sequence length, batch size
    data = np.arange(100)
    targets = data * 2
    dataset = timeseries.timeseries_dataset(
        data, targets, sequence_length=9, batch_size=5)
    # Expect 19 batches
    for i, batch in enumerate(dataset):
      self.assertLen(batch, 2)
      if i < 18:
        self.assertEqual(batch[0].shape, (5, 9))
      if i == 18:
        # Last batch: size 2
        self.assertEqual(batch[0].shape, (2, 9))
      # Check target values
      self.assertAllClose(batch[0] * 2, batch[1])
      for j in range(min(5, len(batch[0]))):
        # Check each sample in the batch
        self.assertAllClose(batch[0][j], np.arange(i * 5 + j, i * 5 + j + 9))

  def test_no_targets(self):
    data = np.arange(50)
    dataset = timeseries.timeseries_dataset(
        data, None, sequence_length=10, batch_size=5)
    # Expect 9 batches
    for i, batch in enumerate(dataset):
      if i < 8:
        self.assertEqual(batch.shape, (5, 10))
      elif i == 8:
        self.assertEqual(batch.shape, (1, 10))
      for j in range(min(5, len(batch))):
        # Check each sample in the batch
        self.assertAllClose(batch[j], np.arange(i * 5 + j, i * 5 + j + 10))

  def test_shuffle(self):
    # Test cross-epoch random order and seed determinism
    data = np.arange(10)
    targets = data * 2
    dataset = timeseries.timeseries_dataset(
        data, targets, sequence_length=5, batch_size=1, shuffle=True, seed=123)
    first_seq = None
    for x, y in dataset.take(1):
      self.assertNotAllClose(x, np.arange(0, 5))
      self.assertAllClose(x * 2, y)
      first_seq = x
    # Check that a new iteration with the same dataset yields different results
    for x, _ in dataset.take(1):
      self.assertNotAllClose(x, first_seq)
    # Check determism with same seed
    dataset = timeseries.timeseries_dataset(
        data, targets, sequence_length=5, batch_size=1, shuffle=True, seed=123)
    for x, _ in dataset.take(1):
      self.assertAllClose(x, first_seq)

  def test_sampling_rate(self):
    data = np.arange(100)
    targets = data * 2
    dataset = timeseries.timeseries_dataset(
        data, targets, sequence_length=9, batch_size=5, sampling_rate=2)
    for i, batch in enumerate(dataset):
      self.assertLen(batch, 2)
      if i < 16:
        self.assertEqual(batch[0].shape, (5, 9))
      if i == 16:
        # Last batch: size 3
        self.assertEqual(batch[0].shape, (3, 9))
      # Check target values
      self.assertAllClose(batch[0] * 2, batch[1])
      for j in range(min(5, len(batch[0]))):
        # Check each sample in the batch
        start_index = i * 5 + j
        end_index = start_index + 9 * 2
        self.assertAllClose(batch[0][j],
                            np.arange(start_index, end_index, 2))

  def test_sequence_stride(self):
    data = np.arange(100)
    targets = data * 2
    dataset = timeseries.timeseries_dataset(
        data, targets, sequence_length=9, batch_size=5, sequence_stride=3)
    for i, batch in enumerate(dataset):
      self.assertLen(batch, 2)
      if i < 6:
        self.assertEqual(batch[0].shape, (5, 9))
      if i == 6:
        # Last batch: size 1
        self.assertEqual(batch[0].shape, (1, 9))
      # Check target values
      self.assertAllClose(batch[0] * 2, batch[1])
      for j in range(min(5, len(batch[0]))):
        # Check each sample in the batch
        start_index = i * 5 * 3 + j * 3
        end_index = start_index + 9
        self.assertAllClose(batch[0][j],
                            np.arange(start_index, end_index))

  def test_start_and_end_index(self):
    data = np.arange(100)
    dataset = timeseries.timeseries_dataset(
        data, None,
        sequence_length=9, batch_size=5, sequence_stride=3, sampling_rate=2,
        start_index=10, end_index=90)
    for batch in dataset:
      self.assertAllLess(batch[0], 90)
      self.assertAllGreater(batch[0], 9)

  def test_errors(self):
    # bad targets
    with self.assertRaisesRegex(ValueError,
                                'data and targets to have the same number'):
      _ = timeseries.timeseries_dataset(np.arange(10), np.arange(9), 3)
    # bad start index
    with self.assertRaisesRegex(ValueError, 'start_index must be '):
      _ = timeseries.timeseries_dataset(np.arange(10), None, 3, start_index=-1)
    with self.assertRaisesRegex(ValueError, 'start_index must be '):
      _ = timeseries.timeseries_dataset(np.arange(10), None, 3, start_index=11)
    # bad end index
    with self.assertRaisesRegex(ValueError, 'end_index must be '):
      _ = timeseries.timeseries_dataset(np.arange(10), None, 3, end_index=-1)
    with self.assertRaisesRegex(ValueError, 'end_index must be '):
      _ = timeseries.timeseries_dataset(np.arange(10), None, 3, end_index=11)
    # bad sampling_rate
    with self.assertRaisesRegex(ValueError, 'sampling_rate must be '):
      _ = timeseries.timeseries_dataset(np.arange(10), None, 3, sampling_rate=0)
    # bad sequence stride
    with self.assertRaisesRegex(ValueError, 'sequence_stride must be '):
      _ = timeseries.timeseries_dataset(
          np.arange(10), None, 3, sequence_stride=0)


if __name__ == '__main__':
  v2_compat.enable_v2_behavior()
  test.main()
