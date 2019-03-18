// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_DEMUX_FILE_READER_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_DEMUX_FILE_READER_H_

#include <lib/async/dispatcher.h>
#include <lib/zx/channel.h>

#include "src/lib/files/unique_fd.h"
#include "src/media/playback/mediaplayer/demux/reader.h"

namespace media_player {

// Reads from a file on behalf of a demux.
class FileReader : public Reader {
 public:
  static std::shared_ptr<FileReader> Create(zx::channel file_channel);

  FileReader(fxl::UniqueFD fd);

  ~FileReader() override;

  // Reader implementation.
  void Describe(DescribeCallback callback) override;

  void ReadAt(size_t position, uint8_t* buffer, size_t bytes_to_read,
              ReadAtCallback callback) override;

 private:
  async_dispatcher_t* dispatcher_;
  fxl::UniqueFD fd_;
  Result result_ = Result::kOk;
  uint64_t size_ = kUnknownSize;
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_DEMUX_FILE_READER_H_
