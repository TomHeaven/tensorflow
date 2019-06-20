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

#include "tensorflow/compiler/jit/xla_activity_listener.h"

#include "absl/synchronization/mutex.h"
#include "tensorflow/core/lib/core/errors.h"

namespace tensorflow {
namespace {
// The list of all registered `XlaActivityListener`s.
struct XlaActivityListenerList {
  absl::Mutex mutex;
  std::vector<std::unique_ptr<XlaActivityListener>> listeners GUARDED_BY(mutex);
};

XlaActivityListenerList* GetXlaActivityListenerList() {
  static XlaActivityListenerList* listener_list = new XlaActivityListenerList;
  return listener_list;
}

template <typename FnTy>
Status ForEachListener(FnTy fn) {
  XlaActivityListenerList* listener_list = GetXlaActivityListenerList();
  absl::ReaderMutexLock reader_lock(&listener_list->mutex);

  for (const std::unique_ptr<XlaActivityListener>& listener :
       listener_list->listeners) {
    TF_RETURN_IF_ERROR(fn(listener.get()));
  }

  return Status::OK();
}
}  // namespace

Status BroadcastXlaActivity(
    XlaAutoClusteringActivity auto_clustering_activity) {
  return ForEachListener([&](XlaActivityListener* listener) {
    return listener->Listen(auto_clustering_activity);
  });
}

Status BroadcastXlaActivity(
    XlaJitCompilationActivity jit_compilation_activity) {
  return ForEachListener([&](XlaActivityListener* listener) {
    return listener->Listen(jit_compilation_activity);
  });
}

void RegisterXlaActivityListener(
    std::unique_ptr<XlaActivityListener> listener) {
  XlaActivityListenerList* listener_list = GetXlaActivityListenerList();
  absl::WriterMutexLock writer_lock(&listener_list->mutex);

  listener_list->listeners.push_back(std::move(listener));
}

XlaActivityListener::~XlaActivityListener() {}

}  // namespace tensorflow
