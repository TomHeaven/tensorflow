# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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
"""Test configs for leaky_relu."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import tensorflow.compat.v1 as tf
from tensorflow.lite.testing.zip_test_utils import create_tensor_data
from tensorflow.lite.testing.zip_test_utils import make_zip_of_tests
from tensorflow.lite.testing.zip_test_utils import register_make_test_function


@register_make_test_function()
def make_leaky_relu_tests(options):
  """Make a set of tests to do LeakyRelu."""

  test_parameters = [
      {
          "input_shape": [[], [1], [5], [1, 10, 10, 3], [3, 3, 3, 3]],
          "alpha": [0.1, 1.0, 2.0, -0.1, -1.0, -2.0],
          "fully_quantize": [False],
          "quant_16x8": [False],
      },
      {
          "input_shape": [[1, 10, 10, 3]],
          "alpha": [0.1],
          "fully_quantize": [True],
          "quant_16x8": [True],
      },
      {
          "input_shape": [[1, 10, 10, 3]],
          "alpha": [0.1],
          "fully_quantize": [True],
          "quant_16x8": [False]
      }]

  def build_graph(parameters):
    """Build the graph for the test case."""

    input_tensor = tf.compat.v1.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    out = tf.nn.leaky_relu(input_tensor, alpha=parameters["alpha"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    """Build the inputs for the test case."""
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value=-3, max_value=10)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(options, test_parameters, build_graph, build_inputs)
