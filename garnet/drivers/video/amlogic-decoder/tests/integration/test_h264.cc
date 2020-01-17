// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/compiler.h>

#include "amlogic-video.h"
#include "gtest/gtest.h"
#include "h264_decoder.h"
#include "macros.h"
#include "pts_manager.h"
#include "test_frame_allocator.h"
#include "tests/test_support.h"
#include "vdec1.h"

std::vector<std::vector<uint8_t>> SplitNalUnits(const uint8_t* start_data, uint32_t size) {
  std::vector<std::vector<uint8_t>> out_vector;

  const uint8_t* this_nal_start = start_data;
  while (true) {
    if (size < 3)
      return out_vector;
    uint8_t start_code[3] = {0, 0, 1};
    // Add 2 to ensure the next start code found isn't the start of this nal
    // unit.
    uint8_t* next_nal_start =
        static_cast<uint8_t*>(memmem(this_nal_start + 2, size - 2, start_code, sizeof(start_code)));
    if (next_nal_start && next_nal_start[-1] == 0)
      next_nal_start--;
    uint32_t data_size = next_nal_start ? next_nal_start - this_nal_start : size;
    if (data_size > 0) {
      std::vector<uint8_t> new_data(data_size);
      memcpy(new_data.data(), this_nal_start, data_size);
      out_vector.push_back(std::move(new_data));
    }

    if (!next_nal_start) {
      return out_vector;
    }

    size -= data_size;
    this_nal_start = next_nal_start;
  }
}

uint8_t GetNalUnitType(const std::vector<uint8_t>& nal_unit) {
  // Also works with 4-byte startcodes.
  uint8_t start_code[3] = {0, 0, 1};
  uint8_t* next_start = static_cast<uint8_t*>(memmem(nal_unit.data(), nal_unit.size(), start_code,
                                                     sizeof(start_code))) +
                        sizeof(start_code);
  return *next_start & 0xf;
}

static void ValidateInputRegisters(AmlogicVideo* video) {
  // Check that input is the correct endianness.
  EXPECT_EQ(7u, VldMemVififoControl::Get().ReadFrom(video->mmio()->dosbus).endianness());
}

