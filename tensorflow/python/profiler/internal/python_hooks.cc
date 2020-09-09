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
#include "tensorflow/python/profiler/internal/python_hooks.h"

#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/profiler/utils/xplane_builder.h"
#include "tensorflow/core/profiler/utils/xplane_schema.h"
#include "tensorflow/core/profiler/utils/xplane_utils.h"

namespace tensorflow {
namespace profiler {

namespace py = ::pybind11;

namespace {

template <typename T>
int ProfileFunction(PyObject* obj, PyFrameObject* frame, int what,
                    PyObject* arg) {
  T::GetSingleton()->ProfileFast(frame, what, arg);
  return 0;
}

void SysSetProfileNone() {
  py::object setprofile = py::module::import("sys").attr("setprofile");
  setprofile(py::none());
}

void ThreadingSetProfile(const py::object& callback) {
  py::object setprofile = py::module::import("threading").attr("setprofile");
  setprofile(callback);
}

std::string GetEventName(PyCodeObject* py_code) {
  string filename(py::reinterpret_borrow<py::str>(py_code->co_filename));
  string function;
  if (py_code->co_name == nullptr) {
    function = "<unknown>";
  } else {
    function = py::reinterpret_borrow<py::str>(py_code->co_name);
  }

  return absl::StrCat("$", io::Basename(filename), ":", py_code->co_firstlineno,
                      " ", function);
}

string GetEventName(PyCFunctionObject* py_cfunc) {
  PyObject* module = py_cfunc->m_module;
  string filename;
  bool filename_ok;
#if PY_MAJOR_VERSION < 3
  filename_ok = (module != nullptr && PyString_Check(module));
#else
  filename_ok = (module != nullptr && PyUnicode_Check(module));
#endif
  if (filename_ok) {
    filename = py::reinterpret_borrow<py::str>(module);
  } else {
    filename = "<unknown>";
  }

  return absl::StrCat("$", filename, " ", py_cfunc->m_ml->ml_name);
}

void AddEventToXLine(const PythonTraceEntry& event, XLineBuilder* line,
                     XPlaneBuilder* plane) {
  // TODO(jiesun): maybe add full filename as event stats.
  auto xevent = line->AddEvent(*plane->GetOrCreateEventMetadata(event.Name()));
  xevent.SetTimestampNs(event.start_time_ns);
  xevent.SetEndTimestampNs(event.end_time_ns);
}

}  // namespace

std::string PythonTraceEntry::Name() const {
  std::string event_name;
  if (code_object) {
    return GetEventName(code_object);
  } else if (function_object) {
    return GetEventName(function_object);
  }
  return "<unknown>";
}

PythonHooks* PythonHooks::GetSingleton() {
  static PythonHooks* singleton = new PythonHooks;
  return singleton;
}

void PythonHooks::Start(const PythonHooksOptions& options) {
  if (!Py_IsInitialized()) return;
  options_ = options;
  start_timestamp_ns_ = EnvTime::NowNanos();
  if (options_.enable_python_traceme || options_.enable_trace_python_function) {
    PyGILState_STATE gil_state = PyGILState_Ensure();
    if (options_.enable_trace_python_function) {
      SetProfilerInAllThreads();
    }
    if (options_.enable_python_traceme) {
      EnableTraceMe(true);
    }
    if (options_.end_to_end_mode) {
      // When end to end mode is used, Stop() and Finalize() i.e. symbolization
      // and data collection happens during C's atexit(), when Py_FinalizeEx()
      // already called.
      try {
        auto atexit = py::module::import("atexit");
        atexit.attr("register")(py::cpp_function([]() {
          PythonHooks* singleton = PythonHooks::GetSingleton();
          singleton->Stop();
          singleton->CollectData(&(singleton->end_to_end_xplane_.emplace()));
        }));
      } catch (const py::error_already_set& e) {
        LOG(ERROR) << "Can't install atexit handler for e2e mode." << e.what();
      }
    }
    PyGILState_Release(gil_state);
    active_session_ = true;
  }
}

void PythonHooks::Stop() {
  if (!Py_IsInitialized()) return;
  if (!active_session_) return;  // Makes sure Stop() can be reentrant.
  if (options_.enable_python_traceme || options_.enable_trace_python_function) {
    PyGILState_STATE gil_state = PyGILState_Ensure();
    if (options_.enable_trace_python_function) {
      ClearProfilerInAllThreads();
    }
    if (options_.enable_python_traceme) {
      EnableTraceMe(false);
    }
    PyGILState_Release(gil_state);
    active_session_ = false;
  }
}

void PythonHooks::CollectData(XPlane* raw_plane) {
  DCHECK(raw_plane);
  XPlaneBuilder plane(raw_plane);
  for (auto& it : entries_) {
    uint64 thread_id = it.first;
    auto& thread_events = it.second;
    VLOG(1) << "Collecting " << thread_events.completed.size() << ":"
            << thread_events.active.size() << " events on thread " << thread_id;
    auto line = plane.GetOrCreateLine(thread_id);
    line.SetTimestampNs(start_timestamp_ns_);
    for (const auto& event : thread_events.completed) {
      AddEventToXLine(event, &line, &plane);
    }
    if (options_.include_incomplete_events) {
      uint64 now = EnvTime::NowNanos();
      while (!thread_events.active.empty()) {
        auto& event = thread_events.active.top();
        event.end_time_ns = now;
        AddEventToXLine(event, &line, &plane);
        thread_events.active.pop();
      }
    }
  }
  entries_.clear();
}

void PythonHooks::Finalize(XSpace* space) {
  if (space) {
    XPlane* plane =
        FindOrAddMutablePlaneWithName(space, kPythonTracerPlaneName);
    if (options_.end_to_end_mode) {
      if (end_to_end_xplane_) {
        end_to_end_xplane_->set_name(plane->name());
        plane->Swap(&*end_to_end_xplane_);
        end_to_end_xplane_.reset();
      }
    } else {
      PyGILState_STATE gil_state = PyGILState_Ensure();
      CollectData(plane);
      PyGILState_Release(gil_state);
    }
  }
}

void PythonHooks::ProfileSlow(const py::object& frame, const string& event,
                              const py::object& arg) {
  int what;
  absl::string_view event_name(event);

  if (absl::ConsumePrefix(&event_name, "c_")) {
    if (event_name == "call") {
      what = PyTrace_C_CALL;
    } else if (event_name == "return") {
      what = PyTrace_C_RETURN;
    } else if (event_name == "exception") {
      what = PyTrace_C_EXCEPTION;
    } else {
      return;
    }
  } else {
    if (event_name == "call") {
      what = PyTrace_CALL;
    } else if (event_name == "return") {
      what = PyTrace_RETURN;
    } else if (event_name == "exception") {
      what = PyTrace_EXCEPTION;
    } else {
      return;
    }
  }

  ProfileFast(reinterpret_cast<PyFrameObject*>(frame.ptr()), what, arg.ptr());
}

void PythonHooks::ProfileFast(PyFrameObject* frame, int what, PyObject* arg) {
  const int64 thread_id = Env::Default()->GetCurrentThreadId();
  uint64 now = EnvTime::NowNanos();
  auto& thread_traces = entries_[thread_id];

  switch (what) {
    case PyTrace_CALL: {
      PyCodeObject* f_code = frame->f_code;
      thread_traces.active.emplace(now, 0, f_code, nullptr);
      break;
    }
    case PyTrace_RETURN:
    case PyTrace_EXCEPTION: {
      if (!thread_traces.active.empty()) {
        auto& entry = thread_traces.active.top();
        entry.end_time_ns = now;
        thread_traces.completed.emplace_back(std::move(entry));
        thread_traces.active.pop();
      } else if (options_.include_incomplete_events) {
        PyCodeObject* f_code = frame->f_code;
        thread_traces.completed.emplace_back(start_timestamp_ns_, now, f_code,
                                             nullptr);
      }
      break;
    }
    case PyTrace_C_CALL: {
      if (PyCFunction_Check(arg)) {
        // Python stack does not have a filename/line_no for native calls.
        auto* func = reinterpret_cast<PyCFunctionObject*>(arg);
        entries_[thread_id].active.emplace(now, 0, nullptr, func);
      }
      break;
    }
    case PyTrace_C_RETURN:
    case PyTrace_C_EXCEPTION: {
      if (!thread_traces.active.empty()) {
        auto& entry = thread_traces.active.top();
        entry.end_time_ns = now;
        thread_traces.completed.emplace_back(std::move(entry));
        thread_traces.active.pop();
      } else if (options_.include_incomplete_events) {
        // Only the end of the events is recorded, use profiler start as start.
        if (PyCFunction_Check(arg)) {
          // Python stack does not have a filename/line_no for native calls.
          auto* func = reinterpret_cast<PyCFunctionObject*>(arg);
          entries_[thread_id].completed.emplace_back(start_timestamp_ns_, now,
                                                     nullptr, func);
        }
      }
      break;
    }
    default:
      break;
  }
}

void PythonHooks::SetProfilerInAllThreads() {
  // We also want any new threads started to use our profiler.
  // NOTE: threading does not provide a C API equivalent to
  // `threading.setprofile` so we are forced to go via Python to setup the
  // profile when a new thread is created. After the first callback in that
  // thread we unregister the Python profile function and use
  // `PyEval_SetProfile` to register a C profiler which has significantly less
  // overhead (>2x faster).
  py::cpp_function callback =
      py::cpp_function([this](const py::object& frame, const string& event,
                              const py::object& arg) {
        ProfileSlow(frame, event, arg);
        SysSetProfileNone();
        PyEval_SetProfile(ProfileFunction<PythonHooks>, nullptr);
      });

  ThreadingSetProfile(callback);

  // NOTE: This must be after `threading.setprofile` otherwise we
  // end up recording that in our trace.
  PyThreadState* curr_thread = PyThreadState_Get();
  PyThreadState* next_thread = curr_thread;
  while (next_thread != nullptr) {
    VLOG(1) << "Setting profiler in " << next_thread->thread_id;
    PyThreadState_Swap(next_thread);
    PyEval_SetProfile(ProfileFunction<PythonHooks>, nullptr);
    next_thread = next_thread->next;
  }
  PyThreadState_Swap(curr_thread);
}

void PythonHooks::ClearProfilerInAllThreads() {
  PyThreadState* curr_thread = PyThreadState_Get();
  PyThreadState* next_thread = curr_thread;
  while (next_thread != nullptr) {
    VLOG(1) << "Clearing profiler in " << next_thread->thread_id;
    PyThreadState_Swap(next_thread);
    PyEval_SetProfile(nullptr, nullptr);
    next_thread = next_thread->next;
  }
  PyThreadState_Swap(curr_thread);

  // And notify the threading library that we're done.
  ThreadingSetProfile(py::none());
}

void PythonHooks::EnableTraceMe(bool enable) {
  const char* kModuleName =
      "tensorflow.python.profiler.trace";
  try {
    auto trace_module = py::module::import(kModuleName);
    trace_module.attr("enabled") = py::bool_(enable);
  } catch (const py::error_already_set& e) {
    LOG(ERROR) << "Can't import " << kModuleName;
  }
}

}  // namespace profiler
}  // namespace tensorflow
