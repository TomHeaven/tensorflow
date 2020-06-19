/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/tests/test_macros.h"

#include "tensorflow/core/platform/logging.h"

namespace xla {

static bool InitModule() {
  kDisabledManifestPath = XLA_DISABLED_MANIFEST;
  VLOG(1) << "kDisabledManifestPath: " << kDisabledManifestPath;
  kTestPlatform = XLA_PLATFORM;
  VLOG(1) << "kTestPlatform: " << kTestPlatform;
  return false;
}

static bool module_initialized = InitModule();

}  // namespace xla
