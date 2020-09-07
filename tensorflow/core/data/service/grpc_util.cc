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

#include "tensorflow/core/data/service/grpc_util.h"

#include "tensorflow/core/distributed_runtime/rpc/grpc_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/env_time.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/status.h"

namespace tensorflow {
namespace data {
namespace grpc_util {

Status WrapError(const std::string& message, const ::grpc::Status& status) {
  if (status.ok()) {
    return errors::Internal("Expected a non-ok grpc status. Wrapping message: ",
                            message);
  } else {
    return Status(static_cast<tensorflow::error::Code>(status.error_code()),
                  absl::StrCat(message, ": ", status.error_message()));
  }
}

Status Retry(const std::function<Status()>& f, const std::string& description,
             int64 deadline_micros) {
  Status s = f();
  for (int num_retries = 0;; ++num_retries) {
    if (!errors::IsUnavailable(s) && !errors::IsAborted(s) &&
        !errors::IsCancelled(s)) {
      return s;
    }
    int64 now_micros = EnvTime::NowMicros();
    if (now_micros > deadline_micros) {
      return s;
    }
    int64 deadline_with_backoff_micros =
        now_micros + ::tensorflow::ComputeBackoffMicroseconds(num_retries);
    // Wait for a short period of time before retrying. If our backoff would put
    // us past the deadline, we truncate it to ensure our attempt starts before
    // the deadline.
    int64 backoff_until =
        std::min(deadline_with_backoff_micros, deadline_micros);
    int64 wait_time_micros = backoff_until - now_micros;
    if (wait_time_micros > 100 * 1000) {
      LOG(INFO) << "Failed to " << description << ": " << s
                << ". Will retry in " << wait_time_micros / 1000 << "ms.";
    }
    Env::Default()->SleepForMicroseconds(wait_time_micros);
    s = f();
  }
  return s;
}

}  // namespace grpc_util
}  // namespace data
}  // namespace tensorflow
