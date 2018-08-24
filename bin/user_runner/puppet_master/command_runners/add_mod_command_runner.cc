// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/puppet_master/command_runners/add_mod_command_runner.h"

#include <lib/fxl/logging.h>
#include "peridot/bin/user_runner/puppet_master/command_runners/operation_calls/add_mod_call.h"

namespace modular {

AddModCommandRunner::AddModCommandRunner(
    fuchsia::modular::ModuleResolver* const module_resolver,
    fuchsia::modular::EntityResolver* const entity_resolver)
    : module_resolver_(module_resolver), entity_resolver_(entity_resolver) {
  FXL_DCHECK(module_resolver_);
  FXL_DCHECK(entity_resolver_);
}

AddModCommandRunner::~AddModCommandRunner() = default;

void AddModCommandRunner::Execute(
    fidl::StringPtr story_id, StoryStorage* const story_storage,
    fuchsia::modular::StoryCommand command,
    std::function<void(fuchsia::modular::ExecuteResult)> done) {
  FXL_CHECK(command.is_add_mod());

  auto& add_mod = command.add_mod();
  operation_queue_.Add(new AddModCall(
      story_storage, module_resolver_, entity_resolver_,
      std::move(add_mod.mod_name), std::move(add_mod.intent),
      std::make_unique<fuchsia::modular::SurfaceRelation>(
          std::move(add_mod.surface_relation)),
      std::move(add_mod.surface_parent_mod_name),
      fuchsia::modular::ModuleSource::EXTERNAL,
      [done = std::move(done)](fuchsia::modular::ExecuteResult result,
                               fuchsia::modular::ModuleData module_data) {
        done(std::move(result));
      }));
}

}  // namespace modular
