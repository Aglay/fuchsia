// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/sessionmgr/story_runner/story_controller_impl.h"

#include <memory>
#include <string>
#include <vector>

#include <fuchsia/ledger/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/modular/storymodel/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/viewsv1/cpp/fidl.h>
#include <lib/async/cpp/future.h>
#include <lib/async/default.h>
#include <lib/component/cpp/connect.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/entity/cpp/json.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_ptr_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fsl/types/type_converters.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/functional/make_copyable.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/strings/join_strings.h>
#include <lib/fxl/strings/split_string.h>
#include <lib/fxl/type_converter.h>

#include "peridot/bin/basemgr/cobalt/cobalt.h"
#include "peridot/bin/sessionmgr/puppet_master/command_runners/operation_calls/add_mod_call.h"
#include "peridot/bin/sessionmgr/puppet_master/command_runners/operation_calls/find_modules_call.h"
#include "peridot/bin/sessionmgr/puppet_master/command_runners/operation_calls/get_types_from_entity_call.h"
#include "peridot/bin/sessionmgr/puppet_master/command_runners/operation_calls/initialize_chain_call.h"
#include "peridot/bin/sessionmgr/storage/constants_and_utils.h"
#include "peridot/bin/sessionmgr/storage/story_storage.h"
#include "peridot/bin/sessionmgr/story/model/story_observer.h"
#include "peridot/bin/sessionmgr/story_runner/link_impl.h"
#include "peridot/bin/sessionmgr/story_runner/module_context_impl.h"
#include "peridot/bin/sessionmgr/story_runner/module_controller_impl.h"
#include "peridot/bin/sessionmgr/story_runner/ongoing_activity_impl.h"
#include "peridot/bin/sessionmgr/story_runner/story_provider_impl.h"
#include "peridot/bin/sessionmgr/story_runner/story_shell_context_impl.h"
#include "peridot/lib/common/names.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/ledger_client/operations.h"
#include "peridot/lib/util/string_escape.h"

// Used to create std::set<LinkPath>.
namespace std {
template <>
struct less<fuchsia::modular::LinkPath> {
  bool operator()(const fuchsia::modular::LinkPath& left,
                  const fuchsia::modular::LinkPath& right) const {
    if (left.module_path == right.module_path) {
      return left.link_name < right.link_name;
    }
    return *left.module_path < *right.module_path;
  }
};
}  // namespace std

namespace modular {

constexpr char kStoryEnvironmentLabelPrefix[] = "story-";
constexpr auto kUpdateSnapshotTimeout = zx::sec(1);

namespace {

constexpr char kSurfaceIDSeparator[] = ":";
fidl::StringPtr ModulePathToSurfaceID(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  std::vector<std::string> path;
  // Sanitize all the |module_name|s that make up this |module_path|.
  for (const auto& module_name : module_path.get()) {
    path.push_back(StringEscape(module_name.get(), kSurfaceIDSeparator));
  }
  return fxl::JoinStrings(path, kSurfaceIDSeparator);
}

fidl::VectorPtr<fidl::StringPtr> ModulePathFromSurfaceID(
    const fidl::StringPtr& surface_id) {
  std::vector<std::string> path;
  for (const auto& parts : SplitEscapedString(fxl::StringView(surface_id.get()),
                                              kSurfaceIDSeparator[0])) {
    path.push_back(parts.ToString());
  }
  return fxl::To<fidl::VectorPtr<fidl::StringPtr>>(path);
}

fidl::VectorPtr<fidl::StringPtr> ParentModulePath(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  fidl::VectorPtr<fidl::StringPtr> ret =
      fidl::VectorPtr<fidl::StringPtr>::New(0);

  if (module_path->size() > 0) {
    for (size_t i = 0; i < module_path->size() - 1; i++) {
      ret.push_back(module_path->at(i));
    }
  }
  return ret;
}

}  // namespace

bool ShouldRestartModuleForNewIntent(
    const fuchsia::modular::Intent& old_intent,
    const fuchsia::modular::Intent& new_intent) {
  if (old_intent.handler != new_intent.handler) {
    return true;
  }

  return false;
}

// Launches (brings up a running instance) of a module.
//
// If the module is to be composed into the story shell, notifies the story
// shell of the new module. If the module is composed internally, connects the
// view owner request appropriately.
class StoryControllerImpl::LaunchModuleCall : public Operation<> {
 public:
  LaunchModuleCall(StoryControllerImpl* const story_controller_impl,
                   fuchsia::modular::ModuleData module_data,
                   fidl::InterfaceRequest<fuchsia::modular::ModuleController>
                       module_controller_request,
                   fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
                       view_owner_request,
                   ResultCall result_call)
      : Operation("StoryControllerImpl::LaunchModuleCall",
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        module_controller_request_(std::move(module_controller_request)),
        view_owner_request_(std::move(view_owner_request)),
        start_time_(zx_clock_get(ZX_CLOCK_UTC)) {
    FXL_DCHECK(!module_data_.module_path.is_null());
  }

 private:
  void Run() override {
    FlowToken flow{this};

    RunningModInfo* const running_mod_info =
        story_controller_impl_->FindRunningModInfo(module_data_.module_path);

    // We launch the new module if it doesn't run yet.
    if (!running_mod_info) {
      Launch(flow);
      return;
    }

    // If the new module is already running, but with a different Intent, we
    // tear it down then launch a new instance.
    if (ShouldRestartModuleForNewIntent(*running_mod_info->module_data->intent,
                                        *module_data_.intent)) {
      running_mod_info->module_controller_impl->Teardown([this, flow] {
        // NOTE(mesch): |running_mod_info| is invalid at this point.
        Launch(flow);
      });
      return;
    }

    // Otherwise, the module is already running. Connect
    // |module_controller_request_| to the existing instance of
    // fuchsia::modular::ModuleController.
    if (module_controller_request_.is_valid()) {
      running_mod_info->module_controller_impl->Connect(
          std::move(module_controller_request_));
    }

    // Since the module is already running send it the new intent.
    NotifyModuleOfIntent(*running_mod_info);
  }

  void Launch(FlowToken /*flow*/) {
    FXL_LOG(INFO) << "StoryControllerImpl::LaunchModule() "
                  << module_data_.module_url << " "
                  << ModulePathToSurfaceID(module_data_.module_path);
    fuchsia::modular::AppConfig module_config;
    module_config.url = module_data_.module_url;

    fuchsia::ui::viewsv1::ViewProviderPtr view_provider;
    fidl::InterfaceRequest<fuchsia::ui::viewsv1::ViewProvider>
        view_provider_request = view_provider.NewRequest();
    view_provider->CreateView(std::move(view_owner_request_), nullptr);

    fuchsia::sys::ServiceProviderPtr module_context_provider;
    auto module_context_provider_request = module_context_provider.NewRequest();
    auto service_list = fuchsia::sys::ServiceList::New();
    service_list->names.push_back(fuchsia::modular::ComponentContext::Name_);
    service_list->names.push_back(fuchsia::modular::ModuleContext::Name_);
    service_list->names.push_back(
        fuchsia::modular::IntelligenceServices::Name_);
    service_list->provider = std::move(module_context_provider);

    RunningModInfo running_mod_info;
    running_mod_info.module_data = CloneOptional(module_data_);

    // ModuleControllerImpl's constructor launches the child application.
    running_mod_info.module_controller_impl =
        std::make_unique<ModuleControllerImpl>(
            story_controller_impl_,
            story_controller_impl_->story_environment_->GetLauncher(),
            std::move(module_config), running_mod_info.module_data.get(),
            std::move(service_list), std::move(view_provider_request));

    // Modules added/started through PuppetMaster don't have a module
    // controller request.
    if (module_controller_request_.is_valid()) {
      running_mod_info.module_controller_impl->Connect(
          std::move(module_controller_request_));
    }

    ModuleContextInfo module_context_info = {
        story_controller_impl_->story_provider_impl_->component_context_info(),
        story_controller_impl_,
        story_controller_impl_->story_visibility_system_,
        story_controller_impl_->story_provider_impl_
            ->user_intelligence_provider()};

    running_mod_info.module_context_impl = std::make_unique<ModuleContextImpl>(
        module_context_info, running_mod_info.module_data.get(),
        std::move(module_context_provider_request));

    NotifyModuleOfIntent(running_mod_info);

    story_controller_impl_->running_mod_infos_.emplace_back(
        std::move(running_mod_info));

    for (auto& i : story_controller_impl_->watchers_.ptrs()) {
      fuchsia::modular::ModuleData module_data;
      module_data_.Clone(&module_data);
      (*i)->OnModuleAdded(std::move(module_data));
    }

    ReportModuleLaunchTime(
        module_data_.module_url,
        zx::duration(zx_clock_get(ZX_CLOCK_UTC) - start_time_));
  }

  // Connects to the module's intent handler and sends it the intent from
  // |module_data_.intent|.
  void NotifyModuleOfIntent(const RunningModInfo& running_mod_info) {
    if (!module_data_.intent) {
      return;
    }
    fuchsia::modular::IntentHandlerPtr intent_handler;
    running_mod_info.module_controller_impl->services().ConnectToService(
        intent_handler.NewRequest());
    fuchsia::modular::Intent intent;
    module_data_.intent->Clone(&intent);

    for (auto& parameter : *intent.parameters) {
      // When an Intent is created the creator specifies a link either by
      // absolute path, or by name in the creator's namespace.
      //
      // The link_path and link_name both get converted to the handler's
      // namespace before the intent is sent to the handler. The link name in
      // the handler's namespace is always the same as the parameter name.
      if (parameter.data.is_link_path() || parameter.data.is_link_name()) {
        parameter.data.set_link_name(parameter.name);
      }
    }

    intent_handler->HandleIntent(std::move(intent));
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  fuchsia::modular::ModuleData module_data_;
  fidl::InterfaceRequest<fuchsia::modular::ModuleController>
      module_controller_request_;
  fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
      view_owner_request_;
  const zx_time_t start_time_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchModuleCall);
};

// KillModuleCall tears down the module by the given module_data. It is enqueued
// when ledger confirms that the module was stopped, see OnModuleDataUpdated().
class StoryControllerImpl::KillModuleCall : public Operation<> {
 public:
  KillModuleCall(StoryControllerImpl* const story_controller_impl,
                 fuchsia::modular::ModuleData module_data,
                 const std::function<void()>& done)
      : Operation("StoryControllerImpl::KillModuleCall", [] {}),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        done_(done) {}

