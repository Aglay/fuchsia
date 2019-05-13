// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/system_monitor/dockyard/dockyard.h"

#include <grpc++/grpc++.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "garnet/lib/system_monitor/protos/dockyard.grpc.pb.h"
#include "src/lib/fxl/logging.h"

namespace dockyard {

namespace {

// This is an arbitrary default port.
constexpr char DEFAULT_SERVER_ADDRESS[] = "0.0.0.0:50051";

// To calculate the slope, a range of time is needed. |prior_time| and  |time|
// define that range. The very first |prior_time| is one stride prior to the
// requested start time.
SampleValue calculate_slope(SampleValue value, SampleValue* prior_value,
                            SampleTimeNs time, SampleTimeNs* prior_time) {
  if (value < *prior_value) {
    // A lower value will produce a negative slope, which is not currently
    // supported. As a workaround the value is pulled up to |prior_value| to
    // create a convex surface.
    value = *prior_value;
  }
  assert(time >= *prior_time);
  SampleValue delta_value = value - *prior_value;
  SampleTimeNs delta_time = time - *prior_time;
  SampleValue result =
      delta_time ? delta_value * SLOPE_LIMIT / delta_time : 0ULL;
  *prior_value = value;
  *prior_time = time;
  return result;
}

// Logic and data behind the server's behavior.
class DockyardServiceImpl final : public dockyard_proto::Dockyard::Service {
 public:
  void SetDockyard(Dockyard* dockyard) { dockyard_ = dockyard; }

 private:
  Dockyard* dockyard_;

  grpc::Status Init(grpc::ServerContext* context,
                    const dockyard_proto::InitRequest* request,
                    dockyard_proto::InitReply* reply) override {
    auto now = std::chrono::system_clock::now();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now.time_since_epoch())
                           .count();
    dockyard_->SetDeviceTimeDeltaNs(nanoseconds - request->device_time_ns());
    if (request->version() != DOCKYARD_VERSION) {
      return grpc::Status::CANCELLED;
    }
    reply->set_version(DOCKYARD_VERSION);
    dockyard_->OnConnection();
    return grpc::Status::OK;
  }

  grpc::Status SendInspectJson(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<dockyard_proto::EmptyMessage,
                               dockyard_proto::InspectJson>* stream) override {
    dockyard_proto::InspectJson inspect;
    while (stream->Read(&inspect)) {
      FXL_LOG(INFO) << "Received inspect at " << inspect.time() << ", key "
                    << inspect.dockyard_id() << ": " << inspect.json();
      // TODO(smbug.com/43): interpret the data.
    }
    return grpc::Status::OK;
  }

  // This is the handler for the client sending a `SendSample` message. A better
  // name would be `ReceiveSample` but then it wouldn't match the message
  // name.
  grpc::Status SendSample(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<dockyard_proto::EmptyMessage,
                               dockyard_proto::RawSample>* stream) override {
    dockyard_proto::RawSample sample;
    while (stream->Read(&sample)) {
      FXL_LOG(INFO) << "Received sample at " << sample.time() << ", key "
                    << sample.sample().key() << ": " << sample.sample().value();

      dockyard_->AddSample(sample.sample().key(),
                           Sample(sample.time(), sample.sample().value()));
    }
    return grpc::Status::OK;
  }

  // Handler for the Harvester calling `SendSamples()`.
  grpc::Status SendSamples(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<dockyard_proto::EmptyMessage,
                               dockyard_proto::RawSamples>* stream) override {
    dockyard_proto::RawSamples samples;
    while (stream->Read(&samples)) {
      int limit = samples.sample_size();
      for (int i = 0; i < limit; ++i) {
        auto sample = samples.sample(i);
        dockyard_->AddSample(sample.key(),
                             Sample(samples.time(), sample.value()));
      }
    }
    return grpc::Status::OK;
  }

