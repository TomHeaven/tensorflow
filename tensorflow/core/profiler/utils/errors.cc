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

#include "tensorflow/core/profiler/utils/errors.h"

namespace tensorflow {
namespace profiler {

const absl::string_view kErrorIncompleteStep =
    "Visualization based on incomplete step. No step markers observed and "
    "hence the step time is actually unknown. Instead, we use the trace "
    "duration as the step time. This may happen if your profiling duration "
    "is shorter than the step time. In that case, you may try to profile "
    "longer.";

const absl::string_view kErrorNoStepMarker =
    "Visualization contains on step based analysis. No step markers observed "
    "and hence the step time is actually unknown. This may happen if your "
    "profiling duration is shorter than the step time. In that case, you may "
    "try to profile longer.";

}  // namespace profiler
}  // namespace tensorflow
