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
#include "tensorflow/compiler/tf2xla/frontend_attributes_util.h"

#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/lib/core/errors.h"

namespace tensorflow {

namespace {
const char kFrontendAttributesAttribute[] = "_XlaFrontendAttributes";
}  // namespace

xla::StatusOr<absl::optional<xla::FrontendAttributes>>
GetFrontendAttributesFromNodeDef(const AttrSlice& attrs) {
  auto attr = attrs.Find(kFrontendAttributesAttribute);
  if (attr == nullptr) {
    return xla::StatusOr<absl::optional<xla::FrontendAttributes>>(
        absl::nullopt);
  }
  xla::FrontendAttributes attributes;
  if (!attributes.ParseFromString(attr->s())) {
    return errors::InvalidArgument(
        "Experimental _XlaFrontendAttributes attribute was not a valid encoded "
        "xla::FrontendAttributes proto.");
  }
  return absl::optional<xla::FrontendAttributes>(attributes);
}

}  // namespace tensorflow
