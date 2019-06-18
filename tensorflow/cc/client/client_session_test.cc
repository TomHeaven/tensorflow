/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/cc/client/client_session.h"

#include <vector>

#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/core/threadpool_options.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {

using ops::Add;
using ops::Const;
using ops::Mul;
using ops::Placeholder;
using ops::Sub;

class CustomThreadPoolImpl : public thread::ThreadPoolInterface {
 public:
  CustomThreadPoolImpl() {
    underlying_threadpool_.reset(new thread::ThreadPool(
        tensorflow::Env::Default(), "custom_threadpool", 2));
    num_schedule_called_ = 0;
  }

  void Schedule(std::function<void()> fn) override {
    num_schedule_called_ += 1;
    underlying_threadpool_->Schedule(std::move(fn));
  }

  void ScheduleWithHint(std::function<void()> fn, int start, int end) override {
    num_schedule_called_ += 1;
    underlying_threadpool_->ScheduleWithHint(std::move(fn), start, end);
  }

  void Cancel() override {}

  int NumThreads() const override {
    return underlying_threadpool_->NumThreads();
  }

  int CurrentThreadId() const override {
    return underlying_threadpool_->CurrentThreadId();
  }

  int GetNumScheduleCalled() { return num_schedule_called_; }

 private:
  int num_schedule_called_;
  std::unique_ptr<tensorflow::thread::ThreadPool> underlying_threadpool_;
};

TEST(ClientSessionTest, Basic) {
  Scope root = Scope::NewRootScope();
  auto c = Const(root, {{1, 1}});
  ClientSession session(root);
  std::vector<Tensor> outputs;

  TF_EXPECT_OK(session.Run({c}, &outputs));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({1, 1}, {1, 2}));
}

TEST(ClientSessionTest, Feed) {
  Scope root = Scope::NewRootScope();
  auto a = Placeholder(root, DT_INT32);
  auto b = Placeholder(root, DT_INT32);
  auto c = Add(root, a, b);
  ClientSession session(root);
  std::vector<Tensor> outputs;

  TF_EXPECT_OK(session.Run({{a, 1}, {b, 41}}, {c}, &outputs));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({42}, {}));
}

TEST(ClientSessionTest, Extend) {
  Scope root = Scope::NewRootScope();
  auto a = Placeholder(root, DT_INT32, Placeholder::Shape({2}));
  auto c = Add(root, a, {2, 2});
  ClientSession session(root);
  std::vector<Tensor> outputs;

  TF_EXPECT_OK(session.Run({{a, {1, 1}}}, {c}, &outputs));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({3, 3}, {2}));

  auto d = Add(root, c, {39, 39});
  outputs.clear();
  TF_EXPECT_OK(session.Run({{a, {-10, 1}}}, {d}, &outputs));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({31, 42}, {2}));
}

TEST(ClientSessionTest, MultiThreaded) {
  Scope root = Scope::NewRootScope();
  auto a = Add(root, {1, 2}, {3, 4});
  auto b = Mul(root, {1, 2}, {3, 4});
  ClientSession session(root);
  {
    thread::ThreadPool thread_pool(Env::Default(), "pool", 2);
    thread_pool.Schedule([&session, a]() {
      std::vector<Tensor> outputs;
      TF_EXPECT_OK(session.Run({a}, &outputs));
      test::ExpectTensorEqual<int>(outputs[0],
                                   test::AsTensor<int>({4, 6}, {2}));
    });
    thread_pool.Schedule([&session, b]() {
      std::vector<Tensor> outputs;
      TF_EXPECT_OK(session.Run({b}, &outputs));
      test::ExpectTensorEqual<int>(outputs[0],
                                   test::AsTensor<int>({3, 8}, {2}));
    });
  }
  auto c = Sub(root, b, a);
  std::vector<Tensor> outputs;
  TF_EXPECT_OK(session.Run({c}, &outputs));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({-1, 2}, {2}));
}

TEST(ClientSessionTest, CallableWithDefaultThreadPool) {
  Scope root = Scope::NewRootScope();
  auto a = Placeholder(root, DT_INT32);
  auto b = Placeholder(root, DT_INT32);
  auto c = Add(root, a, b);
  ClientSession session(root);
  std::vector<Tensor> outputs;

  CallableOptions options;
  options.add_feed(a.node()->name());
  options.add_feed(b.node()->name());
  options.add_fetch(c.node()->name());
  ClientSession::CallableHandle callable;
  TF_CHECK_OK(session.MakeCallable(options, &callable));
  TF_EXPECT_OK(session.RunCallable(
      callable, {test::AsTensor<int>({1}, {}), test::AsTensor<int>({41}, {})},
      &outputs, nullptr));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({42}, {}));
  TF_EXPECT_OK(session.ReleaseCallable(callable));
}

TEST(ClientSessionTest, CallableWithCustomThreadPool) {
  Scope root = Scope::NewRootScope();
  auto a = Placeholder(root, DT_INT32);
  auto b = Placeholder(root, DT_INT32);
  auto c = Add(root, a, b);
  ClientSession session(root);
  std::vector<Tensor> outputs;

  auto inter_op_threadpool = absl::make_unique<CustomThreadPoolImpl>();
  ASSERT_EQ(inter_op_threadpool->GetNumScheduleCalled(), 0);

  tensorflow::thread::ThreadPoolOptions threadPoolOptions;
  threadPoolOptions.inter_op_threadpool = inter_op_threadpool.get();

  CallableOptions options;
  options.add_feed(a.node()->name());
  options.add_feed(b.node()->name());
  options.add_fetch(c.node()->name());
  ClientSession::CallableHandle callable;
  TF_CHECK_OK(session.MakeCallable(options, &callable));
  TF_EXPECT_OK(session.RunCallable(
      callable, {test::AsTensor<int>({1}, {}), test::AsTensor<int>({41}, {})},
      &outputs, nullptr, threadPoolOptions));
  test::ExpectTensorEqual<int>(outputs[0], test::AsTensor<int>({42}, {}));
  TF_EXPECT_OK(session.ReleaseCallable(callable));
  ASSERT_GT(inter_op_threadpool->GetNumScheduleCalled(), 0);
}

}  // namespace
}  // namespace tensorflow