  grpc::Status GetDockyardIdsForPaths(
      grpc::ServerContext* context,
      const dockyard_proto::DockyardPaths* request,
      dockyard_proto::DockyardIds* reply) override {
    for (int i = 0; i < request->path_size(); ++i) {
      DockyardId id = dockyard_->GetDockyardId(request->path(i));
      reply->add_id(id);
#ifdef VERBOSE_OUTPUT
      FXL_LOG(INFO) << "Received DockyardIds "
                    << ": " << request->path(i) << ", id " << id;
#endif  // VERBOSE_OUTPUT
    }
    return grpc::Status::OK;
  }
};

// Listen for Harvester connections from the Fuchsia device.
void RunGrpcServer(const char* listen_at, Dockyard* dockyard) {
  std::string server_address(listen_at);
  DockyardServiceImpl service;
  service.SetDockyard(dockyard);

  grpc::ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to a *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  FXL_LOG(INFO) << "Server listening on " << server_address;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

// Calculates the (edge) time for each column of the result data.
SampleTimeNs CalcTimeForStride(const StreamSetsRequest& request,
                               ssize_t index) {
  // These need to be signed to support a signed |index|.
  int64_t delta = (request.end_time_ns - request.start_time_ns);
  int64_t count = int64_t(request.sample_count);
  return request.start_time_ns + (delta * index / count);
}

}  // namespace

bool StreamSetsRequest::HasFlag(StreamSetsRequestFlags flag) const {
  return (flags & flag) != 0;
}

std::ostream& operator<<(std::ostream& out, const StreamSetsRequest& request) {
  out << "StreamSetsRequest {" << std::endl;
  out << "  request_id: " << request.request_id << std::endl;
  out << "  start_time_ns: " << request.start_time_ns << std::endl;
  out << "  end_time_ns:   " << request.end_time_ns << std::endl;
  out << "    delta time in seconds: "
      << double(request.end_time_ns - request.start_time_ns) /
             kNanosecondsPerSecond
      << std::endl;
  out << "  sample_count: " << request.sample_count << std::endl;
  out << "  min: " << request.min;
  out << "  max: " << request.max;
  out << "  reserved: " << request.reserved << std::endl;
  out << "  render_style: " << request.render_style;
  out << "  flags: " << request.flags << std::endl;
  out << "  ids (" << request.dockyard_ids.size() << "): [";
  for (auto iter = request.dockyard_ids.begin();
       iter != request.dockyard_ids.end(); ++iter) {
    out << " " << *iter;
  }
  out << " ]" << std::endl;
  out << "}" << std::endl;
  return out;
}

std::ostream& operator<<(std::ostream& out,
                         const StreamSetsResponse& response) {
  out << "StreamSetsResponse {" << std::endl;
  out << "  request_id: " << response.request_id << std::endl;
  out << "  lowest_value: " << response.lowest_value << std::endl;
  out << "  highest_value: " << response.highest_value << std::endl;
  out << "  data_sets (" << response.data_sets.size() << "): [";
  for (auto list = response.data_sets.begin(); list != response.data_sets.end();
       ++list) {
    out << "  data_set: {";
    for (auto data = list->begin(); data != list->end(); ++data) {
      if (*data == NO_DATA) {
        out << " NO_DATA";
      } else {
        out << " " << *data;
      }
    }
    out << " }, " << std::endl;
  }
  out << "]" << std::endl;
  out << "}" << std::endl;
  return out;
}

Dockyard::Dockyard()
    : device_time_delta_ns_(0ULL),
      latest_sample_time_ns_(0ULL),
      on_connection_handler_(nullptr),
      on_paths_handler_(nullptr),
      on_stream_sets_handler_(nullptr),
      next_context_id_(0ULL) {}

Dockyard::~Dockyard() {
  std::lock_guard<std::mutex> guard(mutex_);
#ifdef VERBOSE_OUTPUT
  FXL_LOG(INFO) << "Stopping dockyard server";
#endif  // VERBOSE_OUTPUT
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
  for (SampleStreamMap::iterator i = sample_streams_.begin();
       i != sample_streams_.end(); ++i) {
    delete i->second;
  }
}

SampleTimeNs Dockyard::DeviceDeltaTimeNs() const {
  return device_time_delta_ns_;
}

void Dockyard::SetDeviceTimeDeltaNs(SampleTimeNs delta_ns) {
  device_time_delta_ns_ = delta_ns;
}

SampleTimeNs Dockyard::LatestSampleTimeNs() const {
  return latest_sample_time_ns_;
}

void Dockyard::AddSample(DockyardId dockyard_id, Sample sample) {
  std::lock_guard<std::mutex> guard(mutex_);
  // Find or create a sample_stream for this dockyard_id.
  SampleStream* sample_stream;
  auto search = sample_streams_.find(dockyard_id);
  if (search == sample_streams_.end()) {
    sample_stream = new SampleStream();
    sample_streams_.emplace(dockyard_id, sample_stream);
  } else {
    sample_stream = search->second;
  }
  latest_sample_time_ns_ = sample.time;
  sample_stream->emplace(sample.time, sample.value);

  // Track the overall lowest and highest values encountered.
  sample_stream_low_high_.try_emplace(dockyard_id,
                                      std::make_pair(SAMPLE_MAX_VALUE, 0ULL));
  auto low_high = sample_stream_low_high_.find(dockyard_id);
  SampleValue lowest = low_high->second.first;
  SampleValue highest = low_high->second.second;
  bool change = false;
  if (lowest > sample.value) {
    lowest = sample.value;
    change = true;
  }
  if (highest < sample.value) {
    highest = sample.value;
    change = true;
  }
  if (change) {
    sample_stream_low_high_[dockyard_id] = std::make_pair(lowest, highest);
  }
}

void Dockyard::AddSamples(DockyardId dockyard_id, std::vector<Sample> samples) {
  std::lock_guard<std::mutex> guard(mutex_);
  // Find or create a sample_stream for this dockyard_id.
  SampleStream* sample_stream;
  auto search = sample_streams_.find(dockyard_id);
  if (search == sample_streams_.end()) {
    sample_stream = new SampleStream();
    sample_streams_.emplace(dockyard_id, sample_stream);
  } else {
    sample_stream = search->second;
  }

  // Track the overall lowest and highest values encountered.
  sample_stream_low_high_.try_emplace(dockyard_id,
                                      std::make_pair(SAMPLE_MAX_VALUE, 0ULL));
  auto low_high = sample_stream_low_high_.find(dockyard_id);
  SampleValue lowest = low_high->second.first;
  SampleValue highest = low_high->second.second;
  for (auto i = samples.begin(); i != samples.end(); ++i) {
    if (lowest > i->value) {
      lowest = i->value;
    }
    if (highest < i->value) {
      highest = i->value;
    }
    sample_stream->emplace(i->time, i->value);
  }
  sample_stream_low_high_[dockyard_id] = std::make_pair(lowest, highest);
}

DockyardId Dockyard::GetDockyardId(const std::string& dockyard_path) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto search = dockyard_path_to_id_.find(dockyard_path);
  if (search != dockyard_path_to_id_.end()) {
    return search->second;
  }
  DockyardId id = dockyard_path_to_id_.size();
  dockyard_path_to_id_.emplace(dockyard_path, id);
  dockyard_id_to_path_.emplace(id, dockyard_path);
#ifdef VERBOSE_OUTPUT
  FXL_LOG(INFO) << "Path " << dockyard_path << ": ID " << id;
#endif  // VERBOSE_OUTPUT
  assert(dockyard_path_to_id_.find(dockyard_path) !=
         dockyard_path_to_id_.end());
  return id;
}

