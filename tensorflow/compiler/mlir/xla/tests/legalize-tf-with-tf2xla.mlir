// RUN: tf-opt -xla-legalize-tf-with-tf2xla=device-type=XLA_CPU %s | FileCheck %s --dump-input-on-failure

// INVALID_DEVICE: tf-opt -xla-legalize-tf-with-tf2xla=device-type=INVALID_DEVICE %s | FileCheck %s --dump-input-on-failure

module attributes {tf.versions = {bad_consumers = [], min_consumer = 0 : i32, producer = 268 : i32}} {

// CHECK-LABEL: abs
// expected-error@+1 {{unsupported device}}
func @abs(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  // CHECK: %[[RESULT:.*]] = "xla_hlo.abs"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  %0 = "tf.Abs"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>

  // return %[[RESULT]]
  return %0 : tensor<2xf32>
}

// CHECK-LABEL: unknown_op
func @unknown_op(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  // CHECK: tf.CustomTestOp
  // expected-remark@+1 {{constant 20}}
  %0 = "tf.CustomTestOp"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>

  return %0 : tensor<2xf32>
}

// CHECK-LABEL: dynamic_operand
func @dynamic_operand(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  // CHECK: tf.Abs
  // expected-remark@+1 {{lowering requires static shaped operands}}
  %0 = "tf.Abs"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>

  return %0 : tensor<?xf32>
}

// CHECK-LABEL: multiple_dialect_ops
func @multiple_dialect_ops(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  // CHECK: xla_hlo.neg
  %0 = "xla_hlo.neg"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  // CHECK: xla_hlo.abs
  %1 = "tf.Abs"(%0) : (tensor<2xf32>) -> tensor<2xf32>

  return %1 : tensor<2xf32>
}

// TODO(hinsu): Add a test with variant type once one of the ops supporting
// the type is whitelisted. It should be rejected with unsupported type remark.

// TODO(hinsu): Add a test with uint8 type once one of the ops supporting the
// type is whitelisted. Unsigned types are not yet added to the HLO dialect so
// it should return an error. See b/130356985

// TODO(hinsu): Add a test with a valid TF op for which tf2xla kernel is
// available but doesn't support this instance.
}
