# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""Libsvm decoder."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.contrib.libsvm.ops import gen_libsvm_ops
from tensorflow.contrib.util import loader
from tensorflow.python.framework import ops
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.platform import resource_loader
from tensorflow.python.util.deprecation import deprecated


_libsvm_ops_so = loader.load_op_library(
    resource_loader.get_path_to_datafile("_libsvm_ops.so"))

@deprecated(
    None,
    'tf.contrib.libsvm will be removed in 2.0, the support for libsvm '
    'format will continue to be provided in tensorflow-io: '
    'https://github.com/tensorflow/io')
def decode_libsvm(content, num_features, dtype=None, label_dtype=None):
  """Convert Libsvm records to a tensor of label and a tensor of feature.

  Args:
    content: A `Tensor` of type `string`. Each string is a record/row in
      the Libsvm format.
    num_features: The number of features.
    dtype: The type of the output feature tensor. Default to tf.float32.
    label_dtype: The type of the output label tensor. Default to tf.int64.

  Returns:
    features: A `SparseTensor` of the shape `[input_shape, num_features]`.
    labels: A `Tensor` of the same shape as content.
  """
  labels, indices, values, shape = gen_libsvm_ops.decode_libsvm(
      content, num_features, dtype=dtype, label_dtype=label_dtype)
  return sparse_tensor.SparseTensor(indices, values, shape), labels


ops.NotDifferentiable("DecodeLibSVM")
