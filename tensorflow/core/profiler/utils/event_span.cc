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
#include "tensorflow/core/profiler/utils/event_span.h"

#include <chrono>  // NOLINT
#include <ctime>
#include <thread>  // NOLINT
#include <vector>

#include "absl/strings/match.h"
#include "tensorflow/core/lib/gtl/map_util.h"

namespace tensorflow {
namespace profiler {

namespace {

// Representing a boundary of an event.
struct EventBoundary {
  // Time at this boundary.
  uint64 time_ps;
  // Type of the event.
  EventType type;
  // True if this is the start of the event; False if this is the end.
  bool is_start;
  EventBoundary(uint64 time_ps, EventType type, bool is_start)
      : time_ps(time_ps), type(type), is_start(is_start) {}
};

// Returns true if EventBoundary a should appear before EventBoundary b.
bool CmpEventBoundaries(const EventBoundary& a, const EventBoundary& b) {
  if (a.time_ps == b.time_ps) {
    if (a.is_start == b.is_start) {
      // Puts the higher-priority type before the lower-priority type if they
      // have the same time and same boundary type.
      return a.type > b.type;
    } else {
      // Puts the "end" bounary before the "start" boundary if they have the
      // same time.
      return !a.is_start;
    }
  }
  // In ascending order of time.
  return a.time_ps < b.time_ps;
}

// Generates vector of event boundaries from the given overlapped_events.
std::vector<EventBoundary> GenerateEventBoundaries(
    const std::vector<EventTypeSpan>& overlapped_events) {
  std::vector<EventBoundary> boundaries;
  boundaries.reserve(2 * overlapped_events.size());
  for (const auto& event : overlapped_events) {
    boundaries.push_back(
        {event.span.begin_ps(), event.type, /*is_start=*/true});
    boundaries.push_back({event.span.end_ps(), event.type, /*is_start=*/false});
  }
  absl::c_sort(boundaries, CmpEventBoundaries);
  return boundaries;
}

// A class to track the highest priority that an event should be assigned.
class PriorityTracker {
 private:
  // The current maximum priority.
  EventType current_max_priority_;
  // A count for each possible priority.
  std::vector<int64> priority_count_;

