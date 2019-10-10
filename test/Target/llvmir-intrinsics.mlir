// RUN: mlir-translate -mlir-to-llvmir %s | FileCheck %s

// CHECK-LABEL: @fmuladd_test
llvm.func @fmuladd_test(%arg0: !llvm.float, %arg1: !llvm.float, %arg2: !llvm<"<8 x float>">) {
  // CHECK: call float @llvm.fmuladd.f32.f32.f32
  "llvm.intr.fmuladd"(%arg0, %arg1, %arg0) : (!llvm.float, !llvm.float, !llvm.float) -> !llvm.float
  // CHECK: call <8 x float> @llvm.fmuladd.v8f32.v8f32.v8f32
  "llvm.intr.fmuladd"(%arg2, %arg2, %arg2) : (!llvm<"<8 x float>">, !llvm<"<8 x float>">, !llvm<"<8 x float>">) -> !llvm<"<8 x float>">
  llvm.return
}

// CHECK-LABEL: @exp_test
llvm.func @exp_test(%arg0: !llvm.float, %arg1: !llvm<"<8 x float>">) {
  // CHECK: call float @llvm.exp.f32
  "llvm.intr.exp"(%arg0) : (!llvm.float) -> !llvm.float
  // CHECK: call <8 x float> @llvm.exp.v8f32
  "llvm.intr.exp"(%arg1) : (!llvm<"<8 x float>">) -> !llvm<"<8 x float>">
  llvm.return
}

// Check that intrinsics are declared with appropriate types.
// CHECK: declare float @llvm.fmuladd.f32.f32.f32(float, float, float)
// CHECK: declare <8 x float> @llvm.fmuladd.v8f32.v8f32.v8f32(<8 x float>, <8 x float>, <8 x float>) #0
// CHECK: declare float @llvm.exp.f32(float)
// CHECK: declare <8 x float> @llvm.exp.v8f32(<8 x float>) #0
