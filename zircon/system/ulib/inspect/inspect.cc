// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fit/result.h>
#include <lib/inspect/common.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/inspect/cpp/vmo/heap.h>

#include <sstream>

using inspect::internal::Heap;
using inspect::internal::State;

namespace inspect {

namespace {
const InspectSettings kDefaultInspectSettings = {.maximum_size = 256 * 1024};
}  // namespace

Inspector::Inspector() : Inspector(kDefaultInspectSettings) {}

Inspector::Inspector(const InspectSettings& settings) : root_(std::make_unique<Node>()) {
  if (settings.maximum_size == 0) {
    return;
  }

  state_ = State::CreateWithSize(settings.maximum_size);
  if (!state_) {
    return;
  }

  *root_ = state_->CreateRootNode();
}

Inspector::Inspector(zx::vmo vmo) : root_(std::make_unique<Node>()) {
  size_t size;

  zx_status_t status;
  if (ZX_OK != (status = vmo.get_size(&size))) {
    return;
  }

  if (size == 0) {
    // VMO cannot be zero size.
    return;
  }

  // Decommit all pages, reducing memory usage of the VMO and zeroing it.
  if (ZX_OK != (status = vmo.op_range(ZX_VMO_OP_DECOMMIT, 0, size, nullptr, 0))) {
    return;
  }

  state_ = State::Create(std::make_unique<Heap>(std::move(vmo)));
  if (!state_) {
    return;
  }

  *root_ = state_->CreateRootNode();
}

zx::vmo Inspector::DuplicateVmo() const {
  zx::vmo ret;

  if (state_) {
    state_->GetVmo().duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_READ | ZX_RIGHT_MAP, &ret);
  }

  return ret;
}

zx::vmo Inspector::CopyVmo() const {
  zx::vmo ret;

  state_->Copy(&ret);

  return ret;
}

std::vector<uint8_t> Inspector::CopyBytes() const {
  std::vector<uint8_t> ret;
  state_->CopyBytes(&ret);
  return ret;
}

Node& Inspector::GetRoot() const { return *root_; }

std::string UniqueName(const std::string& prefix) {
  std::ostringstream out;
  uint64_t value = inspect_counter_increment(kUniqueNameCounterId);
  out << prefix << "0x" << std::hex << value;
  return out.str();
}

}  // namespace inspect
