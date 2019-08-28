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
#include "tensorflow/core/profiler/internal/parse_annotation.h"

#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace profiler {
namespace {

TEST(ParseAnnotationStackTest, EmptyAnnotationStackTest) {
  std::vector<Annotation> annotations = ParseAnnotationStack("");
  ASSERT_TRUE(annotations.empty());
}

TEST(ParseAnnotationStackTest, SingleAnnotationStackTest) {
  std::vector<Annotation> annotations = ParseAnnotationStack("name");
  ASSERT_FALSE(annotations.empty());
  EXPECT_EQ(annotations.back().name, "name");
  EXPECT_TRUE(annotations.back().metadata.empty());
}

TEST(ParseAnnotationStackTest, MultiLevelAnnotationStackTest) {
  std::vector<Annotation> annotations = ParseAnnotationStack("outer::inner");
  ASSERT_EQ(annotations.size(), 2);
  EXPECT_EQ(annotations.front().name, "outer");
  EXPECT_TRUE(annotations.front().metadata.empty());
  EXPECT_EQ(annotations.back().name, "inner");
  EXPECT_TRUE(annotations.back().metadata.empty());
}

TEST(ParseAnnotationTest, EmptyAnnotationTest) {
  Annotation annotation = ParseAnnotation("");
  EXPECT_TRUE(annotation.name.empty());
  EXPECT_TRUE(annotation.metadata.empty());
}

TEST(ParseAnnotationTest, SimpleNameTest) {
  Annotation annotation = ParseAnnotation("name");
  EXPECT_EQ(annotation.name, "name");
  EXPECT_TRUE(annotation.metadata.empty());
}

TEST(ParseAnnotationTest, EmptyMetadataTest) {
  Annotation annotation = ParseAnnotation("name#");
  EXPECT_EQ(annotation.name, "name");
  EXPECT_TRUE(annotation.metadata.empty());

  annotation = ParseAnnotation("name1##");
  EXPECT_EQ(annotation.name, "name1");
  EXPECT_TRUE(annotation.metadata.empty());

  annotation = ParseAnnotation("name2###");
  EXPECT_EQ(annotation.name, "name2");
  EXPECT_TRUE(annotation.metadata.empty());
}

TEST(ParseAnnotationTest, SingleMetadataTest) {
  Annotation annotation = ParseAnnotation("name#key=value#");
  EXPECT_EQ(annotation.name, "name");
  EXPECT_EQ(annotation.metadata.size(), 1);
  EXPECT_EQ(annotation.metadata.at(0).key, "key");
  EXPECT_EQ(annotation.metadata.at(0).value, "value");
}

TEST(ParseAnnotationTest, MultipleMetadataTest) {
  Annotation annotation = ParseAnnotation("name#k1=v1,k2=v2,k3=v3#");
  EXPECT_EQ(annotation.name, "name");
  EXPECT_EQ(annotation.metadata.size(), 3);
  EXPECT_EQ(annotation.metadata.at(0).key, "k1");
  EXPECT_EQ(annotation.metadata.at(0).value, "v1");
  EXPECT_EQ(annotation.metadata.at(1).key, "k2");
  EXPECT_EQ(annotation.metadata.at(1).value, "v2");
  EXPECT_EQ(annotation.metadata.at(2).key, "k3");
  EXPECT_EQ(annotation.metadata.at(2).value, "v3");
}

TEST(ParseAnnotationTest, ExtraCharactersTest) {
  Annotation annotation = ParseAnnotation("name#k1=v1,k2=,k3=v3,k4=v4=#more#");
  EXPECT_EQ(annotation.name, "name");
  EXPECT_EQ(annotation.metadata.size(), 2);
  EXPECT_EQ(annotation.metadata.at(0).key, "k1");
  EXPECT_EQ(annotation.metadata.at(0).value, "v1");
  // "k2=" is ignored due to missing value.
  EXPECT_EQ(annotation.metadata.at(1).key, "k3");
  EXPECT_EQ(annotation.metadata.at(1).value, "v3");
  // "k4=v4=" is ignored due to extra '='.
  // "more#" is ignored.
}

}  // namespace
}  // namespace profiler
}  // namespace tensorflow