 public:
  PriorityTracker() {
    current_max_priority_ = UNKNOWN_TIME;
    priority_count_.resize(LAST_EVENT_TYPE + 1, 0);
  }
  // Updates current_max_priority_ and priority_count_[] given the boundary.
  // Returns the new current_max_priority_.
  EventType Update(const EventBoundary& boundary) {
    EventType event_type = boundary.type;
    bool is_start = boundary.is_start;
    if (is_start) {
      priority_count_[event_type]++;
      if (event_type > current_max_priority_) {
        current_max_priority_ = event_type;
      }
    } else {
      priority_count_[event_type]--;
      if (event_type == current_max_priority_ &&
          priority_count_[event_type] == 0) {
        // Reduces current_max_priority_ to the first event type (starting from
        // the highest priority) that has a non-zero count.
        bool found = false;
        for (int i = event_type - 1; i >= 0; i--) {
          if (priority_count_[i] > 0) {
            current_max_priority_ = static_cast<EventType>(i);
            found = true;
            break;
          }
        }
        if (!found) current_max_priority_ = UNKNOWN_TIME;
      }
    }
    return current_max_priority_;
  }
};

std::vector<EventTypeSpan> ToNonOverlappedEvents(
    const std::vector<EventTypeSpan>& overlapped_events) {
  std::vector<EventBoundary> event_boundaries =
      GenerateEventBoundaries(overlapped_events);
  std::vector<EventTypeSpan> result;
  result.reserve(event_boundaries.size());
  PriorityTracker priority_tracker;
  for (int64 i = 0; i < (event_boundaries.size() - 1); i++) {
    EventType highest_priority = priority_tracker.Update(event_boundaries[i]);
    result.push_back({highest_priority, Timespan::FromEndPoints(
                                            event_boundaries[i].time_ps,
                                            event_boundaries[i + 1].time_ps)});
  }
  return result;
}

void CombineStepDetails(const StepDetails& src, StepDetails* dst) {
  dst->AppendMarkers(src.Markers());
  dst->AppendEvents(src.Events());
}

}  // namespace

EventType ClassifyGpuEvent(absl::string_view event_name) {
  if (absl::StartsWithIgnoreCase(event_name, "MEMCPYHtoD"))
    return HOST_TO_DEVICE;
  if (absl::StartsWithIgnoreCase(event_name, "MEMCPYDtoH"))
    return DEVICE_TO_HOST;
  if (absl::StartsWithIgnoreCase(event_name, "MEMCPYDtoD"))
    return DEVICE_TO_DEVICE;
  return DEVICE_COMPUTE;
}

EventType ClassifyCpuEvent(absl::string_view event_name, int64 correlation_id) {
  if (absl::StartsWithIgnoreCase(event_name, "MEMCPYHtoD") ||
      absl::StrContains(event_name, "Infeed"))
    return HOST_TO_DEVICE;
  if (absl::StartsWithIgnoreCase(event_name, "MEMCPYHtoH")) return HOST_TO_HOST;
  if (correlation_id >= 0 ||
      absl::StartsWithIgnoreCase(event_name, "ExecutorState::Process")) {
    return HOST_PREPARE;
  }
  if (absl::StartsWithIgnoreCase(event_name, "IteratorGetNext"))
    return HOST_WAIT_INPUT;
  return HOST_COMPUTE;
}

std::string PrintEventType(EventType event_type) {
  switch (event_type) {
    case UNKNOWN_TIME:
      return "unknown_time";
    case HOST_COMPUTE:
      return "host_compute";
    case HOST_COMPILE:
      return "host_compile";
    case HOST_TO_HOST:
      return "host_to_host";
    case HOST_TO_DEVICE:
      return "host_to_device";
    case HOST_PREPARE:
      return "host_prepare";
    case HOST_WAIT_INPUT:
      return "host_wait_input";
    case DEVICE_TO_DEVICE:
      return "device_to_device";
    case DEVICE_TO_HOST:
      return "device_to_host";
    case DEVICE_COMPUTE:
      return "device_compute";
    case DEVICE_WAIT_DEVICE:
      return "device_wait_device";
    case DEVICE_WAIT_HOST:
      return "device_wait_host";
    default:
      return "unexpected";
  }
}

std::string PrintEventTypeSpan(const EventTypeSpan& event_type_span) {
  return absl::StrCat("(", PrintEventType(event_type_span.type), ", ",
                      event_type_span.span.DebugString(), ")");
}

std::string PrintStepMarker(const StepMarker& step_marker) {
  std::string device_or_host = step_marker.on_device ? "device" : "host";
  return absl::StrCat("(", device_or_host, ", ", step_marker.event_name, ", ",
                      step_marker.span.DebugString(), ")");
}

std::string PrintStepEvents(const StepEvents& step_events) {
  std::vector<int64> step_ids;
  step_ids.reserve(step_events.size());
  for (const auto& id_details : step_events) {
    step_ids.push_back(id_details.first);
  }
  absl::c_sort(step_ids);
  std::string result = "{";
  for (auto id : step_ids) {
    absl::StrAppend(&result, "\n");
    auto* details = gtl::FindOrNull(step_events, id);
    std::string details_str = details ? details->DebugString() : "()";
    absl::StrAppend(&result, id, ":", details_str);
  }
  return absl::StrCat(result, "\n}");
}

void CombineStepEvents(const StepEvents& src, StepEvents* dst) {
  for (const auto& step_details : src) {
    int64 step_id = step_details.first;
    const StepDetails& src_details = step_details.second;
    StepDetails* dst_details = &(*dst)[step_id];
    CombineStepDetails(src_details, dst_details);
  }
}

// Converts from overlapped step-events to non-overlapped step-events.
StepEvents ToNonOverlappedStepEvents(const StepEvents& overlapped_step_events) {
  auto start_time = std::chrono::steady_clock::now();
  StepEvents non_overlapped_step_events;

  // We could parallelize the following loop if necessary.
  for (const auto& step_events : overlapped_step_events) {
    const auto& step_id = step_events.first;
    const auto& step_details = step_events.second;
    *non_overlapped_step_events[step_id].MutableMarkers() =
        step_details.Markers();
    *non_overlapped_step_events[step_id].MutableEvents() =
        ToNonOverlappedEvents(step_details.Events());
  }
  auto end_time = std::chrono::steady_clock::now();
  auto elapsed_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  double elapsed_time_ms = elapsed_time_us.count() / 1000.0;
  LOG(INFO) << "Generation of step-events took " << elapsed_time_ms << " ms"
            << std::endl;
  return non_overlapped_step_events;
}

void StepDetails::AddMarker(const StepMarker& m) { markers_.push_back(m); }

void StepDetails::AddEvent(const EventTypeSpan& e) { events_.push_back(e); }

void StepDetails::AppendMarkers(const std::vector<StepMarker>& other_markers) {
  markers_.insert(markers_.end(), other_markers.begin(), other_markers.end());
}

void StepDetails::AppendEvents(const std::vector<EventTypeSpan>& other_events) {
  events_.insert(events_.end(), other_events.begin(), other_events.end());
}

Timespan StepDetails::StepTime() const {
  // If there are multiple step-markers, uses the one that has the maximum
  // duration.
  Timespan max_steptime;
  for (const auto& marker : markers_) {
    const Timespan& timespan = marker.span;
    if (timespan.duration_ps() > max_steptime.duration_ps())
      max_steptime = timespan;
  }
  return max_steptime;
}

std::string StepDetails::DebugString() const {
  std::string result = "([";
  for (int i = 0; i < markers_.size(); i++) {
    if (i > 0) absl::StrAppend(&result, ", ");
    absl::StrAppend(&result, PrintStepMarker(markers_[i]));
  }
  absl::StrAppend(&result, "], [");
  for (int i = 0; i < events_.size(); i++) {
    if (i > 0) absl::StrAppend(&result, ", ");
    absl::StrAppend(&result, PrintEventTypeSpan(events_[i]));
  }
  return absl::StrCat(result, "])");
}

bool StepDetails::operator==(const StepDetails& other) const {
  const auto& other_markers = other.Markers();
  if (markers_.size() != other_markers.size()) return false;
  for (uint64 i = 0; i < markers_.size(); i++) {
    if (markers_[i] != other_markers[i]) return false;
  }
  const auto& other_events = other.Events();
  if (events_.size() != other_events.size()) return false;
  for (uint64 i = 0; i < events_.size(); i++) {
    if (events_[i] != other_events[i]) return false;
  }
  return true;
}

bool operator==(const StepEvents& a, const StepEvents& b) {
  if (a.size() != b.size()) return false;
  for (const auto& id_details : a) {
    const auto a_id = id_details.first;
    const auto& a_details = id_details.second;
    const auto* b_details = gtl::FindOrNull(b, a_id);
    if (b_details == nullptr) return false;
    if (a_details != *b_details) return false;
  }
  return true;
}

}  // namespace profiler
}  // namespace tensorflow
