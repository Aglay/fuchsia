// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/media/codec_impl/fourcc.h>
#include <lib/sys/cpp/component_context.h>
#include <stdio.h>
#include <stdlib.h>

#include <map>
#include <set>

#include "../use_video_decoder.h"
#include "../util.h"
#include "src/lib/fxl/logging.h"
#include "use_video_decoder_test.h"

namespace {

constexpr char kInputFilePath[] = "/pkg/data/bear.h264";
constexpr int kInputFileFrameCount = 990;

const char* kGoldenSha256 = "0ff588a0cc86954a3c58a15445b57081e4c9adfd9f87b5b80d93f2c11c40889c";

}  // namespace

int main(int argc, char* argv[]) {
  const UseVideoDecoderTestParams test_params{
      .keep_stream_modulo = 4,
      .loop_stream_count = 65,
  };
  return use_video_decoder_test(kInputFilePath, kInputFileFrameCount, use_h264_multi_decoder,
                                /*is_secure_output=*/false, /*is_secure_input=*/false,
                                /*min_output_buffer_count=*/0, kGoldenSha256, &test_params);
}
