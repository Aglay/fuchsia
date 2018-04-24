// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/module_manifest_source/json.h"

#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "third_party/rapidjson/rapidjson/document.h"

namespace modular {

namespace {

void XdrParameterConstraint(modular::XdrContext* const xdr,
                            modular::ParameterConstraint* const data) {
  xdr->Field("name", &data->name);
  xdr->Field("type", &data->type);
}

void XdrEntry(modular::XdrContext* const xdr,
              modular::ModuleManifest* const data) {
  xdr->Field("binary", &data->binary);
  xdr->Field("suggestion_headline", &data->suggestion_headline);
  xdr->ReadErrorHandler([data] { data->action = ""; })
      ->Field("action", &data->action);
  xdr->ReadErrorHandler([data] { data->composition_pattern = ""; })
      ->Field("composition_pattern", &data->composition_pattern);
  xdr->ReadErrorHandler([data] { data->parameter_constraints = nullptr; })
      ->Field("parameters", &data->parameter_constraints,
              XdrParameterConstraint);
}

}  // namespace

bool ModuleManifestEntryFromJson(const std::string& json,
                                 modular::ModuleManifest* entry) {
  rapidjson::Document doc;
  // Schema validation of the JSON is happening at publish time. By the time we
  // get here, we assume it's valid manifest JSON.
  doc.Parse(json.c_str());

  // Handle bad manifests, including older files expressed as an array.
  // Any mismatch causes XdrRead to DCHECK.
  if (!doc.IsObject()) {
    return false;
  }

  // Our tooling validates |doc|'s JSON schema so that we don't have to here.
  // It may be good to do this, though.
  // TODO(thatguy): Do this if it becomes a problem.
  if (!modular::XdrRead(&doc, entry, XdrEntry)) {
    return false;
  }
  return true;
}

void ModuleManifestEntryToJson(const modular::ModuleManifest& entry,
                               std::string* json) {
  rapidjson::Document doc;
  modular::ModuleManifest local_entry;
  fidl::Clone(entry, &local_entry);
  modular::XdrWrite(&doc, &local_entry, XdrEntry);

  *json = JsonValueToPrettyString(doc);
}

}  // namespace modular
