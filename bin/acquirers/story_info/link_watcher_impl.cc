// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/acquirers/story_info/link_watcher_impl.h"

#include <set>
#include <sstream>

#include <lib/context/cpp/context_metadata_builder.h>
#include <lib/entity/cpp/json.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/functional/make_copyable.h>

#include "peridot/bin/acquirers/story_info/story_watcher_impl.h"
#include "peridot/bin/sessionmgr/storage/constants_and_utils.h"  // MakeLinkKey
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/rapidjson/rapidjson.h"

namespace maxwell {

LinkWatcherImpl::LinkWatcherImpl(
    StoryWatcherImpl* const owner,
    fuchsia::modular::StoryController* const story_controller,
    const std::string& story_id,
    fuchsia::modular::ContextValueWriter* const story_value,
    fuchsia::modular::LinkPath link_path)
    : owner_(owner),
      story_controller_(story_controller),
      story_id_(story_id),
      link_path_(std::move(link_path)),
      link_watcher_binding_(this) {
  // We hold onto a LinkPtr for the lifetime of this LinkWatcherImpl so that
  // our watcher handle stays alive. Incidentally, this also means that the
  // observed link remains "active" in the FW forever.
  // TODO(thatguy): Use the new PuppetMaster observation API. MI4-1084
  story_controller_->GetLink(fidl::Clone(link_path_), link_ptr_.NewRequest());

  story_value->CreateChildValue(link_node_writer_.NewRequest(),
                                fuchsia::modular::ContextValueType::LINK);
  link_node_writer_->Set(
      nullptr, fidl::MakeOptional(ContextMetadataBuilder()
                                      .SetLinkPath(link_path_.module_path,
                                                   link_path_.link_name)
                                      .Build()));

  link_ptr_->Watch(link_watcher_binding_.NewBinding());

  // If the link becomes inactive, we stop watching it. It might still receive
  // updates from other devices, but nothing can tell us as it isn't kept in
  // memory on the current device.
  //
  // The fuchsia::modular::Link itself is not kept here, because otherwise it
  // never becomes inactive (i.e. loses all its fuchsia::modular::Link
  // connections).
  link_watcher_binding_.set_error_handler([this](zx_status_t status) {
    owner_->DropLink(modular::MakeLinkKey(link_path_));
  });
}

LinkWatcherImpl::~LinkWatcherImpl() = default;

void LinkWatcherImpl::Notify(fuchsia::mem::Buffer json) {
  std::string json_string;
  FXL_CHECK(fsl::StringFromVmo(json, &json_string));
  ProcessNewValue(json_string);
}

void LinkWatcherImpl::ProcessNewValue(const fidl::StringPtr& value) {
  // We are looking for the following |value| structures:
  //
  // 1) |value| contains a JSON-style entity:
  //   { "@type": ..., ... }
  // 2) |value| contains a JSON-encoded fuchsia::modular::Entity reference
  // (EntityReferenceFromJson() will return true).
  // 3) |value| is a JSON dictionary, and any of the members satisfies either
  // (1) or (2).
  //
  // TODO(thatguy): Moving to Bundles allows us to ignore (3), and using
  // Entities everywhere allows us to ignore (1).
  modular::JsonDoc doc;
  doc.Parse(value);
  FXL_CHECK(!doc.HasParseError());

  if (!doc.IsObject()) {
    return;
  }

  // (1) & (2)
  std::vector<std::string> types;
  std::string ref;
  if (modular::ExtractEntityTypesFromJson(doc, &types) ||
      modular::EntityReferenceFromJson(doc, &ref)) {
    // There is only *one* fuchsia::modular::Entity in this
    // fuchsia::modular::Link.
    entity_node_writers_.clear();
    if (!single_entity_node_writer_.is_bound()) {
      link_node_writer_->CreateChildValue(
          single_entity_node_writer_.NewRequest(),
          fuchsia::modular::ContextValueType::ENTITY);
    }
    // TODO(thatguy): The context engine expects an fuchsia::modular::Entity
    // reference to be written directly as the content, versus the way Links
    // wrap the reference in JSON. It'd be good to normalize on one encoded
    // representation for fuchsia::modular::Entity references in the context
    // engine.
    if (ref.empty()) {
      single_entity_node_writer_->Set(value, nullptr);
    } else {
      single_entity_node_writer_->Set(ref, nullptr);
    }
    return;
  } else {
    // There is not simply a *single* fuchsia::modular::Entity in this
    // fuchsia::modular::Link. There may be multiple Entities (see below).
    single_entity_node_writer_.Unbind();
  }

  // (3)
  std::set<std::string> keys_that_have_entities;
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    if (modular::ExtractEntityTypesFromJson(it->value, &types) ||
        modular::EntityReferenceFromJson(it->value, &ref)) {
      keys_that_have_entities.insert(it->name.GetString());

      auto value_it = entity_node_writers_.find(it->name.GetString());
      if (value_it == entity_node_writers_.end()) {
        fuchsia::modular::ContextValueWriterPtr writer;
        link_node_writer_->CreateChildValue(
            writer.NewRequest(), fuchsia::modular::ContextValueType::ENTITY);
        value_it = entity_node_writers_
                       .emplace(it->name.GetString(), std::move(writer))
                       .first;
      }
      value_it->second->Set(modular::JsonValueToString(it->value), nullptr);
    }
  }

  // Clean up any old entries in |entity_node_writers_|.
  std::set<std::string> to_remove;
  for (const auto& entry : entity_node_writers_) {
    if (keys_that_have_entities.count(entry.first) == 0) {
      to_remove.insert(entry.first);
    }
  }
  for (const auto& key : to_remove) {
    entity_node_writers_.erase(key);
  }
}

}  // namespace maxwell
