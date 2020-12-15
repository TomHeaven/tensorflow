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

#include "tensorflow/lite/delegates/gpu/cl/kernels/convolution_transposed.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"

namespace tflite {
namespace gpu {
namespace cl {

ConvolutionTransposed::ConvolutionTransposed(
    const OperationDef& definition, const ConvolutionTransposedAttributes& attr,
    const DeviceInfo& device_info)
    : GPUOperation(definition),
      stride_(attr.stride.w, attr.stride.h, 1, 1),
      block_size_(2, 2, 1, 2) {
  const bool weights_are_buffer = device_info.IsMali();
  const bool is_f16 = definition.precision == CalculationsPrecision::F16;
  if (device_info.IsMali()) {
    if (device_info.mali_info.IsMidgard()) {
      block_size_ = is_f16 ? int4(2, 1, 1, 2) : int4(2, 1, 1, 1);
    } else {
      block_size_ = is_f16 ? int4(2, 2, 1, 2) : int4(2, 2, 1, 1);
    }
  }
  const int dst_depth = DivideRoundUp(attr.weights.shape.o, 4);
  if (dst_depth == 1 || dst_depth == 3) {
    if (!device_info.IsMali()) {
      block_size_.y *= block_size_.w;
    }
    block_size_.w = 1;
  }

  args_.AddInt("stride_x", stride_.x);
  args_.AddInt("stride_y", stride_.y);
  args_.AddInt("padding_x", attr.padding.prepended.w);
  args_.AddInt("padding_y", attr.padding.prepended.h);
  args_.AddInt("kernel_size_x", attr.weights.shape.w);
  args_.AddInt("kernel_size_y", attr.weights.shape.h);
  code_ = GenerateConvolutionTransposedCode(definition_, device_info,
                                            weights_are_buffer, block_size_);
  UploadWeights(attr.weights, weights_are_buffer);
}

ConvolutionTransposed::ConvolutionTransposed(
    const OperationDef& definition,
    const ConvolutionTransposed3DAttributes& attr,
    const DeviceInfo& device_info)
    : GPUOperation(definition),
      stride_(attr.stride.w, attr.stride.h, attr.stride.d, 1),
      block_size_(2, 2, 1, 2) {
  const bool weights_are_buffer = device_info.IsMali();
  const bool is_f16 = definition.precision == CalculationsPrecision::F16;
  if (device_info.IsMali()) {
    if (device_info.mali_info.IsMidgard()) {
      block_size_ = is_f16 ? int4(2, 1, 1, 2) : int4(2, 1, 1, 1);
    } else {
      block_size_ = is_f16 ? int4(2, 2, 1, 2) : int4(2, 2, 1, 1);
    }
  }
  const int dst_depth = DivideRoundUp(attr.weights.shape.o, 4);
  if (dst_depth == 1 || dst_depth == 3) {
    if (!device_info.IsMali()) {
      block_size_.y *= block_size_.w;
    }
    block_size_.w = 1;
  }

  args_.AddInt("stride_x", stride_.x);
  args_.AddInt("stride_y", stride_.y);
  args_.AddInt("stride_z", stride_.z);
  args_.AddInt("padding_x", attr.padding.prepended.w);
  args_.AddInt("padding_y", attr.padding.prepended.h);
  args_.AddInt("padding_z", attr.padding.prepended.d);
  args_.AddInt("kernel_size_x", attr.weights.shape.w);
  args_.AddInt("kernel_size_y", attr.weights.shape.h);
  args_.AddInt("kernel_size_z", attr.weights.shape.d);
  args_.AddInt("grid_size_y");
  code_ = GenerateConvolutionTransposedCode(definition_, device_info,
                                            weights_are_buffer, block_size_);
  UploadWeights(attr.weights, weights_are_buffer);
}

ConvolutionTransposed::ConvolutionTransposed(ConvolutionTransposed&& operation)
    : GPUOperation(std::move(operation)),
      stride_(operation.stride_),
      block_size_(operation.block_size_) {}

ConvolutionTransposed& ConvolutionTransposed::operator=(
    ConvolutionTransposed&& operation) {
  if (this != &operation) {
    std::swap(stride_, operation.stride_);
    std::swap(block_size_, operation.block_size_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

std::string ConvolutionTransposed::GenerateConvolutionTransposedCode(
    const OperationDef& op_def, const DeviceInfo& device_info,
    bool weights_are_buffer, const int4& block_size) {
  auto src_desc = op_def.src_tensors[0];
  src_desc.SetTextureAddressMode(TextureAddressMode::ZERO);
  AddSrcTensor("src_tensor", src_desc);
  AddDstTensor("dst_tensor", op_def.dst_tensors[0]);

  const auto& src_def = op_def.src_tensors[0];

  std::string c = GetCommonDefines(op_def.precision);

  for (int s = 0; s < block_size.w; ++s) {
    const std::string f0 =
        weights_are_buffer ? "weights_cache[" + std::to_string(s) + "].s0123"
                           : "f" + std::to_string(s * 4 + 0);
    const std::string f1 =
        weights_are_buffer ? "weights_cache[" + std::to_string(s) + "].s4567"
                           : "f" + std::to_string(s * 4 + 1);
    const std::string f2 =
        weights_are_buffer ? "weights_cache[" + std::to_string(s) + "].s89ab"
                           : "f" + std::to_string(s * 4 + 2);
    const std::string f3 =
        weights_are_buffer ? "weights_cache[" + std::to_string(s) + "].scdef"
                           : "f" + std::to_string(s * 4 + 3);
    switch (op_def.precision) {
      case CalculationsPrecision::F32:
      case CalculationsPrecision::F16:
        c += "#define CONV" + std::to_string(s) + "(R, S)    \\\n";
        c += "R += S.x * " + f0 + "; \\\n";
        c += "R += S.y * " + f1 + "; \\\n";
        c += "R += S.z * " + f2 + "; \\\n";
        c += "R += S.w * " + f3 + ";   \n";
        break;
      case CalculationsPrecision::F32_F16:
        c += "#define CONV" + std::to_string(s) + "(R, S) \\\n";
        c += "R += convert_float4(S.x * " + f0 + " + S.y * " + f1 +
             " + S.z * " + f2 + " + S.w * " + f3 + ");\n";
        break;
    }
  }

  auto generate_id = [&](const std::string& x, const std::string& y,
                         const std::string& z) {
    std::string id;
    if (src_def.HasAxis(Axis::WIDTH)) {
      id += "_w" + x;
    }
    if (src_def.HasAxis(Axis::HEIGHT)) {
      id += "_h" + y;
    }
    if (src_def.HasAxis(Axis::DEPTH)) {
      id += "_d" + z;
    }
    return id;
  };

  auto generate_id_full = [&](const std::string& x, const std::string& y,
                              const std::string& z, const std::string& s) {
    return generate_id(x, y, z) + "_s" + s;
  };

  auto generate_check = [&](const std::string& x, const std::string& y,
                            const std::string& z) {
    std::string check;
    const std::vector<Axis> axes{Axis::WIDTH, Axis::HEIGHT, Axis::DEPTH};
    const std::vector<std::string> names{"in_x", "in_y", "in_z"};
    const std::vector<std::string> coords{x, y, z};
    for (int i = 0; i < axes.size(); ++i) {
      const auto& axis = axes[i];
      if (src_def.HasAxis(axis) && !src_def.SupportsZeroClamp(axis) &&
          block_size[i] != 1) {
        if (!check.empty()) {
          check += " && ";
        }
        check += names[i] + coords[i];
      }
    }
    return check;
  };

  switch (op_def.precision) {
    case CalculationsPrecision::F32:
      c += "#define FLT16 float16\n";
      break;
    case CalculationsPrecision::F32_F16:
    case CalculationsPrecision::F16:
      c += "#define FLT16 half16\n";
      break;
  }

  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  if (op_def.IsBatchSupported()) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int dst_x = (linear_id / args.dst_tensor.Batch());\n";
    c += "  int B = linear_id % args.dst_tensor.Batch();\n";
    c += "  args.dst_tensor.SetBatchRef(B);\n";
    c += "  args.src_tensor.SetBatchRef(B);\n";
  } else {
    c += "  int dst_x = get_global_id(0);\n";
  }
  c += "  int rem_x = dst_x % args.stride_x;\n";
  c += "  int ceil_x = dst_x / args.stride_x;\n";
  c += "  dst_x = ceil_x * args.stride_x * " + std::to_string(block_size.x) +
       " + rem_x;\n";
  if (src_def.HasAxis(Axis::DEPTH)) {
    c += "  int linear_id_y = get_global_id(1);\n";
    c += "  int dst_y = linear_id_y % args.grid_size_y;\n";
    c += "  int dst_z = linear_id_y / args.grid_size_y;\n";
    c += "  int rem_z = dst_z % args.stride_z;\n";
    c += "  int ceil_z = dst_z / args.stride_z;\n";
    c += "  dst_z = ceil_z * args.stride_z * " + std::to_string(block_size.z) +
         " + rem_z;\n";
    c += "  if (dst_z >= args.dst_tensor.Depth()) return;\n";
  } else {
    c += "  int dst_y = get_global_id(1);\n";
  }
  c += "  int rem_y = dst_y % args.stride_y;\n";
  c += "  int ceil_y = dst_y / args.stride_y;\n";
  c += "  dst_y = ceil_y * args.stride_y * " + std::to_string(block_size.y) +
       " + rem_y;\n";
  c += "  int dst_s = get_global_id(2) * " + std::to_string(block_size.w) +
       ";\n";
  c += "  if (dst_x >= args.dst_tensor.Width() || dst_y >= "
       "args.dst_tensor.Height() || dst_s >= "
       "args.dst_tensor.Slices()) return;\n";
  if (weights_are_buffer) {
    c += "  int f_base = dst_s * args.src_tensor.Slices() * args.kernel_size_x "
         "* args.kernel_size_y";
    if (src_def.HasAxis(Axis::DEPTH)) {
      c += " * args.kernel_size_z";
    }
    c += ";\n";
  }
  for (int s = 0; s < block_size.w; ++s) {
    const std::string sind = std::to_string(s);
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zind = std::to_string(z);
      for (int y = 0; y < block_size.y; ++y) {
        const std::string yind = std::to_string(y);
        for (int x = 0; x < block_size.x; ++x) {
          const std::string xind = std::to_string(x);
          c += "  ACCUM_FLT4 r" + generate_id_full(xind, yind, zind, sind) +
               " = (ACCUM_FLT4)(0.0f, 0.0f, 0.0f, 0.0f);\n";
        }
      }
    }
  }
  c += "  int kernel_first_dst_x = dst_x + args.padding_x;\n";
  c += "  int kernel_first_dst_y = dst_y + args.padding_y;\n";
  c += "  int kernel_last_dst_x = kernel_first_dst_x - args.kernel_size_x;\n";
  c += "  int kernel_last_dst_y = kernel_first_dst_y - args.kernel_size_y;\n";
  c += "  int offset_x = abs(args.padding_x);\n";
  c += "  int offset_x_strided = offset_x * args.stride_x;\n";
  c +=
      "  int src_x = (kernel_first_dst_x + offset_x_strided) / args.stride_x - "
      "offset_x;\n";
  c += "  int offset_y = abs(args.padding_y);\n";
  c += "  int offset_y_strided = offset_y * args.stride_y;\n";
  c +=
      "  int src_y = (kernel_first_dst_y + offset_y_strided) / args.stride_y - "
      "offset_y;\n";
  if (src_def.HasAxis(Axis::DEPTH)) {
    c += "  int kernel_first_dst_z = dst_z + args.padding_z;\n";
    c += "  int kernel_last_dst_z = kernel_first_dst_z - args.kernel_size_z;\n";
    c += "  int offset_z = abs(args.padding_z);\n";
    c += "  int offset_z_strided = offset_z * args.stride_z;\n";
    c += "  int src_z = (kernel_first_dst_z + offset_z_strided) / "
         "args.stride_z - offset_z;\n";
    c += "  int src_as_dst_z = src_z * args.stride_z;\n";
    c +=
        "  for (;src_as_dst_z > kernel_last_dst_z; src_z -= 1, src_as_dst_z -= "
        "args.stride_z) {\n";
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zindex = std::to_string(z);
      c += "    int sz" + zindex + " = src_z + " + zindex + ";\n";
      if (!src_def.SupportsZeroClamp(Axis::DEPTH)) {
        c += "    bool in_z" + zindex + " = sz" + zindex + " >= 0 && sz" +
             zindex + " < args.src_tensor.Depth();\n";
        if (!src_def.CanReadOutOfBorder(Axis::DEPTH)) {
          c += "    sz" + zindex + " = clamp(sz" + zindex +
               ", 0, args.src_tensor.Depth() - 1);\n";
        }
      }
    }
    if (block_size.z == 1 && !src_def.SupportsZeroClamp(Axis::DEPTH)) {
      c += "    if (!in_z0) continue;\n";
    }
    c += "    int kernel_z = kernel_first_dst_z - src_as_dst_z;\n";
    c += "    int src_as_dst_y = src_y * args.stride_y;\n";
    c += "    int src_y_copy = src_y;\n";
    c += "    for (;src_as_dst_y > kernel_last_dst_y; src_y_copy -= 1, "
         "src_as_dst_y -= args.stride_y) {\n";
  } else {
    c += "  int src_as_dst_y = src_y * args.stride_y;\n";
    c += "  for (;src_as_dst_y > kernel_last_dst_y; src_y -= 1, src_as_dst_y "
         "-= args.stride_y) {\n";
  }
  for (int y = 0; y < block_size.y; ++y) {
    const std::string yindex = std::to_string(y);
    const std::string src_y =
        src_def.HasAxis(Axis::DEPTH) ? "src_y_copy" : "src_y";
    c += "    int sy" + yindex + " = " + src_y + " + " + yindex + ";\n";
    if (!src_def.SupportsZeroClamp(Axis::HEIGHT)) {
      c += "    bool in_y" + yindex + " = sy" + yindex + " >= 0 && sy" +
           yindex + " < args.src_tensor.Height();\n";
      if (!src_def.CanReadOutOfBorder(Axis::HEIGHT)) {
        c += "    sy" + yindex + " = clamp(sy" + yindex +
             ", 0, args.src_tensor.Height() - 1);\n";
      }
    }
  }
  if (block_size.y == 1 && !src_def.SupportsZeroClamp(Axis::HEIGHT)) {
    c += "      if (!in_y0) continue;\n";
  }
  c += "    int kernel_y = kernel_first_dst_y - src_as_dst_y;\n";
  c += "    int src_as_dst_x = src_x * args.stride_x;\n";
  c += "    int src_x_copy = src_x;\n";
  c += "    for (;src_as_dst_x > kernel_last_dst_x; src_x_copy -= 1, "
       "src_as_dst_x "
       "-= args.stride_x) {\n";
  for (int x = 0; x < block_size.x; ++x) {
    const std::string xindex = std::to_string(x);
    c += "      int sx" + xindex + " = src_x_copy + " + xindex + ";\n";
    if (!src_def.SupportsZeroClamp(Axis::WIDTH)) {
      c += "      bool in_x" + xindex + " = sx" + xindex + " >= 0 && sx" +
           xindex + " < args.src_tensor.Width();\n";
      if (!src_def.CanReadOutOfBorder(Axis::WIDTH)) {
        c += "      sx" + xindex + " = clamp(sx" + xindex +
             ", 0, args.src_tensor.Width() - 1);\n";
      }
    }
  }
  if (block_size.x == 1 && !src_def.SupportsZeroClamp(Axis::WIDTH)) {
    c += "      if (!in_x0) continue;\n";
  }
  for (int z = 0; z < block_size.z; ++z) {
    const std::string zind = std::to_string(z);
    for (int y = 0; y < block_size.y; ++y) {
      const std::string yind = std::to_string(y);
      for (int x = 0; x < block_size.x; ++x) {
        const std::string xind = std::to_string(x);
        const std::string id = generate_id(xind, yind, zind);
        const std::string check = generate_check(xind, yind, zind);
        std::string coords = "sx" + xind + ", sy" + yind;
        if (src_def.HasAxis(Axis::DEPTH)) {
          coords += ", sz" + zind;
        }
        if (src_def.IsLinear()) {
          c += "      args.src_tensor.GetAddress(addr" + id + ", " + coords +
               ", 0);\n";
        }
        if (src_def.ReturnsZeroForNegOneRead()) {
          c += "      addr" + id + " = select(-1, addr" + id + ", (" + check +
               "));\n";
          c += "      int ds" + id +
               " = select(0, args.src_tensor.SliceStride(), (" + check +
               "));\n";
        }
      }
    }
  }
  if (src_def.storage_type == TensorStorageType::BUFFER) {
    c += "      int ds = args.src_tensor.SliceStride();\n";
  }
  c += "      int kernel_x = kernel_first_dst_x - src_as_dst_x;\n";
  if (src_def.HasAxis(Axis::DEPTH)) {
    c += "      int kernel_index = (kernel_z * args.kernel_size_y + kernel_y) "
         "*  args.kernel_size_x + kernel_x;\n";
  } else {
    c += "      int kernel_index = kernel_y * args.kernel_size_x + kernel_x;\n";
  }
  if (weights_are_buffer) {
    c += "      int f_offset = f_base + kernel_index * "
         "args.src_tensor.Slices() * " +
         std::to_string(block_size.w) + ";\n";
  } else {
    c += "      int x_c = kernel_index * args.src_tensor.Slices();\n";
  }
  c += "      for (int s = 0; s < args.src_tensor.Slices(); ++s) {\n";
  const bool conditional_read = device_info.IsMali();
  for (int z = 0; z < block_size.z; ++z) {
    const std::string zind = std::to_string(z);
    for (int y = 0; y < block_size.y; ++y) {
      const std::string yind = std::to_string(y);
      for (int x = 0; x < block_size.x; ++x) {
        const std::string xind = std::to_string(x);
        const std::string id = generate_id(xind, yind, zind);
        std::string address;
        if (src_def.IsLinear()) {
          address = "addr" + id;
        } else {
          address = "sx" + xind + ", sy" + yind;
          if (src_def.HasAxis(Axis::DEPTH)) {
            address += ", sz" + zind;
          }
          address += ", s";
        }
        if (src_def.ReturnsZeroForNegOneRead()) {
          c += "        FLT4 src" + id + " = args.src_tensor.Read(" + address +
               "); " + address + " += ds" + id + ";\n";
        } else {
          const std::string check = generate_check(xind, yind, zind);
          if (!check.empty()) {
            if (conditional_read) {
              c += "        FLT4 src" + id + " = " + check +
                   " ? args.src_tensor.Read(" + address + ") : (FLT4)(0.0f);\n";
            } else {
              c += "        FLT4 src" + id + " = args.src_tensor.Read(" +
                   address + ") * (FLT)(" + check + ");\n";
            }
          } else {
            c += "        FLT4 src" + id + " = args.src_tensor.Read(" +
                 address + ");\n";
          }
          if (src_def.IsLinear()) {
            c += "        addr" + id + " += ds;\n";
          }
        }
      }
    }
  }
  if (weights_are_buffer) {
    c += "        __global FLT16* weights_cache = "
         "args.weights.GetPtr(f_offset);\n";
    c += "        f_offset += " + std::to_string(block_size.w) + ";\n";
  } else {
    for (int s = 0; s < block_size.w; ++s) {
      c += absl::Substitute(
          R"(        FLT4 f$1 = args.weights0.Read(dst_s + $0, x_c);
        FLT4 f$2 = args.weights1.Read(dst_s + $0, x_c);
        FLT4 f$3 = args.weights2.Read(dst_s + $0, x_c);
        FLT4 f$4 = args.weights3.Read(dst_s + $0, x_c);
)",
          s, s * 4 + 0, s * 4 + 1, s * 4 + 2, s * 4 + 3);
    }
    c += "        x_c++;\n";
  }
  for (int s = 0; s < block_size.w; ++s) {
    const std::string sind = std::to_string(s);
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zind = std::to_string(z);
      for (int y = 0; y < block_size.y; ++y) {
        const std::string yind = std::to_string(y);
        for (int x = 0; x < block_size.x; ++x) {
          const std::string xind = std::to_string(x);
          const std::string id = generate_id(xind, yind, zind);
          const std::string full_id = generate_id_full(xind, yind, zind, sind);
          c += "        CONV" + sind + "(r" + full_id + ", src" + id + ");\n";
        }
      }
    }
  }
  c += "      }\n";
  c += "    }\n";
  c += "  }\n";
  if (src_def.HasAxis(Axis::DEPTH)) {
    c += "  }\n";
  }
  for (int s = 0; s < block_size.w; ++s) {
    const std::string sind = std::to_string(s);
    c += "  if (dst_s < args.dst_tensor.Slices()) {\n";
    c += "    FLT4 bias_val = args.biases.Read(dst_s);\n";
    for (int z = 0; z < block_size.z; ++z) {
      const std::string zind = std::to_string(z);
      for (int y = 0; y < block_size.y; ++y) {
        const std::string yind = std::to_string(y);
        for (int x = 0; x < block_size.x; ++x) {
          const std::string xind = std::to_string(x);
          const std::string id = generate_id_full(xind, yind, zind, sind);
          std::string checks =
              "xc < args.dst_tensor.Width() && yc < args.dst_tensor.Height()";
          std::string coords = "xc, yc";
          c += "    {\n";
          c += "      int xc = dst_x + args.stride_x * " + xind + ";\n";
          c += "      int yc = dst_y + args.stride_y * " + yind + ";\n";
          if (src_def.HasAxis(Axis::DEPTH)) {
            c += "      int zc = dst_z + args.stride_z * " + zind + ";\n";
            checks += " && zc < args.dst_tensor.Depth()";
            coords += ", zc";
          }
          c += "      if (" + checks + ") {\n";
          c += "        FLT4 res = TO_FLT4(r" + id + ") + bias_val;\n";
          c += "        args.dst_tensor.Write(res, " + coords + ", dst_s);\n";
          c += "      }\n";
          c += "    }\n";
        }
      }
    }
    c += "  }\n";
    c += "  dst_s++;\n";
  }
  c += "}\n";
  return c;
}

absl::Status ConvolutionTransposed::BindArguments(ArgumentsBinder* args) {
  if (definition_.src_tensors[0].HasAxis(Axis::DEPTH)) {
    const int aligned_h =
        AlignByN(dst_[0]->Height(), stride_.y * block_size_.y);
    RETURN_IF_ERROR(
        args->SetInt("grid_size_y", DivideRoundUp(aligned_h, block_size_.y)));
  }
  return absl::OkStatus();
}

int3 ConvolutionTransposed::GetGridSize() const {
  const int aligned_w = AlignByN(dst_[0]->Width(), stride_.x * block_size_.x);
  const int aligned_h = AlignByN(dst_[0]->Height(), stride_.y * block_size_.y);
  const int aligned_d = AlignByN(dst_[0]->Depth(), stride_.z * block_size_.z);
  const int grid_x = DivideRoundUp(aligned_w, block_size_.x) * dst_[0]->Batch();
  const int grid_y = DivideRoundUp(aligned_h, block_size_.y) *
                     DivideRoundUp(aligned_d, block_size_.z);
  const int grid_z = DivideRoundUp(dst_[0]->Slices(), block_size_.w);
  return int3(grid_x, grid_y, grid_z);
}

void ConvolutionTransposed::GetPossibleKernelWorkGroups(
    TuningType tuning_type, const DeviceInfo& device_info,
    const KernelInfo& kernel_info, std::vector<int3>* work_groups) const {
  GetPossibleWorkGroupsConv(tuning_type, device_info, kernel_info, grid_size_,
                            work_groups);
}

ConvolutionTransposed CreateConvolutionTransposed(
    const DeviceInfo& device_info, const OperationDef& definition,
    const ConvolutionTransposedAttributes& attr) {
  ConvolutionTransposed result(definition, attr, device_info);

  TensorLinearDescriptor desc;
  desc.storage_type =
      DeduceLinearStorageType(definition.GetPrimaryStorageType());
  desc.element_type = definition.GetDataType();
  desc.UploadLinearData(attr.bias);
  result.args_.AddObject(
      "biases", absl::make_unique<TensorLinearDescriptor>(std::move(desc)));
  return result;
}

ConvolutionTransposed CreateConvolutionTransposed3D(
    const DeviceInfo& device_info, const OperationDef& definition,
    const ConvolutionTransposed3DAttributes& attr) {
  ConvolutionTransposed result(definition, attr, device_info);

  TensorLinearDescriptor desc;
  desc.storage_type =
      DeduceLinearStorageType(definition.GetPrimaryStorageType());
  desc.element_type = definition.GetDataType();
  desc.UploadLinearData(attr.bias);
  result.args_.AddObject(
      "biases", absl::make_unique<TensorLinearDescriptor>(std::move(desc)));
  return result;
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
