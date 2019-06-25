/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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
#ifndef TENSORFLOW_LITE_KERNELS_INTERNAL_OPTIMIZED_CPU_CHECK_H_
#define TENSORFLOW_LITE_KERNELS_INTERNAL_OPTIMIZED_CPU_CHECK_H_

#include "tensorflow/lite/kernels/cpu_backend_context.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#define USE_NEON
#include <arm_neon.h>
#endif  // __ARM_NEON

#if defined __GNUC__ && defined __SSE4_1__ && !defined TF_LITE_DISABLE_X86_NEON
#define USE_NEON
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wattributes"
#pragma GCC diagnostic ignored "-Wnarrowing"
#pragma GCC diagnostic ignored "-Wsequence-point"
#include "NEON_2_SSE.h"
#pragma GCC diagnostic pop
#endif  // __SSE4_1__

namespace tflite {

struct CpuFlags {
  bool neon_dotprod = false;
};

inline void GetCpuFlags(CpuBackendContext* cpu_backend_context,
                        CpuFlags* cpu_flags) {
  ruy::Context* ruy_context = cpu_backend_context->ruy_context();
  cpu_flags->neon_dotprod =
      ruy_context != nullptr && (ruy_context->GetRuntimeEnabledPaths() &
                                 ruy::Path::kNeonDotprod) != ruy::Path::kNone;
}

}  // namespace tflite

// NEON_OR_PORTABLE(SomeFunc, args) calls NeonSomeFunc(args) if USE_NEON is
// defined, PortableSomeFunc(args) otherwise.
#ifdef USE_NEON
// Always use Neon code
#define NEON_OR_PORTABLE(funcname, ...) Neon##funcname(__VA_ARGS__)

#else
// No NEON available: Use Portable code
#define NEON_OR_PORTABLE(funcname, ...) Portable##funcname(__VA_ARGS__)

#endif  // defined(USE_NEON)

#endif  // TENSORFLOW_LITE_KERNELS_INTERNAL_OPTIMIZED_CPU_CHECK_H_
