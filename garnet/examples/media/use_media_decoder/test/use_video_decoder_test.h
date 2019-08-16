// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_EXAMPLES_MEDIA_USE_MEDIA_DECODER_TEST_USE_VIDEO_DECODER_TEST_H_
#define GARNET_EXAMPLES_MEDIA_USE_MEDIA_DECODER_TEST_USE_VIDEO_DECODER_TEST_H_

#include <lib/sys/cpp/component_context.h>

#include "../in_stream_peeker.h"
#include "../use_video_decoder.h"

// For tests that just want to decode an input file with a known number of
// frames.
int use_video_decoder_test(std::string input_file_path, int expected_frame_count,
                           UseVideoDecoderFunction use_video_decoder, std::string golden_sha256);

// For tests that want to provide their own InStreamPeeker and EmitFrame.
bool decode_video_stream_test(async::Loop* fidl_loop, thrd_t fidl_thread,
                              sys::ComponentContext* component_context,
                              InStreamPeeker* in_stream_peeker,
                              UseVideoDecoderFunction use_video_decoder,
                              uint64_t min_output_buffer_size, EmitFrame emit_frame);

#endif  // GARNET_EXAMPLES_MEDIA_USE_MEDIA_DECODER_TEST_USE_VIDEO_DECODER_TEST_H_
