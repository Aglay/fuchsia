// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <string>

#include "lib/fidl/cpp/optional.h"
#include "lib/media/timeline/timeline.h"
#include "lib/media/timeline/timeline_rate.h"
#include "peridot/bin/suggestion_engine/media_player.h"

namespace modular {

MediaPlayer::MediaPlayer(media::AudioServerPtr audio_server,
                         std::shared_ptr<SuggestionDebugImpl> debug)
    : audio_server_(std::move(audio_server)), debug_(debug) {
  audio_server_.set_error_handler([this] {
    // TODO(miguelfrde): better error handling. If we observe this message it
    // means that the underlying channel was closed.
    FXL_LOG(WARNING) << "Audio server connection error";
    audio_server_ = nullptr;
    media_packet_producer_ = nullptr;
  });
}

MediaPlayer::~MediaPlayer() = default;

void MediaPlayer::SetSpeechStatusCallback(SpeechStatusCallback callback) {
  speech_status_callback_ = std::move(callback);
}

void MediaPlayer::PlayMediaResponse(MediaResponsePtr media_response) {
  if (!audio_server_) {
    FXL_LOG(ERROR) << "Not playing query media response because our connection "
                   << "to the AudioServer died earlier.";
    return;
  }

  auto activity = debug_->GetIdleWaiter()->RegisterOngoingActivity();

  media::AudioRendererPtr audio_renderer;
  audio_server_->CreateRenderer(audio_renderer.NewRequest(),
                                media_renderer_.NewRequest());

  media_packet_producer_ = media_response->media_packet_producer.Bind();
  media_renderer_->SetMediaType(std::move(media_response->media_type));
  media::MediaPacketConsumerPtr consumer;
  media_renderer_->GetPacketConsumer(consumer.NewRequest());

  media_packet_producer_->Connect(
      std::move(consumer), [this, activity] {
    OnMediaPacketProducerConnected(activity);
  });

  media_packet_producer_.set_error_handler([this] {
    speech_status_callback_(SpeechStatus::IDLE);
  });
}

void MediaPlayer::OnMediaPacketProducerConnected(
    util::IdleWaiter::ActivityToken activity) {
  time_lord_.Unbind();
  media_timeline_consumer_.Unbind();

  speech_status_callback_(SpeechStatus::RESPONDING);

  media_renderer_->GetTimelineControlPoint(time_lord_.NewRequest());
  time_lord_->GetTimelineConsumer(media_timeline_consumer_.NewRequest());
  time_lord_->Prime([this, activity] {
    media::TimelineTransform tt;
    tt.reference_time =
        media::Timeline::local_now() + media::Timeline::ns_from_ms(30);
    tt.subject_time = media::kUnspecifiedTime;
    tt.reference_delta = tt.subject_delta = 1;

    HandleMediaUpdates(media::kInitialStatus, nullptr);

    media_timeline_consumer_->SetTimelineTransform(
        std::move(tt), [activity](bool completed) {});
  });
}


void MediaPlayer::HandleMediaUpdates(
    uint64_t version,
    media::MediaTimelineControlPointStatusPtr status) {
  auto activity = debug_->GetIdleWaiter()->RegisterOngoingActivity();

  if (status && status->end_of_stream) {
    media_renderer_ = nullptr;
  } else {
    time_lord_->GetStatus(
        version,
        [this, activity](uint64_t next_version,
                         media::MediaTimelineControlPointStatus next_status) {
          HandleMediaUpdates(next_version,
                             fidl::MakeOptional(std::move(next_status)));
        });
  }
}

}  // namespace modular
