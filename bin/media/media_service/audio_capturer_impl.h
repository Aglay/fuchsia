// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "lib/media/fidl/media_capturer.fidl.h"
#include "garnet/bin/media/fidl/fidl_packet_producer.h"
#include "garnet/bin/media/media_service/media_service_impl.h"
#include "garnet/bin/media/framework/graph.h"

namespace media {

class AudioInput;

// Fidl agent that captures audio.
class AudioCapturerImpl : public MediaServiceImpl::Product<MediaCapturer>,
                          public MediaCapturer {
 public:
  static std::shared_ptr<AudioCapturerImpl> Create(
      fidl::InterfaceRequest<MediaCapturer> request,
      MediaServiceImpl* owner);

  ~AudioCapturerImpl() override;

  // MediaCapturer implementation.
  void GetSupportedMediaTypes(
      const GetSupportedMediaTypesCallback& callback) override;

  void SetMediaType(MediaTypePtr media_type) override;

  void GetPacketProducer(fidl::InterfaceRequest<MediaPacketProducer>
                             packet_producer_request) override;

  void Start() override;

  void Stop() override;

 private:
  AudioCapturerImpl(fidl::InterfaceRequest<MediaCapturer> request,
                    MediaServiceImpl* owner);

  Graph graph_;
  std::shared_ptr<AudioInput> source_;
  std::shared_ptr<FidlPacketProducer> producer_;

  FTL_DISALLOW_COPY_AND_ASSIGN(AudioCapturerImpl);
};

}  // namespace media
