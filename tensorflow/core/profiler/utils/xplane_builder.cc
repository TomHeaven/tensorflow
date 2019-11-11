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
#include "tensorflow/core/profiler/utils/xplane_builder.h"

namespace tensorflow {
namespace profiler {

XEventMetadata* XPlaneBuilder::GetOrCreateEventMetadata(int64 metadata_id) {
  XEventMetadata& metadata = (*plane_->mutable_event_metadata())[metadata_id];
  metadata.set_id(metadata_id);
  return &metadata;
}

XStatMetadata* XPlaneBuilder::GetOrCreateStatMetadata(int64 metadata_id) {
  XStatMetadata& metadata = (*plane_->mutable_stat_metadata())[metadata_id];
  metadata.set_id(metadata_id);
  return &metadata;
}

XEventBuilder XLineBuilder::AddEvent(const XEventMetadata& metadata) {
  XEvent* event = line_->add_events();
  event->set_metadata_id(metadata.id());
  return XEventBuilder(line_, event);
}

XStat* XEventBuilder::AddStat(const XStatMetadata& metadata) {
  XStat* stat = event_->add_stats();
  stat->set_metadata_id(metadata.id());
  return stat;
}

void XEventBuilder::ParseAndAddStatValue(const XStatMetadata& metadata,
                                         absl::string_view value) {
  int64 int_value;
  uint64 uint_value;
  double double_value;
  if (absl::SimpleAtoi(value, &int_value)) {
    AddStatValue(metadata, int_value);
  } else if (absl::SimpleAtoi(value, &uint_value)) {
    AddStatValue(metadata, uint_value);
  } else if (absl::SimpleAtod(value, &double_value)) {
    AddStatValue(metadata, double_value);
  } else {
    AddStatValue(metadata, value);
  }
}

}  // namespace profiler
}  // namespace tensorflow
