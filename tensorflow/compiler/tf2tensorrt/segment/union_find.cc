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

#include "tensorflow/compiler/tf2tensorrt/segment/union_find.h"

#include "absl/strings/str_format.h"
#include "tensorflow/compiler/tf2tensorrt/convert/utils.h"
#include "tensorflow/core/lib/core/errors.h"

#if GOOGLE_CUDA && GOOGLE_TENSORRT

namespace tensorflow {
namespace tensorrt {
namespace segment {

namespace {
template <typename T>
inline bool CheckIfCompatible(const absl::optional<T>& a,
                              const absl::optional<T>& b) {
  if (!a.has_value() && !b.has_value()) {
    return true;
  }
  if (a.has_value() && b.has_value()) {
    return *a == *b;
  }
  return true;
}

template <typename T>
inline bool UnifyValues(absl::optional<T>& a, absl::optional<T>& b) {
  if (a.has_value()) {
    b = a;
  } else {
    a = b;
  }
  return true;
}

template <typename T>
inline absl::optional<T> MergeCompatible(const absl::optional<T>& a,
                                         const absl::optional<T>& b) {
  DCHECK(CheckIfCompatible(a, b));
  return b;
}

}  // namespace

ClusterBatchSize::ClusterBatchSize()
    : has_dynamic_batch_size_(false), static_batch_size_(absl::nullopt) {}

bool ClusterBatchSize::operator==(const ClusterBatchSize& other) {
  return has_dynamic_batch_size_ == other.has_dynamic_batch_size_ &&
         static_batch_size_ == other.static_batch_size_;
}

int ClusterBatchSize::GetStaticBatchSize() const {
  DCHECK(HasStaticBatchSize());
  return static_batch_size_.value();
}

// Sets the batch size assuming that the object doesn't have a batch size yet:
//   a non-negative input value representing a static batch size.
//   a negative input value representing a dynamic batch size.
ClusterBatchSize& ClusterBatchSize::SetBatchSize(int batch_size) {
  if (batch_size < 0) {
    has_dynamic_batch_size_ = true;
    return *this;
  }
  static_batch_size_ = MergeCompatible<int>(static_batch_size_, batch_size);
  return *this;
}

bool ClusterBatchSize::MergeIfCompatible(const ClusterBatchSize& other) {
  if (!CheckIfCompatible(static_batch_size_, other.static_batch_size_)) {
    return false;
  }
  if (other.HasStaticBatchSize()) {
    static_batch_size_ = other.GetStaticBatchSize();
  }
  if (other.HasDynamicBatchSize()) {
    has_dynamic_batch_size_ = true;
  }
  return true;
}

string ClusterBatchSize::ToString() const {
  string s;
  absl::StrAppendFormat(&s, "batch_size=(%d,%d", HasDynamicBatchSize(),
                        HasStaticBatchSize());
  if (HasStaticBatchSize()) {
    absl::StrAppendFormat(&s, ",%d", GetStaticBatchSize());
  }
  absl::StrAppend(&s, ")");
  return s;
}

ClusterProperty::ClusterProperty(const ClusterBatchSize& batch_size,
                                 const DeviceNameUtils::ParsedName& device_name)
    : batch_size_(batch_size), device_name_(device_name) {}

Status ClusterProperty::Merge(const ClusterProperty& other) {
  ClusterBatchSize merged_batch_size(batch_size_);
  if (!merged_batch_size.MergeIfCompatible(other.batch_size_)) {
    return errors::Internal(
        "trying to merge clusters with incompatible batch sizes.");
  }

  absl::optional<DeviceNameUtils::ParsedName> merged_device_name =
      MergeIfCompatible(device_name_, other.device_name_);
  if (!merged_device_name.has_value()) {
    return errors::Internal(
        "trying to merge clusters with incompatible device assignment.");
  }

  batch_size_ = std::move(merged_batch_size);
  device_name_ = std::move(merged_device_name.value());
  return Status::OK();
}

}  // namespace segment
}  // namespace tensorrt
}  // namespace tensorflow

#endif  // GOOGLE_CUDA && GOOGLE_TENSORRT
