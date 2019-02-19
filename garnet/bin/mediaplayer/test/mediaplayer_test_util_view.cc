// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/mediaplayer/test/mediaplayer_test_util_view.h"

#include <fcntl.h>
#include <hid/usages.h>
#include <zx/eventpair.h>
#include "garnet/bin/mediaplayer/graph/formatting.h"
#include "garnet/bin/mediaplayer/test/mediaplayer_test_util_params.h"
#include "lib/fidl/cpp/clone.h"
#include "lib/fidl/cpp/optional.h"
#include "lib/fsl/io/fd.h"
#include "lib/fxl/logging.h"
#include "lib/media/timeline/timeline.h"
#include "lib/media/timeline/type_converters.h"
#include "lib/url/gurl.h"

namespace media_player {
namespace test {

namespace {
constexpr uint32_t kVideoChildKey = 0u;

constexpr int32_t kDefaultWidth = 640;
constexpr int32_t kDefaultHeight = 100;

constexpr float kBackgroundElevation = 0.f;
constexpr float kVideoElevation = 1.0f;
constexpr float kProgressBarElevation = 1.0f;
constexpr float kProgressBarSliderElevation = 2.0f;

constexpr float kControlsGap = 12.0f;
constexpr float kControlsHeight = 36.0f;

// Determines whether the rectangle contains the point x,y.
bool Contains(const fuchsia::math::RectF& rect, float x, float y) {
  return rect.x <= x && rect.y <= y && rect.x + rect.width >= x &&
         rect.y + rect.height >= y;
}

int64_t rand_less_than(int64_t limit) {
  return (static_cast<int64_t>(std::rand()) * RAND_MAX + std::rand()) % limit;
}

}  // namespace

MediaPlayerTestUtilView::MediaPlayerTestUtilView(
    scenic::ViewContext view_context, fit::function<void(int)> quit_callback,
    const MediaPlayerTestUtilParams& params)
    : scenic::V1BaseView(std::move(view_context), "Media Player"),
      quit_callback_(std::move(quit_callback)),
      params_(params),
      background_node_(session()),
      progress_bar_node_(session()),
      progress_bar_slider_node_(session()) {
  FXL_DCHECK(quit_callback_);
  FXL_DCHECK(params_.is_valid());
  FXL_DCHECK(!params_.urls().empty());

  scenic::Material background_material(session());
  background_material.SetColor(0x00, 0x00, 0x00, 0xff);
  background_node_.SetMaterial(background_material);
  parent_node().AddChild(background_node_);

  scenic::Material progress_bar_material(session());
  progress_bar_material.SetColor(0x23, 0x23, 0x23, 0xff);
  progress_bar_node_.SetMaterial(progress_bar_material);
  parent_node().AddChild(progress_bar_node_);

  scenic::Material progress_bar_slider_material(session());
  progress_bar_slider_material.SetColor(0x00, 0x00, 0xff, 0xff);
  progress_bar_slider_node_.SetMaterial(progress_bar_slider_material);
  parent_node().AddChild(progress_bar_slider_node_);

  // We start with a non-zero size so we get a progress bar regardless of
  // whether we get video.
  video_size_.width = 0;
  video_size_.height = 0;
  pixel_aspect_ratio_.width = 1;
  pixel_aspect_ratio_.height = 1;

  // Create a player from all that stuff.
  player_ = startup_context()
                ->ConnectToEnvironmentService<fuchsia::mediaplayer::Player>();

  // Create the video view.
  zx::eventpair view_owner_token, view_token;
  if (zx::eventpair::create(0u, &view_owner_token, &view_token) != ZX_OK)
    FXL_NOTREACHED() << "failed to create tokens.";
  player_->CreateView2(std::move(view_token));

  zx::eventpair video_host_import_token;
  video_host_node_.reset(new scenic::EntityNode(session()));
  video_host_node_->ExportAsRequest(&video_host_import_token);
  parent_node().AddChild(*video_host_node_);
  GetViewContainer()->AddChild2(kVideoChildKey, std::move(view_owner_token),
                                std::move(video_host_import_token));

  commands_.Init(player_.get());

  player_.events().OnStatusChanged =
      [this](fuchsia::mediaplayer::PlayerStatus status) {
        HandleStatusChanged(status);
      };

  // Seed the random number generator.
  std::srand(std::time(nullptr));

  if (params_.experiment()) {
    RunExperiment();
  } else if (params_.test_seek()) {
    TestSeek();
  } else {
    // Get the player primed now.
    commands_.SetUrl(params_.urls().front());
    commands_.Pause();
    commands_.WaitForViewReady();

    if (params_.auto_play()) {
      commands_.Play();
    }

    ScheduleNextUrl();
  }

  commands_.Execute();
}

void MediaPlayerTestUtilView::RunExperiment() {
  // Add experimental code here.
  // In general, no implementation for this method should be submitted.
}

void MediaPlayerTestUtilView::TestSeek() {
  commands_.SetUrl(params_.urls().front());
  commands_.WaitForViewReady();

  // Need to load content before deciding where to seek.
  commands_.WaitForContentLoaded();

  commands_.Invoke([this]() { ContinueTestSeek(); });
}

void MediaPlayerTestUtilView::ContinueTestSeek() {
  if (duration_ns_ == 0) {
    // We have no duration yet. Just start over at zero.
    commands_.Seek(0);
    commands_.Play();
    commands_.WaitForEndOfStream();
    commands_.Invoke([this]() { ContinueTestSeek(); });
    FXL_LOG(INFO) << "Seek interval: beginning to end";
    return;
  }

  // For the start position, generate a number in the range [0..duration_ns_]
  // with a 10% chance of being zero.
  int64_t seek_interval_start =
      rand_less_than(duration_ns_ + duration_ns_ / 10);
  if (seek_interval_start >= duration_ns_) {
    seek_interval_start = 0;
  }

  // For the end position, choose a position between start and 10% past the
  // duration. If this value is greater than the duration, the interval
  // effectively ends at the end of the file.
  int64_t seek_interval_end =
      seek_interval_start +
      rand_less_than((duration_ns_ + duration_ns_ / 10) - seek_interval_start);

  commands_.Seek(seek_interval_start);
  commands_.Play();
  if (seek_interval_end >= duration_ns_) {
    FXL_LOG(INFO) << "Seek interval: " << AsNs(seek_interval_start)
                  << " to end";
    commands_.WaitForEndOfStream();
  } else {
    FXL_LOG(INFO) << "Seek interval: " << AsNs(seek_interval_start) << " to "
                  << AsNs(seek_interval_end);
    commands_.WaitForSeekCompletion();
    commands_.WaitForPosition(seek_interval_end);
  }

  commands_.Invoke([this]() { ContinueTestSeek(); });
}

void MediaPlayerTestUtilView::ScheduleNextUrl() {
  if (++next_url_index_ == params_.urls().size()) {
    if (!params_.loop()) {
      // No more files, not looping.
      return;
    }

    next_url_index_ = 0;
  }

  commands_.WaitForEndOfStream();

  if (params_.urls().size() > 1) {
    commands_.SetUrl(params_.urls()[next_url_index_]);
  } else {
    // Just one file...seek to the beginning.
    commands_.Seek(0);
  }

  commands_.Play();

  commands_.Invoke([this]() { ScheduleNextUrl(); });
}

MediaPlayerTestUtilView::~MediaPlayerTestUtilView() {}

bool MediaPlayerTestUtilView::OnInputEvent(
    fuchsia::ui::input::InputEvent event) {
  bool handled = false;
  if (event.is_pointer()) {
    const auto& pointer = event.pointer();
    if (pointer.phase == fuchsia::ui::input::PointerEventPhase::DOWN) {
      if (duration_ns_ != 0 && Contains(controls_rect_, pointer.x, pointer.y)) {
        // User poked the progress bar...seek.
        player_->Seek((pointer.x - controls_rect_.x) * duration_ns_ /
                      controls_rect_.width);
        if (state_ != State::kPlaying) {
          player_->Play();
        }
      } else {
        // User poked elsewhere.
        TogglePlayPause();
      }

      handled = true;
    }
  } else if (event.is_keyboard()) {
    auto& keyboard = event.keyboard();
    if (keyboard.phase == fuchsia::ui::input::KeyboardEventPhase::PRESSED) {
      switch (keyboard.hid_usage) {
        case HID_USAGE_KEY_SPACE:
          TogglePlayPause();
          handled = true;
          break;
        case HID_USAGE_KEY_Q:
          quit_callback_(0);
          handled = true;
          break;
        default:
          break;
      }
    }
  }

  return handled;
}

void MediaPlayerTestUtilView::OnPropertiesChanged(
    ::fuchsia::ui::viewsv1::ViewProperties old_properties) {
  Layout();
}

void MediaPlayerTestUtilView::Layout() {
  if (!has_logical_size())
    return;

  if (!scenic_ready_) {
    scenic_ready_ = true;
    commands_.NotifyViewReady();
  }

  // Make the background fill the space.
  scenic::Rectangle background_shape(session(), logical_size().width,
                                     logical_size().height);
  background_node_.SetShape(background_shape);
  background_node_.SetTranslationRH(logical_size().width * .5f,
                                    logical_size().height * .5f,
                                    -kBackgroundElevation);

  // Compute maximum size of video content after reserving space
  // for decorations.
  fuchsia::math::SizeF max_content_size;
  max_content_size.width = logical_size().width;
  max_content_size.height =
      logical_size().height - kControlsHeight - kControlsGap;

  // Shrink video to fit if needed.
  uint32_t video_width =
      (video_size_.width == 0 ? kDefaultWidth : video_size_.width) *
      pixel_aspect_ratio_.width;
  uint32_t video_height =
      (video_size_.height == 0 ? kDefaultHeight : video_size_.height) *
      pixel_aspect_ratio_.height;

  if (max_content_size.width * video_height <
      max_content_size.height * video_width) {
    content_rect_.width = max_content_size.width;
    content_rect_.height = video_height * max_content_size.width / video_width;
  } else {
    content_rect_.width = video_width * max_content_size.height / video_height;
    content_rect_.height = max_content_size.height;
  }

  // Position the video.
  content_rect_.x = (logical_size().width - content_rect_.width) / 2.0f;
  content_rect_.y = (logical_size().height - content_rect_.height -
                     kControlsHeight - kControlsGap) /
                    2.0f;

  // Position the controls.
  controls_rect_.x = content_rect_.x;
  controls_rect_.y = content_rect_.y + content_rect_.height + kControlsGap;
  controls_rect_.width = content_rect_.width;
  controls_rect_.height = kControlsHeight;

  // Put the progress bar under the content.
  scenic::Rectangle progress_bar_shape(session(), controls_rect_.width,
                                       controls_rect_.height);
  progress_bar_node_.SetShape(progress_bar_shape);
  progress_bar_node_.SetTranslationRH(
      controls_rect_.x + controls_rect_.width * 0.5f,
      controls_rect_.y + controls_rect_.height * 0.5f, -kProgressBarElevation);

  // Put the progress bar slider on top of the progress bar.
  scenic::Rectangle progress_bar_slider_shape(session(), controls_rect_.width,
                                              controls_rect_.height);
  progress_bar_slider_node_.SetShape(progress_bar_slider_shape);
  progress_bar_slider_node_.SetTranslationRH(
      controls_rect_.x + controls_rect_.width * 0.5f,
      controls_rect_.y + controls_rect_.height * 0.5f,
      -kProgressBarSliderElevation);

  // Ask the view to fill the space.
  ::fuchsia::ui::viewsv1::ViewProperties view_properties;
  view_properties.view_layout = ::fuchsia::ui::viewsv1::ViewLayout::New();
  view_properties.view_layout->size.width = content_rect_.width;
  view_properties.view_layout->size.height = content_rect_.height;
  GetViewContainer()->SetChildProperties(
      kVideoChildKey, fidl::MakeOptional(std::move(view_properties)));

  InvalidateScene();
}

void MediaPlayerTestUtilView::OnSceneInvalidated(
    fuchsia::images::PresentationInfo presentation_info) {
  if (!has_physical_size())
    return;

  // Position the video.
  if (video_host_node_) {
    // TODO(dalesat): Fix this when SCN-1041 is fixed. Should be:
    // video_host_node_->SetTranslationRH(
    //     content_rect_.x + content_rect_.width * 0.5f,
    //     content_rect_.y + content_rect_.height * 0.5f, kVideoElevation);
    video_host_node_->SetTranslationRH(content_rect_.x, content_rect_.y,
                                       -kVideoElevation);
  }

  float progress_bar_slider_width =
      controls_rect_.width * normalized_progress();
  scenic::Rectangle progress_bar_slider_shape(
      session(), progress_bar_slider_width, controls_rect_.height);
  progress_bar_slider_node_.SetShape(progress_bar_slider_shape);
  progress_bar_slider_node_.SetTranslationRH(
      controls_rect_.x + progress_bar_slider_width * 0.5f,
      controls_rect_.y + controls_rect_.height * 0.5f,
      -kProgressBarSliderElevation);

  if (state_ == State::kPlaying) {
    InvalidateScene();
  }
}

void MediaPlayerTestUtilView::OnChildAttached(
    uint32_t child_key, ::fuchsia::ui::viewsv1::ViewInfo child_view_info) {
  FXL_DCHECK(child_key == kVideoChildKey);

  parent_node().AddChild(*video_host_node_);
  Layout();
}

void MediaPlayerTestUtilView::OnChildUnavailable(uint32_t child_key) {
  FXL_DCHECK(child_key == kVideoChildKey);
  FXL_LOG(ERROR) << "Video view died unexpectedly";

  video_host_node_->Detach();
  video_host_node_.reset();

  GetViewContainer()->RemoveChild2(child_key, zx::eventpair());
  Layout();
}

void MediaPlayerTestUtilView::HandleStatusChanged(
    const fuchsia::mediaplayer::PlayerStatus& status) {
  // Process status received from the player.
  if (status.timeline_function) {
    timeline_function_ =
        fxl::To<media::TimelineFunction>(*status.timeline_function);
    state_ = status.end_of_stream
                 ? State::kEnded
                 : (timeline_function_.subject_delta() == 0) ? State::kPaused
                                                             : State::kPlaying;
  } else {
    state_ = State::kPaused;
  }

  commands_.NotifyStatusChanged(status);

  if (status.problem) {
    if (!problem_shown_) {
      FXL_LOG(ERROR) << "PROBLEM: " << status.problem->type << ", "
                     << status.problem->details;
      problem_shown_ = true;
    }
  } else {
    problem_shown_ = false;
  }

  if (status.video_size && status.pixel_aspect_ratio &&
      (video_size_ != *status.video_size ||
       pixel_aspect_ratio_ != *status.pixel_aspect_ratio)) {
    video_size_ = *status.video_size;
    pixel_aspect_ratio_ = *status.pixel_aspect_ratio;
    Layout();
  }

  duration_ns_ = status.duration_ns;
  metadata_ = fidl::Clone(status.metadata);

  InvalidateScene();
}

void MediaPlayerTestUtilView::TogglePlayPause() {
  switch (state_) {
    case State::kPaused:
      player_->Play();
      break;
    case State::kPlaying:
      player_->Pause();
      break;
    case State::kEnded:
      player_->Seek(0);
      player_->Play();
      break;
    default:
      break;
  }
}

int64_t MediaPlayerTestUtilView::progress_ns() const {
  if (duration_ns_ == 0) {
    return 0;
  }

  // Apply the timeline function to the current time.
  int64_t position = timeline_function_(media::Timeline::local_now());

  if (position < 0) {
    position = 0;
  }

  if (position > duration_ns_) {
    position = duration_ns_;
  }

  return position;
}

float MediaPlayerTestUtilView::normalized_progress() const {
  if (duration_ns_ == 0) {
    return 0.0f;
  }

  return progress_ns() / static_cast<float>(duration_ns_);
}

}  // namespace test
}  // namespace media_player