 private:
  void Run() override {
    FlowToken flow{this};
    // If the module is external, we also notify story shell about it going
    // away. An internal module is stopped by its parent module, and it's up to
    // the parent module to defocus it first. TODO(mesch): Why not always
    // defocus?
    auto future =
        Future<>::Create("StoryControllerImpl.KillModuleCall.Run.future");
    if (story_controller_impl_->story_shell_ &&
        module_data_.module_source ==
            fuchsia::modular::ModuleSource::EXTERNAL) {
      story_controller_impl_->story_shell_->DefocusSurface(
          ModulePathToSurfaceID(module_data_.module_path), future->Completer());
    } else {
      future->Complete();
    }

    future->Then([this, flow] {
      // Teardown the module, which discards the module controller. Since
      // multiple KillModuleCall operations can be queued by module data
      // updates, we must check whether the module has already been killed.
      auto* const running_mod_info =
          story_controller_impl_->FindRunningModInfo(module_data_.module_path);
      if (!running_mod_info) {
        FXL_LOG(INFO) << "No ModuleController for Module '"
                      << ModulePathToSurfaceID(module_data_.module_path)
                      << "'. Was ModuleController.Stop() called twice?";
        InvokeDone();
        return;
      }

      // The result callback |done_| must be invoked BEFORE the Teardown()
      // callback returns, just in case it is, or it invokes, a callback of a
      // FIDL method on ModuleController (happens in the case that this
      // Operation instance executes a ModuleController.Stop() FIDL method
      // invocation).
      //
      // After the Teardown() callback returns, the ModuleControllerImpl is
      // deleted, and any FIDL connections that have invoked methods on it are
      // closed.
      //
      // Be aware that done_ is NOT the Done() callback of the Operation.
      running_mod_info->module_controller_impl->Teardown(
          [this, flow] { InvokeDone(); });
    });
  }

  void InvokeDone() {
    // Whatever the done_ callback captures (specifically, a flow token) must be
    // released after the done_ callback has returned. Otherwise, the calling
    // operation will not call Done() and does not get deleted until this
    // Operation instance gets deleted. This is probably fine, but it's
    // different from calling operations without flow tokens, which call their
    // own Done() directly.
    //
    // Notice the StopCall doesn't use a flow token, but just calls Done()
    // directly from within done_, but the OnModuleDataUpadatedCall has a flow
    // token.

    // We must guard against the possibility that done_() causes this to be
    // deleted (happens when called from StopCall).
    auto weak_this = GetWeakPtr();

    done_();

    if (weak_this) {
      done_ = nullptr;
    }
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  fuchsia::modular::ModuleData module_data_;
  std::function<void()> done_;

  FXL_DISALLOW_COPY_AND_ASSIGN(KillModuleCall);
};

// Calls LaunchModuleCall to get a running instance, and delegates visual
// composition to the story shell.
class StoryControllerImpl::LaunchModuleInShellCall : public Operation<> {
 public:
  LaunchModuleInShellCall(
      StoryControllerImpl* const story_controller_impl,
      fuchsia::modular::ModuleData module_data,
      fidl::InterfaceRequest<fuchsia::modular::ModuleController>
          module_controller_request,
      ResultCall result_call)
      : Operation("StoryControllerImpl::LaunchModuleInShellCall",
                  std::move(result_call), module_data.module_url),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        module_controller_request_(std::move(module_controller_request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // TODO(mesch): The LaunchModuleCall may result in just a new
    // fuchsia::modular::ModuleController connection to an existing
    // ModuleControllerImpl. In that case, the view owner request is
    // closed, and the view owner should not be sent to the story
    // shell.
    operation_queue_.Add(new LaunchModuleCall(
        story_controller_impl_, fidl::Clone(module_data_),
        std::move(module_controller_request_), view_owner_.NewRequest(),
        [this, flow] { Cont(flow); }));
  }

  void Cont(FlowToken flow) {
    // If this is called during Stop(), story_shell_ might already have been
    // reset. TODO(mesch): Then the whole operation should fail.
    if (!story_controller_impl_->story_shell_) {
      return;
    }

    // We only add a module to story shell if its either a root module or its
    // anchor is already known to story shell.

    if (module_data_.module_path->size() == 1) {
      ConnectView(flow, "");
      return;
    }

    auto* const running_mod_info =
        story_controller_impl_->FindRunningModInfo(module_data_.module_path);
    FXL_CHECK(running_mod_info);  // Was just created.

    auto* const anchor = story_controller_impl_->FindAnchor(running_mod_info);
    if (anchor) {
      const auto anchor_surface_id =
          ModulePathToSurfaceID(anchor->module_data->module_path);
      if (story_controller_impl_->connected_views_.count(anchor_surface_id)) {
        ConnectView(flow, anchor_surface_id);
        return;
      }
    }

    auto manifest_clone = fuchsia::modular::ModuleManifest::New();
    fidl::Clone(module_data_.module_manifest, &manifest_clone);
    auto surface_relation_clone = fuchsia::modular::SurfaceRelation::New();
    module_data_.surface_relation->Clone(surface_relation_clone.get());
    story_controller_impl_->pending_views_.emplace(
        ModulePathToSurfaceID(module_data_.module_path),
        PendingView{module_data_.module_path.Clone(), std::move(manifest_clone),
                    std::move(surface_relation_clone),
                    module_data_.module_source, std::move(view_owner_)});
  }

  void ConnectView(FlowToken flow, fidl::StringPtr anchor_surface_id) {
    const auto surface_id = ModulePathToSurfaceID(module_data_.module_path);

    fuchsia::modular::ViewConnection view_connection;
    view_connection.surface_id = surface_id;
    view_connection.owner = std::move(view_owner_);

    fuchsia::modular::SurfaceInfo surface_info;
    surface_info.parent_id = anchor_surface_id;
    surface_info.surface_relation = std::move(module_data_.surface_relation);
    surface_info.module_manifest = std::move(module_data_.module_manifest);
    surface_info.module_source = std::move(module_data_.module_source);
    story_controller_impl_->story_shell_->AddSurface(std::move(view_connection),
                                                     std::move(surface_info));

    story_controller_impl_->connected_views_.emplace(surface_id);
    story_controller_impl_->ProcessPendingViews();
    story_controller_impl_->story_shell_->FocusSurface(surface_id);
  }

  StoryControllerImpl* const story_controller_impl_;
  fuchsia::modular::ModuleData module_data_;
  fidl::InterfaceRequest<fuchsia::modular::ModuleController>
      module_controller_request_;

  fuchsia::modular::ModuleControllerPtr module_controller_;
  fuchsia::ui::viewsv1token::ViewOwnerPtr view_owner_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchModuleInShellCall);
};

class StoryControllerImpl::StopCall : public Operation<> {
 public:
  StopCall(StoryControllerImpl* const story_controller_impl, const bool bulk,
           std::function<void()> done)
      : Operation("StoryControllerImpl::StopCall", done),
        story_controller_impl_(story_controller_impl),
        bulk_(bulk) {}

