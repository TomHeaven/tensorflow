/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/delegates/gpu/cl/kernels/upsample.h"

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

std::string GetUpsampleCode(
    const OperationDef& op_def,
    const std::vector<ElementwiseOperation*>& linked_operations) {
  TensorCodeGenerator src_tensor(
      "src_data", WHSPoint{"src_size.x", "src_size.y", "src_size.z"},
      op_def.src_tensors[0]);
  TensorCodeGenerator dst_tensor(
      "dst_data", WHSPoint{"dst_size.x", "dst_size.y", "dst_size.z"},
      op_def.dst_tensors[0]);

  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += src_tensor.GetDeclaration(AccessType::READ);
  c += GetArgsDeclaration(linked_operations);
  c += dst_tensor.GetDeclaration(AccessType::WRITE) + ",\n";
  c += "    int4 src_size,         \n";
  c += "    int4 dst_size,         \n";
  c += "    int2 border,           \n";
  c += "    float2 scale_factor    \n";
  c += ") {\n";
  c += "  int Y = get_global_id(1);\n";
  c += "  int Z = get_global_id(2);\n";
  if (op_def.batch_support) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int X = linear_id / dst_size.w;\n";
    c += "  int B = linear_id % dst_size.w;\n";
    c += "  if (get_global_id(0) >= dst_size.x || Y >= dst_size.y || Z >= "
         "dst_size.z) return;\n";
  } else {
    c += "  int X = get_global_id(0);\n";
    c += "  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) "
         "return;\n";
  }
  c += "  float2 f_coords = (float2)(X, Y) * scale_factor;\n";
  c += "  int4 st;\n";
  c += "  st.xy = (int2)(f_coords.x, f_coords.y);\n";
  c += "  st.zw = min(st.xy + (int2)(1, 1), border);\n";
  c += "  float2 t = f_coords - (float2)(st.x, st.y);\n";
  if (op_def.batch_support) {
    c += "  st.x = st.x * src_size.w + B;\n";
    c += "  st.z = st.z * src_size.w + B;\n";
    c += "  X = X * dst_size.w + B;\n";
  }
  c += "  float4 src0 = " + src_tensor.ReadAsFloatWHS("st.x", "st.y", "Z") +
       ";\n";
  c += "  float4 src1 = " + src_tensor.ReadAsFloatWHS("st.z", "st.y", "Z") +
       ";\n";
  c += "  float4 src2 = " + src_tensor.ReadAsFloatWHS("st.x", "st.w", "Z") +
       ";\n";
  c += "  float4 src3 = " + src_tensor.ReadAsFloatWHS("st.z", "st.w", "Z") +
       ";\n";
  c += "  FLT4 r0 = TO_FLT4(mix(mix(src0, src1, t.x), mix(src2, src3, t.x), "
       "t.y));\n";
  const LinkingContext context{"r0", "X", "Y", "Z"};
  c += PostProcess(linked_operations, context);
  c += "  " + dst_tensor.WriteWHS("r0", "X", "Y", "Z");
  c += "}\n";
  return c;
}

