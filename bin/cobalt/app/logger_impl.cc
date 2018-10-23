#include "garnet/bin/cobalt/app/logger_impl.h"

#include "garnet/bin/cobalt/app/utils.h"

namespace cobalt {

LoggerImpl::LoggerImpl(std::unique_ptr<logger::ProjectContext> project_context,
                       logger::Encoder* encoder,
                       logger::ObservationWriter* observation_writer,
                       TimerManager* timer_manager)
    : project_context_(std::move(project_context)),
      logger_(encoder, observation_writer, project_context_.get()),
      timer_manager_(timer_manager) {}

void LoggerImpl::LogEvent(
    uint32_t metric_id, uint32_t event_type_index,
    fuchsia::cobalt::LoggerBase::LogEventCallback callback) {
  callback(ToCobaltStatus(logger_.LogEvent(metric_id, event_type_index)));
}

void LoggerImpl::LogEventCount(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    int64_t period_duration_micros, int64_t count,
    fuchsia::cobalt::LoggerBase::LogEventCountCallback callback) {
  callback(ToCobaltStatus(
      logger_.LogEventCount(metric_id, event_type_index, component.get(),
                            period_duration_micros, count)));
}

void LoggerImpl::LogElapsedTime(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    int64_t elapsed_micros,
    fuchsia::cobalt::LoggerBase::LogElapsedTimeCallback callback) {
  callback(ToCobaltStatus(logger_.LogElapsedTime(
      metric_id, event_type_index, component.get(), elapsed_micros)));
}

void LoggerImpl::LogFrameRate(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    float fps, fuchsia::cobalt::LoggerBase::LogFrameRateCallback callback) {
  callback(ToCobaltStatus(
      logger_.LogFrameRate(metric_id, event_type_index, component.get(), fps)));
}

void LoggerImpl::LogMemoryUsage(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    int64_t bytes,
    fuchsia::cobalt::LoggerBase::LogMemoryUsageCallback callback) {
  callback(ToCobaltStatus(logger_.LogMemoryUsage(metric_id, event_type_index,
                                                 component.get(), bytes)));
}

void LoggerImpl::LogString(
    uint32_t metric_id, fidl::StringPtr s,
    fuchsia::cobalt::LoggerBase::LogStringCallback callback) {
  callback(ToCobaltStatus(logger_.LogString(metric_id, s.get())));
}

void LoggerImpl::LogIntHistogram(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    fidl::VectorPtr<fuchsia::cobalt::HistogramBucket> histogram,
    fuchsia::cobalt::Logger::LogIntHistogramCallback callback) {
  logger::HistogramPtr histogram_ptr(
      new google::protobuf::RepeatedPtrField<HistogramBucket>());
  for (auto it = histogram->begin(); histogram->end() != it; it++) {
    auto bucket = histogram_ptr->Add();
    bucket->set_index((*it).index);
    bucket->set_count((*it).count);
  }
  callback(ToCobaltStatus(logger_.LogIntHistogram(
      metric_id, event_type_index, component.get(), std::move(histogram_ptr))));
}

void LoggerImpl::LogIntHistogram(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    fidl::VectorPtr<uint32_t> bucket_indices,
    fidl::VectorPtr<uint64_t> bucket_counts,
    fuchsia::cobalt::LoggerSimple::LogIntHistogramCallback callback) {
  if (bucket_indices->size() != bucket_counts->size()) {
    FXL_LOG(ERROR) << "[" << metric_id
                   << "]: bucket_indices.size() != bucket_counts.size().";
    callback(Status::INVALID_ARGUMENTS);
    return;
  }
  logger::HistogramPtr histogram_ptr(
      new google::protobuf::RepeatedPtrField<HistogramBucket>());
  for (auto i = 0; i < bucket_indices->size(); i++) {
    auto bucket = histogram_ptr->Add();
    bucket->set_index(bucket_indices->at(i));
    bucket->set_count(bucket_counts->at(i));
  }

  callback(ToCobaltStatus(logger_.LogIntHistogram(
      metric_id, event_type_index, component.get(), std::move(histogram_ptr))));
}

void LoggerImpl::LogCustomEvent(
    uint32_t metric_id,
    fidl::VectorPtr<fuchsia::cobalt::CustomEventValue> event_values,
    fuchsia::cobalt::Logger::LogCustomEventCallback callback) {
  logger::EventValuesPtr inner_event_values(
      new google::protobuf::Map<std::string, CustomDimensionValue>());
  for (auto it = event_values->begin(); event_values->end() != it; it++) {
    CustomDimensionValue value;
    if (it->value.is_string_value()) {
      value.set_string_value(it->value.string_value());
    } else if (it->value.is_int_value()) {
      value.set_int_value(it->value.int_value());
    } else if (it->value.is_double_value()) {
      value.set_double_value(it->value.double_value());
    } else if (it->value.is_index_value()) {
      value.set_index_value(it->value.index_value());
    }

    auto pair = google::protobuf::MapPair(it->dimension_name.get(), value);
    inner_event_values->insert(pair);
  }
  callback(ToCobaltStatus(
      logger_.LogCustomEvent(metric_id, std::move(inner_event_values))));
}

template <class CB>
void LoggerImpl::AddTimerObservationIfReady(
    std::unique_ptr<TimerVal> timer_val_ptr, CB callback) {
  if (!TimerManager::isReady(timer_val_ptr)) {
    // TimerManager has not received both StartTimer and EndTimer calls. Return
    // OK status and wait for the other call.
    callback(Status::OK);
    return;
  }

  callback(ToCobaltStatus(logger_.LogElapsedTime(
      timer_val_ptr->metric_id, timer_val_ptr->event_type_index,
      timer_val_ptr->component,
      timer_val_ptr->end_timestamp - timer_val_ptr->start_timestamp)));
}

void LoggerImpl::StartTimer(
    uint32_t metric_id, uint32_t event_type_index, fidl::StringPtr component,
    fidl::StringPtr timer_id, uint64_t timestamp, uint32_t timeout_s,
    fuchsia::cobalt::LoggerBase::StartTimerCallback callback) {
  std::unique_ptr<TimerVal> timer_val_ptr;
  auto status = timer_manager_->GetTimerValWithStart(
      metric_id, event_type_index, component.get(), 0, timer_id.get(),
      timestamp, timeout_s, &timer_val_ptr);

  if (status != Status::OK) {
    callback(status);
    return;
  }

  AddTimerObservationIfReady(std::move(timer_val_ptr), std::move(callback));
}

void LoggerImpl::EndTimer(
    fidl::StringPtr timer_id, uint64_t timestamp, uint32_t timeout_s,
    fuchsia::cobalt::LoggerBase::EndTimerCallback callback) {
  std::unique_ptr<TimerVal> timer_val_ptr;
  auto status = timer_manager_->GetTimerValWithEnd(timer_id.get(), timestamp,
                                                   timeout_s, &timer_val_ptr);

  if (status != Status::OK) {
    callback(status);
    return;
  }

  AddTimerObservationIfReady(std::move(timer_val_ptr), std::move(callback));
}

}  // namespace cobalt
