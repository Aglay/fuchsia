// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_data/system_log_recorder/log_message_store.h"

#include <lib/trace/event.h>

#include "src/developer/feedback/utils/log_format.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace feedback {
namespace {

const std::string kDroppedFormatStr = "!!! DROPPED %lu LOG MESSAGES !!!\n";

}  // namespace

LogMessageStore::LogMessageStore(size_t max_capacity_bytes)
    : mtx_(),
      queue_(),
      max_capacity_bytes_(max_capacity_bytes),
      bytes_remaining_(max_capacity_bytes_) {}

bool LogMessageStore::Add(fuchsia::logger::LogMessage msg) {
  TRACE_DURATION("feedback:io", "LogMessageStore::Add");

  std::lock_guard<std::mutex> lk(mtx_);

  // Early return on full buffer.
  if (bytes_remaining_ == 0) {
    ++num_messages_dropped;
    return false;
  }

  std::string str = Format(msg);

  if (bytes_remaining_ >= str.size()) {
    bytes_remaining_ -= str.size();
    queue_.push_back(std::move(str));
    return true;
  } else {
    // We will drop the rest of the incoming messages until the next Consume(). This avoids trying
    // to squeeze in a shorter message that will wrongfully appear before the DROPPED message.
    bytes_remaining_ = 0;
    ++num_messages_dropped;
    return false;
  }
}

std::string LogMessageStore::Consume() {
  TRACE_DURATION("feedback:io", "LogMessageStore::Consume");

  std::lock_guard<std::mutex> lk(mtx_);

  // We assume all messages end with a newline character.
  std::string str = fxl::JoinStrings(queue_);

  if (num_messages_dropped > 0) {
    str += fxl::StringPrintf(kDroppedFormatStr.c_str(), num_messages_dropped);
  }

  queue_.clear();
  bytes_remaining_ = max_capacity_bytes_;
  num_messages_dropped = 0;

  return str;
}

}  // namespace feedback
