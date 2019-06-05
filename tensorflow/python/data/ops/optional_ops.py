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
"""An Optional type for representing potentially missing values."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import abc

import six

from tensorflow.python.framework import composite_tensor
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import type_spec
from tensorflow.python.ops import gen_dataset_ops
from tensorflow.python.util.tf_export import tf_export


@tf_export("data.experimental.Optional")
@six.add_metaclass(abc.ABCMeta)
class Optional(composite_tensor.CompositeTensor):
  """Wraps a nested structure of tensors that may/may not be present at runtime.

  An `Optional` can represent the result of an operation that may fail as a
  value, rather than raising an exception and halting execution. For example,
  `tf.data.experimental.get_next_as_optional` returns an `Optional` that either
  contains the next value from a `tf.compat.v1.data.Iterator` if one exists, or
  a "none"
  value that indicates the end of the sequence has been reached.
  """

  @abc.abstractmethod
  def has_value(self, name=None):
    """Returns a tensor that evaluates to `True` if this optional has a value.

    Args:
      name: (Optional.) A name for the created operation.

    Returns:
      A scalar `tf.Tensor` of type `tf.bool`.
    """
    raise NotImplementedError("Optional.has_value()")

  @abc.abstractmethod
  def get_value(self, name=None):
    """Returns a nested structure of values wrapped by this optional.

    If this optional does not have a value (i.e. `self.has_value()` evaluates
    to `False`), this operation will raise `tf.errors.InvalidArgumentError`
    at runtime.

    Args:
      name: (Optional.) A name for the created operation.

    Returns:
      A nested structure of `tf.Tensor` and/or `tf.SparseTensor` objects.
    """
    raise NotImplementedError("Optional.get_value()")

  @abc.abstractproperty
  def value_structure(self):
    """The structure of the components of this optional.

    Returns:
      A `Structure` object representing the structure of the components of this
        optional.
    """
    raise NotImplementedError("Optional.value_structure")

  @staticmethod
  def from_value(value):
    """Returns an `Optional` that wraps the given value.

    Args:
      value: A nested structure of `tf.Tensor` and/or `tf.SparseTensor` objects.

    Returns:
      An `Optional` that wraps `value`.
    """
    with ops.name_scope("optional") as scope:
      with ops.name_scope("value"):
        value_structure = type_spec.type_spec_from_value(value)
        encoded_value = value_structure._to_tensor_list(value)  # pylint: disable=protected-access

    return _OptionalImpl(
        gen_dataset_ops.optional_from_value(encoded_value, name=scope),
        value_structure)

  @staticmethod
  def none_from_structure(value_structure):
    """Returns an `Optional` that has no value.

    NOTE: This method takes an argument that defines the structure of the value
    that would be contained in the returned `Optional` if it had a value.

    Args:
      value_structure: A `Structure` object representing the structure of the
        components of this optional.

    Returns:
      An `Optional` that has no value.
    """
    return _OptionalImpl(gen_dataset_ops.optional_none(), value_structure)


class _OptionalImpl(Optional):
  """Concrete implementation of `tf.data.experimental.Optional`.

  NOTE(mrry): This implementation is kept private, to avoid defining
  `Optional.__init__()` in the public API.
  """

  def __init__(self, variant_tensor, value_structure):
    self._variant_tensor = variant_tensor
    self._value_structure = value_structure

  def has_value(self, name=None):
    return gen_dataset_ops.optional_has_value(self._variant_tensor, name=name)

  def get_value(self, name=None):
    # TODO(b/110122868): Consolidate the restructuring logic with similar logic
    # in `Iterator.get_next()` and `StructuredFunctionWrapper`.
    with ops.name_scope(name, "OptionalGetValue",
                        [self._variant_tensor]) as scope:
      # pylint: disable=protected-access
      return self._value_structure._from_tensor_list(
          gen_dataset_ops.optional_get_value(
              self._variant_tensor,
              name=scope,
              output_types=self._value_structure._flat_types,
              output_shapes=self._value_structure._flat_shapes))

  @property
  def value_structure(self):
    return self._value_structure

  def _to_components(self):
    return [self._variant_tensor]

  def _component_metadata(self):
    return self._value_structure

  @classmethod
  def _from_components(cls, components, metadata):
    return _OptionalImpl(components[0], metadata)

  def _shape_invariant_to_components(self, shape=None):
    del shape  # not used
    return tensor_shape.TensorShape([])  # optional component is always a scalar

  @property
  def _is_graph_tensor(self):
    return hasattr(self._variant_tensor, "graph")


# TODO(b/133606651) Rename this class to OptionalSpec
@tf_export("data.experimental.OptionalStructure")
class OptionalStructure(type_spec.TypeSpec):
  """Represents an optional potentially containing a structured value."""

  __slots__ = ["_value_structure"]

  def __init__(self, value_structure):
    self._value_structure = value_structure

  @property
  def value_type(self):
    return _OptionalImpl

  def _serialize(self):
    return (self._value_structure,)

  @property
  def _component_specs(self):
    return [tensor_spec.TensorSpec((), dtypes.variant)]

  def _to_components(self, value):
    return [value._variant_tensor]  # pylint: disable=protected-access

  def _from_components(self, flat_value):
    # pylint: disable=protected-access
    return _OptionalImpl(flat_value[0], self._value_structure)

  @staticmethod
  def from_value(value):
    return OptionalStructure(value.value_structure)

  def _to_legacy_output_types(self):
    return self

  def _to_legacy_output_shapes(self):
    return self

  def _to_legacy_output_classes(self):
    return self


type_spec.register_type_spec_from_value_converter(Optional,
                                                  OptionalStructure.from_value)
type_spec.register_type_spec_from_value_converter(_OptionalImpl,
                                                  OptionalStructure.from_value)
