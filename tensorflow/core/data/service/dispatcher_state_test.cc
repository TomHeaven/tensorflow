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
#include "tensorflow/core/data/service/dispatcher_state.h"

#include <memory>

#include "tensorflow/core/data/service/common.pb.h"
#include "tensorflow/core/data/service/journal.h"
#include "tensorflow/core/data/service/journal.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace data {

namespace {
using Dataset = DispatcherState::Dataset;
using Worker = DispatcherState::Worker;
using NamedJobKey = DispatcherState::NamedJobKey;
using Job = DispatcherState::Job;
using Task = DispatcherState::Task;
using ::testing::IsEmpty;
using ::testing::SizeIs;

Status RegisterDataset(int64 id, uint64 fingerprint, DispatcherState& state) {
  Update update;
  RegisterDatasetUpdate* register_dataset = update.mutable_register_dataset();
  register_dataset->set_dataset_id(id);
  register_dataset->set_fingerprint(fingerprint);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status RegisterDataset(int64 id, DispatcherState& state) {
  return RegisterDataset(id, /*fingerprint=*/1, state);
}

Status RegisterWorker(std::string worker_address, DispatcherState& state) {
  Update update;
  update.mutable_register_worker()->set_worker_address(worker_address);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status CreateAnonymousJob(int64 job_id, int64 dataset_id,
                          DispatcherState& state) {
  Update update;
  CreateJobUpdate* create_job = update.mutable_create_job();
  create_job->set_job_id(job_id);
  create_job->set_dataset_id(dataset_id);
  create_job->set_processing_mode(ProcessingModeDef::PARALLEL_EPOCHS);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status CreateNamedJob(int64 job_id, int64 dataset_id, NamedJobKey named_job_key,
                      DispatcherState& state) {
  Update update;
  CreateJobUpdate* create_job = update.mutable_create_job();
  create_job->set_job_id(job_id);
  create_job->set_dataset_id(dataset_id);
  create_job->set_processing_mode(ProcessingModeDef::PARALLEL_EPOCHS);
  NamedJobKeyDef* key = create_job->mutable_named_job_key();
  key->set_name(named_job_key.name);
  key->set_index(named_job_key.index);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status AcquireJobClientId(int64 job_id, int64 job_client_id,
                          DispatcherState& state) {
  Update update;
  AcquireJobClientUpdate* acquire_job_client =
      update.mutable_acquire_job_client();
  acquire_job_client->set_job_id(job_id);
  acquire_job_client->set_job_client_id(job_client_id);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status ReleaseJobClientId(int64 job_client_id, int64 release_time,
                          DispatcherState& state) {
  Update update;
  ReleaseJobClientUpdate* release_job_client =
      update.mutable_release_job_client();
  release_job_client->set_job_client_id(job_client_id);
  release_job_client->set_time_micros(release_time);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status CreateTask(int64 task_id, int64 job_id, int64 dataset_id,
                  const std::string& worker_address, DispatcherState& state) {
  Update update;
  CreateTaskUpdate* create_task = update.mutable_create_task();
  create_task->set_task_id(task_id);
  create_task->set_job_id(job_id);
  create_task->set_dataset_id(dataset_id);
  create_task->set_worker_address(worker_address);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}

Status FinishTask(int64 task_id, DispatcherState& state) {
  Update update;
  FinishTaskUpdate* finish_task = update.mutable_finish_task();
  finish_task->set_task_id(task_id);
  TF_RETURN_IF_ERROR(state.Apply(update));
  return Status::OK();
}
}  // namespace

TEST(DispatcherState, RegisterDataset) {
  uint64 fingerprint = 20;
  DispatcherState state;
  int64 id = state.NextAvailableDatasetId();
  TF_EXPECT_OK(RegisterDataset(id, fingerprint, state));
  EXPECT_EQ(state.NextAvailableDatasetId(), id + 1);

  {
    std::shared_ptr<const Dataset> dataset;
    TF_EXPECT_OK(state.DatasetFromFingerprint(fingerprint, dataset));
    EXPECT_EQ(dataset->dataset_id, id);
  }
  {
    std::shared_ptr<const Dataset> dataset;
    TF_EXPECT_OK(state.DatasetFromId(id, dataset));
    EXPECT_EQ(dataset->fingerprint, fingerprint);
  }
}

TEST(DispatcherState, MissingDatasetId) {
  DispatcherState state;
  std::shared_ptr<const Dataset> dataset;
  Status s = state.DatasetFromId(0, dataset);
  EXPECT_EQ(s.code(), error::NOT_FOUND);
}

TEST(DispatcherState, MissingDatasetFingerprint) {
  DispatcherState state;
  std::shared_ptr<const Dataset> dataset;
  Status s = state.DatasetFromFingerprint(0, dataset);
  EXPECT_EQ(s.code(), error::NOT_FOUND);
}

TEST(DispatcherState, NextAvailableDatasetId) {
  DispatcherState state;
  int64 id = state.NextAvailableDatasetId();
  uint64 fingerprint = 20;
  TF_EXPECT_OK(RegisterDataset(id, fingerprint, state));
  EXPECT_NE(state.NextAvailableDatasetId(), id);
  EXPECT_EQ(state.NextAvailableDatasetId(), state.NextAvailableDatasetId());
}

TEST(DispatcherState, RegisterWorker) {
  DispatcherState state;
  std::string address = "test_worker_address";
  TF_EXPECT_OK(RegisterWorker(address, state));
  std::shared_ptr<const Worker> worker;
  TF_EXPECT_OK(state.WorkerFromAddress(address, worker));
  EXPECT_EQ(worker->address, address);
}

TEST(DispatcherState, ListWorkers) {
  DispatcherState state;
  std::string address_1 = "address_1";
  std::string address_2 = "address_2";
  {
    std::vector<std::shared_ptr<const Worker>> workers = state.ListWorkers();
    EXPECT_THAT(workers, IsEmpty());
  }
  TF_EXPECT_OK(RegisterWorker(address_1, state));
  {
    std::vector<std::shared_ptr<const Worker>> workers = state.ListWorkers();
    EXPECT_THAT(workers, SizeIs(1));
  }
  TF_EXPECT_OK(RegisterWorker(address_2, state));
  {
    std::vector<std::shared_ptr<const Worker>> workers = state.ListWorkers();
    EXPECT_THAT(workers, SizeIs(2));
  }
}

TEST(DispatcherState, MissingWorker) {
  DispatcherState state;
  std::shared_ptr<const Worker> worker;
  Status s = state.WorkerFromAddress("test_worker_address", worker);
  EXPECT_EQ(s.code(), error::NOT_FOUND);
}

TEST(DispatcherState, UnknownUpdate) {
  DispatcherState state;
  Update update;
  Status s = state.Apply(update);
  EXPECT_EQ(s.code(), error::INTERNAL);
}

TEST(DispatcherState, AnonymousJob) {
  int64 dataset_id = 10;
  DispatcherState state;
  int64 job_id = state.NextAvailableJobId();
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  std::shared_ptr<const Job> job;
  TF_EXPECT_OK(state.JobFromId(job_id, job));
  EXPECT_EQ(state.NextAvailableJobId(), job_id + 1);
  EXPECT_EQ(job->dataset_id, dataset_id);
  EXPECT_EQ(job->job_id, job_id);
  std::vector<std::shared_ptr<const Task>> tasks;
  TF_EXPECT_OK(state.TasksForJob(job_id, tasks));
  EXPECT_THAT(tasks, IsEmpty());
  EXPECT_FALSE(job->finished);
}

TEST(DispatcherState, NamedJob) {
  int64 dataset_id = 10;
  DispatcherState state;
  int64 job_id = state.NextAvailableJobId();
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  NamedJobKey named_job_key("test", 1);
  TF_EXPECT_OK(CreateNamedJob(job_id, dataset_id, named_job_key, state));
  std::shared_ptr<const Job> job;
  TF_EXPECT_OK(state.NamedJobByKey(named_job_key, job));
  EXPECT_EQ(state.NextAvailableJobId(), job_id + 1);
  EXPECT_EQ(job->dataset_id, dataset_id);
  EXPECT_EQ(job->job_id, job_id);
  EXPECT_FALSE(job->finished);
}

TEST(DispatcherState, CreateTask) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  int64 task_id = state.NextAvailableTaskId();
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(CreateTask(task_id, job_id, dataset_id, worker_address, state));
  EXPECT_EQ(state.NextAvailableTaskId(), task_id + 1);
  {
    std::shared_ptr<const Task> task;
    TF_EXPECT_OK(state.TaskFromId(task_id, task));
    EXPECT_EQ(task->task_id, task_id);
    EXPECT_EQ(task->job_id, job_id);
    EXPECT_EQ(task->dataset_id, dataset_id);
    EXPECT_EQ(task->worker_address, worker_address);
  }
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForJob(job_id, tasks));
    EXPECT_THAT(tasks, SizeIs(1));
  }
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForWorker(worker_address, tasks));
    EXPECT_EQ(1, tasks.size());
  }
}

TEST(DispatcherState, CreateTasksForSameJob) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  int64 task_id_1 = 8;
  int64 task_id_2 = 9;
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(
      CreateTask(task_id_1, job_id, dataset_id, worker_address, state));
  TF_EXPECT_OK(
      CreateTask(task_id_2, job_id, dataset_id, worker_address, state));
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForJob(job_id, tasks));
    EXPECT_THAT(tasks, SizeIs(2));
  }
}

