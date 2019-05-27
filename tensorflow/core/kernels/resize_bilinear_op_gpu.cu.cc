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

// See docs in ../ops/image_ops.cc.

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM

#define EIGEN_USE_GPU

#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/kernels/resize_bilinear_op.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/gpu_kernel_helper.h"

// auxilary 16-byte datatype for ResizeBilinearKernel_faster
// the fields are not important. The only purpose of this is to read 16 bytes
// from GPU gloal memory
struct four_floats{
  float a;
  float b;
  float c;
  float d;
};

namespace tensorflow {

typedef Eigen::GpuDevice GPUDevice;

namespace {

template <typename T, int C_UNROLL>
__global__ void ResizeBilinearKernel_faster(const int num_channel_thread, const T* __restrict__ images,
                                     float height_scale, float width_scale,
                                     int batch, int in_height, int in_width,
                                     int channels, int out_height,
                                     int out_width, float* __restrict__ output) {

  for (int out_idx = blockIdx.x * blockDim.x + threadIdx.x; out_idx < out_width*out_height*num_channel_per_thread; out_idx += blockDim.x * gridDim.x){
    int idx = out_idx;
    const int c_start = idx % num_channel_thread;
    idx /= num_channel_thread;
    const int x = idx % out_width;
    idx /= out_width;
    const int y = idx % out_height;

    const float in_y = (static_cast<float>(y) + 0.5f) * height_scale - 0.5f;

    const int top_y_index = in_y > 0.0 ? floorf(in_y) : 0;
    const int bottom_y_index =
        (in_y < in_height - 1) ? ceilf(in_y) : in_height - 1;
    const float y_lerp = in_y - floorf(in_y);

    const float in_x = (static_cast<float>(x) + 0.5f) * width_scale - 0.5f;
    const int left_x_index = in_x > 0.0 ? floorf(in_x) : 0;
    const int right_x_index =
        (in_x < in_width - 1) ? ceilf(in_x) : in_width - 1;
    const float x_lerp = in_x - left_x_index;


    float top_left_reg[C_UNROLL];
    float top_right_reg[C_UNROLL];
    float bottom_left_reg[C_UNROLL];
    float bottom_right_reg[C_UNROLL];
    float out_reg[C_UNROLL];
    for (int b =0; b < batch; b++) {
      for (int c = c_start*C_UNROLL; c < channels; c+= C_UNROLL*num_channel_per_thread) {

        // 16 byte read from global memroy and cache them in registers
        ((four_floats*) top_left_reg)[0] = ((four_floats*) images)[(((b * in_height + top_y_index) * in_width + left_x_index) * channels + c)/4 ];
        ((four_floats*) top_right_reg)[0] = ((four_floats*) images)[(((b * in_height + top_y_index) * in_width + right_x_index) * channels + c)/4];
        ((four_floats*) bottom_left_reg)[0] = ((four_floats*) images)[(((b * in_height + bottom_y_index) * in_width + left_x_index) * channels + c)/4];
        ((four_floats*) bottom_right_reg)[0] = ((four_floats*) images)[(((b * in_height + bottom_y_index) * in_width + right_x_index) * channels +c)/4];
#pragma unroll
        for (int unroll = 0; unroll < C_UNROLL; unroll+=1){
          const float top = top_left_reg[unroll] + (top_right_reg[unroll] - top_left_reg[unroll]) * x_lerp;
          const float bottom = bottom_left_reg[unroll] + (bottom_right_reg[unroll] - bottom_left_reg[unroll]) * x_lerp;
          out_reg[unroll] = top + (bottom - top) * y_lerp;
        }
        ((four_floats*) output)[(((b *out_height + y) * out_width + x) * channels + c)/4] = ((four_floats*) out_reg)[0];
      }
    }
  }
}



template <typename T>
__global__ void ResizeBilinearKernel(const int32 nthreads, const T* images,
                                     float height_scale, float width_scale,
                                     int batch, int in_height, int in_width,
                                     int channels, int out_height,
                                     int out_width, float* output) {
  GPU_1D_KERNEL_LOOP(out_idx, nthreads) {
    // out_idx = c + channels * (x + out_width * (y + out_height * b))
    int idx = out_idx;
    const int c = idx % channels;
    idx /= channels;
    const int x = idx % out_width;
    idx /= out_width;
    const int y = idx % out_height;
    const int b = idx / out_height;

    const float in_y = (static_cast<float>(y) + 0.5f) * height_scale - 0.5f;

    const int top_y_index = in_y > 0.0 ? floorf(in_y) : 0;
    const int bottom_y_index =
        (in_y < in_height - 1) ? ceilf(in_y) : in_height - 1;
    const float y_lerp = in_y - floorf(in_y);

    const float in_x = (static_cast<float>(x) + 0.5f) * width_scale - 0.5f;
    const int left_x_index = in_x > 0.0 ? floorf(in_x) : 0;
    const int right_x_index =
        (in_x < in_width - 1) ? ceilf(in_x) : in_width - 1;
    const float x_lerp = in_x - left_x_index;

    const float top_left(
        images[((b * in_height + top_y_index) * in_width + left_x_index) *
                   channels +
               c]);
    const float top_right(
        images[((b * in_height + top_y_index) * in_width + right_x_index) *
                   channels +
               c]);
    const float bottom_left(
        images[((b * in_height + bottom_y_index) * in_width + left_x_index) *
                   channels +
               c]);
    const float bottom_right(
        images[((b * in_height + bottom_y_index) * in_width + right_x_index) *
                   channels +
               c]);

    const float top = top_left + (top_right - top_left) * x_lerp;
    const float bottom = bottom_left + (bottom_right - bottom_left) * x_lerp;
    output[out_idx] = top + (bottom - top) * y_lerp;
  }
}

template <typename T>
__global__ void ResizeBilinearGradKernel(
    const int32 nthreads, const float* input_grad, float height_scale,
    float width_scale, int batch, int original_height, int original_width,
    int channels, int resized_height, int resized_width, T* output_grad) {
  GPU_1D_KERNEL_LOOP(in_idx, nthreads) {
    // in_idx = c + channels * (x + resized_width * (y + resized_height * b))
    int idx = in_idx;
    const int c = idx % channels;
    idx /= channels;
    const int x = idx % resized_width;
    idx /= resized_width;
    const int y = idx % resized_height;
    const int b = idx / resized_height;

    const float original_y =
        (static_cast<float>(y) + 0.5f) * height_scale - 0.5f;
    const int top_y_index = original_y > 0.0 ? floorf(original_y) : 0;
    const int bottom_y_index = (original_y < original_height - 1)
                                   ? ceilf(original_y)
                                   : original_height - 1;
    const float y_lerp = original_y - floorf(original_y);

    const float original_x =
        (static_cast<float>(x) + 0.5f) * width_scale - 0.5f;

    const int left_x_index = original_x > 0.0 ? floorf(original_x) : 0;
    const int right_x_index = (original_x < original_width - 1)
                                  ? ceilf(original_x)
                                  : original_width - 1;
    const float x_lerp = original_x - floorf(original_x);

    const float dtop = (1 - y_lerp) * input_grad[in_idx];
    GpuAtomicAdd(output_grad +
                     ((b * original_height + top_y_index) * original_width +
                      left_x_index) *
                         channels +
                     c,
                 static_cast<T>((1 - x_lerp) * dtop));
    GpuAtomicAdd(output_grad +
                     ((b * original_height + top_y_index) * original_width +
                      right_x_index) *
                         channels +
                     c,
                 static_cast<T>(x_lerp * dtop));

    const float dbottom = y_lerp * input_grad[in_idx];
    GpuAtomicAdd(output_grad +
                     ((b * original_height + bottom_y_index) * original_width +
                      left_x_index) *
                         channels +
                     c,
                 static_cast<T>((1 - x_lerp) * dbottom));
    GpuAtomicAdd(output_grad +
                     ((b * original_height + bottom_y_index) * original_width +
                      right_x_index) *
                         channels +
                     c,
                 static_cast<T>(x_lerp * dbottom));
  }
}

template <typename T>
__global__ void LegacyResizeBilinearKernel(const int32 nthreads,
                                           const T* images, float height_scale,
                                           float width_scale, int batch,
                                           int in_height, int in_width,
                                           int channels, int out_height,
                                           int out_width, float* output) {
  GPU_1D_KERNEL_LOOP(out_idx, nthreads) {
    // out_idx = c + channels * (x + out_width * (y + out_height * b))
    int idx = out_idx;
    const int c = idx % channels;
    idx /= channels;
    const int x = idx % out_width;
    idx /= out_width;
    const int y = idx % out_height;
    const int b = idx / out_height;

    const float in_y = y * height_scale;
    const int top_y_index = floorf(in_y);
    const int bottom_y_index =
        (in_y < in_height - 1) ? ceilf(in_y) : in_height - 1;
    const float y_lerp = in_y - top_y_index;

    const float in_x = x * width_scale;
    const int left_x_index = floorf(in_x);
    const int right_x_index =
        (in_x < in_width - 1) ? ceilf(in_x) : in_width - 1;
    const float x_lerp = in_x - left_x_index;

    const float top_left(
        images[((b * in_height + top_y_index) * in_width + left_x_index) *
                   channels +
               c]);
    const float top_right(
        images[((b * in_height + top_y_index) * in_width + right_x_index) *
                   channels +
               c]);
    const float bottom_left(
        images[((b * in_height + bottom_y_index) * in_width + left_x_index) *
                   channels +
               c]);
    const float bottom_right(
        images[((b * in_height + bottom_y_index) * in_width + right_x_index) *
                   channels +
               c]);

    const float top = top_left + (top_right - top_left) * x_lerp;
    const float bottom = bottom_left + (bottom_right - bottom_left) * x_lerp;
    output[out_idx] = top + (bottom - top) * y_lerp;
  }
}

template <typename T>
__global__ void LegacyResizeBilinearGradKernel(
    const int32 nthreads, const float* input_grad, float height_scale,
    float width_scale, int batch, int original_height, int original_width,
    int channels, int resized_height, int resized_width, T* output_grad) {
  GPU_1D_KERNEL_LOOP(in_idx, nthreads) {
    // in_idx = c + channels * (x + resized_width * (y + resized_height * b))
    int idx = in_idx;
    const int c = idx % channels;
    idx /= channels;
    const int x = idx % resized_width;
    idx /= resized_width;
    const int y = idx % resized_height;
    const int b = idx / resized_height;

    const float original_y = y * height_scale;
    const int top_y_index = floorf(original_y);
    const int bottom_y_index = (original_y < original_height - 1)
                                   ? ceilf(original_y)
                                   : original_height - 1;
    const float y_lerp = original_y - top_y_index;

    const float original_x = x * width_scale;
    const int left_x_index = floorf(original_x);
    const int right_x_index = (original_x < original_width - 1)
                                  ? ceilf(original_x)
                                  : original_width - 1;
    const float x_lerp = original_x - left_x_index;

    const float dtop = (1 - y_lerp) * input_grad[in_idx];
    GpuAtomicAdd(output_grad +
                     ((b * original_height + top_y_index) * original_width +
                      left_x_index) *
                         channels +
                     c,
                 static_cast<T>((1 - x_lerp) * dtop));
    GpuAtomicAdd(output_grad +
                     ((b * original_height + top_y_index) * original_width +
                      right_x_index) *
                         channels +
                     c,
                 static_cast<T>(x_lerp * dtop));

    const float dbottom = y_lerp * input_grad[in_idx];
    GpuAtomicAdd(output_grad +
                     ((b * original_height + bottom_y_index) * original_width +
                      left_x_index) *
                         channels +
                     c,
                 static_cast<T>((1 - x_lerp) * dbottom));
    GpuAtomicAdd(output_grad +
                     ((b * original_height + bottom_y_index) * original_width +
                      right_x_index) *
                         channels +
                     c,
                 static_cast<T>(x_lerp * dbottom));
  }
}

}  // namespace

namespace functor {

// Partial specialization of ResizeBilinear functor for a GPUDevice.
template <typename T>
struct ResizeBilinear<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T, 4>::ConstTensor images,
                  const float height_scale, const float width_scale,
                  const bool half_pixel_centers,
                  typename TTypes<float, 4>::Tensor output) {
    const int batch = images.dimension(0);
    const int in_height = images.dimension(1);
    const int in_width = images.dimension(2);
    const int channels = images.dimension(3);

    const int out_height = output.dimension(1);
    const int out_width = output.dimension(2);

    const int total_count = batch * out_height * out_width * channels;
    if (total_count == 0) return;

    GpuLaunchConfig config = GetGpuLaunchConfig(total_count, d);
    if (half_pixel_centers) {
      TF_CHECK_OK(
          GpuLaunchKernel(ResizeBilinearKernel<T>, dim3(config.block_count),
                          dim3(config.thread_per_block), 0, d.stream(),
                          config.virtual_thread_count, images.data(),
                          height_scale, width_scale, batch, in_height, in_width,
                          channels, out_height, out_width, output.data()));
    } else {
      TF_CHECK_OK(GpuLaunchKernel(
          LegacyResizeBilinearKernel<T>, dim3(config.block_count),
          dim3(config.thread_per_block), 0, d.stream(),
          config.virtual_thread_count, images.data(), height_scale, width_scale,
          batch, in_height, in_width, channels, out_height, out_width,
          output.data()));

    }

  }
};