bool Dockyard::GetDockyardPath(DockyardId dockyard_id,
                               std::string* dockyard_path) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto search = dockyard_id_to_path_.find(dockyard_id);
  if (search != dockyard_id_to_path_.end()) {
    *dockyard_path = search->second;
    return true;
  }
  return false;
}

uint64_t Dockyard::GetStreamSets(StreamSetsRequest* request) {
  std::lock_guard<std::mutex> guard(mutex_);
  request->request_id = next_context_id_;
  pending_requests_.push_back(request);
  ++next_context_id_;
  return request->request_id;
}

void Dockyard::OnConnection() {
  if (on_connection_handler_ != nullptr) {
    on_connection_handler_("");
  }
}

void Dockyard::StartCollectingFrom(const std::string& device) {
  Initialize();
  FXL_LOG(INFO) << "Starting collecting from " << device;
  // TODO(smbug.com/39): Connect to the device and start the harvester.
}

void Dockyard::StopCollectingFrom(const std::string& device) {
  FXL_LOG(INFO) << "Stop collecting from " << device;
  // TODO(smbug.com/40): Stop the harvester.
}

bool Dockyard::Initialize() {
  if (server_thread_.joinable()) {
    return true;
  }
  FXL_LOG(INFO) << "Starting dockyard server";
  server_thread_ =
      std::thread([this] { RunGrpcServer(DEFAULT_SERVER_ADDRESS, this); });
  return server_thread_.joinable();
}

OnConnectionCallback Dockyard::SetConnectionHandler(
    OnConnectionCallback callback) {
  assert(!server_thread_.joinable());
  auto old_handler = on_connection_handler_;
  on_connection_handler_ = callback;
  return old_handler;
}

