/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0(the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/c/experimental/stream_executor/stream_executor.h"

#include "tensorflow/c/experimental/stream_executor/stream_executor_internal.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/protobuf/error_codes.pb.h"
#include "tensorflow/stream_executor/event.h"
#include "tensorflow/stream_executor/multi_platform_manager.h"
#include "tensorflow/stream_executor/stream.h"
#include "tensorflow/stream_executor/stream_executor_pimpl.h"
#include "tensorflow/stream_executor/timer.h"

struct SP_Stream_st {
  explicit SP_Stream_st(int id) : stream_id(id) {}
  int stream_id;
};

struct SP_Event_st {
  explicit SP_Event_st(int id) : event_id(id) {}
  int event_id;
};

struct SP_Timer_st {
  explicit SP_Timer_st(int id) : timer_id(id) {}
  int timer_id;
};

namespace stream_executor {
namespace {
constexpr int DEVICE_COUNT = 2;
constexpr char DEVICE_NAME[] = "MyDevice";
constexpr char DEVICE_TYPE[] = "GPU";

/*** Create SP_StreamExecutor (with empty functions) ***/
void allocate(const SP_Device* const device, uint64_t size,
              int64_t memory_space, SP_DeviceMemoryBase* const mem) {}
void deallocate(const SP_Device* const device, SP_DeviceMemoryBase* const mem) {
}
void* host_memory_allocate(const SP_Device* const device, uint64_t size) {
  return nullptr;
}
void host_memory_deallocate(const SP_Device* const device, void* mem) {}
TF_Bool get_allocator_stats(const SP_Device* const device,
                            SP_AllocatorStats* const stats) {
  return true;
}
TF_Bool device_memory_usage(const SP_Device* const device, int64_t* const free,
                            int64_t* const total) {
  return true;
}
void create_stream(const SP_Device* const device, SP_Stream* stream,
                   TF_Status* const status) {
  stream = nullptr;
}
void destroy_stream(const SP_Device* const device, SP_Stream stream) {}
void create_stream_dependency(const SP_Device* const device,
                              SP_Stream dependent, SP_Stream other,
                              TF_Status* const status) {}
void get_stream_status(const SP_Device* const device, SP_Stream stream,
                       TF_Status* const status) {}
void create_event(const SP_Device* const device, SP_Event* event,
                  TF_Status* const status) {
  event = nullptr;
}
void destroy_event(const SP_Device* const device, SP_Event event) {}
SE_EventStatus get_event_status(const SP_Device* const device, SP_Event event) {
  return SE_EVENT_UNKNOWN;
}
void record_event(const SP_Device* const device, SP_Stream stream,
                  SP_Event event, TF_Status* const status) {}
void wait_for_event(const SP_Device* const device, SP_Stream stream,
                    SP_Event event, TF_Status* const status) {}
void create_timer(const SP_Device* const device, SP_Timer* timer,
                  TF_Status* const status) {}
void destroy_timer(const SP_Device* const device, SP_Timer timer) {}
void start_timer(const SP_Device* const device, SP_Stream stream,
                 SP_Timer timer, TF_Status* const status) {}
void stop_timer(const SP_Device* const device, SP_Stream stream, SP_Timer timer,
                TF_Status* const status) {}
void memcpy_dtoh(const SP_Device* const device, SP_Stream stream,
                 void* host_dst, const SP_DeviceMemoryBase* const device_src,
                 uint64_t size, TF_Status* const status) {}
void memcpy_htod(const SP_Device* const device, SP_Stream stream,
                 SP_DeviceMemoryBase* const device_dst, const void* host_src,
                 uint64_t size, TF_Status* const status) {}
void sync_memcpy_dtoh(const SP_Device* const device, void* host_dst,
                      const SP_DeviceMemoryBase* const device_src,
                      uint64_t size, TF_Status* const status) {}
void sync_memcpy_htod(const SP_Device* const device,
                      SP_DeviceMemoryBase* const device_dst,
                      const void* host_src, uint64_t size,
                      TF_Status* const status) {}
void block_host_for_event(const SP_Device* const device, SP_Event event,
                          TF_Status* const status) {}
void synchronize_all_activity(const SP_Device* const device,
                              TF_Status* const status) {}
TF_Bool host_callback(SP_Device* const device, SP_Stream stream,
                      SE_StatusCallbackFn const callback_fn,
                      void* const callback_arg) {
  return true;
}

void PopulateDefaultStreamExecutor(SP_StreamExecutor* se) {
  se->struct_size = SP_STREAMEXECUTOR_STRUCT_SIZE;
  se->allocate = allocate;
  se->deallocate = deallocate;
  se->host_memory_allocate = host_memory_allocate;
  se->host_memory_deallocate = host_memory_deallocate;
  se->get_allocator_stats = get_allocator_stats;
  se->device_memory_usage = device_memory_usage;
  se->create_stream = create_stream;
  se->destroy_stream = destroy_stream;
  se->create_stream_dependency = create_stream_dependency;
  se->get_stream_status = get_stream_status;
  se->create_event = create_event;
  se->destroy_event = destroy_event;
  se->get_event_status = get_event_status;
  se->record_event = record_event;
  se->wait_for_event = wait_for_event;
  se->create_timer = create_timer;
  se->destroy_timer = destroy_timer;
  se->start_timer = start_timer;
  se->stop_timer = stop_timer;
  se->memcpy_dtoh = memcpy_dtoh;
  se->memcpy_htod = memcpy_htod;
  se->sync_memcpy_dtoh = sync_memcpy_dtoh;
  se->sync_memcpy_htod = sync_memcpy_htod;
  se->block_host_for_event = block_host_for_event;
  se->synchronize_all_activity = synchronize_all_activity;
  se->host_callback = host_callback;
}

/*** Create SP_TimerFns ***/
uint64_t nanoseconds(SP_Timer timer) { return timer->timer_id; }

void PopulateDefaultTimerFns(SP_TimerFns* timer_fns) {
  timer_fns->nanoseconds = nanoseconds;
}

/*** Create SP_Platform ***/
void create_timer_fns(const SP_Platform* platform, SP_TimerFns* timer_fns,
                      TF_Status* status) {
  TF_SetStatus(status, TF_OK, "");
  PopulateDefaultTimerFns(timer_fns);
}
void destroy_timer_fns(const SP_Platform* platform, SP_TimerFns* timer_fns) {}

void create_stream_executor(const SP_Platform* platform,
                            SE_CreateStreamExecutorParams* params,
                            TF_Status* status) {
  TF_SetStatus(status, TF_OK, "");
  PopulateDefaultStreamExecutor(params->stream_executor);
}
void destroy_stream_executor(const SP_Platform* platform,
                             SP_StreamExecutor* se) {}

void create_device(const SP_Platform* platform, SE_CreateDeviceParams* params,
                   TF_Status* status) {
  TF_SetStatus(status, TF_OK, "");
  params->device->struct_size = SP_DEVICE_STRUCT_SIZE;
}
void destroy_device(const SP_Platform* platform, SP_Device* device) {}

void PopulateDefaultPlatform(SP_Platform* platform,
                             SP_PlatformFns* platform_fns) {
  platform->struct_size = SP_PLATFORM_STRUCT_SIZE;
  platform->name = DEVICE_NAME;
  platform->type = DEVICE_TYPE;
  platform->visible_device_count = DEVICE_COUNT;
  platform_fns->create_device = create_device;
  platform_fns->destroy_device = destroy_device;
  platform_fns->create_stream_executor = create_stream_executor;
  platform_fns->destroy_stream_executor = destroy_stream_executor;
  platform_fns->create_timer_fns = create_timer_fns;
  platform_fns->destroy_timer_fns = destroy_timer_fns;
}

void destroy_platform(SP_Platform* const platform) {}
void destroy_platform_fns(SP_PlatformFns* const platform_fns) {}

/*** Registration tests ***/
TEST(StreamExecutor, SuccessfulRegistration) {
  auto plugin_init = [](SE_PlatformRegistrationParams* const params,
                        TF_Status* const status) -> void {
    TF_SetStatus(status, TF_OK, "");
    PopulateDefaultPlatform(params->platform, params->platform_fns);
    params->destroy_platform = destroy_platform;
    params->destroy_platform_fns = destroy_platform_fns;
  };
  port::Status status = RegisterDevicePlugin(plugin_init);
  TF_ASSERT_OK(status);
  port::StatusOr<Platform*> maybe_platform =
      MultiPlatformManager::PlatformWithName("MyDevice");
  TF_ASSERT_OK(maybe_platform.status());
  Platform* platform = maybe_platform.ConsumeValueOrDie();
  ASSERT_EQ(platform->Name(), DEVICE_NAME);
  ASSERT_EQ(platform->VisibleDeviceCount(), DEVICE_COUNT);

  port::StatusOr<StreamExecutor*> maybe_executor =
      platform->ExecutorForDevice(0);
  TF_ASSERT_OK(maybe_executor.status());
  StreamExecutor* executor = maybe_executor.ConsumeValueOrDie();
  ASSERT_EQ(executor->GetDeviceDescription().name(), "MyDevice");
}

TEST(StreamExecutor, NameNotSet) {
  auto plugin_init = [](SE_PlatformRegistrationParams* const params,
                        TF_Status* const status) -> void {
    TF_SetStatus(status, TF_OK, "");
    PopulateDefaultPlatform(params->platform, params->platform_fns);
    params->platform->name = nullptr;
    params->destroy_platform = destroy_platform;
    params->destroy_platform_fns = destroy_platform_fns;
  };

  port::Status status = RegisterDevicePlugin(plugin_init);
  ASSERT_EQ(status.code(), tensorflow::error::FAILED_PRECONDITION);
  ASSERT_EQ(status.error_message(), "'name' field in SP_Platform must be set.");
}

TEST(StreamExecutor, CreateDeviceNotSet) {
  auto plugin_init = [](SE_PlatformRegistrationParams* const params,
                        TF_Status* const status) -> void {
    TF_SetStatus(status, TF_OK, "");
    PopulateDefaultPlatform(params->platform, params->platform_fns);
    params->platform_fns->create_device = nullptr;
    params->destroy_platform = destroy_platform;
    params->destroy_platform_fns = destroy_platform_fns;
  };

  port::Status status = RegisterDevicePlugin(plugin_init);
  ASSERT_EQ(status.code(), tensorflow::error::FAILED_PRECONDITION);
  ASSERT_EQ(status.error_message(),
            "'create_device' field in SP_PlatformFns must be set.");
}

TEST(StreamExecutor, UnifiedMemoryAllocateNotSet) {
  auto plugin_init = [](SE_PlatformRegistrationParams* const params,
                        TF_Status* const status) -> void {
    TF_SetStatus(status, TF_OK, "");
    PopulateDefaultPlatform(params->platform, params->platform_fns);
    params->platform->supports_unified_memory = true;
    params->destroy_platform = destroy_platform;
    params->destroy_platform_fns = destroy_platform_fns;
  };

  port::Status status = RegisterDevicePlugin(plugin_init);
  ASSERT_EQ(status.code(), tensorflow::error::FAILED_PRECONDITION);
  ASSERT_EQ(
      status.error_message(),
      "'unified_memory_allocate' field in SP_StreamExecutor must be set.");
}

/*** StreamExecutor behavior tests ***/
class StreamExecutorTest : public ::testing::Test {
 protected:
  StreamExecutorTest() {}
  void SetUp() override {
    PopulateDefaultPlatform(&platform_, &platform_fns_);
    PopulateDefaultStreamExecutor(&se_);
    PopulateDefaultTimerFns(&timer_fns_);
  }
  void TearDown() override {}

  StreamExecutor* GetExecutor(int ordinal) {
    if (!cplatform_) {
      cplatform_ = absl::make_unique<CPlatform>(
          platform_, destroy_platform, platform_fns_, destroy_platform_fns, se_,
          timer_fns_);
    }
    port::StatusOr<StreamExecutor*> maybe_executor =
        cplatform_->ExecutorForDevice(ordinal);
    TF_CHECK_OK(maybe_executor.status());
    return maybe_executor.ConsumeValueOrDie();
  }
  SP_Platform platform_;
  SP_PlatformFns platform_fns_;
  SP_StreamExecutor se_;
  SP_TimerFns timer_fns_;
  std::unique_ptr<CPlatform> cplatform_;
};

TEST_F(StreamExecutorTest, Allocate) {
  se_.allocate = [](const SP_Device* const device, uint64_t size,
                    int64_t memory_space, SP_DeviceMemoryBase* const mem) {
    mem->struct_size = SP_DEVICE_MEMORY_BASE_STRUCT_SIZE;
    mem->opaque = malloc(size);
    mem->size = size;
  };
  se_.deallocate = [](const SP_Device* const device,
                      SP_DeviceMemoryBase* const mem) {
    EXPECT_EQ(mem->size, 2 * sizeof(int));
    free(mem->opaque);
    mem->opaque = nullptr;
    mem->size = 0;
  };
  StreamExecutor* executor = GetExecutor(0);
  DeviceMemory<int> mem = executor->AllocateArray<int>(2);
  ASSERT_NE(mem.opaque(), nullptr);
  ASSERT_EQ(mem.size(), 2 * sizeof(int));
  executor->Deallocate(&mem);
  ASSERT_EQ(mem.opaque(), nullptr);
}

TEST_F(StreamExecutorTest, HostMemoryAllocate) {
  static bool allocate_called = false;
  static bool deallocate_called = false;
  se_.host_memory_allocate = [](const SP_Device* const device, uint64_t size) {
    allocate_called = true;
    return malloc(size);
  };
  se_.host_memory_deallocate = [](const SP_Device* const device, void* mem) {
    free(mem);
    deallocate_called = true;
  };
  StreamExecutor* executor = GetExecutor(0);
  ASSERT_FALSE(allocate_called);
  void* mem = executor->HostMemoryAllocate(8);
  ASSERT_NE(mem, nullptr);
  ASSERT_TRUE(allocate_called);
  ASSERT_FALSE(deallocate_called);
  executor->HostMemoryDeallocate(mem);
  ASSERT_TRUE(deallocate_called);
}

TEST_F(StreamExecutorTest, UnifiedMemoryAllocate) {
  static bool allocate_called = false;
  static bool deallocate_called = false;
  se_.unified_memory_allocate = [](const SP_Device* const device,
                                   uint64_t size) {
    allocate_called = true;
    return malloc(size);
  };
  se_.unified_memory_deallocate = [](const SP_Device* const device, void* mem) {
    free(mem);
    deallocate_called = true;
  };
  StreamExecutor* executor = GetExecutor(0);
  ASSERT_FALSE(allocate_called);
  void* mem = executor->UnifiedMemoryAllocate(8);
  ASSERT_NE(mem, nullptr);
  ASSERT_TRUE(allocate_called);
  ASSERT_FALSE(deallocate_called);
  executor->UnifiedMemoryDeallocate(mem);
  ASSERT_TRUE(deallocate_called);
}

TEST_F(StreamExecutorTest, GetAllocatorStats) {
  se_.get_allocator_stats = [](const SP_Device* const device,
                               SP_AllocatorStats* const stat) -> TF_Bool {
    stat->struct_size = SP_ALLOCATORSTATS_STRUCT_SIZE;
    stat->bytes_in_use = 123;
    return true;
  };

  StreamExecutor* executor = GetExecutor(0);
  absl::optional<AllocatorStats> optional_stats = executor->GetAllocatorStats();
  ASSERT_TRUE(optional_stats.has_value());
  AllocatorStats stats = optional_stats.value();
  ASSERT_EQ(stats.bytes_in_use, 123);
}

TEST_F(StreamExecutorTest, DeviceMemoryUsage) {
  se_.device_memory_usage = [](const SP_Device* const device,
                               int64_t* const free,
                               int64_t* const total) -> TF_Bool {
    *free = 45;
    *total = 7;
    return true;
  };

  StreamExecutor* executor = GetExecutor(0);
  int64 free = 0;
  int64 total = 0;
  executor->DeviceMemoryUsage(&free, &total);
  ASSERT_EQ(free, 45);
  ASSERT_EQ(total, 7);
}

TEST_F(StreamExecutorTest, CreateStream) {
  static bool stream_created = false;
  static bool stream_deleted = false;
  se_.create_stream = [](const SP_Device* const device, SP_Stream* stream,
                         TF_Status* const status) -> void {
    *stream = new SP_Stream_st(14);
    stream_created = true;
  };
  se_.destroy_stream = [](const SP_Device* const device,
                          SP_Stream stream) -> void {
    auto custom_stream = static_cast<SP_Stream_st*>(stream);
    ASSERT_EQ(custom_stream->stream_id, 14);
    delete custom_stream;
    stream_deleted = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  ASSERT_FALSE(stream_created);
  Stream* stream = new Stream(executor);
  stream->Init();
  ASSERT_TRUE(stream->ok());
  ASSERT_TRUE(stream_created);
  ASSERT_FALSE(stream_deleted);
  delete stream;
  ASSERT_TRUE(stream_deleted);
}

TEST_F(StreamExecutorTest, CreateStreamDependency) {
  static bool create_stream_dependency_called = false;
  se_.create_stream_dependency = [](const SP_Device* const device,
                                    SP_Stream dependent, SP_Stream other,
                                    TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    create_stream_dependency_called = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  Stream dependent(executor);
  dependent.Init();
  Stream other(executor);
  other.Init();
  ASSERT_FALSE(create_stream_dependency_called);
  dependent.ThenWaitFor(&other);
  ASSERT_TRUE(create_stream_dependency_called);
}

TEST_F(StreamExecutorTest, StreamStatus) {
  static bool status_ok = true;
  se_.get_stream_status = [](const SP_Device* const device, SP_Stream stream,
                             TF_Status* const status) -> void {
    if (status_ok) {
      TF_SetStatus(status, TF_OK, "");
    } else {
      TF_SetStatus(status, TF_INTERNAL, "Test error");
    }
  };

  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());
  TF_ASSERT_OK(stream.RefreshStatus());
  status_ok = false;
  auto updated_status = stream.RefreshStatus();
  ASSERT_FALSE(stream.ok());
  ASSERT_EQ(updated_status.error_message(), "Test error");
}

TEST_F(StreamExecutorTest, CreateEvent) {
  static bool event_created = false;
  static bool event_deleted = false;
  se_.create_event = [](const SP_Device* const device, SP_Event* event,
                        TF_Status* const status) -> void {
    *event = new SP_Event_st(123);
    event_created = true;
  };
  se_.destroy_event = [](const SP_Device* const device,
                         SP_Event event) -> void {
    auto custom_event = static_cast<SP_Event_st*>(event);
    ASSERT_EQ(custom_event->event_id, 123);
    delete custom_event;
    event_deleted = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  ASSERT_FALSE(event_created);
  Event* event = new Event(executor);
  event->Init();
  ASSERT_TRUE(event_created);
  ASSERT_FALSE(event_deleted);
  delete event;
  ASSERT_TRUE(event_deleted);
}

TEST_F(StreamExecutorTest, PollForEventStatus) {
  static SE_EventStatus event_status = SE_EVENT_COMPLETE;
  se_.create_event = [](const SP_Device* const device, SP_Event* event,
                        TF_Status* const status) -> void {
    *event = new SP_Event_st(123);
  };
  se_.destroy_event = [](const SP_Device* const device,
                         SP_Event event) -> void { delete event; };
  se_.get_event_status = [](const SP_Device* const device,
                            SP_Event event) -> SE_EventStatus {
    EXPECT_EQ(event->event_id, 123);
    return event_status;
  };

  StreamExecutor* executor = GetExecutor(0);
  Event event(executor);
  event.Init();
  ASSERT_EQ(event.PollForStatus(), Event::Status::kComplete);
  event_status = SE_EVENT_ERROR;
  ASSERT_EQ(event.PollForStatus(), Event::Status::kError);
}

TEST_F(StreamExecutorTest, RecordAndWaitForEvent) {
  static bool record_called = false;
  static bool wait_called = false;
  se_.create_stream = [](const SP_Device* const device, SP_Stream* stream,
                         TF_Status* const status) -> void {
    *stream = new SP_Stream_st(1);
  };
  se_.destroy_stream = [](const SP_Device* const device,
                          SP_Stream stream) -> void { delete stream; };
  se_.create_event = [](const SP_Device* const device, SP_Event* event,
                        TF_Status* const status) -> void {
    *event = new SP_Event_st(2);
  };
  se_.destroy_event = [](const SP_Device* const device,
                         SP_Event event) -> void { delete event; };
  se_.record_event = [](const SP_Device* const device, SP_Stream stream,
                        SP_Event event, TF_Status* const status) {
    EXPECT_EQ(stream->stream_id, 1);
    EXPECT_EQ(event->event_id, 2);
    TF_SetStatus(status, TF_OK, "");
    record_called = true;
  };
  se_.wait_for_event = [](const SP_Device* const device, SP_Stream stream,
                          SP_Event event, TF_Status* const status) {
    EXPECT_EQ(stream->stream_id, 1);
    EXPECT_EQ(event->event_id, 2);
    TF_SetStatus(status, TF_OK, "");
    wait_called = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  Event event(executor);
  event.Init();
  Stream stream(executor);
  stream.Init();
  ASSERT_FALSE(record_called);
  stream.ThenRecordEvent(&event);
  ASSERT_TRUE(record_called);
  ASSERT_FALSE(wait_called);
  stream.ThenWaitFor(&event);
  ASSERT_TRUE(wait_called);
}

TEST_F(StreamExecutorTest, CreateTimer) {
  static bool timer_created = false;
  static bool timer_deleted = false;
  se_.create_timer = [](const SP_Device* const device, SP_Timer* timer,
                        TF_Status* const status) -> void {
    *timer = new SP_Timer_st(25);
    timer_created = true;
  };
  se_.destroy_timer = [](const SP_Device* const device,
                         SP_Timer timer) -> void {
    auto custom_timer = static_cast<SP_Timer_st*>(timer);
    EXPECT_EQ(custom_timer->timer_id, 25);
    delete custom_timer;
    timer_deleted = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  ASSERT_FALSE(timer_created);
  Stream stream(executor);
  stream.Init();
  Timer* timer = new Timer(executor);
  stream.InitTimer(timer);
  ASSERT_TRUE(stream.ok());
  ASSERT_TRUE(timer_created);
  ASSERT_FALSE(timer_deleted);
  delete timer;
  ASSERT_TRUE(timer_deleted);
}

TEST_F(StreamExecutorTest, StartTimer) {
  static bool start_called = false;
  static bool stop_called = false;
  static TF_Code start_timer_status = TF_OK;
  static TF_Code stop_timer_status = TF_OK;
  se_.create_timer = [](const SP_Device* const device, SP_Timer* timer,
                        TF_Status* const status) -> void {
    *timer = new SP_Timer_st(7);
  };
  se_.destroy_timer = [](const SP_Device* const device,
                         SP_Timer timer) -> void { delete timer; };
  se_.start_timer = [](const SP_Device* const device, SP_Stream stream,
                       SP_Timer timer, TF_Status* const status) {
    TF_SetStatus(status, start_timer_status, "");
    EXPECT_EQ(timer->timer_id, 7);
    start_called = true;
  };
  se_.stop_timer = [](const SP_Device* const device, SP_Stream stream,
                      SP_Timer timer, TF_Status* const status) {
    TF_SetStatus(status, stop_timer_status, "");
    EXPECT_EQ(timer->timer_id, 7);
    stop_called = true;
  };
  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  Timer timer(executor);
  stream.InitTimer(&timer);

  // Check both start and stop succeed
  ASSERT_FALSE(start_called);
  stream.ThenStartTimer(&timer);
  ASSERT_TRUE(start_called);
  ASSERT_FALSE(stop_called);
  stream.ThenStopTimer(&timer);
  ASSERT_TRUE(stop_called);

  // Check start timer fails
  ASSERT_TRUE(stream.ok());
  start_timer_status = TF_UNKNOWN;
  stream.ThenStartTimer(&timer);
  ASSERT_FALSE(stream.ok());

  // Check stop timer fails
  start_timer_status = TF_OK;
  stop_timer_status = TF_UNKNOWN;
  Stream stream2(executor);
  stream2.Init();
  Timer timer2(executor);
  stream2.InitTimer(&timer2);
  stream2.ThenStartTimer(&timer2);
  ASSERT_TRUE(stream2.ok());
  stream2.ThenStopTimer(&timer2);
  ASSERT_FALSE(stream2.ok());
}

TEST_F(StreamExecutorTest, TimerFns) {
  se_.create_timer = [](const SP_Device* const device, SP_Timer* timer,
                        TF_Status* const status) -> void {
    *timer = new SP_Timer_st(25000);
  };
  se_.destroy_timer = [](const SP_Device* const device,
                         SP_Timer timer) -> void { delete timer; };

  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  Timer timer(executor);
  stream.InitTimer(&timer);
  // Our test nanoseconds function just returns value
  // passed to SP_Timer_st constructor.
  ASSERT_EQ(timer.Nanoseconds(), 25000);
  ASSERT_EQ(timer.Microseconds(), 25);
}

TEST_F(StreamExecutorTest, MemcpyToHost) {
  se_.create_stream = [](const SP_Device* const device, SP_Stream* stream,
                         TF_Status* const status) -> void {
    *stream = new SP_Stream_st(14);
  };
  se_.destroy_stream = [](const SP_Device* const device,
                          SP_Stream stream) -> void { delete stream; };

  se_.memcpy_dtoh = [](const SP_Device* const device, SP_Stream stream,
                       void* host_dst,
                       const SP_DeviceMemoryBase* const device_src,
                       uint64_t size, TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    EXPECT_EQ(stream->stream_id, 14);
    std::memcpy(host_dst, device_src->opaque, size);
  };

  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  size_t size = sizeof(int);
  int src_data = 34;
  int dst_data = 2;
  DeviceMemoryBase device_src(&src_data, size);
  Stream& stream_ref = stream.ThenMemcpy(&dst_data, device_src, size);
  ASSERT_EQ(dst_data, 34);
  ASSERT_EQ(stream_ref.implementation(), stream.implementation());
}

TEST_F(StreamExecutorTest, MemcpyFromHost) {
  se_.memcpy_htod = [](const SP_Device* const device, SP_Stream stream,
                       SP_DeviceMemoryBase* const device_dst,
                       const void* host_src, uint64_t size,
                       TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    std::memcpy(device_dst->opaque, host_src, size);
  };

  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  size_t size = sizeof(int);
  int src_data = 18;
  int dst_data = 0;
  DeviceMemoryBase device_dst(&dst_data, size);
  stream.ThenMemcpy(&device_dst, &src_data, size);
  ASSERT_EQ(dst_data, 18);
}

TEST_F(StreamExecutorTest, MemcpyDeviceToDevice) {
  se_.memcpy_dtod = [](const SP_Device* const device, SP_Stream stream,
                       SP_DeviceMemoryBase* const device_dst,
                       const SP_DeviceMemoryBase* const device_src,
                       uint64_t size, TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    std::memcpy(device_dst->opaque, device_src->opaque, size);
  };

  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  size_t size = sizeof(int);
  int src_data = 18;
  int dst_data = 0;
  DeviceMemoryBase device_dst(&dst_data, size);
  DeviceMemoryBase device_src(&src_data, size);
  stream.ThenMemcpy(&device_dst, device_src, size);
  ASSERT_EQ(dst_data, 18);
}

TEST_F(StreamExecutorTest, SyncMemcpyToHost) {
  se_.sync_memcpy_dtoh = [](const SP_Device* const device, void* host_dst,
                            const SP_DeviceMemoryBase* const device_src,
                            uint64_t size, TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    std::memcpy(host_dst, device_src->opaque, size);
  };

  StreamExecutor* executor = GetExecutor(0);
  size_t size = sizeof(int);
  int src_data = 34;
  int dst_data = 2;
  DeviceMemoryBase device_src(&src_data, size);
  TF_ASSERT_OK(executor->SynchronousMemcpyD2H(device_src, size, &dst_data));
  ASSERT_EQ(dst_data, 34);
}

TEST_F(StreamExecutorTest, SyncMemcpyFromHost) {
  se_.sync_memcpy_htod =
      [](const SP_Device* const device, SP_DeviceMemoryBase* const device_dst,
         const void* host_src, uint64_t size, TF_Status* const status) {
        TF_SetStatus(status, TF_OK, "");
        std::memcpy(device_dst->opaque, host_src, size);
      };

  StreamExecutor* executor = GetExecutor(0);
  size_t size = sizeof(int);
  int src_data = 18;
  int dst_data = 0;
  DeviceMemoryBase device_dst(&dst_data, size);
  TF_ASSERT_OK(executor->SynchronousMemcpyH2D(&src_data, size, &device_dst));
  ASSERT_EQ(dst_data, 18);
}

TEST_F(StreamExecutorTest, SyncMemcpyDeviceToDevice) {
  se_.sync_memcpy_dtod = [](const SP_Device* const device,
                            SP_DeviceMemoryBase* const device_dst,
                            const SP_DeviceMemoryBase* const device_src,
                            uint64_t size, TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    std::memcpy(device_dst->opaque, device_src->opaque, size);
  };

  StreamExecutor* executor = GetExecutor(0);
  size_t size = sizeof(int);
  int src_data = 18;
  int dst_data = 0;
  DeviceMemoryBase device_dst(&dst_data, size);
  DeviceMemoryBase device_src(&src_data, size);
  ASSERT_TRUE(executor->SynchronousMemcpy(&device_dst, device_src, size));
  ASSERT_EQ(dst_data, 18);
}

TEST_F(StreamExecutorTest, BlockHostForEvent) {
  static bool block_host_for_event_called = false;
  se_.create_event = [](const SP_Device* const device, SP_Event* event,
                        TF_Status* const status) {
    *event = new SP_Event_st(357);
  };
  se_.destroy_event = [](const SP_Device* const device, SP_Event event) {
    delete event;
  };
  se_.block_host_for_event = [](const SP_Device* const device, SP_Event event,
                                TF_Status* const status) -> void {
    ASSERT_EQ(event->event_id, 357);
    TF_SetStatus(status, TF_OK, "");
    block_host_for_event_called = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  ASSERT_FALSE(block_host_for_event_called);
  TF_ASSERT_OK(stream.BlockHostUntilDone());
  ASSERT_TRUE(block_host_for_event_called);
}

TEST_F(StreamExecutorTest, SynchronizeAllActivity) {
  static bool synchronize_all_called = false;
  se_.synchronize_all_activity = [](const SP_Device* const device,
                                    TF_Status* const status) {
    TF_SetStatus(status, TF_OK, "");
    synchronize_all_called = true;
  };

  StreamExecutor* executor = GetExecutor(0);
  ASSERT_FALSE(synchronize_all_called);
  ASSERT_TRUE(executor->SynchronizeAllActivity());
  ASSERT_TRUE(synchronize_all_called);
}

TEST_F(StreamExecutorTest, HostCallbackOk) {
  se_.host_callback = [](SP_Device* const device, SP_Stream stream,
                         SE_StatusCallbackFn const callback_fn,
                         void* const callback_arg) -> TF_Bool {
    TF_Status* status = TF_NewStatus();
    callback_fn(callback_arg, status);
    bool ok = TF_GetCode(status) == TF_OK;
    TF_DeleteStatus(status);
    return ok;
  };
  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  std::function<port::Status()> callback = []() -> port::Status {
    return port::Status::OK();
  };
  stream.ThenDoHostCallbackWithStatus(callback);
  ASSERT_TRUE(stream.ok());
}

TEST_F(StreamExecutorTest, HostCallbackError) {
  se_.host_callback = [](SP_Device* const device, SP_Stream stream,
                         SE_StatusCallbackFn const callback_fn,
                         void* const callback_arg) -> TF_Bool {
    TF_Status* status = TF_NewStatus();
    callback_fn(callback_arg, status);
    bool ok = TF_GetCode(status) == TF_OK;
    TF_DeleteStatus(status);
    return ok;
  };
  StreamExecutor* executor = GetExecutor(0);
  Stream stream(executor);
  stream.Init();
  std::function<port::Status()> callback = []() -> port::Status {
    return port::UnimplementedError("Unimplemented");
  };
  stream.ThenDoHostCallbackWithStatus(callback);
  ASSERT_FALSE(stream.ok());
}
}  // namespace
}  // namespace stream_executor
