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
#include "tensorflow/lite/experimental/support/metadata/cc/metadata_version.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "flatbuffers/flatbuffers.h"  // from @flatbuffers
#include "tensorflow/lite/experimental/support/metadata/metadata_schema_generated.h"

namespace tflite {
namespace metadata {
namespace {

using ::testing::MatchesRegex;

TEST(MetadataVersionTest,
     GetMinimumMetadataParserVersionSucceedsWithValidMetadata) {
  // Creates a dummy metadata flatbuffer for test.
  flatbuffers::FlatBufferBuilder builder(1024);
  auto name = builder.CreateString("Foo");
  ModelMetadataBuilder metadata_builder(builder);
  metadata_builder.add_name(name);
  auto metadata = metadata_builder.Finish();
  FinishModelMetadataBuffer(builder, metadata);

  // Gets the mimimum metadata parser version.
  std::string min_version;
  EXPECT_EQ(GetMinimumMetadataParserVersion(builder.GetBufferPointer(),
                                            builder.GetSize(), &min_version),
            kTfLiteOk);
  // Validates that the version is well-formed (x.y.z).
  EXPECT_THAT(min_version, MatchesRegex("[0-9]*\\.[0-9]*\\.[0-9]"));
}

TEST(MetadataVersionTest,
     GetMinimumMetadataParserVersionSucceedsWithInvalidIdentifier) {
  // Creates a dummy metadata flatbuffer without identifier.
  flatbuffers::FlatBufferBuilder builder(1024);
  ModelMetadataBuilder metadata_builder(builder);
  auto metadata = metadata_builder.Finish();
  builder.Finish(metadata);

  // Gets the mimimum metadata parser version and triggers error.
  std::string min_version;
  EXPECT_EQ(GetMinimumMetadataParserVersion(builder.GetBufferPointer(),
                                            builder.GetSize(), &min_version),
            kTfLiteError);
  EXPECT_TRUE(min_version.empty());
}

}  // namespace
}  // namespace metadata
}  // namespace tflite