// Partial specialization of ResizeBilinearGrad functor for a GPUDevice.
template <typename T>
struct ResizeBilinearGrad<GPUDevice, T> {
  void operator()(const GPUDevice& d,
                  typename TTypes<float, 4>::ConstTensor input_grad,
                  const float height_scale, const float width_scale,
                  const bool half_pixel_centers,
                  typename TTypes<T, 4>::Tensor output_grad) {
    const int batch = output_grad.dimension(0);
    const int original_height = output_grad.dimension(1);
    const int original_width = output_grad.dimension(2);
    const int channels = output_grad.dimension(3);

    const int resized_height = input_grad.dimension(1);
    const int resized_width = input_grad.dimension(2);

    int total_count;
    GpuLaunchConfig config;

    // Initialize output_grad with all zeros.
    total_count = batch * original_height * original_width * channels;
    if (total_count == 0) return;
    config = GetGpuLaunchConfig(total_count, d);
    TF_CHECK_OK(GpuLaunchKernel(
        SetZero<T>, config.block_count, config.thread_per_block, 0, d.stream(),
        config.virtual_thread_count, output_grad.data()));

    // Accumulate.
    total_count = batch * resized_height * resized_width * channels;
    config = GetGpuLaunchConfig(total_count, d);
    if (half_pixel_centers) {
      TF_CHECK_OK(GpuLaunchKernel(
          ResizeBilinearGradKernel<T>, config.block_count,
          config.thread_per_block, 0, d.stream(), config.virtual_thread_count,
          input_grad.data(), height_scale, width_scale, batch, original_height,
          original_width, channels, resized_height, resized_width,
          output_grad.data()));
    } else {
      TF_CHECK_OK(GpuLaunchKernel(
          LegacyResizeBilinearGradKernel<T>, config.block_count,
          config.thread_per_block, 0, d.stream(), config.virtual_thread_count,
          input_grad.data(), height_scale, width_scale, batch, original_height,
          original_width, channels, resized_height, resized_width,
          output_grad.data()));
    }
  }
};

#define DEFINE_GPU_SPECS(T)                     \
  template struct ResizeBilinear<GPUDevice, T>; \
  template struct ResizeBilinearGrad<GPUDevice, T>;

TF_CALL_GPU_NUMBER_TYPES_NO_HALF(DEFINE_GPU_SPECS);

#undef DEFINE_GPU_SPECS

}  // namespace functor
}  // namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