class TestH264 {
 public:
  static void Decode(bool use_parser) {
    auto video = std::make_unique<AmlogicVideo>();
    ASSERT_TRUE(video);
    TestFrameAllocator client(video.get());

    auto bear_h264 = TestSupport::LoadFirmwareFile("video_test_data/bear.h264");
    ASSERT_NE(nullptr, bear_h264);
    auto larger_h264 = TestSupport::LoadFirmwareFile("video_test_data/test-25fps.h264");
    ASSERT_NE(nullptr, larger_h264);
    zx_status_t status = video->InitRegisters(TestSupport::parent_device());
    EXPECT_EQ(ZX_OK, status);
    EXPECT_EQ(ZX_OK, video->InitDecoder());

    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      video->SetDefaultInstance(
          std::make_unique<H264Decoder>(video.get(), &client, /*is_secure=*/false),
          /*hevc=*/false);
      client.set_decoder(video->video_decoder_);
    }
    status = video->InitializeStreamBuffer(use_parser, use_parser ? PAGE_SIZE : PAGE_SIZE * 1024,
                                           /*is_secure=*/false);
    ValidateInputRegisters(video.get());
    EXPECT_EQ(ZX_OK, status);
    std::promise<void> first_wait_valid;
    std::promise<void> second_wait_valid;
    uint32_t frame_count = 0;
    constexpr uint32_t kFirstVideoFrameCount = 26;
    constexpr uint32_t kSecondVideoFrameCount = 244;

    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      EXPECT_EQ(ZX_OK, video->video_decoder_->Initialize());
      client.SetFrameReadyNotifier([&video, &frame_count, &first_wait_valid,
                                    &second_wait_valid](std::shared_ptr<VideoFrame> frame) {
        ++frame_count;
        DLOG("Got frame %d coded_width: %d coded_height: %d\n", frame_count, frame->coded_width,
             frame->coded_height);
#if DUMP_VIDEO_TO_FILE
        DumpVideoFrameToFile(frame, "/tmp/bearh264.yuv");
#endif
        if (frame_count == kFirstVideoFrameCount)
          first_wait_valid.set_value();
        if (frame_count == kFirstVideoFrameCount + kSecondVideoFrameCount)
          second_wait_valid.set_value();
        ReturnFrame(video.get(), frame);
      });
    }

    if (use_parser) {
      EXPECT_EQ(ZX_OK, video->InitializeEsParser());
      EXPECT_EQ(ZX_OK, video->parser()->ParseVideo(bear_h264->ptr, bear_h264->size));
      EXPECT_EQ(ZX_OK, video->parser()->WaitForParsingCompleted(ZX_SEC(10)));
    } else {
      video->core_->InitializeDirectInput();
      EXPECT_EQ(ZX_OK, video->ProcessVideoNoParser(bear_h264->ptr, bear_h264->size));
    }

    EXPECT_EQ(std::future_status::ready,
              first_wait_valid.get_future().wait_for(std::chrono::seconds(1)));

    DLOG("After first wait: frame_count: %u\n", frame_count);

    if (use_parser) {
      EXPECT_EQ(ZX_OK, video->parser()->ParseVideo(larger_h264->ptr, larger_h264->size));
      EXPECT_EQ(ZX_OK, video->parser()->WaitForParsingCompleted(ZX_SEC(10)));
    } else {
      EXPECT_EQ(ZX_OK, video->ProcessVideoNoParser(larger_h264->ptr, larger_h264->size));
    }

    EXPECT_EQ(std::future_status::ready,
              second_wait_valid.get_future().wait_for(std::chrono::seconds(1)));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(kFirstVideoFrameCount + kSecondVideoFrameCount, frame_count);

    video.reset();
  }

  static void DelayedReturn() {
    auto video = std::make_unique<AmlogicVideo>();
    ASSERT_TRUE(video);

    TestFrameAllocator client(video.get());
    zx_status_t status = video->InitRegisters(TestSupport::parent_device());
    EXPECT_EQ(ZX_OK, status);
    EXPECT_EQ(ZX_OK, video->InitDecoder());

    auto bear_h264 = TestSupport::LoadFirmwareFile("video_test_data/bear.h264");
    ASSERT_NE(nullptr, bear_h264);
    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      video->SetDefaultInstance(
          std::make_unique<H264Decoder>(video.get(), &client, /*is_secure=*/false),
          /*hevc=*/false);
      client.set_decoder(video->video_decoder_);
    }
    status = video->InitializeStreamBuffer(/*use_parser=*/false, PAGE_SIZE, /*is_secure=*/false);
    EXPECT_EQ(ZX_OK, status);
    std::promise<void> wait_valid;
    // Guarded by decoder lock.
    std::vector<std::shared_ptr<VideoFrame>> frames_to_return;
    uint32_t frame_count = 0;
    bool return_frames_immediately = false;
    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      EXPECT_EQ(ZX_OK, video->video_decoder_->Initialize());

      client.SetFrameReadyNotifier([&frames_to_return, &frame_count, &wait_valid,
                                    &return_frames_immediately,
                                    &video](std::shared_ptr<VideoFrame> frame) {
        ++frame_count;
        EXPECT_EQ(320u, frame->display_width);
        EXPECT_EQ(180u, frame->display_height);
        DLOG("Got frame %d coded_width: %d coded_height: %d\n", frame_count, frame->coded_width,
             frame->coded_height);
        constexpr uint32_t kFirstVideoFrameCount = 26;
        if (frame_count == kFirstVideoFrameCount)
          wait_valid.set_value();
        if (return_frames_immediately) {
          DLOG("Before ReturnFrame()\n");
          // video->video_decoder_lock_ already held here
          ReturnFrame(video.get(), frame);
        } else {
          DLOG("Before push_back()\n");
          frames_to_return.push_back(frame);
        }
        DLOG("Done with frame.\n");
      });
    }

    std::atomic<bool> stop_parsing(false);
    video->core_->InitializeDirectInput();
    auto as = std::async([&video, &bear_h264, &stop_parsing]() {
      uint32_t current_offset = 0;
      uint8_t* data = bear_h264->ptr;
      while (!stop_parsing) {
        uint32_t processed_data;
        EXPECT_EQ(ZX_OK,
                  video->ProcessVideoNoParser(data + current_offset,
                                              bear_h264->size - current_offset, &processed_data));
        current_offset += processed_data;
        if (current_offset == bear_h264->size)
          break;
        zx_nanosleep(zx_deadline_after(ZX_MSEC(15)));
      }
    });

    zx_nanosleep(zx_deadline_after(ZX_SEC(1)));

    {
      DLOG("Returning frames\n");
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      return_frames_immediately = true;
      for (auto frame : frames_to_return) {
        video->video_decoder_->ReturnFrame(frame);
      }
      frames_to_return.clear();
    }
    DLOG("Done returning frames.\n");
    EXPECT_EQ(std::future_status::ready, wait_valid.get_future().wait_for(std::chrono::seconds(1)));

    stop_parsing = true;
    as.wait();
    video.reset();
  }

  static void DecodeNalUnits(bool use_parser) {
    auto video = std::make_unique<AmlogicVideo>();
    ASSERT_TRUE(video);

    TestFrameAllocator client(video.get());
    zx_status_t status = video->InitRegisters(TestSupport::parent_device());
    EXPECT_EQ(ZX_OK, status);
    EXPECT_EQ(ZX_OK, video->InitDecoder());
    auto bear_h264 = TestSupport::LoadFirmwareFile("video_test_data/bear.h264");
    ASSERT_NE(nullptr, bear_h264);

    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      video->SetDefaultInstance(
          std::make_unique<H264Decoder>(video.get(), &client, /*is_secure=*/false),
          /*hevc=*/false);
      client.set_decoder(video->video_decoder_);
    }
    status = video->InitializeStreamBuffer(use_parser, use_parser ? PAGE_SIZE : PAGE_SIZE * 1024,
                                           /*is_secure=*/false);
    EXPECT_EQ(ZX_OK, status);
    std::promise<void> first_wait_valid;
    std::set<uint64_t> received_pts_set;
    uint32_t frame_count = 0;
    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      EXPECT_EQ(ZX_OK, video->video_decoder_->Initialize());

      client.SetFrameReadyNotifier([&video, &frame_count, &first_wait_valid,
                                    &received_pts_set](std::shared_ptr<VideoFrame> frame) {
        ++frame_count;
        DLOG("Got frame %d coded_width: %d coded_height: %d\n", frame_count, frame->coded_width,
             frame->coded_height);
#if DUMP_VIDEO_TO_FILE
        DumpVideoFrameToFile(frame, "/tmp/bearh264.yuv");
#endif
        constexpr uint32_t kFirstVideoFrameCount = 26;
        if (frame_count == kFirstVideoFrameCount)
          first_wait_valid.set_value();
        ReturnFrame(video.get(), frame);
        EXPECT_TRUE(frame->has_pts);
        // In the test video the decode order isn't exactly the same as the
        // presentation order, so allow the current PTS to be 2 frames
        // older then the last received.
        if (received_pts_set.size() > 0)
          EXPECT_LE(*std::prev(received_pts_set.end()), frame->pts + 2);
        EXPECT_EQ(0u, received_pts_set.count(frame->pts));
        received_pts_set.insert(frame->pts);
      });
    }

    auto split_nal = SplitNalUnits(bear_h264->ptr, bear_h264->size);
    uint32_t parsed_video_size = 0;
    uint64_t pts_count = 0;
    if (use_parser) {
      EXPECT_EQ(ZX_OK, video->InitializeEsParser());
    } else {
      video->core_->InitializeDirectInput();
    }
    uint32_t total_size = 0;
    for (auto& nal : split_nal) {
      total_size += nal.size();
    }
    EXPECT_EQ(bear_h264->size, total_size);
    for (auto& nal : split_nal) {
      uint8_t nal_type = GetNalUnitType(nal);
      if (nal_type == 1 || nal_type == 5) {
        video->pts_manager()->InsertPts(parsed_video_size, true, pts_count++);
      }
      if (use_parser) {
        EXPECT_EQ(ZX_OK, video->parser()->ParseVideo(nal.data(), nal.size()));
        EXPECT_EQ(ZX_OK, video->parser()->WaitForParsingCompleted(ZX_SEC(10)));
      } else {
        EXPECT_EQ(ZX_OK, video->ProcessVideoNoParser(nal.data(), nal.size()));
      }
      parsed_video_size += nal.size();
    }

    EXPECT_EQ(std::future_status::ready,
              first_wait_valid.get_future().wait_for(std::chrono::seconds(1)));

    for (uint32_t i = 0; i < 27; i++) {
      // Frame 25 isn't flushed out of the decoder.
      if (i != 25)
        EXPECT_TRUE(received_pts_set.count(i));
    }

    video.reset();
  }

  static void DecodeMalformed(uint64_t location, uint8_t value) {
    auto video = std::make_unique<AmlogicVideo>();
    ASSERT_TRUE(video);
    TestFrameAllocator client(video.get());

    auto bear_h264 = TestSupport::LoadFirmwareFile("video_test_data/bear.h264");
    ASSERT_NE(nullptr, bear_h264);
    zx_status_t status = video->InitRegisters(TestSupport::parent_device());
    EXPECT_EQ(ZX_OK, status);
    EXPECT_EQ(ZX_OK, video->InitDecoder());

    std::promise<void> first_wait_valid;
    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      video->SetDefaultInstance(
          std::make_unique<H264Decoder>(video.get(), &client, /*is_secure=*/false),
          /*hevc=*/false);
      client.set_decoder(video->video_decoder_);
      client.SetErrorHandler([&first_wait_valid]() {
        DECODE_ERROR("Got error");
        first_wait_valid.set_value();
      });
    }
    status = video->InitializeStreamBuffer(/*use_parser=*/true, PAGE_SIZE,
                                           /*is_secure=*/false);
    EXPECT_EQ(ZX_OK, status);
    uint32_t frame_count = 0;

    {
      std::lock_guard<std::mutex> lock(video->video_decoder_lock_);
      EXPECT_EQ(ZX_OK, video->video_decoder_->Initialize());
      client.SetFrameReadyNotifier([&video, &frame_count](std::shared_ptr<VideoFrame> frame) {
        ++frame_count;
        DLOG("Got frame %d coded_width: %d coded_height: %d\n", frame_count, frame->coded_width,
             frame->coded_height);
        ReturnFrame(video.get(), frame);
      });
    }

    std::vector<uint8_t> video_data(bear_h264->ptr, bear_h264->ptr + bear_h264->size);
    video_data[location] = value;

    EXPECT_EQ(ZX_OK, video->InitializeEsParser());
    EXPECT_EQ(ZX_OK, video->parser()->ParseVideo(video_data.data(), video_data.size()));

    EXPECT_EQ(std::future_status::ready,
              first_wait_valid.get_future().wait_for(std::chrono::seconds(1)));
    // The decoder should now be hung without having gotten through all the input so we should
    // cancel parsing before teardown.
    video->parser()->CancelParsing();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // No frames should be returned because the error happened too early.
    EXPECT_EQ(0u, frame_count);

    video.reset();
  }

 private:
  // This is called from the interrupt handler, which already holds the lock.
  static void ReturnFrame(AmlogicVideo* video, std::shared_ptr<VideoFrame> frame) {
    video->AssertVideoDecoderLockHeld();
    video->video_decoder_->ReturnFrame(frame);
  }
};

TEST(H264, Decode) { TestH264::Decode(true); }

TEST(H264, DecodeNoParser) { TestH264::Decode(false); }

TEST(H264, DelayedReturn) { TestH264::DelayedReturn(); }

TEST(H264, DecodeNalUnits) { TestH264::DecodeNalUnits(true); }

TEST(H264, DecodeNalUnitsNoParser) { TestH264::DecodeNalUnits(false); }

TEST(H264, DecodeMalformedHang) {
  // Parameters found through fuzzing.
  TestH264::DecodeMalformed(638, 44);
}

TEST(H264, DecodeMalformedTooLarge) {
  // Parameters found through fuzzing - causes mb_width=3 and total_mbs=4986, so the height is
  // calculated as 26592 pixels.
  TestH264::DecodeMalformed(593, 176);
}
