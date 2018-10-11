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

#include "tensorflow/contrib/lite/experimental/micro/examples/micro_speech/yes_features_data.h"

/* File automatically created by
 * tensorflow/examples/speech_commands/wav_to_features.py \
 * --sample_rate=16000 \
 * --clip_duration_ms=1000 \
 * --window_size_ms=30 \
 * --window_stride_ms=20 \
 * --feature_bin_count=40 \
 * --quantize \
 * --preprocess="average" \
 * --input_wav="speech_commands_test_set_v0.02/yes/f2e59fea_nohash_1.wav" \
 * --output_c_file="yes_features_data.cc" \
 */

const int g_yes_f2e59fea_nohash_1_width = 43;
const int g_yes_f2e59fea_nohash_1_height = 49;
const unsigned char g_yes_f2e59fea_nohash_1_data[] = {
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  1,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  1,   1,  1,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  4,   5,   1,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  1,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   2,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    1,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   1,  19, 1,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   1,   0,  1,  3,   3,   1,  1,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   8,   89, 8,   0,   0,  0,  0,   0,   0,  0,  0,   4,  13,
    1,  6,  23,  20,  6,   4,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  19, 177, 42, 1,
    1,  0,  0,   0,   0,   2,  3,   119, 51, 5,  139, 92,  58, 58, 15,  2,  1,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   13, 165, 176, 3,  1,  1,   0,   0,  1,  1,   32, 214,
    26, 19, 113, 103, 28,  22, 27,  3,   1,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  12,  55, 128,
    27, 1,  1,   0,   1,   4,  2,   52,  93, 10, 28,  156, 10, 21, 21,  3,  3,
    1,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  14,  99,  32, 65, 7,   1,   2,  2,  6,   13, 121,
    36, 15, 11,  112, 125, 14, 5,   13,  4,  4,  2,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   24, 25,
    32, 5,  1,   0,   0,   0,  1,   0,   7,  5,  1,   1,   3,  3,  0,   3,  3,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   13,  13, 5,  1,   0,   0,  0,  0,   0,  3,
    4,  1,  0,   1,   2,   3,  1,   1,   1,  4,  8,   1,   2,  1,  3,   1,  1,
    0,  1,  1,   3,   1,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  1,
    8,  2,  1,   0,   0,   0,  0,   0,   1,  1,  0,   0,   1,  1,  2,   0,  2,
    1,  0,  2,   0,   2,   2,  3,   1,   1,  0,  1,   1,   4,  5,  1,   0,  1,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  1,   1,   1,  0,  1,   2,   1,  0,  1,   3,  1,
    1,  3,  1,   1,   6,   2,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  2,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  1,   1,   0,  1,  2,   6,   2,  4,  2,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  3,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  1,
    0,  0,  1,   2,   1,   1,  2,   1,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  4,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   2,  1,  0,   0,   2,  3,  5,   2,  0,
    1,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   1,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   1,   2,  2,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  1,  0,   0,   0,  0,  1,   2,  3,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   1,  1,   1,   1,  0,  0,   0,   1,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,  0,
    0,  0,  0,   0,   0,   0,  0,   0,   0,  0,  0,   0,   0,  0,  0,   0,
};