TEST(DispatcherState, CreateTasksForDifferentJobs) {
  int64 job_id_1 = 3;
  int64 job_id_2 = 4;
  int64 dataset_id = 10;
  int64 task_id_1 = 8;
  int64 task_id_2 = 9;
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id_1, dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id_2, dataset_id, state));
  TF_EXPECT_OK(
      CreateTask(task_id_1, job_id_1, dataset_id, worker_address, state));
  TF_EXPECT_OK(
      CreateTask(task_id_2, job_id_2, dataset_id, worker_address, state));
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForJob(job_id_1, tasks));
    EXPECT_THAT(tasks, SizeIs(1));
  }
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForJob(job_id_2, tasks));
    EXPECT_THAT(tasks, SizeIs(1));
  }
}

TEST(DispatcherState, CreateTasksForSameWorker) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  int64 task_id_1 = 8;
  int64 task_id_2 = 9;
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(
      CreateTask(task_id_1, job_id, dataset_id, worker_address, state));
  TF_EXPECT_OK(
      CreateTask(task_id_2, job_id, dataset_id, worker_address, state));
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForWorker(worker_address, tasks));
    EXPECT_EQ(2, tasks.size());
  }
}

TEST(DispatcherState, CreateTasksForDifferentWorkers) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  int64 task_id_1 = 8;
  int64 task_id_2 = 9;
  std::string worker_address_1 = "test_worker_address_1";
  std::string worker_address_2 = "test_worker_address_2";
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(
      CreateTask(task_id_1, job_id, dataset_id, worker_address_1, state));
  TF_EXPECT_OK(
      CreateTask(task_id_2, job_id, dataset_id, worker_address_2, state));
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForWorker(worker_address_1, tasks));
    EXPECT_EQ(1, tasks.size());
  }
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForWorker(worker_address_2, tasks));
    EXPECT_EQ(1, tasks.size());
  }
}

