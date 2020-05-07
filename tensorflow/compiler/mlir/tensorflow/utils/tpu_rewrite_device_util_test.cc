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

#include "tensorflow/compiler/mlir/tensorflow/utils/tpu_rewrite_device_util.h"

#include <cstdint>
#include <tuple>

#include "llvm/Support/FormatVariadic.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/protobuf/tpu/topology.pb.h"
#include "tensorflow/core/util/device_name_utils.h"

namespace tensorflow {
namespace {

using Device = DeviceNameUtils::ParsedName;

bool DeviceNamesToParsedNames(llvm::ArrayRef<std::string> device_names,
                              llvm::SmallVectorImpl<Device>* parsed_devices) {
  parsed_devices->reserve(device_names.size());
  for (const auto& device_name : device_names) {
    Device parsed_name;
    if (!DeviceNameUtils::ParseFullName(device_name, &parsed_name))
      return false;

    parsed_devices->push_back(parsed_name);
  }
  return true;
}

using DeviceNames = llvm::SmallVector<std::string, 8>;

struct ParameterizedDeviceSetTest
    : ::testing::TestWithParam<std::tuple<DeviceNames, std::string>> {};

TEST_P(ParameterizedDeviceSetTest, BadDeviceSet) {
  llvm::SmallVector<Device, 8> devices;
  ASSERT_TRUE(DeviceNamesToParsedNames(std::get<0>(GetParam()), &devices));
  std::string topology_attr;
  std::vector<int64_t> device_assignment_attr;

  auto status_or = GetTPUCompilationAndExecutionDevices(
      devices, /*num_replicas=*/1, /*num_cores_per_replica=*/1, topology_attr,
      device_assignment_attr);
  ASSERT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().error_message(), std::get<1>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    BadDeviceSet, ParameterizedDeviceSetTest,
    ::testing::Values(
        std::make_tuple<DeviceNames, std::string>(
            {"/job:localhost/replica:0/task:0/device:CPU:0"},
            "no TPU_SYSTEM devices found"),
        std::make_tuple<DeviceNames, std::string>(
            {"/job:localhost/replica:0/task:0/device:TPU_SYSTEM:0",
             "/job:worker/replica:0/task:0/device:TPU_SYSTEM:0"},
            "found TPU_SYSTEM devices with conflicting jobs 'localhost' and "
            "'worker'"),
        std::make_tuple<DeviceNames, std::string>(
            {"/job:localhost/replica:0/task:0/device:TPU_SYSTEM:0",
             "/job:localhost/replica:1/task:0/device:TPU_SYSTEM:0"},
            "found TPU_SYSTEM devices with conflicting replicas '0' and '1'"),
        std::make_tuple<DeviceNames, std::string>(
            {"/job:localhost/replica:0/task:0/device:TPU_SYSTEM:0",
             "/job:localhost/replica:0/task:0/device:TPU:0",
             "/job:localhost/replica:0/task:0/device:TPU:1",
             "/job:localhost/replica:0/task:1/device:TPU_SYSTEM:0",
             "/job:localhost/replica:0/task:1/device:TPU:0"},
            "expected the number of TPU devices per host to be 2, got 1")));

struct ParameterizedMetadataTest
    : ::testing::TestWithParam<std::tuple<int, int, std::string,
                                          std::vector<int64_t>, std::string>> {
};

TEST_P(ParameterizedMetadataTest, BadMetadata) {
  llvm::SmallVector<Device, 8> devices;
  ASSERT_TRUE(DeviceNamesToParsedNames(
      {"/job:worker/replica:0/task:0/device:TPU_SYSTEM:0",
       "/job:worker/replica:0/task:0/device:TPU:0",
       "/job:worker/replica:0/task:1/device:TPU_SYSTEM:0",
       "/job:worker/replica:0/task:1/device:TPU:0"},
      &devices));
  std::string compilation_device;
  llvm::SmallVector<llvm::SmallVector<std::string, 8>, 8> execution_devices;
  llvm::Optional<xla::DeviceAssignmentProto> xla_device_assignment;

  auto status_or = GetTPUCompilationAndExecutionDevices(
      devices, std::get<0>(GetParam()), std::get<1>(GetParam()),
      std::get<2>(GetParam()), std::get<3>(GetParam()));
  ASSERT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().error_message(), std::get<4>(GetParam()));
}

std::string TopologyWithMeshShape(llvm::ArrayRef<int> mesh_shape) {
  tpu::TopologyProto topology_proto;
  for (int mesh_dim : mesh_shape) topology_proto.add_mesh_shape(mesh_dim);
  return topology_proto.SerializeAsString();
}

std::string TopologyWithMeshShapeAndTasks(llvm::ArrayRef<int> mesh_shape,
                                          int num_tasks,
                                          int num_tpu_devices_per_task) {
  tpu::TopologyProto topology_proto;
  for (int mesh_dim : mesh_shape) topology_proto.add_mesh_shape(mesh_dim);
  topology_proto.set_num_tasks(num_tasks);
  topology_proto.set_num_tpu_devices_per_task(num_tpu_devices_per_task);
  return topology_proto.SerializeAsString();
}

std::string TopologyWithDeviceCoordinates(
    llvm::ArrayRef<int> device_coordinates) {
  tpu::TopologyProto topology_proto;
  topology_proto.add_mesh_shape(2);
  topology_proto.add_mesh_shape(1);
  topology_proto.add_mesh_shape(1);
  topology_proto.add_mesh_shape(1);
  topology_proto.set_num_tasks(2);
  topology_proto.set_num_tpu_devices_per_task(1);
  for (int device_coordinate : device_coordinates)
    topology_proto.add_device_coordinates(device_coordinate);
  return topology_proto.SerializeAsString();
}

INSTANTIATE_TEST_SUITE_P(
    BadFullMeshMetadata, ParameterizedMetadataTest,
    ::testing::Values(
        std::make_tuple(
            2, 1, "", std::vector<int64_t>{0},
            "'device_assignment' must not be set when 'topology' is not set"),
        std::make_tuple(8, 1, "", std::vector<int64_t>(),
                        "'num_replicas' must be equal to 1 or 2, got 8"),
        std::make_tuple(2, 2, "", std::vector<int64_t>(),
                        "'num_cores_per_replica' must be equal to 1, got 2")));

INSTANTIATE_TEST_SUITE_P(
    BadGeneralTopologyMetadata, ParameterizedMetadataTest,
    ::testing::Values(
        std::make_tuple(
            2, 1, "BAD_TOPOLOGY", std::vector<int64_t>(),
            "failed to parse 'topology' attribute to TopologyProto"),
        std::make_tuple(4, 2, TopologyWithMeshShape({0}),
                        std::vector<int64_t>(),
                        "'topology' 'mesh_shape' must be rank 4, got rank 1"),
        std::make_tuple(
            2, 1, TopologyWithMeshShape({2, 0, 1, 2}), std::vector<int64_t>(),
            "'topology' 'mesh_shape' dimension 1 must be positive, got 0"),
        std::make_tuple(2, 1, TopologyWithMeshShapeAndTasks({1, 1, 1, 1}, 1, 1),
                        std::vector<int64_t>(),
                        "number of tasks from available TPU devices must be "
                        "'num_tasks' in 'topology' (1), got 2"),
        std::make_tuple(2, 1, TopologyWithMeshShapeAndTasks({1, 1, 1, 1}, 2, 2),
                        std::vector<int64_t>(),
                        "number of TPU devices available per task must be "
                        "'num_tpu_devices_per_task' in 'topology' (2), got 1"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({}), std::vector<int64_t>(),
            "length of 'device_coordinates' in 'topology' must be 'num_tasks' "
            "* 'num_tpus_per_task' * 4 (2 * 1 * 4), got 0"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({-1, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>(),
            "device coordinate (-1, 0, 0, 0) in 'topology' is outside "
            "of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({2, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>(),
            "device coordinate (2, 0, 0, 0) in 'topology' is outside "
            "of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, -1, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>(),
            "device coordinate (0, -1, 0, 0) in 'topology' is outside "
            "of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 1, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>(),
            "device coordinate (0, 1, 0, 0) in 'topology' is outside "
            "of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, -1, 1, 0, 0, 0}),
            std::vector<int64_t>(),
            "device coordinate (0, 0, 0, -1) in 'topology' is outside "
            "of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 1, 1, 0, 0, 0}),
            std::vector<int64_t>(),
            "device coordinate (0, 0, 0, 1) in 'topology' is outside "
            "of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 0, 0, 0, 0}),
            std::vector<int64_t>(),
            "'topology' has duplicate device coordinate (0, 0, 0, 0)")));

INSTANTIATE_TEST_SUITE_P(
    BadGeneralDeviceAssignmentMetadata, ParameterizedMetadataTest,
    ::testing::Values(
        std::make_tuple(2, 1,
                        TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
                        std::vector<int64_t>(),
                        "length of 'device_assignment' must be 'num_replicas' "
                        "* 'num_cores_per_replica' * 4 (2 * 1 * 4), got 0"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>{-1, 0, 0, 0, 0, 0, 0, 0},
            "device coordinate (-1, 0, 0, 0) in 'device_assignment' "
            "is outside of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>{2, 0, 0, 0, 0, 0, 0, 0},
            "device coordinate (2, 0, 0, 0) in 'device_assignment' is "
            "outside of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>{0, -1, 0, 0, 0, 0, 0, 0},
            "device coordinate (0, -1, 0, 0) in 'device_assignment' "
            "is outside of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>{0, 1, 0, 0, 0, 0, 0, 0},
            "device coordinate (0, 1, 0, 0) in 'device_assignment' is "
            "outside of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>{0, 0, 0, -1, 0, 0, 0, 0},
            "device coordinate (0, 0, 0, -1) in 'device_assignment' "
            "is outside of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(
            2, 1, TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
            std::vector<int64_t>{0, 0, 0, 1, 0, 0, 0, 0},
            "device coordinate (0, 0, 0, 1) in 'device_assignment' is "
            "outside of mesh shape (2, 1, 1, 1)"),
        std::make_tuple(2, 1,
                        TopologyWithDeviceCoordinates({0, 0, 0, 0, 1, 0, 0, 0}),
                        std::vector<int64_t>{0, 0, 0, 0, 0, 0, 0, 0},
                        "'device_assignment' has duplicate device coordinate "
                        "(0, 0, 0, 0)")));

std::vector<std::string> MakeDeviceSet(int num_tasks,
                                       int num_devices_per_task) {
  std::vector<std::string> devices{
      "/job:localhost/replica:0/task:0/device:CPU:0"};
  devices.reserve(num_tasks * num_devices_per_task + num_tasks + 1);

  for (int task = 0; task < num_tasks; ++task) {
    devices.push_back(
        llvm::formatv("/job:worker/replica:0/task:{0}/device:CPU:0", task)
            .str());
    devices.push_back(
        llvm::formatv("/job:worker/replica:0/task:{0}/device:TPU_SYSTEM:0",
                      task)
            .str());
    for (int device = 0; device < num_devices_per_task; ++device)
      devices.push_back(
          llvm::formatv("/job:worker/replica:0/task:{0}/device:TPU:{1}", task,
                        device)
              .str());
  }

  return devices;
}

TEST(TPURewriteDeviceUtilTest,
     BadGeneralDeviceAssignmentMetadataMissingDevice) {
  tpu::TopologyProto topology_proto;
  {
    topology_proto.add_mesh_shape(2);
    topology_proto.add_mesh_shape(1);
    topology_proto.add_mesh_shape(1);
    topology_proto.add_mesh_shape(1);
    topology_proto.set_num_tasks(1);
    topology_proto.set_num_tpu_devices_per_task(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
  }

  std::string topology_attr = topology_proto.SerializeAsString();
  std::vector<int64_t> device_assignment_attr{1, 0, 0, 0};

  llvm::SmallVector<Device, 8> devices;
  std::vector<std::string> device_names =
      MakeDeviceSet(/*num_tasks=*/1, /*num_devices_per_task=*/1);
  ASSERT_TRUE(DeviceNamesToParsedNames(device_names, &devices));

  auto status_or = GetTPUCompilationAndExecutionDevices(
      devices, /*num_replicas=*/1, /*num_cores_per_replica=*/1, topology_attr,
      device_assignment_attr);

  ASSERT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().error_message(),
            "no TPU device found for 'device_assignment' device coordinate (1, "
            "0, 0, 0)");
}

TEST(TPURewriteDeviceUtilTest, ValidFullMeshDeviceAssignment) {
  llvm::SmallVector<Device, 8> devices;
  std::vector<std::string> device_names =
      MakeDeviceSet(/*num_tasks=*/2, /*num_devices_per_task=*/4);
  ASSERT_TRUE(DeviceNamesToParsedNames(device_names, &devices));
  std::string topology_attr;
  std::vector<int64_t> device_assignment_attr;

  auto status_or = GetTPUCompilationAndExecutionDevices(
      devices, /*num_replicas=*/8, /*num_cores_per_replica=*/1, topology_attr,
      device_assignment_attr);

  TF_ASSERT_OK(status_or.status());

  auto& tpu_device_assignment = status_or.ValueOrDie();
  EXPECT_EQ(tpu_device_assignment.compilation_device,
            "/job:worker/replica:0/task:0/device:CPU:0");
  auto& execution_devices = tpu_device_assignment.execution_devices;
  ASSERT_EQ(execution_devices.size(), 8);
  for (const auto& replica_execution_device : execution_devices)
    ASSERT_EQ(replica_execution_device.size(), 1);

  EXPECT_EQ(execution_devices[0][0],
            "/job:worker/replica:0/task:0/device:TPU:0");
  EXPECT_EQ(execution_devices[1][0],
            "/job:worker/replica:0/task:0/device:TPU:1");
  EXPECT_EQ(execution_devices[2][0],
            "/job:worker/replica:0/task:0/device:TPU:2");
  EXPECT_EQ(execution_devices[3][0],
            "/job:worker/replica:0/task:0/device:TPU:3");
  EXPECT_EQ(execution_devices[4][0],
            "/job:worker/replica:0/task:1/device:TPU:0");
  EXPECT_EQ(execution_devices[5][0],
            "/job:worker/replica:0/task:1/device:TPU:1");
  EXPECT_EQ(execution_devices[6][0],
            "/job:worker/replica:0/task:1/device:TPU:2");
  EXPECT_EQ(execution_devices[7][0],
            "/job:worker/replica:0/task:1/device:TPU:3");

  EXPECT_FALSE(tpu_device_assignment.xla_device_assignment.hasValue());
}

TEST(TPURewriteDeviceUtilTest, ValidGeneralDeviceAssignmentMesh2x2x2) {
  tpu::TopologyProto topology_proto;
  {
    topology_proto.add_mesh_shape(2);
    topology_proto.add_mesh_shape(2);
    topology_proto.add_mesh_shape(1);
    topology_proto.add_mesh_shape(2);
    topology_proto.set_num_tasks(2);
    topology_proto.set_num_tpu_devices_per_task(4);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
  }

  std::string topology_attr = topology_proto.SerializeAsString();
  std::vector<int64_t> device_assignment_attr{0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0,
                                              0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0,
                                              0, 1, 1, 1, 0, 0, 1, 1, 0, 1};

  llvm::SmallVector<Device, 8> devices;
  std::vector<std::string> device_names =
      MakeDeviceSet(/*num_tasks=*/2, /*num_devices_per_task=*/4);
  ASSERT_TRUE(DeviceNamesToParsedNames(device_names, &devices));

  auto status_or = GetTPUCompilationAndExecutionDevices(
      devices, /*num_replicas=*/4, /*num_cores_per_replica=*/2, topology_attr,
      device_assignment_attr);

  TF_ASSERT_OK(status_or.status());

  auto& tpu_device_assignment = status_or.ValueOrDie();
  EXPECT_EQ(tpu_device_assignment.compilation_device,
            "/job:worker/replica:0/task:0/device:CPU:0");
  auto& execution_devices = tpu_device_assignment.execution_devices;
  ASSERT_EQ(execution_devices.size(), 4);
  for (const auto& replica_execution_device : execution_devices)
    ASSERT_EQ(replica_execution_device.size(), 2);

  EXPECT_EQ(execution_devices[0][0],
            "/job:worker/replica:0/task:0/device:TPU:0");
  EXPECT_EQ(execution_devices[0][1],
            "/job:worker/replica:0/task:1/device:TPU:3");
  EXPECT_EQ(execution_devices[1][0],
            "/job:worker/replica:0/task:0/device:TPU:1");
  EXPECT_EQ(execution_devices[1][1],
            "/job:worker/replica:0/task:1/device:TPU:2");
  EXPECT_EQ(execution_devices[2][0],
            "/job:worker/replica:0/task:0/device:TPU:3");
  EXPECT_EQ(execution_devices[2][1],
            "/job:worker/replica:0/task:1/device:TPU:0");
  EXPECT_EQ(execution_devices[3][0],
            "/job:worker/replica:0/task:0/device:TPU:2");
  EXPECT_EQ(execution_devices[3][1],
            "/job:worker/replica:0/task:1/device:TPU:1");

  auto& xla_device_assignment = tpu_device_assignment.xla_device_assignment;
  ASSERT_TRUE(xla_device_assignment.hasValue());
  EXPECT_EQ(xla_device_assignment->replica_count(), 4);
  EXPECT_EQ(xla_device_assignment->computation_count(), 2);
  ASSERT_EQ(xla_device_assignment->computation_devices_size(), 2);
  const auto& computation_device_0 =
      xla_device_assignment->computation_devices(0);
  ASSERT_EQ(computation_device_0.replica_device_ids_size(), 4);
  const auto& computation_device_1 =
      xla_device_assignment->computation_devices(1);
  ASSERT_EQ(computation_device_1.replica_device_ids_size(), 4);

  EXPECT_EQ(computation_device_0.replica_device_ids(0), 0);
  EXPECT_EQ(computation_device_0.replica_device_ids(1), 4);
  EXPECT_EQ(computation_device_0.replica_device_ids(2), 2);
  EXPECT_EQ(computation_device_0.replica_device_ids(3), 6);
  EXPECT_EQ(computation_device_1.replica_device_ids(0), 1);
  EXPECT_EQ(computation_device_1.replica_device_ids(1), 5);
  EXPECT_EQ(computation_device_1.replica_device_ids(2), 3);
  EXPECT_EQ(computation_device_1.replica_device_ids(3), 7);
}

TEST(TPURewriteDeviceUtilTest, ValidGeneralDeviceAssignmentMesh1x2x1x3) {
  tpu::TopologyProto topology_proto;
  {
    topology_proto.add_mesh_shape(1);
    topology_proto.add_mesh_shape(2);
    topology_proto.add_mesh_shape(1);
    topology_proto.add_mesh_shape(3);
    topology_proto.set_num_tasks(3);
    topology_proto.set_num_tpu_devices_per_task(2);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(2);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(1);
    topology_proto.add_device_coordinates(0);
    topology_proto.add_device_coordinates(2);
  }

  std::string topology_attr = topology_proto.SerializeAsString();
  std::vector<int64_t> device_assignment_attr{
      0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 2, 0, 1, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0};

  llvm::SmallVector<Device, 8> devices;
  std::vector<std::string> device_names =
      MakeDeviceSet(/*num_tasks=*/3, /*num_devices_per_task=*/2);
  ASSERT_TRUE(DeviceNamesToParsedNames(device_names, &devices));

  auto status_or = GetTPUCompilationAndExecutionDevices(
      devices, /*num_replicas=*/2, /*num_cores_per_replica=*/3, topology_attr,
      device_assignment_attr);

  TF_ASSERT_OK(status_or.status());

  auto& tpu_device_assignment = status_or.ValueOrDie();
  EXPECT_EQ(tpu_device_assignment.compilation_device,
            "/job:worker/replica:0/task:0/device:CPU:0");

  auto& execution_devices = tpu_device_assignment.execution_devices;
  ASSERT_EQ(execution_devices.size(), 2);
  for (const auto& replica_execution_device : execution_devices)
    ASSERT_EQ(replica_execution_device.size(), 3);

  EXPECT_EQ(execution_devices[0][0],
            "/job:worker/replica:0/task:1/device:TPU:1");
  EXPECT_EQ(execution_devices[0][1],
            "/job:worker/replica:0/task:1/device:TPU:0");
  EXPECT_EQ(execution_devices[0][2],
            "/job:worker/replica:0/task:2/device:TPU:0");
  EXPECT_EQ(execution_devices[1][0],
            "/job:worker/replica:0/task:2/device:TPU:1");
  EXPECT_EQ(execution_devices[1][1],
            "/job:worker/replica:0/task:0/device:TPU:0");
  EXPECT_EQ(execution_devices[1][2],
            "/job:worker/replica:0/task:0/device:TPU:1");

  auto& xla_device_assignment = tpu_device_assignment.xla_device_assignment;
  ASSERT_TRUE(xla_device_assignment.hasValue());
  EXPECT_EQ(xla_device_assignment->replica_count(), 2);
  EXPECT_EQ(xla_device_assignment->computation_count(), 3);
  ASSERT_EQ(xla_device_assignment->computation_devices_size(), 3);
  const auto& computation_device_0 =
      xla_device_assignment->computation_devices(0);
  ASSERT_EQ(computation_device_0.replica_device_ids_size(), 2);
  const auto& computation_device_1 =
      xla_device_assignment->computation_devices(1);
  ASSERT_EQ(computation_device_1.replica_device_ids_size(), 2);
  const auto& computation_device_2 =
      xla_device_assignment->computation_devices(2);
  ASSERT_EQ(computation_device_2.replica_device_ids_size(), 2);

  EXPECT_EQ(computation_device_0.replica_device_ids(0), 1);
  EXPECT_EQ(computation_device_0.replica_device_ids(1), 5);
  EXPECT_EQ(computation_device_1.replica_device_ids(0), 4);
  EXPECT_EQ(computation_device_1.replica_device_ids(1), 0);
  EXPECT_EQ(computation_device_2.replica_device_ids(0), 2);
  EXPECT_EQ(computation_device_2.replica_device_ids(1), 3);
}

struct ParameterizedCPUHostForTPUDeviceTest
    : ::testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(ParameterizedCPUHostForTPUDeviceTest, CPUHostForTPUDevice) {
  auto status_or_device = GetCPUHostForTPUDevice(std::get<0>(GetParam()));
  TF_ASSERT_OK(status_or_device.status());
  EXPECT_EQ(status_or_device.ValueOrDie(), std::get<1>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    CPUHostForTPUDevice, ParameterizedCPUHostForTPUDeviceTest,
    ::testing::Values(
        std::make_tuple("/job:worker/replica:0/task:0/device:TPU:0",
                        "/job:worker/replica:0/task:0/device:CPU:0"),
        std::make_tuple("/job:worker/replica:0/task:1/device:TPU:1",
                        "/job:worker/replica:0/task:1/device:CPU:0")));

TEST(TPURewriteDeviceUtilTest, CPUHostForTPUDeviceInvalidDevice) {
  auto status_or_device = GetCPUHostForTPUDevice("bad_device");
  ASSERT_FALSE(status_or_device.ok());
}

TEST(TPURewriteDeviceUtilTest, CPUHostsForTPUDevices) {
  auto status_or_devices =
      GetCPUHostsForTPUDevices({"/job:worker/replica:0/task:0/device:TPU:0",
                                "/job:worker/replica:0/task:1/device:TPU:1"});
  TF_ASSERT_OK(status_or_devices.status());
  const auto& devices = status_or_devices.ValueOrDie();
  ASSERT_EQ(devices.size(), 2);
  EXPECT_EQ(devices[0], "/job:worker/replica:0/task:0/device:CPU:0");
  EXPECT_EQ(devices[1], "/job:worker/replica:0/task:1/device:CPU:0");
}

TEST(TPURewriteDeviceUtilTest, CPUHostsForTPUDevicesInvalidDevice) {
  auto status_or_devices = GetCPUHostsForTPUDevices(
      {"/job:worker/replica:0/task:0/device:TPU:0", "bad_device"});
  ASSERT_FALSE(status_or_devices.ok());
}

}  // anonymous namespace
}  // namespace tensorflow