OnPathsCallback Dockyard::SetDockyardPathsHandler(OnPathsCallback callback) {
  assert(!server_thread_.joinable());
  auto old_handler = on_paths_handler_;
  on_paths_handler_ = callback;
  return old_handler;
}

OnStreamSetsCallback Dockyard::SetStreamSetsHandler(
    OnStreamSetsCallback callback) {
  auto old_handler = on_stream_sets_handler_;
  on_stream_sets_handler_ = callback;
  return old_handler;
}

void Dockyard::ProcessSingleRequest(const StreamSetsRequest& request,
                                    StreamSetsResponse* response) const {
  std::lock_guard<std::mutex> guard(mutex_);
  FXL_LOG(INFO) << "ProcessSingleRequest request " << request;
  response->request_id = request.request_id;
  for (auto dockyard_id = request.dockyard_ids.begin();
       dockyard_id != request.dockyard_ids.end(); ++dockyard_id) {
    std::vector<SampleValue> samples;
    auto search = sample_streams_.find(*dockyard_id);
    if (search == sample_streams_.end()) {
      samples.push_back(NO_STREAM);
    } else {
      auto sample_stream = *search->second;
      switch (request.render_style) {
        case StreamSetsRequest::SCULPTING:
          ComputeSculpted(*dockyard_id, sample_stream, request, &samples);
          break;
        case StreamSetsRequest::WIDE_SMOOTHING:
          ComputeSmoothed(*dockyard_id, sample_stream, request, &samples);
          break;
        case StreamSetsRequest::LOWEST_PER_COLUMN:
          ComputeLowestPerColumn(*dockyard_id, sample_stream, request,
                                 &samples);
          break;
        case StreamSetsRequest::HIGHEST_PER_COLUMN:
          ComputeHighestPerColumn(*dockyard_id, sample_stream, request,
                                  &samples);
          break;
        case StreamSetsRequest::AVERAGE_PER_COLUMN:
          ComputeAveragePerColumn(*dockyard_id, sample_stream, request,
                                  &samples);
          break;
        default:
          break;
      }
      if (request.HasFlag(StreamSetsRequest::NORMALIZE)) {
        NormalizeResponse(*dockyard_id, sample_stream, request, &samples);
      }
    }
    response->data_sets.push_back(samples);
  }
  ComputeLowestHighestForRequest(request, response);
}

void Dockyard::ComputeAveragePerColumn(
    DockyardId dockyard_id, const SampleStream& sample_stream,
    const StreamSetsRequest& request, std::vector<SampleValue>* samples) const {
  // To calculate the slope, a range of time is needed. |prior_time| and
  // |start_time| define that range. The very first |prior_time| is one stride
  // prior to the requested start time.
  SampleTimeNs prior_time = CalcTimeForStride(request, -1);
  SampleValue prior_value = 0ULL;
  const int64_t limit = request.sample_count;

  for (int64_t sample_n = -1; sample_n < limit; ++sample_n) {
    SampleTimeNs start_time = CalcTimeForStride(request, sample_n);
    SampleTimeNs end_time = CalcTimeForStride(request, sample_n + 1);

    auto begin = sample_stream.lower_bound(start_time);
    if (begin == sample_stream.end()) {
      if (sample_n >= 0) {
        samples->push_back(NO_DATA);
      }
      continue;
    }
    auto end = sample_stream.lower_bound(end_time);
    SampleValue accumulator = 0ULL;
    uint_fast32_t count = 0ULL;
    for (auto i = begin; i != end; ++i) {
      accumulator += i->second;
      ++count;
    }
    SampleValue result = NO_DATA;
    if (count) {
      if (request.HasFlag(StreamSetsRequest::SLOPE)) {
        result = calculate_slope(accumulator / count, &prior_value, start_time,
                                 &prior_time);
      } else {
        result = accumulator / count;
      }
    }
    if (sample_n >= 0) {
      samples->push_back(result);
    }
  }
}

