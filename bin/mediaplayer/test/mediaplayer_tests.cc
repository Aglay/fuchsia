// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/mediaplayer/cpp/fidl.h>
#include <queue>
#include "garnet/bin/mediaplayer/test/fakes/fake_audio.h"
#include "garnet/bin/mediaplayer/test/fakes/fake_scenic.h"
#include "garnet/bin/mediaplayer/test/fakes/fake_wav_reader.h"
#include "garnet/bin/mediaplayer/test/sink_feeder.h"
#include "lib/component/cpp/testing/test_util.h"
#include "lib/component/cpp/testing/test_with_environment.h"
#include "lib/fsl/io/fd.h"
#include "lib/fxl/logging.h"
#include "lib/media/timeline/timeline_function.h"
#include "lib/media/timeline/type_converters.h"

namespace media_player {
namespace test {

static constexpr uint16_t kSamplesPerFrame = 2;      // Stereo
static constexpr uint32_t kFramesPerSecond = 48000;  // 48kHz
static constexpr size_t kSinkFeedSize = 65536;
static constexpr uint32_t kSinkFeedMaxPacketSize = 4096;
static constexpr uint32_t kSinkFeedMaxPacketCount = 10;

constexpr char kBearFilePath[] = "/pkg/data/media_test_data/bear.mp4";

// Base class for mediaplayer tests.
class MediaPlayerTests
    : public component::testing::TestWithEnvironment {
 protected:
  struct Command {
    Command() = default;
    virtual ~Command() = default;
    virtual void Execute(MediaPlayerTests* test) = 0;
  };

  struct OpenCommand : public Command {
    OpenCommand(const std::string& path) : path_(path) {}
    void Execute(MediaPlayerTests* test) override {
      auto fd = fxl::UniqueFD(open(path_.c_str(), O_RDONLY));
      EXPECT_TRUE(fd.is_valid());
      test->player_->SetFileSource(
          fsl::CloneChannelFromFileDescriptor(fd.get()));
      test->start_position_ = 0;
      test->ExecuteNextCommand();
    }
    std::string path_;
  };

  struct PlayCommand : public Command {
    void Execute(MediaPlayerTests* test) override {
      test->player_->Play();
      test->should_play_ = true;
      test->ExecuteNextCommand();
    }
  };

  struct PauseCommand : public Command {
    void Execute(MediaPlayerTests* test) override {
      test->player_->Pause();
      test->should_play_ = false;
      test->ExecuteNextCommand();
    }
  };

  struct SeekCommand : public Command {
    SeekCommand(zx::duration position) : position_(position) {}
    void Execute(MediaPlayerTests* test) override {
      test->player_->Seek(position_.get());
      test->start_position_ = position_.get();
      test->ExecuteNextCommand();
    }
    zx::duration position_;
  };

  struct WaitForPositionCommand : public Command {
    WaitForPositionCommand(zx::duration position) : position_(position) {}
    void Execute(MediaPlayerTests* test) override {
      test->wait_for_position_ = position_.get();
      // The |OnStatusChanged| handler calls |ExecuteNextCommand| for us when
      // the time comes.
    }
    zx::duration position_;
  };

  struct SleepCommand : public Command {
    SleepCommand(zx::duration duration) : duration_(duration) {}
    void Execute(MediaPlayerTests* test) override {
      async::PostDelayedTask(test->dispatcher(),
                             [test]() { test->ExecuteNextCommand(); },
                             zx::duration(duration_));
    }
    zx::duration duration_;
  };

  void SetUp() override {
    auto services = CreateServices();

    // Add the service under test using its launch info.
    fuchsia::sys::LaunchInfo launch_info{
        "fuchsia-pkg://fuchsia.com/mediaplayer#meta/mediaplayer.cmx"};
    zx_status_t status = services->AddServiceWithLaunchInfo(
        std::move(launch_info), fuchsia::mediaplayer::Player::Name_);
    EXPECT_EQ(ZX_OK, status);

    services->AddService(fake_audio_.GetRequestHandler());
    services->AddService(fake_scenic_.GetRequestHandler());
    services->AddService(fake_scenic_.view_manager().GetRequestHandler());

    // Create the synthetic environment.
    environment_ =
        CreateNewEnclosingEnvironment("mediaplayer_tests", std::move(services));

    // Instantiate the player under test.
    environment_->ConnectToService(player_.NewRequest());

    player_.set_error_handler([this](zx_status_t status) {
      FXL_LOG(ERROR) << "Player connection closed, status " << status << ".";
      player_connection_closed_ = true;
      QuitLoop();
    });

    player_.events().OnStatusChanged =
        [this](fuchsia::mediaplayer::PlayerStatus status) {
          if (status.end_of_stream) {
            EXPECT_TRUE(status.ready);
            EXPECT_TRUE(fake_audio_.renderer().expected());
            EXPECT_TRUE(fake_scenic_.session().expected());

            if (when_stream_ends_) {
              when_stream_ends_();
              when_stream_ends_ = nullptr;
            }

            QuitLoop();
            return;
          }

          if (wait_for_position_ != fuchsia::media::NO_TIMESTAMP &&
              status.timeline_function &&
              status.timeline_function->subject_delta != 0 &&
              status.timeline_function->subject_time == start_position_) {
            // We're waiting for a specific position, and the timeline function
            // is current. Apply the timeline function in reverse to find the
            // CLOCK_MONOTONIC time at which we should resume executing
            // commands.
            auto timeline_function =
                fxl::To<media::TimelineFunction>(*status.timeline_function);
            int64_t wait_for_time =
                timeline_function.ApplyInverse(wait_for_position_);
            async::PostTaskForTime(dispatcher(),
                                   [this]() { ExecuteNextCommand(); },
                                   zx::time(wait_for_time));
            wait_for_position_ = fuchsia::media::NO_TIMESTAMP;
          }
        };
  }

  void TearDown() override { EXPECT_FALSE(player_connection_closed_); }

  // Registers an action to be performed the next time end-of-stream is reached.
  void WhenStreamEnds(fit::closure action) {
    when_stream_ends_ = std::move(action);
  }

  // Executes queued commands with the specified timeout.
  void Execute(zx::duration timeout = zx::sec(10)) {
    ExecuteNextCommand();
    EXPECT_FALSE(RunLoopWithTimeout(zx::duration(timeout)));
  }

  // Creates a view.
  void CreateView() {
    fuchsia::ui::viewsv1::ViewManagerPtr fake_view_manager_ptr;
    fake_scenic_.view_manager().Bind(fake_view_manager_ptr.NewRequest());

    player_->CreateView(std::move(fake_view_manager_ptr),
                        view_owner_ptr_.NewRequest());
  }

  // Queues a file open command.
  void Open(const std::string& path) { AddCommand(new OpenCommand(path)); }

  // Queues a play command.
  void Play() { AddCommand(new PlayCommand()); }

  // Queues a pause command.
  void Pause() { AddCommand(new PauseCommand()); }

  // Queues a seek command.
  void Seek(zx::duration position) { AddCommand(new SeekCommand(position)); }

  // Queues a command that waits until the specified position is reached.
  void WaitForPosition(zx::duration position) {
    AddCommand(new WaitForPositionCommand(position));
  }

  // Queues a command that sleeps for the specified duration.
  void Sleep(zx::duration duration) { AddCommand(new SleepCommand(duration)); }

  // Adds a command to the command queue.
  void AddCommand(Command* command) { command_queue_.emplace(command); }

  void ExecuteNextCommand() {
    if (command_queue_.empty()) {
      return;
    }

    async::PostTask(dispatcher(), [this]() {
      auto command = std::move(command_queue_.front());
      command_queue_.pop();
      command->Execute(this);
    });
  }

  fuchsia::mediaplayer::PlayerPtr player_;
  bool player_connection_closed_ = false;

  FakeWavReader fake_reader_;
  FakeAudio fake_audio_;
  FakeScenic fake_scenic_;
  fuchsia::ui::viewsv1token::ViewOwnerPtr view_owner_ptr_;
  std::unique_ptr<component::testing::EnclosingEnvironment> environment_;
  bool sink_connection_closed_ = false;
  SinkFeeder sink_feeder_;
  fit::closure when_stream_ends_;
  std::queue<std::unique_ptr<Command>> command_queue_;
  int64_t start_position_ = 0;
  bool should_play_ = false;
  int64_t wait_for_position_ = fuchsia::media::NO_TIMESTAMP;
};  // namespace media_player

// Play a synthetic WAV file from beginning to end.
TEST_F(MediaPlayerTests, PlayWav) {
  fake_audio_.renderer().ExpectPackets({{0, 4096, 0x20c39d1e31991800},
                                        {1024, 4096, 0xeaf137125d313800},
                                        {2048, 4096, 0x6162095671991800},
                                        {3072, 4096, 0x36e551c7dd41f800},
                                        {4096, 4096, 0x23dcbf6fb1991800},
                                        {5120, 4096, 0xee0a5963dd313800},
                                        {6144, 4096, 0x647b2ba7f1991800},
                                        {7168, 4096, 0x39fe74195d41f800},
                                        {8192, 4096, 0xb3de76b931991800},
                                        {9216, 4096, 0x7e0c10ad5d313800},
                                        {10240, 4096, 0xf47ce2f171991800},
                                        {11264, 4096, 0xca002b62dd41f800},
                                        {12288, 4096, 0xb6f7990ab1991800},
                                        {13312, 4096, 0x812532fedd313800},
                                        {14336, 4096, 0xf7960542f1991800},
                                        {15360, 4052, 0x7308a9824acbd5ea}});

  fuchsia::mediaplayer::SeekingReaderPtr fake_reader_ptr;
  fidl::InterfaceRequest<fuchsia::mediaplayer::SeekingReader> reader_request =
      fake_reader_ptr.NewRequest();
  fake_reader_.Bind(std::move(reader_request));

  fuchsia::mediaplayer::SourcePtr source;
  player_->CreateReaderSource(std::move(fake_reader_ptr), source.NewRequest());
  player_->SetSource(std::move(source));

  Play();

  Execute();
}

// Play an LPCM elementary stream using |StreamSource|
TEST_F(MediaPlayerTests, StreamSource) {
  fake_audio_.renderer().ExpectPackets({{0, 4096, 0xd2fbd957e3bf0000},
                                        {1024, 4096, 0xda25db3fa3bf0000},
                                        {2048, 4096, 0xe227e0f6e3bf0000},
                                        {3072, 4096, 0xe951e2dea3bf0000},
                                        {4096, 4096, 0x37ebf7d3e3bf0000},
                                        {5120, 4096, 0x3f15f9bba3bf0000},
                                        {6144, 4096, 0x4717ff72e3bf0000},
                                        {7168, 4096, 0x4e42015aa3bf0000},
                                        {8192, 4096, 0xeabc5347e3bf0000},
                                        {9216, 4096, 0xf1e6552fa3bf0000},
                                        {10240, 4096, 0xf9e85ae6e3bf0000},
                                        {11264, 4096, 0x01125ccea3bf0000},
                                        {12288, 4096, 0x4fac71c3e3bf0000},
                                        {13312, 4096, 0x56d673aba3bf0000},
                                        {14336, 4096, 0x5ed87962e3bf0000},
                                        {15360, 4096, 0x66027b4aa3bf0000}});

  fuchsia::mediaplayer::StreamSourcePtr stream_source;
  player_->CreateStreamSource(0, false, false, nullptr,
                              stream_source.NewRequest());

  fuchsia::media::AudioStreamType audio_stream_type;
  audio_stream_type.sample_format =
      fuchsia::media::AudioSampleFormat::SIGNED_16;
  audio_stream_type.channels = kSamplesPerFrame;
  audio_stream_type.frames_per_second = kFramesPerSecond;
  fuchsia::media::StreamType stream_type;
  stream_type.medium_specific.set_audio(std::move(audio_stream_type));
  stream_type.encoding = fuchsia::media::AUDIO_ENCODING_LPCM;

  fuchsia::media::SimpleStreamSinkPtr sink;
  stream_source->AddStream(std::move(stream_type), kFramesPerSecond, 1,
                           sink.NewRequest());
  sink.set_error_handler([this](zx_status_t status) {
    FXL_LOG(ERROR) << "SimpleStreamSink connection closed.";
    sink_connection_closed_ = true;
    QuitLoop();
  });

  // Here we're upcasting from a
  // |fidl::InterfaceHandle<fuchsia::mediaplayer::StreamSource>| to a
  // |fidl::InterfaceHandle<fuchsia::mediaplayer::Source>| the only way we
  // currently can. The compiler has no way of knowing whether this is
  // legit.
  // TODO(dalesat): Do this safely once FIDL-329 is fixed.
  player_->SetSource(fidl::InterfaceHandle<fuchsia::mediaplayer::Source>(
      stream_source.Unbind().TakeChannel()));

  sink_feeder_.Init(std::move(sink), kSinkFeedSize,
                    kSamplesPerFrame * sizeof(int16_t), kSinkFeedMaxPacketSize,
                    kSinkFeedMaxPacketCount);

  Play();

  Execute();
  EXPECT_FALSE(sink_connection_closed_);
}

// Play a real A/V file from beginning to end.
TEST_F(MediaPlayerTests, PlayBear) {
  // TODO(dalesat): Use ExpectPackets for audio.
  // This doesn't currently work, because the decoder behaves differently on
  // different targets.

  fake_scenic_.session().SetExpectations(
      {
          .width = 1280,
          .height = 768,
          .stride = 1280,
          .pixel_format = fuchsia::images::PixelFormat::YV12,
      },
      720,
      {{0, 983040, 0x0864378c3655ba47},
       {118811406, 983040, 0x2481a21b1e543c8e},
       {152178073, 983040, 0xe4294049f22539bc},
       {185544739, 983040, 0xde1058aba916ffad},
       {218911406, 983040, 0xc3fc580b34dc0383},
       {252278073, 983040, 0xff31322e5ccdebe0},
       {285644739, 983040, 0x64d31206ece7417f},
       {319011406, 983040, 0xf1c6bf7fe1be29be},
       {352378073, 983040, 0x72f44e5249a05c15},
       {385744739, 983040, 0x1ad7e92183fb3aa4},
       {419111406, 983040, 0x24b78b95d8c8b73d},
       {452478073, 983040, 0x25a798d9af5a1b7e},
       {485844739, 983040, 0x3379288b1f4197a5},
       {519211406, 983040, 0x15fb9c205590cbc9},
       {552578073, 983040, 0xc04a1834aec8b399},
       {585944739, 983040, 0x97eded0e3b6348d3},
       {619311406, 983040, 0x09dba227982ba479},
       {652678073, 983040, 0x4d2a1042babc479c},
       {686044739, 983040, 0x379f96a35774dc2b},
       {719411406, 983040, 0x2d95a4b5506bd4c3},
       {752778073, 983040, 0xda99bf00cd971999},
       {786144739, 983040, 0x20a21550eb717da2},
       {819511406, 983040, 0x3733b96d2279460b},
       {852878073, 983040, 0x8ea51ee0088cda67},
       {886244739, 983040, 0x8d6af19e5d9629ae},
       {919611406, 983040, 0xd9765bd28098f093},
       {952978073, 983040, 0x9a747455b496c9d1},
       {986344739, 983040, 0xfc8e90e73cc086f6},
       {1019711406, 983040, 0xc3dec92946fc0005},
       {1053078073, 983040, 0x215b196e790214c4},
       {1086444739, 983040, 0x30b114015d719041},
       {1119811406, 983040, 0x5ed6e582ac4022a1},
       {1153178073, 983040, 0xbccb6f8ba8601507},
       {1186544739, 983040, 0x34eab6666dc6c717},
       {1219911406, 983040, 0x5e33bfc44650245f},
       {1253278073, 983040, 0x736397b78e0850ff},
       {1286644739, 983040, 0x620d7190a9e49a31},
       {1320011406, 983040, 0x436e952327e311ea},
       {1353378073, 983040, 0xf6fa16fc170a85f3},
       {1386744739, 983040, 0x9f457e1a66323ead},
       {1420111406, 983040, 0xb1747e31ea5358db},
       {1453478073, 983040, 0x4da84ec1c5cb45de},
       {1486844739, 983040, 0x5454f9007dc4de01},
       {1520211406, 983040, 0x8e9777accf38e4f0},
       {1553578073, 983040, 0x16a2ebade809e497},
       {1586944739, 983040, 0x36d323606ebca2f4},
       {1620311406, 983040, 0x17eaf1e84353dec9},
       {1653678073, 983040, 0xdb1b344498520386},
       {1687044739, 983040, 0xec53764065860e7f},
       {1720411406, 983040, 0x110a7dddd4c45a54},
       {1753778073, 983040, 0x6df1c973722f01c7},
       {1787144739, 983040, 0x2e18f1e1544e002a},
       {1820511406, 983040, 0x0de7b784dd8b0494},
       {1853878073, 983040, 0x6e254cd1652be6a9},
       {1887244739, 983040, 0x6353cb7c270b06c2},
       {1920611406, 983040, 0x8d62a2ddb0350ab9},
       {1953978073, 983040, 0xaf0ee1376ded95cd},
       {1987344739, 983040, 0xf617917814de4169},
       {2020711406, 983040, 0xf686efcec861909f},
       {2054078073, 983040, 0x539f93afe6863cca},
       {2087444739, 983040, 0x12c5c5e4eb5b2649},
       {2120811406, 983040, 0x984cf8179effd823},
       {2154178073, 983040, 0xfcb0cc2eb449ed16},
       {2187544739, 983040, 0xf070b3572db477cc},
       {2220911406, 983040, 0x5dd53f712ce8e1a6},
       {2254278073, 983040, 0x02e0600528534bef},
       {2287644739, 983040, 0x53120fbaca19e13b},
       {2321011406, 983040, 0xd66e3cb3e70897eb},
       {2354378073, 983040, 0x9f4138aa8e84cbf4},
       {2387744739, 983040, 0xf350694d6a12ec39},
       {2421111406, 983040, 0x08c986a97ab8fbb3},
       {2454478073, 983040, 0x229d2b908659b728},
       {2487844739, 983040, 0xf54cbe4582a3f8e1},
       {2521211406, 983040, 0x8c8985c6649a3e1c},
       {2554578073, 983040, 0x711e04eccc5e4527},
       {2587944739, 983040, 0x78e2979034921e70},
       {2621311406, 983040, 0x51c3524f5bf83a62},
       {2654678073, 983040, 0x12b6f7b7591e7044},
       {2688044739, 983040, 0xca8d7ac09b973a4b},
       {2721411406, 983040, 0x3e666b376fcaa466},
       {2754778073, 983040, 0x8f3657c9648b6dbb},
       {2788144739, 983040, 0x19a30916a3375f4e}});

  CreateView();
  Open(kBearFilePath);
  Play();

  Execute();
}

}  // namespace test
}  // namespace media_player
