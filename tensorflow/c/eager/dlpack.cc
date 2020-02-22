/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/c/eager/dlpack.h"
#include "include/dlpack/dlpack.h"  // TF:dlpack
#include "tensorflow/c/eager/c_api_internal.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/platform/casts.h"

#include "tensorflow/core/framework/tensor_reference.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {

namespace {

// Managing context for the DLManagedTensor, will manage the lifetime of
// DLManagedTensor. When calling DLManagedTensor::deleter, it will notify the
// original framework of destruction, and this context will be deleted also.
struct TfDlManagedTensorCtx {
  TensorReference reference;
  std::vector<int64_t> shape;
  std::vector<int64_t> strides;
  DLManagedTensor tensor;

  TfDlManagedTensorCtx(const TensorReference& ref)
      : reference(ref), shape(), tensor() {}
};

const Tensor* GetTensorFromHandle(TFE_TensorHandle* h, TF_Status* status) {
  if (h == nullptr || !h->handle->IsValid(&status->status)) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return nullptr;
  }
  tensorflow::TensorHandle* handle =
      tensorflow::down_cast<tensorflow::TensorHandleInterface*>(h->handle.get())
          ->Handle();

  if (handle->IsRemote()) {
    status->status = tensorflow::errors::InvalidArgument(
        "DLPack doesn't support remote tensor");
    return nullptr;
  }
  const tensorflow::Tensor* tensor;
  status->status = handle->Tensor(&tensor);
  if (!status->status.ok()) {
    return nullptr;
  }
  return tensor;
};

void DLManagedTensorDeleter(DLManagedTensor* arg) {
  TfDlManagedTensorCtx* owner =
      static_cast<TfDlManagedTensorCtx*>(arg->manager_ctx);
  owner->reference.Unref();
  delete owner;
}

DLDataType GetDLDataType(TF_DataType data_type, TF_Status* status) {
  DLDataType dtype;
  dtype.lanes = 1;
  dtype.bits = TF_DataTypeSize(data_type) * 8;
  switch (data_type) {
    case TF_DataType::TF_HALF:
    case TF_DataType::TF_FLOAT:
    case TF_DataType::TF_DOUBLE:
      dtype.code = DLDataTypeCode::kDLFloat;
      break;
    case TF_DataType::TF_INT8:
    case TF_DataType::TF_INT16:
    case TF_DataType::TF_INT32:
    case TF_DataType::TF_INT64:
      dtype.code = DLDataTypeCode::kDLInt;
      break;
    case TF_DataType::TF_BOOL:
    case TF_DataType::TF_UINT8:
    case TF_DataType::TF_UINT16:
    case TF_DataType::TF_UINT32:
    case TF_DataType::TF_UINT64:
      dtype.code = DLDataTypeCode::kDLUInt;
      break;
    case TF_DataType::TF_BFLOAT16:
      dtype.code = DLDataTypeCode::kDLBfloat;
      break;
    default:
      status->status = tensorflow::errors::InvalidArgument(
          DataType_Name(static_cast<DataType>(data_type)),
          " is not supported by dlpack");
      break;
  }
  return dtype;
}

DLContext GetDLContext(TFE_TensorHandle* h, TF_Status* status) {
  DLContext ctx;
  const char* device_name = h->handle->DeviceName(&status->status);
  DeviceNameUtils::ParsedName parsed_name;
  tensorflow::DeviceNameUtils::ParseFullName(device_name, &parsed_name);
  std::string device_type = parsed_name.type;
  int device_id = -1;
  if (parsed_name.has_id) {
    device_id = parsed_name.id;
  }  // Question: Is it possible that it doens't have id?

  ctx.device_id = device_id;
  if (device_type == "CPU") {
    ctx.device_type = DLDeviceType::kDLCPU;
  } else if (device_type == "GPU") {
    ctx.device_type = DLDeviceType::kDLGPU;
  } else {
    status->status = tensorflow::errors::InvalidArgument(
        "Unsupported Device Type for dlpack");
  }

  return ctx;
}