 private:
  void Run() override {
    if (!story_controller_impl_->IsRunning()) {
      Done();
      return;
    }

    story_controller_impl_->SetRuntimeState(
        fuchsia::modular::StoryState::STOPPING);

    // If this StopCall is part of a bulk operation of story provider that stops
    // all stories at once, no DetachView() notification is given to the session
    // shell.
    if (bulk_) {
      StopStory();
      return;
    }

    // Invocation of DetachView() follows below.
    //
    // The following callback is scheduled twice, once as response from
    // DetachView(), and again as a timeout.
    //
    // The shared bool did_run keeps track of the number of invocations, and
    // allows to suppress the second one.
    //
    // The weak pointer is needed because the method invocation would not be
    // cancelled when the OperationQueue holding this Operation instance is
    // deleted, because the method is invoked on an instance outside of the
    // instance that owns the OperationQueue that holds this Operation instance.
    //
    // The argument from_timeout informs whether the invocation was from the
    // timeout or from the method callback. It's used only to log diagnostics.
    // If we would not want to have this diagnostic, we could use this callback
    // directly to schedule it twice. In that case, it would be important that
    // passing it to DetachView() has copy semantics, as it does now passing it
    // to the capture list of two separate lambdas, and not move semantics, i.e.
    // that the argument is of DetachView() is NOT a fit::function but a
    // std::function.
    //
    // Both weak and shared pointers are copyable, so we don't need to do
    // anything to make this lambda copyable. (But it will be copied when used
    // below.)
    auto cont = [this, weak_this = GetWeakPtr(),
                 did_run = std::make_shared<bool>(false),
                 story_id = story_controller_impl_->story_id_](
                    const bool from_timeout) {
      if (*did_run) {
        return;
      }

      *did_run = true;

      if (from_timeout) {
        FXL_LOG(INFO) << "DetachView() timed out: story_id=" << story_id;
      }

      if (weak_this) {
        StopStory();
      }
    };

    story_controller_impl_->DetachView([cont] { cont(false); });

    async::PostDelayedTask(
        async_get_default_dispatcher(), [cont] { cont(true); }, kBasicTimeout);
  }

  void StopStory() {
    std::vector<FuturePtr<>> did_teardowns;
    did_teardowns.reserve(story_controller_impl_->running_mod_infos_.size());

    // Tear down all connections with a fuchsia::modular::ModuleController
    // first, then the links between them.
    for (auto& running_mod_info : story_controller_impl_->running_mod_infos_) {
      auto did_teardown =
          Future<>::Create("StoryControllerImpl.StopCall.Run.did_teardown");
      running_mod_info.module_controller_impl->Teardown(
          did_teardown->Completer());
      did_teardowns.emplace_back(did_teardown);
    }

    Wait("StoryControllerImpl.StopCall.Run.Wait", did_teardowns)
        ->AsyncMap([this] {
          auto did_teardown = Future<>::Create(
              "StoryControllerImpl.StopCall.Run.did_teardown2");
          // If StopCall runs on a story that's not running, there is no story
          // shell.
          if (story_controller_impl_->story_shell_) {
            story_controller_impl_->story_shell_app_->Teardown(
                kBasicTimeout, did_teardown->Completer());
          } else {
            did_teardown->Complete();
          }

          return did_teardown;
        })
        ->AsyncMap([this] {
          story_controller_impl_->story_shell_app_.reset();
          story_controller_impl_->story_shell_.Unbind();

          // Ensure every story storage operation has completed.
          return story_controller_impl_->story_storage_->Sync();
        })
        ->Then([this] {
          // Clear the remaining links and connections in case there are some
          // left. At this point, no DisposeLink() calls can arrive anymore.
          story_controller_impl_->link_impls_.CloseAll();

          // There should be no ongoing activities since all the modules have
          // been destroyed at this point.
          FXL_DCHECK(story_controller_impl_->ongoing_activities_.size() == 0);

          story_controller_impl_->SetRuntimeState(
              fuchsia::modular::StoryState::STOPPED);

          story_controller_impl_->DestroyStoryEnvironment();

          Done();
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned

  // Whether this Stop operation is part of stopping all stories at once. In
  // that case, DetachView() is not called.
  const bool bulk_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StopCall);
};

class StoryControllerImpl::StopModuleCall : public Operation<> {
 public:
  StopModuleCall(StoryStorage* const story_storage,
                 const fidl::VectorPtr<fidl::StringPtr>& module_path,
                 const std::function<void()>& done)
      : Operation("StoryControllerImpl::StopModuleCall", done),
        story_storage_(story_storage),
        module_path_(module_path.Clone()) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // Mark this module as stopped, which is a global state shared between
    // machines to track when the module is explicitly stopped. The module will
    // stop when ledger notifies us back about the module state change,
    // see OnModuleDataUpdated().
    story_storage_->UpdateModuleData(
        module_path_, [flow](fuchsia::modular::ModuleDataPtr* module_data_ptr) {
          FXL_DCHECK(*module_data_ptr);
          (*module_data_ptr)->module_deleted = true;
        });
  }

  StoryStorage* const story_storage_;  // not owned
  const fidl::VectorPtr<fidl::StringPtr> module_path_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StopModuleCall);
};

class StoryControllerImpl::StopModuleAndStoryIfEmptyCall : public Operation<> {
 public:
  StopModuleAndStoryIfEmptyCall(
      StoryControllerImpl* const story_controller_impl,
      const fidl::VectorPtr<fidl::StringPtr>& module_path,
      const std::function<void()>& done)
      : Operation("StoryControllerImpl::StopModuleAndStoryIfEmptyCall", done),
        story_controller_impl_(story_controller_impl),
        module_path_(module_path.Clone()) {}