void Dockyard::ComputeHighestPerColumn(
    DockyardId dockyard_id, const SampleStream& sample_stream,
    const StreamSetsRequest& request, std::vector<SampleValue>* samples) const {
  // To calculate the slope, a range of time is needed. |prior_time| and
  // |start_time| define that range. The very first |prior_time| is one stride
  // prior to the requested start time.
  SampleTimeNs prior_time = CalcTimeForStride(request, -1);
  SampleValue prior_value = 0ULL;
  const int64_t limit = request.sample_count;
  for (int64_t sample_n = -1; sample_n < limit; ++sample_n) {
    SampleTimeNs start_time = CalcTimeForStride(request, sample_n);
    SampleTimeNs end_time = CalcTimeForStride(request, sample_n + 1);

    auto begin = sample_stream.lower_bound(start_time);
    if (begin == sample_stream.end()) {
      if (sample_n >= 0) {
        samples->push_back(NO_DATA);
      }
      continue;
    }
    auto end = sample_stream.lower_bound(end_time);
    SampleTimeNs high_time = request.start_time_ns;
    SampleValue highest = 0ULL;
    uint_fast32_t count = 0ULL;
    for (auto i = begin; i != end; ++i) {
      if (highest < i->second) {
        high_time = i->first;
        highest = i->second;
      }
      ++count;
    }
    SampleValue result = NO_DATA;
    if (count) {
      if (request.HasFlag(StreamSetsRequest::SLOPE)) {
        result = calculate_slope(highest, &prior_value, high_time, &prior_time);
      } else {
        result = highest;
      }
    }
    if (sample_n >= 0) {
      samples->push_back(result);
    }
  }
}

void Dockyard::ComputeLowestPerColumn(DockyardId dockyard_id,
                                      const SampleStream& sample_stream,
                                      const StreamSetsRequest& request,
                                      std::vector<SampleValue>* samples) const {
  // To calculate the slope, a range of time is needed. |prior_time| and
  // |start_time| define that range. The very first |prior_time| is one stride
  // prior to the requested start time.
  SampleTimeNs prior_time = CalcTimeForStride(request, -1);
  SampleValue prior_value = 0ULL;
  const int64_t limit = request.sample_count;
  for (int64_t sample_n = -1; sample_n < limit; ++sample_n) {
    SampleTimeNs start_time = CalcTimeForStride(request, sample_n);
    auto begin = sample_stream.lower_bound(start_time);
    if (begin == sample_stream.end()) {
      if (sample_n >= 0) {
        samples->push_back(NO_DATA);
      }
      continue;
    }
    SampleTimeNs end_time = CalcTimeForStride(request, sample_n + 1);
    auto end = sample_stream.lower_bound(end_time);
    SampleTimeNs low_time = request.start_time_ns;
    SampleValue lowest = SAMPLE_MAX_VALUE;
    uint_fast32_t count = 0ULL;
    for (auto i = begin; i != end; ++i) {
      if (lowest > i->second) {
        low_time = i->first;
        lowest = i->second;
      }
      ++count;
    }
    SampleValue result = NO_DATA;
    if (count) {
      if (request.HasFlag(StreamSetsRequest::SLOPE)) {
        result = calculate_slope(lowest, &prior_value, low_time, &prior_time);
      } else {
        result = lowest;
      }
    }
    if (sample_n >= 0) {
      samples->push_back(result);
    }
  }
}

void Dockyard::NormalizeResponse(DockyardId dockyard_id,
                                 const SampleStream& sample_stream,
                                 const StreamSetsRequest& request,
                                 std::vector<SampleValue>* samples) const {
  auto low_high = sample_stream_low_high_.find(dockyard_id);
  SampleValue lowest = low_high->second.first;
  SampleValue highest = low_high->second.second;
  SampleValue value_range = highest - lowest;
  if (value_range == 0) {
    // If there is no range, then all the values drop to zero.
    // Also avoid divide by zero in the code below.
    std::fill(samples->begin(), samples->end(), 0);
    return;
  }

  for (std::vector<SampleValue>::iterator i = samples->begin();
       i != samples->end(); ++i) {
    *i = (*i - lowest) * NORMALIZATION_RANGE / value_range;
  }
}

