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
#include "tensorflow/compiler/xla/tests/verified_hlo_module.h"

#include <string>

#include "absl/strings/str_cat.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/test.h"

namespace xla {

Status VerifiedHloModule::Verify() {
  if (computation_count() == 0) {
    // The computation was never built. Nothing to verify.
    return Status::OK();
  }
  return verifier_.Run(this).status();
}

void VerifiedHloModule::VerifyOrAddFailure(const string& message) {
  Status status = Verify();
  if (!status.ok()) {
    ADD_FAILURE() << "HloVerifier failed on module " << name()
                  << (message.empty() ? "" : absl::StrCat(" (", message, ")"))
                  << ": " << status;
    LOG(ERROR) << "Contents of bad module:";
    XLA_LOG_LINES(tensorflow::ERROR, ToString());
  }
}

}  // namespace xla
