// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/module_manifest_source/xdr.h"

#include "peridot/lib/fidl/json_xdr.h"
#include <modular/cpp/fidl.h>

namespace modular {

namespace {

void XdrParameterConstraint_v1(XdrContext* const xdr,
                               ParameterConstraint* const data) {
  xdr->Field("name", &data->name);
  xdr->Field("type", &data->type);
}

void XdrModuleManifest_v1(modular::XdrContext* const xdr,
                          modular::ModuleManifest* const data) {
  xdr->Field("binary", &data->binary);
  xdr->Field("suggestion_headline", &data->suggestion_headline);
  xdr->ReadErrorHandler([data] { data->action = ""; })
      ->Field("action", &data->action);
  xdr->ReadErrorHandler([data] { data->composition_pattern = ""; })
      ->Field("composition_pattern", &data->composition_pattern);
  xdr->ReadErrorHandler([data] { data->parameter_constraints = nullptr; })
      ->Field("parameters", &data->parameter_constraints,
              XdrParameterConstraint_v1);
}

}  // namespace

extern const XdrFilterType<ModuleManifest> XdrModuleManifest[] = {
  XdrModuleManifest_v1,
  nullptr,
};

}  // namespace modular
