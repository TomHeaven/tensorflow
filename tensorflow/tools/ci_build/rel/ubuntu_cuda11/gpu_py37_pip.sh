#!/bin/bash
# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
set -e
set -x

source tensorflow/tools/ci_build/release/common.sh

install_ubuntu_16_pip_deps pip3.7
# Update bazel
install_bazelisk

# Export required variables for running pip.sh
export OS_TYPE="UBUNTU"
export CONTAINER_TYPE="GPU"
export TF_PYTHON_VERSION='python3.7'

# Run configure.
export PYTHON_BIN_PATH=$(which ${TF_PYTHON_VERSION})
yes "" | "$PYTHON_BIN_PATH" configure.py

# Get the default test targets for bazel.
source tensorflow/tools/ci_build/build_scripts/DEFAULT_TEST_TARGETS.sh

# Export optional variables for running pip.sh
export TF_TEST_FILTER_TAGS='gpu,requires-gpu,-no_gpu,-no_oss,-oss_serial,-no_oss_py37'
# TODO (pkanwar): Revert this CL (cl/326069644) once the cuda 11 migration is complete.
export TF_BUILD_FLAGS="--config=release_common "
export TF_TEST_FLAGS="--test_tag_filters=${TF_TEST_FILTER_TAGS} --build_tag_filters=${TF_TEST_FILTER_TAGS} \
--distinct_host_configuration=false \
--action_env=TF_CUDA_VERSION=11 --action_env=TF_CUDNN_VERSION=8 --test_env=TF2_BEHAVIOR=1 \
--config=cuda --test_output=errors --local_test_jobs=4 --test_lang_filters=py \
--verbose_failures=true --keep_going --define=no_tensorflow_py_deps=true \
--run_under=//tensorflow/tools/ci_build/gpu_build:parallel_gpu_execute \
--config=cuda \
--config=tensorrt \
--action_env=CUDA_TOOLKIT_PATH=/usr/local/cuda-11.0 --action_env=TF_NEED_TENSORRT=1 \
--action_env=TF_CUDA_COMPUTE_CAPABILITIES=sm_35,sm_37,sm_52,sm_60,sm_61,compute_70 \
--action_env=TENSORRT_INSTALL_PATH=/usr/local/tensorrt \
--action_env=LD_LIBRARY_PATH=/usr/local/cuda:/usr/local/cuda/lib64:/usr/local/cuda/extras/CUPTI/lib64:/usr/local/tensorrt/lib \
--action_env=GCC_HOST_COMPILER_PATH=/usr/bin/gcc-5 \
--config=avx_linux \
--crosstool_top=//third_party/toolchains/preconfig/ubuntu16.04/gcc7_manylinux2010-nvcc-cuda11:toolchain"
export TF_TEST_TARGETS="${DEFAULT_BAZEL_TARGETS} -//tensorflow/lite/... "
export TF_PIP_TESTS="test_pip_virtualenv_non_clean test_pip_virtualenv_clean"
#export IS_NIGHTLY=0 # Not nightly; uncomment if building from tf repo.
export TF_PROJECT_NAME=="tensorflow_gpu"
export TF_PIP_TEST_ROOT="pip_test"

# To build both tensorflow and tensorflow-gpu pip packages
export TF_BUILD_BOTH_GPU_PACKAGES=1

./tensorflow/tools/ci_build/builds/pip_new.sh
