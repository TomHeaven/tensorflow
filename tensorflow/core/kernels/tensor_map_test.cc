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

#include "tensorflow/core/kernels/tensor_map.h"
#include "tensorflow/core/framework/tensor.h"
#include "absl/container/flat_hash_map.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/framework/variant.h"

#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"

namespace tensorflow {

namespace {

TEST(TensorMapTest, Empty) {
  TensorMap tm;
  EXPECT_EQ(tm.tensors().size(), 0);
  EXPECT_EQ(tm.tensors().begin(), tm.tensors().end());
}

TEST(TensorKeyTest, Equal) {
  TensorKey k1 = Tensor(15);
  TensorKey k2 = Tensor(15);
  EXPECT_EQ(k1,k2);

  TensorKey k3 = Tensor(15);
  TensorKey k4 = Tensor(37);
  EXPECT_NE(k3,k4);
}

TEST(TensorMapTest, Insert) {
  EXPECT_EQ(1,1);
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  absl::flat_hash_map<TensorKey,Tensor> am;
  am.try_emplace(k,v);

  absl::flat_hash_map<TensorKey,Tensor>::iterator map_it = tm.tensors().begin();
  EXPECT_EQ(map_it->first, k);
  test::ExpectTensorEqual<int32>(map_it->second, v);
  map_it++;
  EXPECT_EQ(map_it, tm.tensors().end());
}

TEST(TensorMapTest, Lookup) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  absl::flat_hash_map<TensorKey,Tensor>::iterator map_it = tm.find(k);
  Tensor f = map_it->second;

  EXPECT_EQ(map_it->first, k);
  test::ExpectTensorEqual<int32>(f, v);
}

TEST(TensorMapTest, Erase) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  tm.erase(k);
  EXPECT_EQ(tm.find(k), tm.tensors().end());
}

TEST(TensorMapTest, SameKeyInsert) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v1 = Tensor(22);
  Tensor v2 = Tensor(23);
  bool b1 = tm.insert(k,v1);
  bool b2 = tm.insert(k,v2);
  EXPECT_EQ(b1, true);
  EXPECT_EQ(b2, false);
  absl::flat_hash_map<TensorKey,Tensor>::iterator map_it = tm.find(k);
  EXPECT_EQ(map_it->first, k);
  test::ExpectTensorEqual<int32>(map_it->second, v1);
}

TEST(TensorMapTest, Replace) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v1 = Tensor(22);
  Tensor v2 = Tensor(23);
  tm[k] = v2;

  absl::flat_hash_map<TensorKey,Tensor>::iterator map_it = tm.find(k);
  EXPECT_EQ(map_it->first, k);
  test::ExpectTensorEqual<int32>(map_it->second, v2);
}

TEST(TensorMapTest, Copy) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  TensorMap tmc = tm.Copy();
  EXPECT_EQ(tm.dtype(), tmc.dtype());
  EXPECT_EQ(tm.size(), tmc.size());
  EXPECT_NE(tm.find(k), tm.tensors().end());
  EXPECT_NE(tmc.find(k), tmc.tensors().end());
  EXPECT_EQ(tm.find(k)->first, tmc.find(k)->first);
  test::ExpectTensorEqual<int32>(tm.find(k)->second, tmc.find(k)->second);
}

TEST(TensorMapTest, EncodeDecode) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  VariantTensorData data;
  tm.Encode(&data);
  TensorMap tmc;
  tmc.Decode(data);
  
  EXPECT_EQ(tm.dtype(), tmc.dtype());
  EXPECT_EQ(tm.size(), tmc.size());
  EXPECT_NE(tm.find(k), tm.tensors().end());
  EXPECT_NE(tmc.find(k), tmc.tensors().end());
  EXPECT_EQ(tm.find(k)->first, tmc.find(k)->first);
  test::ExpectTensorEqual<int32>(tm.find(k)->second, tmc.find(k)->second);
}

TEST(TensorMapTest, Keys) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  TensorKey k2 = Tensor(12);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  tm.insert(k2,v);
  std::vector<Tensor> keys = tm.keys();
  EXPECT_EQ(1,1);
  Tensor t = Tensor(11);
  //std::cout << "keys: " << keys[0] << std::endl;
  //test::ExpectTensorEqual<int32>(keys[0], t);
  //test::ExpectTensorEqual<int32>(keys[1], k2);
}

TEST(TensorMapTest, Zeros) {
  TensorMap tm;
  TensorKey k = Tensor(11);
  Tensor v = Tensor(22);
  tm.insert(k,v);
  TensorMap z = tm.Zeros();
  test::ExpectTensorEqual<int32>(z.find(k)->second,Tensor(0));
}
}  // namespace

}  // namespace tensorflow