DLManagedTensor* TFEHandleToTfDlManagedTensorCtx(TFE_TensorHandle* h,
                                                 TF_Status* status) {
  const Tensor* tensor = GetTensorFromHandle(h, status);
  TF_DataType data_type = static_cast<TF_DataType>(tensor->dtype());
  TensorReference tensor_ref(*tensor);  // This will call buf_->Ref()

  auto* tf_dlm_tensor_ctx = new TfDlManagedTensorCtx(tensor_ref);
  tf_dlm_tensor_ctx->reference = tensor_ref;

  DLManagedTensor* dlm_tensor = &tf_dlm_tensor_ctx->tensor;
  dlm_tensor->manager_ctx = tf_dlm_tensor_ctx;
  dlm_tensor->deleter = &DLManagedTensorDeleter;
  dlm_tensor->dl_tensor.ctx = GetDLContext(h, status);
  int ndim = tensor->dims();
  dlm_tensor->dl_tensor.ndim = ndim;
  dlm_tensor->dl_tensor.data = TFE_TensorHandleDevicePointer(h, status);
  dlm_tensor->dl_tensor.dtype = GetDLDataType(data_type, status);

  std::vector<int64_t>* shape_arr = &tf_dlm_tensor_ctx->shape;
  std::vector<int64_t>* stride_arr = &tf_dlm_tensor_ctx->strides;
  shape_arr->resize(ndim);
  stride_arr->resize(ndim, 1);
  for (int i = 0; i < ndim; i++) {
    (*shape_arr)[i] = tensor->dim_size(i);
  }
  for (int i = ndim - 2; i >= 0; --i) {
    (*stride_arr)[i] = (*shape_arr)[i + 1] * (*stride_arr)[i + 1];
  }

  dlm_tensor->dl_tensor.shape =
      reinterpret_cast<std::int64_t*>(shape_arr->data());
  // There are two ways to represent compact row-major data
  // 1) nullptr indicates tensor is compact and row-majored.
  // 2) fill in the strides array as the real case for compact row-major data
  // Here we choose option 2, since some framework didn't handle the strides
  // argument properly
  dlm_tensor->dl_tensor.strides =
      reinterpret_cast<std::int64_t*>(stride_arr->data());
  dlm_tensor->dl_tensor.byte_offset =
      0;  // TF doesn't handle the strides and byte_offsets here
  return &tf_dlm_tensor_ctx->tensor;
}

absl::optional<std::string> DeviceNameFromDlContext(const DLContext& ctx,
                                                    TF_Status* status) {
  switch (ctx.device_type) {
    case DLDeviceType::kDLCPU:
      return "CPU:0";
    case DLDeviceType::kDLGPU:
      return absl::StrCat("GPU:", ctx.device_id);
    default:
      return absl::nullopt;
  };
}
TF_DataType TfDataTypeFormDlDataType(const DLDataType& dtype,
                                     TF_Status* status) {
  TF_DataType tf_dtype;
  switch (dtype.code) {
    case DLDataTypeCode::kDLUInt:
      switch (dtype.bits) {
        case 8:
          tf_dtype = TF_DataType::TF_UINT8;
          break;
        case 16:
          tf_dtype = TF_DataType::TF_UINT16;
          break;
        case 32:
          tf_dtype = TF_DataType::TF_UINT32;
          break;
        case 64:
          tf_dtype = TF_DataType::TF_UINT64;
          break;
        default:
          status->status = tensorflow::errors::InvalidArgument(
              "Unsupported UInt bits: ", dtype.bits);
      }
      break;
    case DLDataTypeCode::kDLInt:
      switch (dtype.bits) {
        case 8:
          tf_dtype = TF_DataType::TF_INT8;
          break;
        case 16:
          tf_dtype = TF_DataType::TF_INT16;
          break;
        case 32:
          tf_dtype = TF_DataType::TF_INT32;
          break;
        case 64:
          tf_dtype = TF_DataType::TF_INT64;
          break;
        default:
          status->status = tensorflow::errors::InvalidArgument(
              "Unsupported Int bits: ", dtype.bits);
      }
      break;
    case DLDataTypeCode::kDLFloat:
      switch (dtype.bits) {
        case 16:
          tf_dtype = TF_DataType::TF_HALF;
          break;
        case 32:
          tf_dtype = TF_DataType::TF_FLOAT;
          break;
        case 64:
          tf_dtype = TF_DataType::TF_DOUBLE;
          break;
        default:
          status->status = tensorflow::errors::InvalidArgument(
              "Unsupported Float bits: ", dtype.bits);
      }
      break;
    case DLDataTypeCode::kDLBfloat:
      switch (dtype.bits) {
        case 16:
          tf_dtype = TF_DataType::TF_BFLOAT16;
          break;
        default:
          status->status = tensorflow::errors::InvalidArgument(
              "Unsupported BFloat bits: ", dtype.bits);
      }
      break;
    default:
      status->status = tensorflow::errors::InvalidArgument(
          "Unsupported Type Codes: ", dtype.code);
  }

  return tf_dtype;
}