std::string GetUpsample3DCode(
    const OperationDef& op_def,
    const std::vector<ElementwiseOperation*>& linked_operations) {
  TensorCodeGenerator src_tensor(
      "src_data",
      WHDSPoint{"src_size.x", "src_size.y", "src_size.z", "src_size.w"},
      op_def.src_tensors[0]);
  TensorCodeGenerator dst_tensor(
      "dst_data",
      WHDSPoint{"dst_size.x", "dst_size.y", "dst_size.z", "dst_size.w"},
      op_def.dst_tensors[0]);

  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += src_tensor.GetDeclaration(AccessType::READ);
  c += GetArgsDeclaration(linked_operations);
  c += dst_tensor.GetDeclaration(AccessType::WRITE) + ",\n";
  c += "    int4 src_size,         \n";
  c += "    int4 dst_size,         \n";
  if (op_def.batch_support) {
    c += "    int batch_size,      \n";
  }
  c += "    int4 border,           \n";
  c += "    float4 scale_factor    \n";
  c += ") {\n";
  c += "  int Y = get_global_id(1);\n";
  c += "  int linear_id_z = get_global_id(2);\n";
  c += "  int S = linear_id_z % dst_size.w;\n";
  c += "  int Z = linear_id_z / dst_size.w;\n";
  if (op_def.batch_support) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int X = linear_id / batch_size;\n";
    c += "  int B = linear_id % batch_size;\n";
    c += "  if (linear_id >= dst_size.x || Y >= dst_size.y || Z >= "
         "dst_size.z) return;\n";
  } else {
    c += "  int X = get_global_id(0);\n";
    c += "  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) "
         "return;\n";
  }
  c += "  float4 f_coords = (float4)(X, Y, Z, 0) * scale_factor;\n";
  c += "  int4 start = (int4)(f_coords.x, f_coords.y, f_coords.z, 0);\n";
  c += "  int4 end = min(start + (int4)(1, 1, 1, 0), border);\n";
  c += "  float4 t = f_coords - (float4)(start.x, start.y, start.z, 0.0f);\n";
  if (op_def.batch_support) {
    c += "  start.x = start.x * batch_size + B;\n";
    c += "  end.x = end.x * batch_size + B;\n";
    c += "  X = X * batch_size + B;\n";
  }
  c += "  float4 src0 = " +
       src_tensor.ReadAsFloatWHDS("start.x", "start.y", "start.z", "S") + ";\n";
  c += "  float4 src1 = " +
       src_tensor.ReadAsFloatWHDS("end.x", "start.y", "start.z", "S") + ";\n";
  c += "  float4 src2 = " +
       src_tensor.ReadAsFloatWHDS("start.x", "end.y", "start.z", "S") + ";\n";
  c += "  float4 src3 = " +
       src_tensor.ReadAsFloatWHDS("end.x", "end.y", "start.z", "S") + ";\n";
  c += "  float4 src4 = " +
       src_tensor.ReadAsFloatWHDS("start.x", "start.y", "end.z", "S") + ";\n";
  c += "  float4 src5 = " +
       src_tensor.ReadAsFloatWHDS("end.x", "start.y", "end.z", "S") + ";\n";
  c += "  float4 src6 = " +
       src_tensor.ReadAsFloatWHDS("start.x", "end.y", "end.z", "S") + ";\n";
  c += "  float4 src7 = " +
       src_tensor.ReadAsFloatWHDS("end.x", "end.y", "end.z", "S") + ";\n";
  c += "  float4 t0 = mix(mix(src0, src1, t.x), mix(src2, src3, t.x), t.y);\n";
  c += "  float4 t1 = mix(mix(src4, src5, t.x), mix(src6, src7, t.x), t.y);\n";
  c += "  FLT4 r0 = TO_FLT4(mix(t0, t1, t.z));\n";
  const LinkingContext context{"r0", "X", "Y", "S"};
  c += PostProcess(linked_operations, context);
  c += "  " + dst_tensor.WriteWHDS("r0", "X", "Y", "Z", "S");
  c += "}\n";
  return c;
}

}  // namespace

Upsample::Upsample(Upsample&& operation)
    : GPUOperation(std::move(operation)),
      attr_(operation.attr_),
      kernel_(std::move(operation.kernel_)),
      work_group_size_(operation.work_group_size_) {}

Upsample& Upsample::operator=(Upsample&& operation) {
  if (this != &operation) {
    attr_ = operation.attr_;
    kernel_ = std::move(operation.kernel_);
    std::swap(work_group_size_, operation.work_group_size_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

Status Upsample::Compile(const CreationContext& creation_context) {
  const auto code = GetUpsampleCode(definition_, linked_operations_);
  return creation_context.cache->GetOrCreateCLKernel(
      code, "main_function", *creation_context.context,
      *creation_context.device, &kernel_);
}

Status Upsample::BindArguments() {
  kernel_.ResetBindingCounter();
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(src_[0]->GetMemoryPtr()));
  RETURN_IF_ERROR(BindArgs(&kernel_, linked_operations_));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(dst_[0]->GetMemoryPtrForWriting()));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(src_[0]->GetWBatchedHSB()));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(dst_[0]->GetWBatchedHSB()));
  RETURN_IF_ERROR(
      kernel_.SetBytesAuto(int2(src_[0]->Width() - 1, src_[0]->Height() - 1)));
  float2 scale_factor =
      float2(CalculateResizeScale(src_[0]->Width(), dst_[0]->Width(), attr_),
             CalculateResizeScale(src_[0]->Height(), dst_[0]->Height(), attr_));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(scale_factor));
  return OkStatus();
}