void Dockyard::ComputeSculpted(DockyardId dockyard_id,
                               const SampleStream& sample_stream,
                               const StreamSetsRequest& request,
                               std::vector<SampleValue>* samples) const {
  // To calculate the slope, a range of time is needed. |prior_time| and
  // |start_time| define that range. The very first |prior_time| is one stride
  // prior to the requested start time.
  SampleTimeNs prior_time = CalcTimeForStride(request, -1);
  SampleValue prior_value = 0ULL;
  auto overall_average = OverallAverageForStream(dockyard_id);
  const int64_t limit = request.sample_count;
  for (int64_t sample_n = -1; sample_n < limit; ++sample_n) {
    SampleTimeNs start_time = CalcTimeForStride(request, sample_n);
    auto begin = sample_stream.lower_bound(start_time);
    if (begin == sample_stream.end()) {
      if (sample_n >= 0) {
        samples->push_back(NO_DATA);
      }
      continue;
    }
    SampleTimeNs end_time = CalcTimeForStride(request, sample_n + 1);
    auto end = sample_stream.lower_bound(end_time);
    SampleValue accumulator = 0ULL;
    SampleValue highest = 0ULL;
    SampleValue lowest = SAMPLE_MAX_VALUE;
    uint_fast32_t count = 0ULL;
    for (auto i = begin; i != end; ++i) {
      auto value = i->second;
      accumulator += value;
      if (highest < value) {
        highest = value;
      }
      if (lowest > value) {
        lowest = value;
      }
      ++count;
    }
    SampleValue result = NO_DATA;
    if (count) {
      auto average = accumulator / count;
      auto final = average >= overall_average ? highest : lowest;
      if (request.HasFlag(StreamSetsRequest::SLOPE)) {
        result = calculate_slope(final, &prior_value, end_time, &prior_time);
      } else {
        result = final;
      }
    }
    if (sample_n >= 0) {
      samples->push_back(result);
    }
  }
}

void Dockyard::ComputeSmoothed(DockyardId dockyard_id,
                               const SampleStream& sample_stream,
                               const StreamSetsRequest& request,
                               std::vector<SampleValue>* samples) const {
  SampleTimeNs prior_time = CalcTimeForStride(request, -1);
  SampleValue prior_value = 0ULL;
  const int64_t limit = request.sample_count;
  for (int64_t sample_n = -1; sample_n < limit; ++sample_n) {
    SampleTimeNs start_time = CalcTimeForStride(request, sample_n - 1);
    auto begin = sample_stream.lower_bound(start_time);
    if (begin == sample_stream.end()) {
      if (sample_n >= 0) {
        samples->push_back(NO_DATA);
      }
      continue;
    }
    SampleTimeNs end_time = CalcTimeForStride(request, sample_n + 2);
    auto end = sample_stream.lower_bound(end_time);
    SampleValue accumulator = 0ULL;
    uint_fast32_t count = 0ULL;
    for (auto i = begin; i != end; ++i) {
      accumulator += i->second;
      ++count;
    }
    SampleValue result = NO_DATA;
    if (count) {
      result = accumulator / count;
      if (request.HasFlag(StreamSetsRequest::SLOPE)) {
        result = calculate_slope(result, &prior_value, end_time, &prior_time);
      }
    }
    if (sample_n >= 0) {
      samples->push_back(result);
    }
  }
}

SampleValue Dockyard::OverallAverageForStream(DockyardId dockyard_id) const {
  auto low_high = sample_stream_low_high_.find(dockyard_id);
  if (low_high == sample_stream_low_high_.end()) {
    return NO_DATA;
  }
  return (low_high->second.first + low_high->second.second) / 2;
}

void Dockyard::ComputeLowestHighestForRequest(
    const StreamSetsRequest& request, StreamSetsResponse* response) const {
  if (request.HasFlag(StreamSetsRequest::SLOPE)) {
    // Slope responses have fixed low/high values.
    response->lowest_value = 0ULL;
    response->highest_value = SLOPE_LIMIT;
    return;
  }
  // Gather the overall lowest and highest values encountered.
  SampleValue lowest = SAMPLE_MAX_VALUE;
  SampleValue highest = 0ULL;
  for (auto dockyard_id = request.dockyard_ids.begin();
       dockyard_id != request.dockyard_ids.end(); ++dockyard_id) {
    auto low_high = sample_stream_low_high_.find(*dockyard_id);
    if (low_high == sample_stream_low_high_.end()) {
      continue;
    }
    if (lowest > low_high->second.first) {
      lowest = low_high->second.first;
    }
    if (highest < low_high->second.second) {
      highest = low_high->second.second;
    }
  }
  response->lowest_value = lowest;
  response->highest_value = highest;
}

void Dockyard::ProcessRequests() {
  if (on_stream_sets_handler_ != nullptr) {
    StreamSetsResponse response;
    for (auto i = pending_requests_.begin(); i != pending_requests_.end();
         ++i) {
      ProcessSingleRequest(**i, &response);
      on_stream_sets_handler_(response);
    }
  }
  pending_requests_.clear();
}

}  // namespace dockyard
