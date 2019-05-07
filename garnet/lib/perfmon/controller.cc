// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/perfmon/controller.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include <fbl/algorithm.h>
#include <src/lib/fxl/logging.h>
#include <src/lib/fxl/strings/string_printf.h>
#include <zircon/syscalls.h>

#include "garnet/lib/perfmon/config_impl.h"
#include "garnet/lib/perfmon/properties_impl.h"

namespace perfmon {

const char kPerfMonDev[] = "/dev/sys/cpu-trace/perfmon";

static uint32_t RoundUpToPages(uint32_t value) {
  uint32_t size = fbl::round_up(value, Controller::kPageSize);
  FXL_DCHECK(size & ~(Controller::kPageSize - 1));
  return size >> Controller::kLog2PageSize;
}

static uint32_t GetBufferSizeInPages(CollectionMode mode,
                                     uint32_t requested_size_in_pages) {
  switch (mode) {
  case CollectionMode::kSample:
    return requested_size_in_pages;
  case CollectionMode::kTally: {
    // For tally mode we just need something large enough to hold
    // the header + records for each event.
    unsigned num_events = kMaxNumEvents;
    uint32_t size = (sizeof(BufferHeader) +
                     num_events * sizeof(ValueRecord));
    return RoundUpToPages(size);
  }
  default:
    __UNREACHABLE;
  }
}

bool Controller::IsSupported() {
  // The device path isn't present if it's not supported.
  struct stat stat_buffer;
  if (stat(kPerfMonDev, &stat_buffer) != 0)
    return false;
  return S_ISCHR(stat_buffer.st_mode);
}

bool Controller::GetProperties(Properties* props) {
  int raw_fd = open(kPerfMonDev, O_WRONLY);
  if (raw_fd < 0) {
    FXL_LOG(ERROR) << "Failed to open " << kPerfMonDev << ": errno=" << errno;
    return false;
  }
  fxl::UniqueFD fd(raw_fd);

  perfmon_ioctl_properties_t properties;
  auto status = ioctl_perfmon_get_properties(fd.get(), &properties);
  if (status < 0) {
    FXL_LOG(ERROR) << "Failed to get properties: " << status;
    return false;
  }

  internal::IoctlToPerfmonProperties(properties, props);
  return true;
}

bool Controller::Alloc(int fd, uint32_t num_traces,
                       uint32_t buffer_size_in_pages) {
  ioctl_perfmon_alloc_t alloc;
  alloc.num_buffers = num_traces;
  alloc.buffer_size_in_pages = buffer_size_in_pages;
  FXL_VLOG(2) << fxl::StringPrintf("num_buffers=%u, buffer_size_in_pages=0x%x",
                                   num_traces, buffer_size_in_pages);
  auto status = ioctl_perfmon_alloc_trace(fd, &alloc);
  // TODO(dje): If we get BAD_STATE, a previous run may have crashed without
  // resetting the device. The device doesn't reset itself on close yet.
  if (status == ZX_ERR_BAD_STATE) {
    FXL_VLOG(1) << "Got BAD_STATE trying to allocate a trace,"
                << " resetting device and trying again";
    status = ioctl_perfmon_stop(fd);
    if (status != ZX_OK) {
      FXL_VLOG(1) << "Stopping device failed: " << status;
    }
    status = ioctl_perfmon_free_trace(fd);
    if (status != ZX_OK) {
      FXL_VLOG(1) << "Freeing previous trace failed: " << status;
    }
    status = ioctl_perfmon_alloc_trace(fd, &alloc);
    if (status == ZX_OK) {
      FXL_VLOG(1) << "Second allocation succeeded";
    }
  }

  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "ioctl_perfmon_alloc_trace failed: status=" << status;
    return false;
  }

  return true;
}

bool Controller::Create(uint32_t buffer_size_in_pages, const Config& config,
                        std::unique_ptr<Controller>* out_controller) {
  int raw_fd = open(kPerfMonDev, O_WRONLY);
  if (raw_fd < 0) {
    FXL_LOG(ERROR) << "Failed to open " << kPerfMonDev << ": errno=" << errno;
    return false;
  }
  fxl::UniqueFD fd(raw_fd);

  if (buffer_size_in_pages > kMaxBufferSizeInPages) {
    FXL_LOG(ERROR) << "Buffer size is too large, max " << kMaxBufferSizeInPages
                   << " pages";
    return false;
  }

  CollectionMode mode = config.GetMode();
  uint32_t num_traces = zx_system_get_num_cpus();
  // For "tally" mode we only need a small fixed amount, so toss what the
  // caller provided and use our own value.
  uint32_t actual_buffer_size_in_pages =
      GetBufferSizeInPages(mode, buffer_size_in_pages);

  perfmon_ioctl_config_t ioctl_config;
  Config::Status status = internal::PerfmonToIoctlConfig(config, &ioctl_config);
  if (status != Config::Status::OK) {
    FXL_LOG(ERROR) << "Error processing configuration: "
                   << Config::StatusToString(status);
    return false;
  }

  if (!Alloc(raw_fd, num_traces, actual_buffer_size_in_pages)) {
    return false;
  }

  out_controller->reset(new Controller(std::move(fd), mode, num_traces,
                                       buffer_size_in_pages, ioctl_config));
  return true;
}

Controller::Controller(fxl::UniqueFD fd, CollectionMode collection_mode,
                       uint32_t num_traces, uint32_t buffer_size_in_pages,
                       const perfmon_ioctl_config_t& config)
    : fd_(std::move(fd)),
      collection_mode_(collection_mode),
      num_traces_(num_traces),
      buffer_size_in_pages_(buffer_size_in_pages),
      config_(config) {
}

Controller::~Controller() {
  Reset();
}

bool Controller::Start() {
  if (started_) {
    FXL_LOG(ERROR) << "already started";
    return false;
  }

  if (!Stage()) {
    return false;
  }

  auto status = ioctl_perfmon_start(fd_.get());
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "ioctl_perfmon_start failed: status=" << status;
  } else {
    started_ = true;
  }
  return status == ZX_OK;
}

void Controller::Stop() {
  auto status = ioctl_perfmon_stop(fd_.get());
  if (status != ZX_OK) {
    // This can get called while tracing is currently stopped.
    if (!started_ && status == ZX_ERR_BAD_STATE) {
      ;  // dont report an error in this case
    } else {
      FXL_LOG(ERROR) << "ioctl_perfmon_stop failed: status=" << status;
    }
  } else {
    started_ = false;
  }
}

bool Controller::Stage() {
  FXL_DCHECK(!started_);
  auto status = ioctl_perfmon_stage_config(fd_.get(), &config_);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "ioctl_perfmon_stage_config failed: status=" << status;
  }
  return status == ZX_OK;
}

void Controller::Free() {
  auto status = ioctl_perfmon_free_trace(fd_.get());
  if (status != ZX_OK) {
    // This can get called while tracing is currently stopped.
    if (!started_ && status == ZX_ERR_BAD_STATE) {
      ;  // dont report an error in this case
    } else {
      FXL_LOG(ERROR) << "ioctl_perfmon_free_trace failed: status=" << status;
    }
  }
}

void Controller::Reset() {
  Stop();
  Free();
}

std::unique_ptr<DeviceReader> Controller::GetReader() {
  std::unique_ptr<DeviceReader> reader;
  if (DeviceReader::Create(fd_.get(), buffer_size_in_pages_, &reader)) {
    return reader;
  }
  return nullptr;
}

}  // namespace perfmon
