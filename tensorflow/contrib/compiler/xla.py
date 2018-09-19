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
# =============================================================================
"""xla is an experimental library that provides XLA support APIs."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
from six.moves import xrange  # pylint: disable=redefined-builtin

from tensorflow.compiler.jit.ops import xla_ops
from tensorflow.contrib.tpu.python.tpu import tpu_function
from tensorflow.core.framework import attr_value_pb2
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.util import compat

_XLA_COMPILE_ATTR = '_xla_compile_id'
_MAX_WARNING_LINES = 5

# Operations that indicate some error in the users graph. For example, XLA
# computation should not have any Placeholder op.
_BLACKLISTED_OPS = set([
    'Placeholder',
])

# XLA doesn't currently support reading of intermediate tensors, thus some ops
# are not supported.
_UNSUPPORTED_OPS = set([
    'AudioSummary',
    'AudioSummaryV2',
    'HistogramSummary',
    'ImageSummary',
    'MergeSummary',
    'Print',
    'ScalarSummary',
    'TensorSummary',
    'TensorSummaryV2',
])


def compile(computation, inputs=None):  # pylint: disable=redefined-builtin
  """Builds an operator that compiles and runs `computation` with XLA.

  Args:
    computation: A Python function that builds a computation to apply to the
      input. If the function takes n inputs, 'inputs' should be a list of n
      tensors.

      `computation` may return a list of operations and tensors.  Tensors must
      come before operations in the returned list.  The return value of
      `compile` is a list of tensors corresponding to the tensors from the
      output of `computation`.

      All `Operation`s returned from `computation` will be executed when
      evaluating any of the returned output tensors.
    inputs: A list of input tensors or `None` (equivalent to an empty list).

  Returns:
    A list of output tensors.
  """
  # pylint: disable=protected-access
  return _compile_internal(computation, inputs)


class XLACompileContext(control_flow_ops.XLAControlFlowContext):
  """A `ControlFlowContext` for nodes inside an XLA computation cluster.

  THIS IS ONLY FOR TENSORFLOW INTERNAL IMPLEMENTATION, DO NO USE DIRECTLY.

  The primary role of `XLACompileContext` is to mark operators inside a
  xla.compile() computation with attribute "_xla_compile_id=XYZ", where XYZ is
  a unique name.

  `ControlFlowContext` is used to perform the annotation since it integrates
  with Tensorflow constructs like ResourceVariables. For example, if a
  `ResourceVariable` is constructed inside a xla.compile() block, the
  `ResourceVariable` implementation can use
  `with ops.control_dependencies(None)` to build the variable's definition
  outside the compiled computation.
  """

  def __init__(self, name, pivot):
    """Builds a new XLACompileContext.

    Args:
      name: a unique name for the context, used to populate the
        `_xla_compile_id` attribute.
      pivot: a pivot node. Nodes in the XLACompileContext that do not have any
        inputs will have a control dependency on the pivot node. This ensures
        that nodes are correctly included in any enclosing control flow
        contexts.
    """
    super(XLACompileContext, self).__init__()
    self._name = name
    self._name_as_bytes = compat.as_bytes(name)
    self._unsupported_ops = []
    self._pivot = pivot

  def report_unsupported_operations(self):
    if self._unsupported_ops:
      op_str = '\n'.join([
          '  %s (%s)' % (op.type, op.name)
          for op in self._unsupported_ops[:_MAX_WARNING_LINES]
      ])
      logging.warning('%d unsupported operations found: \n%s',
                      len(self._unsupported_ops), op_str)
      if len(self._unsupported_ops) > _MAX_WARNING_LINES:
        logging.warning('... and %d more',
                        len(self._unsupported_ops) - _MAX_WARNING_LINES)

  def AddOp(self, op):
    """Create op in XLACompileContext and notifies outer context recursively."""
    # pylint: disable=protected-access
    if op.type in _BLACKLISTED_OPS:
      logging.error(
          'Operation of type %s (%s) is not supported in XLA. Execution will '
          'fail if this op is used in the graph. ', op.type, op.name)

    # TODO(ycao): Automatically disable summaries instead of reporting them.
    if op.type in _UNSUPPORTED_OPS:
      self._unsupported_ops.append(op)

    if any(x.dtype._is_ref_dtype for x in op.inputs):
      raise NotImplementedError(
          'Non-resource Variables are not supported inside XLA computations '
          '(operator name: %s)' % op.name)

    if _XLA_COMPILE_ATTR in op.node_def.attr:
      raise ValueError('XLA compiled computations cannot be nested, (operator '
                       'name: %s)' % op.name)

    op._set_attr(
        _XLA_COMPILE_ATTR, attr_value_pb2.AttrValue(s=self._name_as_bytes))

    op.graph.prevent_feeding(op)
    op.graph.prevent_fetching(op)

    # Remove any control edges from outer control flow contexts. These may cause
    # mismatched frame errors. An example is when one of op's inputs is
    # generated in a different While control flow context.
    (internal_control_inputs,
     external_control_inputs) = self._RemoveExternalControlEdges(op)

    if not op.inputs:
      # Add a control edge from the control pivot to this op.
      if not internal_control_inputs:
        # pylint: disable=protected-access
        op._add_control_input(self._pivot)
        # pylint: enable=protected-access
    else:
      for index in xrange(len(op.inputs)):
        x = op.inputs[index]
        real_x = self.AddValue(x)
        if real_x != x:
          op._update_input(index, real_x)  # pylint: disable=protected-access

    if external_control_inputs:
      # Use an identity to pull control inputs as data inputs. Note that we
      # ignore ops which don't have outputs. TODO(phawkins): fix that.
      with ops.control_dependencies(None):
        self.Enter()
        external_control_inputs = [
            array_ops.identity(x.outputs[0]).op
            for x in external_control_inputs
            if x.outputs
        ]
        self.Exit()
      # pylint: disable=protected-access
      op._add_control_inputs(external_control_inputs)
      # pylint: enable=protected-access

    # Mark op's outputs as seen by this context and any outer contexts.
    output_names = [x.name for x in op.outputs]
    context = self
    while context is not None:
      # pylint: disable=protected-access
      context._values.update(output_names)
      context = context._outer_context
      # pylint: enable=protected-access

    if self._outer_context:
      self._outer_context.AddInnerOp(op)

  def AddValue(self, val):
    """Add `val` to the current context and its outer context recursively."""
    if val.name in self._values:
      # Use the real value if it comes from outer context.
      result = self._external_values.get(val.name)
      return val if result is None else result

    result = val
    self._values.add(val.name)
    if self._outer_context:
      result = self._outer_context.AddValue(val)
      self._values.add(result.name)

    self._external_values[val.name] = result

    return result

  def AddInnerOp(self, op):
    self.AddOp(op)
    if self._outer_context:
      self._outer_context.AddInnerOp(op)

  @property
  def grad_state(self):
    # Define the gradient loop state associated with the XLACompileContext to
    # be None as the XLACompileContext does not get nested nor does the
    # grad_state outside the XLACompileContext affect the graph inside so the
    # grad_state should be as if this is the top-level gradient state.
    return None

  @property
  def back_prop(self):
    """Forwards to the enclosing while context, if any."""
    if self.GetWhileContext():
      return self.GetWhileContext().back_prop
    return False


def _compile_internal(computation, inputs=None):
  """Builds graph operators that compiles and symbolically executes computation.

  Args:
    computation: A Python function that builds the computation to compile and
      execute.
    inputs: A list of input tensors or `None` (equivalent to `[]`). Its order
      should match ordering of computation arguments.
  Returns:
    A list of output tensors from computation.
  Raises:
    ValueError: If any element in computation outputs is neither an operations
      or a value that can be converted to tensor.
    TypeError: If `inputs` is not a list or tuple.
  """
  if inputs is None:
    inputs = []

  if not isinstance(inputs, collections.Sequence):
    raise TypeError('inputs must be a list')

  # Converts inputs to Tensors.
  inputs = [ops.convert_to_tensor(x) for x in inputs]
  input_arity = len(inputs)

  arg_error = tpu_function.check_function_argument_count(
      computation, input_arity, infeed_queue=None)
  if arg_error is not None:
    raise TypeError(
        'Supplied computation cannot be called with the specified inputs. You '
        'specified %d inputs: %s, but the computation needs %s' %
        (input_arity, str([i.name for i in inputs[0]]), arg_error))

  cluster_name = ops.get_default_graph().unique_name('cluster')
  pivot = control_flow_ops.no_op(name=cluster_name + '/pivot')
  context = XLACompileContext(name=cluster_name, pivot=pivot)
  try:
    context.Enter()

    # Add identity ops so even unused inputs are 'consumed' by the
    # computation.
    computation_inputs = [
        array_ops.identity(x, name='input_{}'.format(i))
        for i, x in enumerate(inputs)
    ]

    # Only resource variables work inside an XLA computation, so turn on
    # resource variables for the computation.
    vscope = variable_scope.get_variable_scope()
    saved_use_resource = vscope.use_resource
    vscope.set_use_resource(True)

    outputs = computation(*computation_inputs)

    # Restore variable scope after computation.
    vscope.set_use_resource(saved_use_resource)

    # If the computation returns `None`, make it an empty tuple.
    if outputs is None:
      outputs = tuple()
    # If the computation only returned one value, make it a tuple.
    if not isinstance(outputs, collections.Sequence):
      outputs = (outputs,)

    # Append `no_op` here so that return value of this function always contains
    # at least one op that can trigger XlaLaunch node.
    outputs += (control_flow_ops.no_op(),)
    try:
      outputs = [
          o if isinstance(o, ops.Operation) else ops.convert_to_tensor(o)
          for o in outputs
      ]
    except Exception as e:
      raise ValueError(
          'XLA computation function return values must all either be Operations'
          ' or convertible to Tensors. Got error: "%s"' % str(e))

    # Separates the returned Operations and Tensors.
    output_operations = [o for o in outputs if isinstance(o, ops.Operation)]
    output_tensors = [o for o in outputs if not isinstance(o, ops.Operation)]

    if outputs != output_tensors + output_operations:
      raise ValueError(
          'XLA computation function must return zero or more Tensor values '
          'followed by zero or more Operations.')
    output_arity = len(output_tensors)

    new_output_tensors = []
    for t in output_tensors:
      with ops.device(t.device if t.device else ''):
        new_output_tensors.append(array_ops.identity(t))

    output_tensors = new_output_tensors
    context.ExitResult(output_tensors)
  finally:
    context.report_unsupported_operations()
    context.Exit()

  outputs = [
      xla_ops.xla_cluster_output(output_tensors[i], name='output{}'.format(i))
      for i in xrange(output_arity)
  ]

  with ops.control_dependencies(output_operations):
    if output_arity == 0:
      # When XLA computation returns only operations and no tensors, a NoOp
      # dependent on the operations in outputs is returned. Otherwise final
      # outputs would be empty and there is no way to trigger returned
      # operations.
      return control_flow_ops.no_op(name='output_0')
    else:
      # Wraps the outputs in identity operators that carries control
      # dependencies.
      return [
          array_ops.identity(outputs[i], name='output_%d' % i)
          for i in xrange(output_arity)
      ]
