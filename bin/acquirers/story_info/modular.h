// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_ACQUIRERS_STORY_INFO_MODULAR_H_
#define PERIDOT_BIN_ACQUIRERS_STORY_INFO_MODULAR_H_

#include <string>

#include <modular/cpp/fidl.h>
#include <modular_private/cpp/fidl.h>
#include "peridot/lib/fidl/json_xdr.h"

namespace maxwell {

std::string StoryStateToString(modular::StoryState state);

void XdrLinkPath(modular::XdrContext* xdr, modular::LinkPath* data);

void XdrModuleData(modular::XdrContext* xdr, modular::ModuleData* data);

}  // namespace maxwell

#endif  // PERIDOT_BIN_ACQUIRERS_STORY_INFO_MODULAR_H_
