// RUN: mlir-opt -convert-gpu-to-spirv %s -o - | FileCheck %s

// CHECK:       spv.module "Logical" "VulkanKHR" {
// CHECK-NEXT:    spv.globalVariable !spv.ptr<f32, StorageBuffer> [[VAR1:@.*]] bind(0, 0)
// CHECK-NEXT:    spv.globalVariable !spv.ptr<!spv.array<12 x f32>, StorageBuffer> [[VAR2:@.*]] bind(0, 1)
// CHECK-NEXT:    func @kernel_1
// CHECK-NEXT:      spv.Return
// CHECK:       spv.EntryPoint "GLCompute" @kernel_1, [[VAR1]], [[VAR2]]
func @kernel_1(%arg0 : f32, %arg1 : memref<12xf32, 1>)
    attributes { gpu.kernel } {
  return
}

func @foo() {
  %0 = "op"() : () -> (f32)
  %1 = "op"() : () -> (memref<12xf32, 1>)
  %cst = constant 1 : index
  "gpu.launch_func"(%cst, %cst, %cst, %cst, %cst, %cst, %0, %1) { kernel = @kernel_1 }
      : (index, index, index, index, index, index, f32, memref<12xf32, 1>) -> ()
  return
}