void DeallocatorWrapperFunc(void* data, size_t len, void* dlmt_vptr) {
  DLManagedTensor* dlmt = static_cast<DLManagedTensor*>(dlmt_vptr);
  dlmt->deleter(const_cast<DLManagedTensor*>(dlmt));
}

bool IsValidStrideCompactRowMajorData(int64_t* shape_arr, int64_t* stride_arr,
                                      int ndim) {
  for (int i = ndim - 2; i >= 0; --i) {
    if (stride_arr[i] != shape_arr[i + 1] * stride_arr[i + 1]) {
      return false;
    };
  }
  return true;
}
}  // namespace

void TFE_CallDLManagedTensorDeleter(void* dlm_ptr) {
  DLManagedTensor* dlMTensor = static_cast<DLManagedTensor*>(dlm_ptr);
  if (dlMTensor->deleter != nullptr) {
    dlMTensor->deleter(dlMTensor);
  }
}

void* TFE_HandleToDLPack(TFE_TensorHandle* h, TF_Status* status) {
  DLManagedTensor* tfdlmtensor = TFEHandleToTfDlManagedTensorCtx(h, status);
  return static_cast<void*>(tfdlmtensor);
}

TFE_TensorHandle* TFE_HandleFromDLPack(void* dlm, TF_Status* status) {
  TFE_ContextOptions* opts = TFE_NewContextOptions();
  TFE_Context* ctx = TFE_NewContext(opts, status);
  DLManagedTensor* dlmt = static_cast<DLManagedTensor*>(dlm);
  DLTensor* dl_tensor = &dlmt->dl_tensor;
  absl::optional<std::string> device_name =
      DeviceNameFromDlContext(dl_tensor->ctx, status);
  if (!device_name.has_value()) {
    status->status =
        tensorflow::errors::InvalidArgument("Unsupported Device Type");
    return nullptr;
  }
  TF_DataType dtype = TfDataTypeFormDlDataType(dl_tensor->dtype, status);
  int num_dims = dl_tensor->ndim;
  const int64_t* dims = dl_tensor->shape;
  void* data = dl_tensor->data;

  size_t total_bytes = dl_tensor->dtype.bits / 8;
  for (int i = 0; i < num_dims; i++) {
    total_bytes *= dims[i];
  }

  if ((dl_tensor->strides != nullptr) &&
      !IsValidStrideCompactRowMajorData(dl_tensor->shape, dl_tensor->strides,
                                        num_dims)) {
    status->status = tensorflow::errors::InvalidArgument(
        "Invalid strides array from DLPack");
    return nullptr;
  }

  TFE_TensorHandle* handle = TFE_NewTensorHandleFromDeviceMemory(
      ctx, device_name.value().c_str(), dtype, dims, num_dims, data,
      total_bytes, &DeallocatorWrapperFunc, &dlmt, status);

  return handle;
};

}  // namespace tensorflow