int3 Upsample::GetGridSize() const {
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = dst_[0]->Height();
  const int grid_z = dst_[0]->Slices();
  return int3(grid_x, grid_y, grid_z);
}

Status Upsample::AddToQueue(CLCommandQueue* queue) {
  RETURN_IF_ERROR(BindArguments());
  return queue->DispatchImplicit(kernel_, GetGridSize(), work_group_size_);
}

Status Upsample::Tune(const TuningParameters& params) {
  RETURN_IF_ERROR(BindArguments());
  return GetBestWorkGroup(params, kernel_, GetGridSize(), &work_group_size_);
}

Upsample CreateUpsample(const OperationDef& definition,
                        const Upsample2DAttributes& attr) {
  return Upsample(definition, attr);
}

Upsample3D::Upsample3D(Upsample3D&& operation)
    : GPUOperation(std::move(operation)),
      attr_(operation.attr_),
      kernel_(std::move(operation.kernel_)),
      work_group_size_(operation.work_group_size_) {}

Upsample3D& Upsample3D::operator=(Upsample3D&& operation) {
  if (this != &operation) {
    attr_ = operation.attr_;
    kernel_ = std::move(operation.kernel_);
    std::swap(work_group_size_, operation.work_group_size_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

Status Upsample3D::Compile(const CreationContext& creation_context) {
  const auto code = GetUpsample3DCode(definition_, linked_operations_);
  return creation_context.cache->GetOrCreateCLKernel(
      code, "main_function", *creation_context.context,
      *creation_context.device, &kernel_);
}

Status Upsample3D::BindArguments() {
  kernel_.ResetBindingCounter();
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(src_[0]->GetMemoryPtr()));
  RETURN_IF_ERROR(BindArgs(&kernel_, linked_operations_));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(dst_[0]->GetMemoryPtrForWriting()));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(src_[0]->GetWBatchedHDS()));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(dst_[0]->GetWBatchedHDS()));
  if (definition_.batch_support) {
    RETURN_IF_ERROR(kernel_.SetBytesAuto(src_[0]->Batch()));
  }
  RETURN_IF_ERROR(kernel_.SetBytesAuto(int4(
      src_[0]->Width() - 1, src_[0]->Height() - 1, src_[0]->Depth() - 1, 0)));
  float4 scale_factor = float4(
      CalculateResizeScale(src_[0]->Width(), dst_[0]->Width(), attr_),
      CalculateResizeScale(src_[0]->Height(), dst_[0]->Height(), attr_),
      CalculateResizeScale(src_[0]->Depth(), dst_[0]->Depth(), attr_), 1.0f);
  RETURN_IF_ERROR(kernel_.SetBytesAuto(scale_factor));
  return OkStatus();
}

int3 Upsample3D::GetGridSize() const {
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = dst_[0]->Height();
  const int grid_z = dst_[0]->Slices() * dst_[0]->Depth();
  return int3(grid_x, grid_y, grid_z);
}

Status Upsample3D::AddToQueue(CLCommandQueue* queue) {
  RETURN_IF_ERROR(BindArguments());
  return queue->DispatchImplicit(kernel_, GetGridSize(), work_group_size_);
}

Status Upsample3D::Tune(const TuningParameters& params) {
  RETURN_IF_ERROR(BindArguments());
  return GetBestWorkGroup(params, kernel_, GetGridSize(), &work_group_size_);
}

Upsample3D CreateUpsample3D(const OperationDef& definition,
                            const Upsample3DAttributes& attr) {
  return Upsample3D(definition, attr);
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