 private:
  void Run() override {
    FlowToken flow{this};
    // If this is the last module in the story, stop the whole story instead
    // (which will cause this mod to be stopped also).
    auto* const running_mod_info =
        story_controller_impl_->FindRunningModInfo(module_path_);
    if (running_mod_info &&
        story_controller_impl_->running_mod_infos_.size() == 1) {
      operation_queue_.Add(
          new StopCall(story_controller_impl_, false /* bulk */, [flow] {}));
    } else {
      // Otherwise, stop this one module.
      operation_queue_.Add(new StopModuleCall(
          story_controller_impl_->story_storage_, module_path_, [flow] {}));
    }
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::VectorPtr<fidl::StringPtr> module_path_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StopModuleAndStoryIfEmptyCall);
};

class StoryControllerImpl::OnModuleDataUpdatedCall : public Operation<> {
 public:
  OnModuleDataUpdatedCall(StoryControllerImpl* const story_controller_impl,
                          fuchsia::modular::ModuleData module_data)
      : Operation("StoryControllerImpl::LedgerNotificationCall", [] {}),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)) {}

 private:
  void Run() override {
    FlowToken flow{this};
    if (!story_controller_impl_->IsRunning()) {
      return;
    }

    // Check for existing module at the given path.
    auto* const running_mod_info =
        story_controller_impl_->FindRunningModInfo(module_data_.module_path);
    if (module_data_.module_deleted) {
      // If the module is running, kill it.
      if (running_mod_info) {
        operation_queue_.Add(new KillModuleCall(
            story_controller_impl_, std::move(module_data_), [flow] {}));
      }
      return;
    }

    // We do not auto-start Modules that were added through ModuleContext on
    // other devices.
    //
    // TODO(thatguy): Revisit this decision. It seems wrong: we do not want
    // to auto-start mods added through ModuleContext.EmbedModule(), because
    // we do not have the necessary capabilities (the ViewOwner). However,
    // mods added through ModuleContext.AddModuleToStory() can be started
    // automatically.
    if (module_data_.module_source !=
        fuchsia::modular::ModuleSource::EXTERNAL) {
      return;
    }

    // We reach this point only if we want to start or update an existing
    // external module.
    operation_queue_.Add(new LaunchModuleInShellCall(
        story_controller_impl_, std::move(module_data_),
        nullptr /* module_controller_request */, [flow] {}));
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  fuchsia::modular::ModuleData module_data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(OnModuleDataUpdatedCall);
};

class StoryControllerImpl::FocusCall : public Operation<> {
 public:
  FocusCall(StoryControllerImpl* const story_controller_impl,
            fidl::VectorPtr<fidl::StringPtr> module_path)
      : Operation("StoryControllerImpl::FocusCall", [] {}),
        story_controller_impl_(story_controller_impl),
        module_path_(std::move(module_path)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    if (!story_controller_impl_->story_shell_) {
      return;
    }

    story_controller_impl_->story_shell_->FocusSurface(
        ModulePathToSurfaceID(module_path_));
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::VectorPtr<fidl::StringPtr> module_path_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FocusCall);
};

class StoryControllerImpl::DefocusCall : public Operation<> {
 public:
  DefocusCall(StoryControllerImpl* const story_controller_impl,
              fidl::VectorPtr<fidl::StringPtr> module_path)
      : Operation("StoryControllerImpl::DefocusCall", [] {}),
        story_controller_impl_(story_controller_impl),
        module_path_(std::move(module_path)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    if (!story_controller_impl_->story_shell_) {
      return;
    }

    // NOTE(mesch): We don't wait for defocus to return. TODO(mesch): What is
    // the return callback good for anyway?
    story_controller_impl_->story_shell_->DefocusSurface(
        ModulePathToSurfaceID(module_path_), [] {});
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::VectorPtr<fidl::StringPtr> module_path_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DefocusCall);
};

// An operation that first performs module resolution with the provided
// fuchsia::modular::Intent and subsequently starts the most appropriate
// resolved module in the story shell.
class StoryControllerImpl::AddIntentCall
    : public Operation<fuchsia::modular::StartModuleStatus> {
 public:
  AddIntentCall(StoryControllerImpl* const story_controller_impl,
                fidl::VectorPtr<fidl::StringPtr> requesting_module_path,
                const std::string& module_name,
                fuchsia::modular::IntentPtr intent,
                fidl::InterfaceRequest<fuchsia::modular::ModuleController>
                    module_controller_request,
                fuchsia::modular::SurfaceRelationPtr surface_relation,
                fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
                    view_owner_request,
                const fuchsia::modular::ModuleSource module_source,
                ResultCall result_call)
      : Operation("StoryControllerImpl::AddIntentCall", std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        requesting_module_path_(std::move(requesting_module_path)),
        module_name_(module_name),
        intent_(std::move(intent)),
        module_controller_request_(std::move(module_controller_request)),
        surface_relation_(std::move(surface_relation)),
        view_owner_request_(std::move(view_owner_request)),
        module_source_(module_source) {}

 private:
  void Run() {
    FlowToken flow{this, &start_module_status_};
    AddAddModOperation(
        &operation_queue_, story_controller_impl_->story_storage_,
        story_controller_impl_->story_provider_impl_->module_resolver(),
        story_controller_impl_->story_provider_impl_->entity_resolver(),
        story_controller_impl_->story_provider_impl_->module_facet_reader(),
        fidl::VectorPtr<fidl::StringPtr>({module_name_}), std::move(*intent_),
        std::move(surface_relation_), std::move(requesting_module_path_),
        module_source_,
        [this, flow](fuchsia::modular::ExecuteResult result,
                     fuchsia::modular::ModuleData module_data) {
          if (result.status ==
              fuchsia::modular::ExecuteStatus::NO_MODULES_FOUND) {
            start_module_status_ =
                fuchsia::modular::StartModuleStatus::NO_MODULES_FOUND;
            return;
          }
          if (result.status != fuchsia::modular::ExecuteStatus::OK) {
            FXL_LOG(WARNING)
                << "StoryController::AddIntentCall::AddModCall returned "
                << "error response with message: " << result.error_message;
          }
          module_data_ = std::move(module_data);
          MaybeLaunchModule(flow);
        });
  }

  void MaybeLaunchModule(FlowToken flow) {
    if (story_controller_impl_->IsRunning()) {
      // TODO(thatguy): Should we be checking surface_relation also?
      if (!view_owner_request_) {
        operation_queue_.Add(new LaunchModuleInShellCall(
            story_controller_impl_, std::move(module_data_),
            std::move(module_controller_request_), [flow] {}));
      } else {
        operation_queue_.Add(new LaunchModuleCall(
            story_controller_impl_, std::move(module_data_),
            std::move(module_controller_request_),
            std::move(view_owner_request_), [this, flow] {
              // LaunchModuleInShellCall above already calls
              // ProcessPendingViews(). NOTE(thatguy): This
              // cannot be moved into LaunchModuleCall, because
              // LaunchModuleInShellCall uses LaunchModuleCall
              // as the very first step of its operation. This
              // would inform the story shell of a new module
              // before we had told it about its
              // surface-relation parent (which we do as the
              // second part of LaunchModuleInShellCall).  So
              // we must defer to here.
              story_controller_impl_->ProcessPendingViews();
            }));
      }
    }

    start_module_status_ = fuchsia::modular::StartModuleStatus::SUCCESS;
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;

  // Arguments passed in from the constructor. Some are used to initialize
  // module_data_ in AddModuleFromResult().
  fidl::VectorPtr<fidl::StringPtr> requesting_module_path_;
  const std::string module_name_;
  fuchsia::modular::IntentPtr intent_;
  fidl::InterfaceRequest<fuchsia::modular::ModuleController>
      module_controller_request_;
  fuchsia::modular::SurfaceRelationPtr surface_relation_;
  fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
      view_owner_request_;
  const fuchsia::modular::ModuleSource module_source_;

  // Created by AddModuleFromResult, and ultimately written to story state.
  fuchsia::modular::ModuleData module_data_;

  fuchsia::modular::StartModuleStatus start_module_status_{
      fuchsia::modular::StartModuleStatus::NO_MODULES_FOUND};

  FXL_DISALLOW_COPY_AND_ASSIGN(AddIntentCall);
};

class StoryControllerImpl::StartContainerInShellCall : public Operation<> {
 public:
  StartContainerInShellCall(
      StoryControllerImpl* const story_controller_impl,
      fidl::VectorPtr<fidl::StringPtr> parent_module_path,
      fidl::StringPtr container_name,
      fuchsia::modular::SurfaceRelationPtr parent_relation,
      fidl::VectorPtr<fuchsia::modular::ContainerLayout> layout,
      fidl::VectorPtr<fuchsia::modular::ContainerRelationEntry> relationships,
      fidl::VectorPtr<fuchsia::modular::ContainerNodePtr> nodes)
      : Operation("StoryControllerImpl::StartContainerInShellCall", [] {}),
        story_controller_impl_(story_controller_impl),
        parent_module_path_(std::move(parent_module_path)),
        container_name_(container_name),
        parent_relation_(std::move(parent_relation)),
        layout_(std::move(layout)),
        relationships_(std::move(relationships)),
        nodes_(std::move(nodes)) {
    for (auto& relationship : *relationships_) {
      relation_map_[relationship.node_name] = CloneOptional(relationship);
    }
  }

 private:
  void Run() override {
    FlowToken flow{this};
    // parent + container used as module path of requesting module for
    // containers
    fidl::VectorPtr<fidl::StringPtr> module_path = parent_module_path_.Clone();
    // module_path.push_back(container_name_);
    // Adding non-module 'container_name_' to the module path results in
    // Ledger Client issuing a ReadData() call and failing with a fatal error
    // when module_data cannot be found
    // TODO(djmurphy): follow up, probably make containers modules
    std::vector<FuturePtr<fuchsia::modular::StartModuleStatus>> did_add_intents;
    did_add_intents.reserve(nodes_->size());

    for (size_t i = 0; i < nodes_->size(); ++i) {
      auto did_add_intent = Future<fuchsia::modular::StartModuleStatus>::Create(
          "StoryControllerImpl.StartContainerInShellCall.Run.did_add_intent");
      auto intent = fuchsia::modular::Intent::New();
      nodes_->at(i)->intent.Clone(intent.get());
      operation_queue_.Add(new AddIntentCall(
          story_controller_impl_, parent_module_path_.Clone(),
          nodes_->at(i)->node_name, std::move(intent),
          nullptr /* module_controller_request */,
          fidl::MakeOptional(
              relation_map_[nodes_->at(i)->node_name]->relationship),
          nullptr /* view_owner_request */,
          fuchsia::modular::ModuleSource::INTERNAL,
          did_add_intent->Completer()));

      did_add_intents.emplace_back(did_add_intent);
    }

    Wait<Future<>>("StoryControllerImpl.StartContainerInShellCall.Run.Wait",
                   did_add_intents)
        ->Then([this, flow] {
          if (!story_controller_impl_->story_shell_) {
            return;
          }
          auto views = fidl::VectorPtr<fuchsia::modular::ContainerView>::New(
              nodes_->size());
          for (size_t i = 0; i < nodes_->size(); i++) {
            fuchsia::modular::ContainerView view;
            view.node_name = nodes_->at(i)->node_name;
            view.owner = std::move(node_views_[nodes_->at(i)->node_name]);
            views->at(i) = std::move(view);
          }
          story_controller_impl_->story_shell_->AddContainer(
              container_name_, ModulePathToSurfaceID(parent_module_path_),
              std::move(*parent_relation_), std::move(layout_),
              std::move(relationships_), std::move(views));
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  OperationQueue operation_queue_;
  const fidl::VectorPtr<fidl::StringPtr> parent_module_path_;
  const fidl::StringPtr container_name_;

  fuchsia::modular::SurfaceRelationPtr parent_relation_;
  fidl::VectorPtr<fuchsia::modular::ContainerLayout> layout_;
  fidl::VectorPtr<fuchsia::modular::ContainerRelationEntry> relationships_;
  const fidl::VectorPtr<fuchsia::modular::ContainerNodePtr> nodes_;
  std::map<std::string, fuchsia::modular::ContainerRelationEntryPtr>
      relation_map_;

  // map of node_name to view_owners
  std::map<fidl::StringPtr, fuchsia::ui::viewsv1token::ViewOwnerPtr>
      node_views_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartContainerInShellCall);
};

class StoryControllerImpl::StartCall : public Operation<> {
 public:
  StartCall(
      StoryControllerImpl* const story_controller_impl,
      StoryStorage* const storage,
      fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request)
      : Operation("StoryControllerImpl::StartCall", [] {}),
        story_controller_impl_(story_controller_impl),
        storage_(storage),
        request_(std::move(request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // If the story is running, we do nothing.
    if (story_controller_impl_->IsRunning()) {
      FXL_LOG(INFO)
          << "StoryControllerImpl::StartCall() while already running: ignored.";
      return;
    }

    story_controller_impl_->StartStoryShell(std::move(request_));

    // Start all modules that were not themselves explicitly started by another
    // module.
    storage_->ReadAllModuleData()->Then(
        [this, flow](fidl::VectorPtr<fuchsia::modular::ModuleData> data) {
          story_controller_impl_->InitStoryEnvironment();

          for (auto& module_data : *data) {
            if (module_data.module_deleted) {
              continue;
            }
            FXL_CHECK(module_data.intent);
            operation_queue_.Add(new LaunchModuleInShellCall(
                story_controller_impl_, std::move(module_data),
                nullptr /* module_controller_request */, [flow] {}));
          }

          story_controller_impl_->SetRuntimeState(
              fuchsia::modular::StoryState::RUNNING);
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  StoryStorage* const storage_;                       // not owned
  fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartCall);
};

class StoryControllerImpl::UpdateSnapshotCall : public Operation<> {
 public:
  UpdateSnapshotCall(StoryControllerImpl* const story_controller_impl,
                     std::function<void()> done)
      : Operation("StoryControllerImpl::UpdateSnapshotCall", done),
        story_controller_impl_(story_controller_impl) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // If the story shell is not running, we avoid updating the snapshot.
    if (!story_controller_impl_->IsRunning()) {
      FXL_LOG(INFO) << "StoryControllerImpl::UpdateSnapshotCall() called when "
                       "story shell is not initialized.";
      return;
    }

    FlowTokenHolder branch{flow};
    // |flow| will branch into normal and timeout paths. |flow| must go out of
    // scope when either of the paths finishes. We pass a weak ptr of
    // story_controller_impl to the callback in case the operation goes out of
    // scope from timeout.
    story_controller_impl_->story_provider_impl_->TakeSnapshot(
        story_controller_impl_->story_id_,
        [weak_ptr = story_controller_impl_->weak_factory_.GetWeakPtr(),
         branch](fuchsia::mem::Buffer snapshot) {
          if (!weak_ptr) {
            return;
          }

          if (snapshot.size == 0) {
            FXL_LOG(INFO)
                << "TakeSnapshot returned an invalid snapshot for story: "
                << weak_ptr->story_id_;
            return;
          }

          // Even if the snapshot comes back after timeout, we attempt to
          // process it by loading the snapshot and saving it to storage. This
          // call assumes that the snapshot loader has already been connected.
          if (!weak_ptr->snapshot_loader_.is_bound()) {
            FXL_LOG(ERROR) << "UpdateSnapshotCall called when snapshot loader "
                              "has not been connected for story: "
                           << weak_ptr->story_id_;
          } else {
            fuchsia::mem::Buffer snapshot_copy;
            snapshot.Clone(&snapshot_copy);
            weak_ptr->snapshot_loader_->Load(std::move(snapshot_copy));
          }

          weak_ptr->session_storage_
              ->WriteSnapshot(weak_ptr->story_id_, std::move(snapshot))
              ->Then([weak_ptr, branch]() {
                auto flow = branch.Continue();
                if (!flow) {
                  FXL_LOG(INFO) << "Saved snapshot for story after timeout: "
                                << weak_ptr->story_id_;
                } else {
                  FXL_LOG(INFO)
                      << "Saved snapshot for story: " << weak_ptr->story_id_;
                }
              });
        });

    async::PostDelayedTask(
        async_get_default_dispatcher(),
        [this, branch] {
          auto flow = branch.Continue();
          if (flow) {
            FXL_LOG(INFO) << "Timed out while updating snapshot for story: "
                          << story_controller_impl_->story_id_;
          }
        },
        kUpdateSnapshotTimeout);
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned

  FXL_DISALLOW_COPY_AND_ASSIGN(UpdateSnapshotCall);
};

class StoryControllerImpl::StartSnapshotLoaderCall : public Operation<> {
 public:
  StartSnapshotLoaderCall(
      StoryControllerImpl* const story_controller_impl,
      fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request)
      : Operation("StoryControllerImpl::StartSnapshotLoaderCall", [] {}),
        story_controller_impl_(story_controller_impl),
        request_(std::move(request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    story_controller_impl_->story_provider_impl_->StartSnapshotLoader(
        std::move(request_),
        story_controller_impl_->snapshot_loader_.NewRequest());

    story_controller_impl_->session_storage_
        ->ReadSnapshot(story_controller_impl_->story_id_)
        ->Then([this, flow](fuchsia::mem::BufferPtr snapshot) {
          if (!snapshot) {
            FXL_LOG(INFO)
                << "ReadSnapshot returned a null/invalid snapshot for story: "
                << story_controller_impl_->story_id_;
            return;
          }

          story_controller_impl_->snapshot_loader_->Load(
              std::move(*snapshot.get()));
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartSnapshotLoaderCall);
};

StoryControllerImpl::StoryControllerImpl(
    SessionStorage* const session_storage, StoryStorage* const story_storage,
    std::unique_ptr<StoryMutator> story_mutator,
    std::unique_ptr<StoryObserver> story_observer,
    StoryVisibilitySystem* const story_visibility_system,
    StoryProviderImpl* const story_provider_impl)
    : story_id_(*story_observer->model().name()),
      story_provider_impl_(story_provider_impl),
      session_storage_(session_storage),
      story_storage_(story_storage),
      story_mutator_(std::move(story_mutator)),
      story_observer_(std::move(story_observer)),
      story_visibility_system_(story_visibility_system),
      story_shell_context_impl_{story_id_, story_provider_impl, this},
      weak_factory_(this) {
  auto story_scope = fuchsia::modular::StoryScope::New();
  story_scope->story_id = story_id_;
  auto scope = fuchsia::modular::ComponentScope::New();
  scope->set_story_scope(std::move(*story_scope));
  story_provider_impl_->user_intelligence_provider()
      ->GetComponentIntelligenceServices(std::move(*scope),
                                         intelligence_services_.NewRequest());
  story_storage_->set_on_module_data_updated(
      [this](fuchsia::modular::ModuleData module_data) {
        OnModuleDataUpdated(std::move(module_data));
      });

  story_observer_->RegisterListener(
      [this](const fuchsia::modular::storymodel::StoryModel& model) {
        NotifyStoryWatchers(model);
      });
}

StoryControllerImpl::~StoryControllerImpl() = default;

void StoryControllerImpl::Connect(
    fidl::InterfaceRequest<fuchsia::modular::StoryController> request) {
  bindings_.AddBinding(this, std::move(request));
}

bool StoryControllerImpl::IsRunning() {
  switch (*story_observer_->model().runtime_state()) {
    case fuchsia::modular::StoryState::RUNNING:
      return true;
    case fuchsia::modular::StoryState::STOPPING:
    case fuchsia::modular::StoryState::STOPPED:
      return false;
  }
}

fidl::VectorPtr<fuchsia::modular::OngoingActivityType>
StoryControllerImpl::GetOngoingActivities() {
  fidl::VectorPtr<fuchsia::modular::OngoingActivityType> ongoing_activities;
  ongoing_activities.resize(0);
  for (auto& entry : ongoing_activities_.bindings()) {
    ongoing_activities.push_back(entry->impl()->GetType());
  }

  return ongoing_activities;
}

void StoryControllerImpl::Sync(const std::function<void()>& done) {
  operation_queue_.Add(new SyncCall(done));
}

void StoryControllerImpl::FocusModule(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  operation_queue_.Add(new FocusCall(this, module_path.Clone()));
}

void StoryControllerImpl::DefocusModule(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  operation_queue_.Add(new DefocusCall(this, module_path.Clone()));
}

void StoryControllerImpl::StopModule(
    const fidl::VectorPtr<fidl::StringPtr>& module_path,
    const std::function<void()>& done) {
  operation_queue_.Add(new StopModuleCall(story_storage_, module_path, done));
}

void StoryControllerImpl::ReleaseModule(
    ModuleControllerImpl* const module_controller_impl) {
  auto fit = std::find_if(running_mod_infos_.begin(), running_mod_infos_.end(),
                          [module_controller_impl](const RunningModInfo& c) {
                            return c.module_controller_impl.get() ==
                                   module_controller_impl;
                          });
  FXL_DCHECK(fit != running_mod_infos_.end());
  fit->module_controller_impl.release();
  pending_views_.erase(ModulePathToSurfaceID(fit->module_data->module_path));
  running_mod_infos_.erase(fit);
}

fidl::StringPtr StoryControllerImpl::GetStoryId() const {
  return *story_observer_->model().name();
}

void StoryControllerImpl::RequestStoryFocus() {
  story_provider_impl_->RequestStoryFocus(story_id_);
}

// TODO(drees) Collapse functionality into GetLink.
void StoryControllerImpl::ConnectLinkPath(
    fuchsia::modular::LinkPathPtr link_path,
    fidl::InterfaceRequest<fuchsia::modular::Link> request) {
  // Cache a copy of the current active links, because link_impls_.AddBinding()
  // will change the set to include the newly created link connection.
  auto active_links = GetActiveLinksInternal();

  LinkPath link_path_clone;
  link_path->Clone(&link_path_clone);
  link_impls_.AddBinding(
      std::make_unique<LinkImpl>(story_storage_, std::move(link_path_clone)),
      std::move(request));

  // TODO: remove this. MI4-1084
  if (active_links.count(*link_path) == 0) {
    // This is a new link: notify watchers.
    for (auto& i : links_watchers_.ptrs()) {
      LinkPath link_path_clone;
      link_path->Clone(&link_path_clone);
      (*i)->OnNewLink(std::move(link_path_clone));
    }
  }
}

fuchsia::modular::LinkPathPtr StoryControllerImpl::GetLinkPathForParameterName(
    const fidl::VectorPtr<fidl::StringPtr>& module_path, fidl::StringPtr name) {
  auto mod_info = FindRunningModInfo(module_path);
  // NOTE: |mod_info| will only be valid if the module at |module_path| is
  // running. Strictly speaking, this is unsafe. The source of truth is the
  // Ledger, accessible through StoryStorage, but the call would be dispatcher,
  // which would change the flow of all clients of this method. For now, we
  // leave as-is.
  FXL_DCHECK(mod_info) << ModulePathToSurfaceID(module_path);

  const auto& param_map = mod_info->module_data->parameter_map;
  auto it = std::find_if(
      param_map.entries->begin(), param_map.entries->end(),
      [&name](const fuchsia::modular::ModuleParameterMapEntry& data) {
        return data.name == name;
      });

  fuchsia::modular::LinkPathPtr link_path = nullptr;
  if (it != param_map.entries->end()) {
    link_path = CloneOptional(it->link_path);
  }

  if (!link_path) {
    link_path = fuchsia::modular::LinkPath::New();
    link_path->module_path = module_path.Clone();
    link_path->link_name = name;
  }

  return link_path;
}

void StoryControllerImpl::EmbedModule(
    const fidl::VectorPtr<fidl::StringPtr>& parent_module_path,
    fidl::StringPtr module_name, fuchsia::modular::IntentPtr intent,
    fidl::InterfaceRequest<fuchsia::modular::ModuleController>
        module_controller_request,
    fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner>
        view_owner_request,
    fuchsia::modular::ModuleSource module_source,
    std::function<void(fuchsia::modular::StartModuleStatus)> callback) {
  operation_queue_.Add(new AddIntentCall(
      this, parent_module_path.Clone(), module_name, std::move(intent),
      std::move(module_controller_request), nullptr /* surface_relation */,
      std::move(view_owner_request), std::move(module_source),
      std::move(callback)));
}

void StoryControllerImpl::StartModule(
    const fidl::VectorPtr<fidl::StringPtr>& parent_module_path,
    fidl::StringPtr module_name, fuchsia::modular::IntentPtr intent,
    fidl::InterfaceRequest<fuchsia::modular::ModuleController>
        module_controller_request,
    fuchsia::modular::SurfaceRelationPtr surface_relation,
    fuchsia::modular::ModuleSource module_source,
    std::function<void(fuchsia::modular::StartModuleStatus)> callback) {
  operation_queue_.Add(new AddIntentCall(
      this, parent_module_path.Clone(), module_name, std::move(intent),
      std::move(module_controller_request), std::move(surface_relation),
      nullptr /* view_owner_request */, std::move(module_source),
      std::move(callback)));
}

void StoryControllerImpl::StartContainerInShell(
    const fidl::VectorPtr<fidl::StringPtr>& parent_module_path,
    fidl::StringPtr name, fuchsia::modular::SurfaceRelationPtr parent_relation,
    fidl::VectorPtr<fuchsia::modular::ContainerLayout> layout,
    fidl::VectorPtr<fuchsia::modular::ContainerRelationEntry> relationships,
    fidl::VectorPtr<fuchsia::modular::ContainerNodePtr> nodes) {
  operation_queue_.Add(new StartContainerInShellCall(
      this, parent_module_path.Clone(), name, std::move(parent_relation),
      std::move(layout), std::move(relationships), std::move(nodes)));
}

void StoryControllerImpl::ProcessPendingViews() {
  // NOTE(mesch): As it stands, this machinery to send modules in traversal
  // order to the story shell is N^3 over the lifetime of the story, where N
  // is the number of modules. This function is N^2, and it's called once for
  // each of the N modules. However, N is small, and moreover its scale is
  // limited my much more severe constraints. Eventually, we will address this
  // by changing story shell to be able to accomodate modules out of traversal
  // order.
  if (!story_shell_) {
    return;
  }

  std::vector<fidl::StringPtr> added_keys;

  for (auto& kv : pending_views_) {
    auto* const running_mod_info = FindRunningModInfo(kv.second.module_path);
    if (!running_mod_info) {
      continue;
    }

    auto* const anchor = FindAnchor(running_mod_info);
    if (!anchor) {
      continue;
    }

    const auto anchor_surface_id =
        ModulePathToSurfaceID(anchor->module_data->module_path);
    if (!connected_views_.count(anchor_surface_id)) {
      continue;
    }

    const auto surface_id = ModulePathToSurfaceID(kv.second.module_path);
    fuchsia::modular::ViewConnection view_connection;
    view_connection.surface_id = surface_id;
    view_connection.owner = std::move(kv.second.view_owner);
    fuchsia::modular::SurfaceInfo surface_info;
    surface_info.parent_id = anchor_surface_id;
    surface_info.surface_relation = std::move(kv.second.surface_relation);
    surface_info.module_manifest = std::move(kv.second.module_manifest);
    surface_info.module_source = std::move(kv.second.module_source);
    story_shell_->AddSurface(std::move(view_connection),
                             std::move(surface_info));
    connected_views_.emplace(surface_id);

    added_keys.push_back(kv.first);
  }

  if (added_keys.size()) {
    for (auto& key : added_keys) {
      pending_views_.erase(key);
    }
    ProcessPendingViews();
  }
}

std::set<fuchsia::modular::LinkPath>
StoryControllerImpl::GetActiveLinksInternal() {
  std::set<fuchsia::modular::LinkPath> paths;
  for (auto& entry : link_impls_.bindings()) {
    LinkPath p;
    entry->impl()->link_path().Clone(&p);
    paths.insert(std::move(p));
  }
  return paths;
}

void StoryControllerImpl::OnModuleDataUpdated(
    fuchsia::modular::ModuleData module_data) {
  // Control reaching here means that this update came from a remote device.
  operation_queue_.Add(
      new OnModuleDataUpdatedCall(this, std::move(module_data)));
}

void StoryControllerImpl::GetInfo(GetInfoCallback callback) {
  // Synced such that if GetInfo() is called after Start() or Stop(), the
  // state after the previously invoked operation is returned.
  //
  // If this call enters a race with a StoryProvider.DeleteStory() call,
  // resulting in |this| being destroyed, |callback| will be dropped.
  operation_queue_.Add(new SyncCall([this, callback] {
    auto story_info = story_provider_impl_->GetCachedStoryInfo(story_id_);
    FXL_CHECK(story_info);
    callback(std::move(*story_info), *story_observer_->model().runtime_state());
  }));
}

void StoryControllerImpl::Start(
    fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request) {
  operation_queue_.Add(new StartCall(this, story_storage_, std::move(request)));
}

void StoryControllerImpl::RequestStart() {
  operation_queue_.Add(
      new StartCall(this, story_storage_, nullptr /* ViewOwner request */));
}

void StoryControllerImpl::Stop(StopCallback done) {
  operation_queue_.Add(new StopCall(this, false /* bulk */, done));
}

void StoryControllerImpl::StopBulk(const bool bulk, StopCallback done) {
  operation_queue_.Add(new StopCall(this, bulk, done));
}

void StoryControllerImpl::TakeAndLoadSnapshot(
    fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request,
    TakeAndLoadSnapshotCallback done) {
  // Currently we start a new snapshot view on every TakeAndLoadSnapshot
  // invocation. We can optimize later by connecting the snapshot loader on
  // start and re-using it for the lifetime of the story.
  operation_queue_.Add(new StartSnapshotLoaderCall(this, std::move(request)));
  operation_queue_.Add(new UpdateSnapshotCall(this, done));
}

void StoryControllerImpl::Watch(
    fidl::InterfaceHandle<fuchsia::modular::StoryWatcher> watcher) {
  auto ptr = watcher.Bind();
  NotifyOneStoryWatcher(story_observer_->model(), ptr.get());
  watchers_.AddInterfacePtr(std::move(ptr));
}

void StoryControllerImpl::GetActiveModules(GetActiveModulesCallback callback) {
  // We execute this in a SyncCall so that we are sure we don't fall in a
  // crack between a module being created and inserted in the connections
  // collection during some Operation.
  operation_queue_.Add(
      new SyncCall(fxl::MakeCopyable([this, callback]() mutable {
        fidl::VectorPtr<fuchsia::modular::ModuleData> result;
        result.resize(running_mod_infos_.size());
        for (size_t i = 0; i < running_mod_infos_.size(); i++) {
          running_mod_infos_[i].module_data->Clone(&result->at(i));
        }
        callback(std::move(result));
      })));
}

void StoryControllerImpl::GetModules(GetModulesCallback callback) {
  auto on_run = Future<>::Create("StoryControllerImpl.GetModules.on_run");
  auto done =
      on_run->AsyncMap([this] { return story_storage_->ReadAllModuleData(); });
  operation_queue_.Add(WrapFutureAsOperation(
      "StoryControllerImpl.GetModules.op", on_run, done, callback));
}

void StoryControllerImpl::GetModuleController(
    fidl::VectorPtr<fidl::StringPtr> module_path,
    fidl::InterfaceRequest<fuchsia::modular::ModuleController> request) {
  operation_queue_.Add(new SyncCall(
      fxl::MakeCopyable([this, module_path = std::move(module_path),
                         request = std::move(request)]() mutable {
        for (auto& running_mod_info : running_mod_infos_) {
          if (module_path == running_mod_info.module_data->module_path) {
            running_mod_info.module_controller_impl->Connect(
                std::move(request));
            return;
          }
        }

        // Trying to get a controller for a module that is not active just
        // drops the connection request.
      })));
}

void StoryControllerImpl::GetActiveLinks(
    fidl::InterfaceHandle<fuchsia::modular::StoryLinksWatcher> watcher,
    GetActiveLinksCallback callback) {
  auto result = fidl::VectorPtr<fuchsia::modular::LinkPath>::New(0);

  std::set<fuchsia::modular::LinkPath> active_links = GetActiveLinksInternal();
  for (auto& p : active_links) {
    LinkPath clone;
    p.Clone(&clone);
    result.push_back(std::move(clone));
  }

  if (watcher) {
    links_watchers_.AddInterfacePtr(watcher.Bind());
  }
  callback(std::move(result));
}

void StoryControllerImpl::GetLink(
    fuchsia::modular::LinkPath link_path,
    fidl::InterfaceRequest<fuchsia::modular::Link> request) {
  ConnectLinkPath(fidl::MakeOptional(std::move(link_path)), std::move(request));
}

void StoryControllerImpl::StartStoryShell(
    fidl::InterfaceRequest<fuchsia::ui::viewsv1token::ViewOwner> request) {
  if (!request) {
    // The start call originated in RequestStart() rather than Start().
    fuchsia::ui::viewsv1token::ViewOwnerPtr view_owner;
    request = view_owner.NewRequest();
    story_provider_impl_->AttachView(story_id_, std::move(view_owner));
    needs_detach_view_ = true;
  }

  story_shell_app_ =
      story_provider_impl_->StartStoryShell(story_id_, std::move(request));
  story_shell_app_->services().ConnectToService(story_shell_.NewRequest());
  fuchsia::modular::StoryShellContextPtr story_shell_context;
  story_shell_context_impl_.Connect(story_shell_context.NewRequest());
  story_shell_->Initialize(std::move(story_shell_context));
  story_shell_.events().OnSurfaceFocused =
      fit::bind_member(this, &StoryControllerImpl::OnSurfaceFocused);
}

void StoryControllerImpl::DetachView(std::function<void()> done) {
  if (needs_detach_view_) {
    story_provider_impl_->DetachView(story_id_, std::move(done));
    needs_detach_view_ = false;
  } else {
    done();
  }
}

void StoryControllerImpl::SetRuntimeState(
    const fuchsia::modular::StoryState new_state) {
  story_mutator_->set_runtime_state(new_state);
}

void StoryControllerImpl::NotifyStoryWatchers(
    const fuchsia::modular::storymodel::StoryModel& model) {
  for (auto& i : watchers_.ptrs()) {
    NotifyOneStoryWatcher(model, (*i).get());
  }
}

void StoryControllerImpl::NotifyOneStoryWatcher(
    const fuchsia::modular::storymodel::StoryModel& model,
    fuchsia::modular::StoryWatcher* watcher) {
  watcher->OnStateChange(*model.runtime_state());
}

bool StoryControllerImpl::IsExternalModule(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  auto* const i = FindRunningModInfo(module_path);
  if (!i) {
    return false;
  }

  return i->module_data->module_source ==
         fuchsia::modular::ModuleSource::EXTERNAL;
}

StoryControllerImpl::RunningModInfo* StoryControllerImpl::FindRunningModInfo(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  for (auto& c : running_mod_infos_) {
    if (c.module_data->module_path == module_path) {
      return &c;
    }
  }
  return nullptr;
}

StoryControllerImpl::RunningModInfo* StoryControllerImpl::FindAnchor(
    RunningModInfo* running_mod_info) {
  if (!running_mod_info) {
    return nullptr;
  }

  auto* anchor = FindRunningModInfo(
      ParentModulePath(running_mod_info->module_data->module_path));

  // Traverse up until there is a non-embedded module. We recognize
  // non-embedded modules by having a non-null SurfaceRelation. If the root
  // module is there at all, it has a non-null surface relation.
  while (anchor && !anchor->module_data->surface_relation) {
    anchor =
        FindRunningModInfo(ParentModulePath(anchor->module_data->module_path));
  }

  return anchor;
}

void StoryControllerImpl::RemoveModuleFromStory(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  operation_queue_.Add(
      new StopModuleAndStoryIfEmptyCall(this, module_path, [] {}));
}

void StoryControllerImpl::InitStoryEnvironment() {
  FXL_DCHECK(!story_environment_)
      << "Story scope already running for story_id = " << story_id_;

  static const auto* const kEnvServices =
      new std::vector<std::string>{fuchsia::modular::ContextWriter::Name_};
  story_environment_ = std::make_unique<Environment>(
      story_provider_impl_->user_environment(),
      kStoryEnvironmentLabelPrefix + story_id_.get(), *kEnvServices,
      /* kill_on_oom = */ false);
  story_environment_->AddService<fuchsia::modular::ContextWriter>(
      [this](fidl::InterfaceRequest<fuchsia::modular::ContextWriter> request) {
        intelligence_services_->GetContextWriter(std::move(request));
      });
}

void StoryControllerImpl::DestroyStoryEnvironment() {
  story_environment_.reset();
}

void StoryControllerImpl::StartOngoingActivity(
    const fuchsia::modular::OngoingActivityType ongoing_activity_type,
    fidl::InterfaceRequest<fuchsia::modular::OngoingActivity> request) {
  // Newly created/destroyed ongoing activities should be dispatched to the
  // story provider.
  auto dispatch_to_story_provider = [this] {
    story_provider_impl_->NotifyStoryActivityChange(story_id_,
                                                    GetOngoingActivities());
  };

  // When a connection is closed on the client-side, the OngoingActivityImpl is
  // destroyed after it is removed from the binding set, so we dispatch to the
  // story provider in the destructor of OngoingActivityImpl.
  ongoing_activities_.AddBinding(
      std::make_unique<OngoingActivityImpl>(
          ongoing_activity_type,
          /* on_destroy= */ dispatch_to_story_provider),
      std::move(request));

  // Conversely, when a connection is created, the OngoingActivityImpl is
  // initialized before added to the binding set, so we need to dispatch after
  // bind.
  dispatch_to_story_provider();
}

void StoryControllerImpl::CreateEntity(
    fidl::StringPtr type, fuchsia::mem::Buffer data,
    fidl::InterfaceRequest<fuchsia::modular::Entity> entity_request,
    std::function<void(std::string /* entity_reference */)> callback) {
  story_provider_impl_->CreateEntity(story_id_, type, std::move(data),
                                     std::move(entity_request),
                                     std::move(callback));
}

void StoryControllerImpl::OnSurfaceFocused(fidl::StringPtr surface_id) {
  auto module_path = ModulePathFromSurfaceID(surface_id);

  for (auto& watcher : watchers_.ptrs()) {
    (*watcher)->OnModuleFocused(std::move(module_path));
  }
}

}  // namespace modular
