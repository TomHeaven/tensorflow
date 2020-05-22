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

// Posix implementation of micro_timer.
// To include this with make, add TAGS=posix.
#include "tensorflow/lite/micro/micro_time.h"

#include <time.h>

namespace tflite {

int32_t ticks_per_second() { return CLOCKS_PER_SEC; }

int32_t GetCurrentTimeTicks() { return clock(); }

}  // namespace tflite