TEST(DispatcherState, GetTasksForWorkerEmpty) {
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  TF_EXPECT_OK(RegisterWorker(worker_address, state));
  {
    std::vector<std::shared_ptr<const Task>> tasks;
    TF_EXPECT_OK(state.TasksForWorker(worker_address, tasks));
    EXPECT_EQ(0, tasks.size());
  }
}

TEST(DispatcherState, FinishTask) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  int64 task_id = 4;
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(CreateTask(task_id, job_id, dataset_id, worker_address, state));
  TF_EXPECT_OK(FinishTask(task_id, state));
  std::shared_ptr<const Task> task;
  TF_EXPECT_OK(state.TaskFromId(task_id, task));
  EXPECT_TRUE(task->finished);
  std::shared_ptr<const Job> job;
  TF_EXPECT_OK(state.JobFromId(job_id, job));
  EXPECT_TRUE(job->finished);
}

TEST(DispatcherState, FinishMultiTaskJob) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  int64 task_id_1 = 4;
  int64 task_id_2 = 5;
  std::string worker_address = "test_worker_address";
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(
      CreateTask(task_id_1, job_id, dataset_id, worker_address, state));
  TF_EXPECT_OK(
      CreateTask(task_id_2, job_id, dataset_id, worker_address, state));

  TF_EXPECT_OK(FinishTask(task_id_1, state));
  {
    std::shared_ptr<const Job> job;
    TF_EXPECT_OK(state.JobFromId(job_id, job));
    EXPECT_FALSE(job->finished);
  }

  TF_EXPECT_OK(FinishTask(task_id_2, state));
  {
    std::shared_ptr<const Job> job;
    TF_EXPECT_OK(state.JobFromId(job_id, job));
    EXPECT_TRUE(job->finished);
  }
}

TEST(DispatcherState, AcquireJobClientId) {
  int64 job_id = 3;
  int64 job_client_id_1 = 1;
  int64 job_client_id_2 = 2;
  int64 dataset_id = 10;
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(AcquireJobClientId(job_id, job_client_id_1, state));
  {
    std::shared_ptr<const Job> job;
    TF_EXPECT_OK(state.JobFromId(job_id, job));
    EXPECT_EQ(job->num_clients, 1);
    TF_EXPECT_OK(AcquireJobClientId(job_id, job_client_id_2, state));
    EXPECT_EQ(job->num_clients, 2);
  }
  {
    std::shared_ptr<const Job> job;
    TF_EXPECT_OK(state.JobForJobClientId(job_client_id_1, job));
    EXPECT_EQ(job->job_id, job_id);
  }
  {
    std::shared_ptr<const Job> job;
    TF_EXPECT_OK(state.JobForJobClientId(job_client_id_2, job));
    EXPECT_EQ(job->job_id, job_id);
  }
}

TEST(DispatcherState, ReleaseJobClientId) {
  int64 job_id = 3;
  int64 dataset_id = 10;
  int64 job_client_id = 6;
  int64 release_time = 100;
  DispatcherState state;
  TF_EXPECT_OK(RegisterDataset(dataset_id, state));
  TF_EXPECT_OK(CreateAnonymousJob(job_id, dataset_id, state));
  TF_EXPECT_OK(AcquireJobClientId(job_id, job_client_id, state));
  TF_EXPECT_OK(ReleaseJobClientId(job_client_id, release_time, state));
  std::shared_ptr<const Job> job;
  TF_EXPECT_OK(state.JobFromId(job_id, job));
  EXPECT_EQ(job->num_clients, 0);
  Status s = state.JobForJobClientId(job_client_id, job);
  EXPECT_EQ(s.code(), error::NOT_FOUND);
}

}  // namespace data
}  // namespace tensorflow
