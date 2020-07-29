/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_MICRO_SIMPLE_MEMORY_ALLOCATOR_H_
#define TENSORFLOW_LITE_MICRO_SIMPLE_MEMORY_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

#include "tensorflow/lite/core/api/error_reporter.h"
#include "tensorflow/lite/micro/compatibility.h"

namespace tflite {

// TODO(petewarden): This allocator never frees up or reuses  any memory, even
// though we have enough information about lifetimes of the tensors to do so.
// This makes it pretty wasteful, so we should use a more intelligent method.
class SimpleMemoryAllocator {
 public:
  // TODO(b/157615197): Cleanup constructors/destructor and use factory
  // functions.
  SimpleMemoryAllocator(ErrorReporter* error_reporter, uint8_t* buffer_head,
                        uint8_t* buffer_tail);
  SimpleMemoryAllocator(ErrorReporter* error_reporter, uint8_t* buffer,
                        size_t buffer_size);
  virtual ~SimpleMemoryAllocator();

  // Creates a new SimpleMemoryAllocator from a given buffer head and size.
  static SimpleMemoryAllocator* Create(ErrorReporter* error_reporter,
                                       uint8_t* buffer_head,
                                       size_t buffer_size);

  // Adjust memory allocations starting at the head of the arena (lowest address
  // and moving upwards). It only tracks the maximum head usage and make sure
  // that memory used at head will not overlap with memory reserved at tail.
  // Calls to this method will also invalidate all temporary allocation values.
  // This call will fail if a chain allocation calls through AllocateTemp() have
  // not been cleaned up with a call to ResetTempAllocations().
  virtual uint8_t* AdjustHead(size_t size, size_t alignment);
  // Allocates memory starting at the tail of the arena (highest address and
  // moving downwards).
  virtual uint8_t* AllocateFromTail(size_t size, size_t alignment);

  // Allocates a temporary buffer from the head of the arena (lowest address and
  // moving upwards) but does not update the actual head allocation size or
  // position. The returned buffer is guaranteed until either
  // ResetTempAllocations() is called or another call to AllocateFromHead().
  // Repeat calls to this function will create a chain of temp allocations. All
  // calls to AllocateTemp() must end with a call to ResetTempAllocations(). If
  // AllocateFromHead() is called before a call to ResetTempAllocations(), it
  // will fail with an error message.
  virtual uint8_t* AllocateTemp(size_t size, size_t alignment);

  // Resets a chain of temporary allocations back to the current head of the
  // arena (lowest address).
  virtual void ResetTempAllocations();

  uint8_t* GetHead() const;
  uint8_t* GetBufferHead() const;
  uint8_t* GetTail() const;

  size_t GetHeadUsedBytes() const;
  size_t GetTailUsedBytes() const;

  size_t GetAvailableMemory() const;
  size_t GetUsedBytes() const;

 private:
  size_t GetBufferSize() const;

  ErrorReporter* error_reporter_;
  uint8_t* buffer_head_;
  uint8_t* buffer_tail_;
  uint8_t* head_watermark_;
  uint8_t* tail_;
  uint8_t* temp_;

  TF_LITE_REMOVE_VIRTUAL_DELETE
};

}  // namespace tflite

#endif  // TENSORFLOW_LITE_MICRO_SIMPLE_MEMORY_ALLOCATOR_H_
