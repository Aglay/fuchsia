// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/story_runner/story_controller_impl.h"

#include <memory>

#include <component/cpp/fidl.h>
#include <ledger/cpp/fidl.h>
#include <modular/cpp/fidl.h>
#include <modular_private/cpp/fidl.h>
#include <presentation/cpp/fidl.h>
#include <views_v1/cpp/fidl.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app/cpp/connect.h"
#include "lib/async/cpp/future.h"
#include "lib/fidl/cpp/clone.h"
#include "lib/fidl/cpp/interface_handle.h"
#include "lib/fidl/cpp/interface_ptr_set.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fidl/cpp/optional.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/types/type_converters.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/join_strings.h"
#include "lib/fxl/type_converter.h"
#include "peridot/bin/device_runner/cobalt/cobalt.h"
#include "peridot/bin/user_runner/story_runner/chain_impl.h"
#include "peridot/bin/user_runner/story_runner/link_impl.h"
#include "peridot/bin/user_runner/story_runner/module_context_impl.h"
#include "peridot/bin/user_runner/story_runner/module_controller_impl.h"
#include "peridot/bin/user_runner/story_runner/story_provider_impl.h"
#include "peridot/lib/common/names.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/fidl/equals.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/operations.h"
#include "peridot/lib/ledger_client/storage.h"

namespace modular {

constexpr char kStoryScopeLabelPrefix[] = "story-";

namespace {

fidl::StringPtr PathString(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  auto path = fxl::To<std::vector<std::string>>(module_path);
  return fxl::JoinStrings(path, ":");
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

void XdrLinkPath(XdrContext* const xdr, LinkPath* const data) {
  xdr->Field("module_path", &data->module_path);
  xdr->Field("link_name", &data->link_name);
}

void XdrChainKeyToLinkData(XdrContext* const xdr,
                           ChainKeyToLinkData* const data) {
  xdr->Field("key", &data->key);
  xdr->Field("link_path", &data->link_path, XdrLinkPath);
}

void XdrChainData(XdrContext* const xdr, ChainData* const data) {
  xdr->Field("key_to_link_map", &data->key_to_link_map, XdrChainKeyToLinkData);
}

void XdrSurfaceRelation(XdrContext* const xdr, SurfaceRelation* const data) {
  xdr->Field("arrangement", &data->arrangement);
  xdr->Field("dependency", &data->dependency);
  xdr->Field("emphasis", &data->emphasis);
}

void XdrIntentParameterData(XdrContext* const xdr,
                            IntentParameterData* const data) {
  static constexpr char kTag[] = "tag";
  static constexpr char kEntityReference[] = "entity_reference";
  static constexpr char kJson[] = "json";
  static constexpr char kEntityType[] = "entity_type";
  static constexpr char kLinkName[] = "link_name";
  static constexpr char kLinkPath[] = "link_path";

  switch (xdr->op()) {
    case XdrOp::FROM_JSON: {
      std::string tag;
      xdr->Field(kTag, &tag);

      if (tag == kEntityReference) {
        fidl::StringPtr value;
        xdr->Field(kEntityReference, &value);
        data->set_entity_reference(std::move(value));
      } else if (tag == kJson) {
        fidl::StringPtr value;
        xdr->Field(kJson, &value);
        data->set_json(std::move(value));
      } else if (tag == kEntityType) {
        ::fidl::VectorPtr<::fidl::StringPtr> value;
        xdr->Field(kEntityType, &value);
        data->set_entity_type(std::move(value));
      } else if (tag == kLinkName) {
        fidl::StringPtr value;
        xdr->Field(kLinkName, &value);
        data->set_link_name(std::move(value));
      } else if (tag == kLinkPath) {
        LinkPath value;
        xdr->Field(kLinkPath, &value, XdrLinkPath);
        data->set_link_path(std::move(value));
      } else {
        FXL_LOG(ERROR) << "XdrIntentParameterData FROM_JSON unknown tag: "
                       << tag;
      }
      break;
    }

    case XdrOp::TO_JSON: {
      std::string tag;

      // The unusual call to operator->() in the cases below is because
      // operator-> for all of FIDL's pointer types to {strings, arrays,
      // structs} returns a _non-const_ reference to the inner pointer,
      // which is required by the xdr->Field() method. Calling get() returns
      // a const pointer for arrays and strings. get() does return a non-const
      // pointer for FIDL structs, but given that operator->() is required for
      // some FIDL types, we might as well be consistent and use operator->()
      // for all types.

      switch (data->Which()) {
        case IntentParameterData::Tag::kEntityReference: {
          tag = kEntityReference;
          fidl::StringPtr value = data->entity_reference();
          xdr->Field(kEntityReference, &value);
          break;
        }
        case IntentParameterData::Tag::kJson: {
          tag = kJson;
          fidl::StringPtr value = data->json();
          xdr->Field(kJson, &value);
          break;
        }
        case IntentParameterData::Tag::kEntityType: {
          tag = kEntityType;
          fidl::VectorPtr<fidl::StringPtr> value = Clone(data->entity_type());
          xdr->Field(kEntityType, &value);
          break;
        }
        case IntentParameterData::Tag::kLinkName: {
          tag = kLinkName;
          fidl::StringPtr value = data->link_name();
          xdr->Field(kLinkName, &value);
          break;
        }
        case IntentParameterData::Tag::kLinkPath: {
          tag = kLinkPath;
          xdr->Field(kLinkPath, &data->link_path(), XdrLinkPath);
          break;
        }
        case IntentParameterData::Tag::Invalid:
          FXL_LOG(ERROR) << "XdrIntentParameterData TO_JSON unknown tag: "
                         << static_cast<int>(data->Which());
          break;
      }

      xdr->Field(kTag, &tag);
      break;
    }
  }
}

void XdrIntentParameter(XdrContext* const xdr, IntentParameter* const data) {
  xdr->Field("name", &data->name);
  xdr->Field("data", &data->data, XdrIntentParameterData);
}

void XdrIntent(XdrContext* const xdr, Intent* const data) {
  xdr->Field("action_name", &data->action.name);
  xdr->Field("action_handler", &data->action.handler);
  xdr->Field("parameters", &data->parameters, XdrIntentParameter);
}

void XdrParameterConstraint(XdrContext* const xdr,
                            ParameterConstraint* const data) {
  xdr->Field("name", &data->name);
  xdr->Field("type", &data->type);
}

void XdrModuleManifest(XdrContext* const xdr, ModuleManifest* const data) {
  xdr->Field("binary", &data->binary);
  xdr->Field("suggestion_headline", &data->suggestion_headline);
  xdr->Field("action", &data->action);
  xdr->Field("parameters", &data->parameter_constraints,
             XdrParameterConstraint);
  xdr->Field("composition_pattern", &data->composition_pattern);
}

void XdrModuleData_v1(XdrContext* const xdr, ModuleData* const data) {
  xdr->Field("url", &data->module_url);
  xdr->Field("module_path", &data->module_path);
  xdr->Field("module_source", &data->module_source);
  xdr->Field("surface_relation", &data->surface_relation, XdrSurfaceRelation);
  xdr->Field("module_stopped", &data->module_stopped);
  xdr->Field("intent", &data->intent, XdrIntent);

  // In previous versions we did not have these fields.
  data->chain_data.key_to_link_map.resize(0);
  data->module_manifest.reset();
}

void XdrModuleData_v2(XdrContext* const xdr, ModuleData* const data) {
  xdr->Field("url", &data->module_url);
  xdr->Field("module_path", &data->module_path);
  xdr->Field("module_source", &data->module_source);
  xdr->Field("surface_relation", &data->surface_relation, XdrSurfaceRelation);
  xdr->Field("module_stopped", &data->module_stopped);
  xdr->Field("intent", &data->intent, XdrIntent);
  xdr->Field("chain_data", &data->chain_data, XdrChainData);

  // In previous versions we did not have these fields.
  data->module_manifest.reset();
}


void XdrModuleData_v3(XdrContext* const xdr, ModuleData* const data) {
  xdr->Field("url", &data->module_url);
  xdr->Field("module_path", &data->module_path);
  xdr->Field("module_source", &data->module_source);
  xdr->Field("surface_relation", &data->surface_relation, XdrSurfaceRelation);
  xdr->Field("module_stopped", &data->module_stopped);
  xdr->Field("intent", &data->intent, XdrIntent);
  xdr->Field("chain_data", &data->chain_data, XdrChainData);
  xdr->Field("module_manifest", &data->module_manifest, XdrModuleManifest);
}

void XdrModuleData_v4(XdrContext* const xdr, ModuleData* const data) {
  if (!xdr->Version(4)) {
    return;
  }
  xdr->Field("url", &data->module_url);
  xdr->Field("module_path", &data->module_path);
  xdr->Field("module_source", &data->module_source);
  xdr->Field("surface_relation", &data->surface_relation, XdrSurfaceRelation);
  xdr->Field("module_stopped", &data->module_stopped);
  xdr->Field("intent", &data->intent, XdrIntent);
  xdr->Field("chain_data", &data->chain_data, XdrChainData);
  xdr->Field("module_manifest", &data->module_manifest, XdrModuleManifest);
}

constexpr XdrFilterType<ModuleData> XdrModuleData[] = {
  XdrModuleData_v4,
  XdrModuleData_v3,
  XdrModuleData_v2,
  XdrModuleData_v1,
  nullptr,
};

void XdrPerDeviceStoryInfo_v1(XdrContext* const xdr,
                              modular_private::PerDeviceStoryInfo* const data) {
  xdr->Field("device", &data->device_id);
  xdr->Field("id", &data->story_id);
  xdr->Field("time", &data->timestamp);
  xdr->Field("state", &data->state);
}

void XdrPerDeviceStoryInfo_v2(XdrContext* const xdr,
                              modular_private::PerDeviceStoryInfo* const data) {
  if (!xdr->Version(2)) {
    return;
  }
  xdr->Field("device", &data->device_id);
  xdr->Field("id", &data->story_id);
  xdr->Field("time", &data->timestamp);
  xdr->Field("state", &data->state);
}

constexpr XdrFilterType<modular_private::PerDeviceStoryInfo>
XdrPerDeviceStoryInfo[] = {
  XdrPerDeviceStoryInfo_v2,
  XdrPerDeviceStoryInfo_v1,
  nullptr,
};

}  // namespace

class StoryControllerImpl::BlockingModuleDataWriteCall : public Operation<> {
 public:
  BlockingModuleDataWriteCall(StoryControllerImpl* const story_controller_impl,
                              std::string key, ModuleDataPtr module_data,
                              ResultCall result_call)
      : Operation("StoryControllerImpl::BlockingModuleDataWriteCall",
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        key_(std::move(key)),
        module_data_(std::move(module_data)) {
    FXL_DCHECK(!module_data_->module_path.is_null());
    ModuleData module_data_clone;
    module_data_->Clone(&module_data_clone);
    story_controller_impl_->blocked_operations_.push_back(
        std::make_pair(std::move(module_data_clone), this));
  }

  void Continue() {
    fn_called_ = true;
    if (fn_) {
      fn_();
    }
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // If the data in the ledger is already the same as |module_data_|, we
    // don't try to write again, as the ledger will not notify us of a change.
    // We rely on the ledger notifying us in
    // StoryControllerImpl::OnPageChange() so that it calls the method we push
    // onto StoryControllerImpl::blocked_operations_.
    operation_queue_.Add(new ReadDataCall<ModuleData>(
        story_controller_impl_->page(), key_, true /* not_found_is_ok */,
        XdrModuleData, [this, flow](ModuleDataPtr data) {
          if (!ModuleDataEqual(data, module_data_)) {
            WriteModuleData(flow);
          }
        }));
  }

  void WriteModuleData(FlowToken flow) {
    operation_queue_.Add(new WriteDataCall<ModuleData>(
        story_controller_impl_->page(), key_, XdrModuleData,
        std::move(module_data_), [this, flow] {
          FlowTokenHolder hold{flow};
          fn_ = [hold] {
            std::unique_ptr<FlowToken> flow = hold.Continue();
            FXL_CHECK(flow) << "Called BlockingModuleDataWriteCall::Continue() "
                            << "twice. Please file a bug.";
          };

          if (fn_called_) {
            fn_();
          }
        }));
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const std::string key_;
  ModuleDataPtr module_data_;

  std::function<void()> fn_;
  bool fn_called_{};

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(BlockingModuleDataWriteCall);
};

// Launches (brings up a running instance) of a module.
//
// If the module is to be composed into the story shell, notifies the story
// shell of the new module. If the module is composed internally, connects the
// view owner request appropriately.
class StoryControllerImpl::LaunchModuleCall : public Operation<> {
 public:
  LaunchModuleCall(
      StoryControllerImpl* const story_controller_impl,
      ModuleDataPtr module_data,
      fidl::InterfaceRequest<ModuleController> module_controller_request,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      ResultCall result_call)
      : Operation("StoryControllerImpl::GetLedgerNotificationCall",
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        module_controller_request_(std::move(module_controller_request)),
        view_owner_request_(std::move(view_owner_request)),
        start_time_(zx_clock_get(ZX_CLOCK_UTC)) {
    FXL_DCHECK(!module_data_->module_path.is_null());
  }

 private:
  void Run() override {
    FlowToken flow{this};

    Connection* const i =
        story_controller_impl_->FindConnection(module_data_->module_path);

    // We launch the new module if it doesn't run yet.
    if (!i) {
      Launch(flow);
      return;
    }

    // If the new module is already running, but with a different URL or on a
    // different link, or if a service exchange is requested, or if transitive
    // embedding is requested, we tear it down then launch a new module.
    if (i->module_data->intent != module_data_->intent) {
      i->module_controller_impl->Teardown([this, flow] {
        // NOTE(mesch): i is invalid at this point.
        Launch(flow);
      });
      return;
    }

    // If the module is already running on the same URL and link, we just
    // connect the module controller request, if there is one. Modules started
    // with StoryController.AddModule() don't have a module controller request.
    if (module_controller_request_.is_valid()) {
      i->module_controller_impl->Connect(std::move(module_controller_request_));
    }
  }

  void Launch(FlowToken /*flow*/) {
    FXL_LOG(INFO) << "StoryControllerImpl::LaunchModule() "
                  << module_data_->module_url << " "
                  << PathString(module_data_->module_path);
    AppConfig module_config;
    module_config.url = module_data_->module_url;

    views_v1::ViewProviderPtr view_provider;
    fidl::InterfaceRequest<views_v1::ViewProvider> view_provider_request =
        view_provider.NewRequest();
    view_provider->CreateView(std::move(view_owner_request_), nullptr);

    component::ServiceProviderPtr module_context_provider;
    auto module_context_provider_request = module_context_provider.NewRequest();
    auto service_list = component::ServiceList::New();
    service_list->names.push_back(ModuleContext::Name_);
    service_list->provider = std::move(module_context_provider);

    Connection connection;
    fidl::Clone(module_data_, &connection.module_data);

    // Ensure that the Module's Chain is available before we launch it.
    // TODO(thatguy): Set up the ChainImpl based on information in ModuleData.
    auto i =
        std::find_if(story_controller_impl_->chains_.begin(),
                     story_controller_impl_->chains_.end(),
                     [this](const std::unique_ptr<ChainImpl>& ptr) {
                       return ptr->chain_path() == module_data_->module_path;
                     });
    if (i == story_controller_impl_->chains_.end()) {
      story_controller_impl_->chains_.emplace_back(
          new ChainImpl(module_data_->module_path, module_data_->chain_data));
    }

    // ModuleControllerImpl's constructor launches the child application.
    connection.module_controller_impl = std::make_unique<ModuleControllerImpl>(
        story_controller_impl_,
        story_controller_impl_->story_scope_.GetLauncher(),
        std::move(module_config), connection.module_data.get(),
        std::move(service_list),
        std::move(view_provider_request));

    // Modules started with StoryController.AddModule() don't have a module
    // controller request.
    if (module_controller_request_.is_valid()) {
      connection.module_controller_impl->Connect(
          std::move(module_controller_request_));
    }

    ModuleContextInfo module_context_info = {
        story_controller_impl_->story_provider_impl_->component_context_info(),
        story_controller_impl_,
        story_controller_impl_->story_provider_impl_
            ->user_intelligence_provider(),
        story_controller_impl_->story_provider_impl_->module_resolver()};

    connection.module_context_impl = std::make_unique<ModuleContextImpl>(
        module_context_info, connection.module_data.get(),
        std::move(module_context_provider_request));

    story_controller_impl_->connections_.emplace_back(std::move(connection));

    for (auto& i : story_controller_impl_->watchers_.ptrs()) {
      ModuleData module_data;
      module_data_->Clone(&module_data);
      (*i)->OnModuleAdded(std::move(module_data));
    }

    for (auto& i : story_controller_impl_->modules_watchers_.ptrs()) {
      ModuleData module_data;
      module_data_->Clone(&module_data);
      (*i)->OnNewModule(std::move(module_data));
    }

    ReportModuleLaunchTime(module_data_->module_url,
                           zx_clock_get(ZX_CLOCK_UTC) - start_time_);
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  ModuleDataPtr module_data_;
  fidl::InterfaceRequest<ModuleController> module_controller_request_;
  fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request_;
  const zx_time_t start_time_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchModuleCall);
};

class StoryControllerImpl::KillModuleCall : public Operation<> {
 public:
  KillModuleCall(StoryControllerImpl* const story_controller_impl,
                 ModuleDataPtr module_data, const std::function<void()>& done)
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

    auto future = Future<>::Create();
    if (story_controller_impl_->story_shell_ &&
        module_data_->module_source == ModuleSource::EXTERNAL) {
      story_controller_impl_->story_shell_->DefocusView(
          PathString(module_data_->module_path), future->Completer());
    } else {
      future->Complete();
    }

    future->Then([this, flow] {
      // Teardown the module, which discards the module controller. A parent
      // module can call ModuleController.Stop() multiple times before the
      // ModuleController connection gets disconnected by Teardown(). Therefore,
      // this StopModuleCall Operation will cause the calls to be queued.
      // The first Stop() will cause the ModuleController to be closed, and
      // so subsequent Stop() attempts will not find a controller and will
      // return.
      auto* const i =
          story_controller_impl_->FindConnection(module_data_->module_path);

      if (!i) {
        FXL_LOG(INFO) << "No ModuleController for Module"
                      << " " << PathString(module_data_->module_path) << ". "
                      << "Was ModuleContext.Stop() called twice?";
        done_();
        return;
      }

      // done_() must be called BEFORE the Teardown() done callback returns. See
      // comment in StopModuleCall::Kill() before making changes here. Be aware
      // that done_ is NOT the Done() callback of the Operation.
      i->module_controller_impl->Teardown([this, flow] {
        for (auto& i : story_controller_impl_->modules_watchers_.ptrs()) {
          ModuleData module_data;
          module_data_->Clone(&module_data);
          (*i)->OnStopModule(std::move(module_data));
        }
        done_();
      });
    });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  ModuleDataPtr module_data_;
  std::function<void()> done_;

  FXL_DISALLOW_COPY_AND_ASSIGN(KillModuleCall);
};

class StoryControllerImpl::ConnectLinkCall : public Operation<> {
 public:
  // TODO(mesch/thatguy): Notifying watchers on new Link connections is overly
  // complex. Sufficient and simpler would be to have a Story watchers notified
  // of Link state changes for all Links within a Story.
  ConnectLinkCall(StoryControllerImpl* const story_controller_impl,
                  LinkPathPtr link_path, CreateLinkInfoPtr create_link_info,
                  bool notify_watchers, fidl::InterfaceRequest<Link> request,
                  ResultCall done)
      : Operation("StoryControllerImpl::ConnectLinkCall", std::move(done)),
        story_controller_impl_(story_controller_impl),
        link_path_(std::move(link_path)),
        create_link_info_(std::move(create_link_info)),
        notify_watchers_(notify_watchers),
        request_(std::move(request)) {}

 private:
  void Run() override {
    FlowToken flow{this};
    auto i = std::find_if(story_controller_impl_->links_.begin(),
                          story_controller_impl_->links_.end(),
                          [this](const std::unique_ptr<LinkImpl>& l) {
                            return l->link_path() == *link_path_;
                          });
    if (i != story_controller_impl_->links_.end()) {
      (*i)->Connect(std::move(request_));
      return;
    }

    link_impl_.reset(
        new LinkImpl(story_controller_impl_->ledger_client_,
                     fidl::Clone(story_controller_impl_->story_page_id_),
                     *link_path_, std::move(create_link_info_)));
    LinkImpl* const link_ptr = link_impl_.get();
    if (request_) {
      link_impl_->Connect(std::move(request_));
      // Transfer ownership of |link_impl_| over to |story_controller_impl_|.
      story_controller_impl_->links_.emplace_back(link_impl_.release());

      // This orphaned handler will be called after this operation has been
      // deleted. So we need to take special care when depending on members.
      // Copies of |story_controller_impl_| and |link_ptr| are ok.
      link_ptr->set_orphaned_handler(
          [link_ptr, story_controller_impl = story_controller_impl_] {
            story_controller_impl->DisposeLink(link_ptr);
          });
    }

    link_ptr->Sync([this, flow] { Cont(flow); });
  }

  void Cont(FlowToken token) {
    if (!notify_watchers_)
      return;

    for (auto& i : story_controller_impl_->links_watchers_.ptrs()) {
      LinkPath link_path;
      link_path_->Clone(&link_path);
      (*i)->OnNewLink(std::move(link_path));
    }
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const LinkPathPtr link_path_;
  CreateLinkInfoPtr create_link_info_;
  const bool notify_watchers_;
  fidl::InterfaceRequest<Link> request_;

  std::unique_ptr<LinkImpl> link_impl_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ConnectLinkCall);
};

// Populates a ChainData struct from a CreateChainInfo struct. May create new
// Links for any CreateChainInfo.property_info if
// property_info[i].is_create_link_info().
class StoryControllerImpl::InitializeChainCall
    : public Operation<ChainDataPtr> {
 public:
  InitializeChainCall(StoryControllerImpl* const story_controller_impl,
                      fidl::VectorPtr<fidl::StringPtr> module_path,
                      CreateChainInfoPtr create_chain_info,
                      ResultCall result_call)
      : Operation("InitializeChainCall", std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        module_path_(std::move(module_path)),
        create_chain_info_(std::move(create_chain_info)) {}

 private:
  void Run() override {
    FlowToken flow{this, &result_};

    result_ = ChainData::New();
    result_->key_to_link_map.resize(0);

    if (!create_chain_info_) {
      return;
    }

    // For each property in |create_chain_info_|, either:
    // a) Copy the |link_path| to |result_| directly or
    // b) Create & populate a new Link and add the correct mapping to
    // |result_|.
    for (auto& entry : *create_chain_info_->property_info) {
      const auto& key = entry.key;
      const auto& info = entry.value;

      auto mapping = ChainKeyToLinkData::New();
      mapping->key = key;
      if (info.is_link_path()) {
        info.link_path().Clone(&mapping->link_path);
      } else {  // info->is_create_link()
        mapping->link_path.module_path.resize(0);
        // Create a new Link. ConnectLinkCall will either create a new Link, or
        // connect to an existing one.
        //
        // TODO(thatguy): If the Link already exists (it shouldn't),
        // |create_link_info.initial_data| will be ignored.
        for (const auto& i : *module_path_) {
          mapping->link_path.module_path.push_back(i);
        }
        mapping->link_path.link_name = key;

        // We create N ConnectLinkCall operations. We rely on the fact that
        // once all refcounted instances of |flow| are destroyed, the
        // InitializeChainCall will automatically finish.
        LinkPathPtr link_path = LinkPath::New();
        mapping->link_path.Clone(link_path.get());
        operation_queue_.Add(new ConnectLinkCall(
            story_controller_impl_, std::move(link_path),
            CloneOptional(info.create_link()), false /* notify_watchers */,
            nullptr /* interface request */, [flow] {}));
      }

      result_->key_to_link_map.push_back(std::move(*mapping));
    }
  }

  StoryControllerImpl* const story_controller_impl_;
  const fidl::VectorPtr<fidl::StringPtr> module_path_;
  const CreateChainInfoPtr create_chain_info_;

  OperationQueue operation_queue_;

  ChainDataPtr result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(InitializeChainCall);
};

// Calls LaunchModuleCall to get a running instance, and delegates visual
// composition to the story shell.
class StoryControllerImpl::LaunchModuleInShellCall : public Operation<> {
 public:
  LaunchModuleInShellCall(
      StoryControllerImpl* const story_controller_impl,
      ModuleDataPtr module_data,
      fidl::InterfaceRequest<ModuleController> module_controller_request,
      ResultCall result_call)
      : Operation("StoryControllerImpl::LaunchModuleInShellCall",
                  std::move(result_call), module_data->module_url),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)),
        module_controller_request_(std::move(module_controller_request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // TODO(mesch): The LaunchModuleCall may result in just a new
    // ModuleController connection to an existing
    // ModuleControllerImpl. In that case, the view owner request is
    // closed, and the view owner should not be sent to the story
    // shell.
    operation_queue_.Add(new LaunchModuleCall(
        story_controller_impl_, fidl::Clone(module_data_),
        std::move(module_controller_request_),
        view_owner_.NewRequest(), [this, flow] { Cont(flow); }));
  }

  void Cont(FlowToken flow) {
    // If this is called during Stop(), story_shell_ might already have been
    // reset. TODO(mesch): Then the whole operation should fail.
    if (!story_controller_impl_->story_shell_) {
      return;
    }

    // We only add a module to story shell if its either a root module or its
    // anchor is already known to story shell.

    if (module_data_->module_path->size() == 1) {
      ConnectView(flow, "");
      return;
    }

    auto* const connection =
        story_controller_impl_->FindConnection(module_data_->module_path);
    FXL_CHECK(connection);  // Was just created.

    auto* const anchor = story_controller_impl_->FindAnchor(connection);
    if (anchor) {
      const auto anchor_view_id = PathString(anchor->module_data->module_path);
      if (story_controller_impl_->connected_views_.count(anchor_view_id)) {
        ConnectView(flow, anchor_view_id);
        return;
      }
    }

    auto manifest_clone = ModuleManifest::New();
    fidl::Clone(module_data_->module_manifest, &manifest_clone);
    auto surface_relation_clone = SurfaceRelation::New();
    module_data_->surface_relation->Clone(surface_relation_clone.get());
    story_controller_impl_->pending_views_.emplace(
        PathString(module_data_->module_path),
        PendingView{module_data_->module_path.Clone(),
                    std::move(manifest_clone),
                    std::move(surface_relation_clone), std::move(view_owner_)});
  }

  void ConnectView(FlowToken flow, fidl::StringPtr anchor_view_id) {
    const auto view_id = PathString(module_data_->module_path);

    story_controller_impl_->story_shell_->ConnectView(
        std::move(view_owner_), view_id, anchor_view_id,
        std::move(module_data_->surface_relation),
        std::move(module_data_->module_manifest));

    story_controller_impl_->connected_views_.emplace(view_id);
    story_controller_impl_->ProcessPendingViews();
    story_controller_impl_->story_shell_->FocusView(view_id, anchor_view_id);
  }

  StoryControllerImpl* const story_controller_impl_;
  ModuleDataPtr module_data_;
  fidl::InterfaceRequest<ModuleController> module_controller_request_;

  ModuleControllerPtr module_controller_;
  views_v1_token::ViewOwnerPtr view_owner_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchModuleInShellCall);
};

class StoryControllerImpl::StopCall : public Operation<> {
 public:
  StopCall(StoryControllerImpl* const story_controller_impl, const bool notify,
           std::function<void()> done)
      : Operation("StoryControllerImpl::StopCall", done),
        story_controller_impl_(story_controller_impl),
        notify_(notify) {}

 private:
  // StopCall may be run even on a story impl that is not running.
  void Run() override {
    // At this point, we don't need notifications from disconnected
    // Links anymore, as they will all be disposed soon anyway.
    for (auto& link : story_controller_impl_->links_) {
      link->set_orphaned_handler(nullptr);
    }

    std::vector<FuturePtr<>> did_teardowns;
    did_teardowns.reserve(story_controller_impl_->connections_.size());

    // Tear down all connections with a ModuleController first, then the
    // links between them.
    for (auto& connection : story_controller_impl_->connections_) {
      auto did_teardown = Future<>::Create();
      connection.module_controller_impl->Teardown(did_teardown->Completer());
      did_teardowns.emplace_back(did_teardown);
    }

    Future<>::Wait(did_teardowns)
        ->AsyncMap([this] {
          auto did_teardown = Future<>::Create();
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
          if (story_controller_impl_->story_context_binding_.is_bound()) {
            // Close() dchecks if called while not bound.
            story_controller_impl_->story_context_binding_.Unbind();
          }

          std::vector<FuturePtr<>> did_sync_links;
          did_sync_links.reserve(story_controller_impl_->links_.size());

          // The links don't need to be written now, because they all were
          // written when they were last changed, but we need to wait for the
          // last write request to finish, which is done with the Sync() request
          // below.
          for (auto& link : story_controller_impl_->links_) {
            auto did_sync_link = Future<>::Create();
            link->Sync(did_sync_link->Completer());
            did_sync_links.emplace_back(did_sync_link);
          }

          return Future<>::Wait(did_sync_links);
        })
        ->Then([this] {
          // Clear the remaining links and connections in case there are some
          // left. At this point, no DisposeLink() calls can arrive anymore.
          story_controller_impl_->links_.clear();
          story_controller_impl_->connections_.clear();

          // If this StopCall is part of a DeleteCall, then we don't notify
          // story state changes; the pertinent state change will be the delete
          // notification instead.
          if (notify_) {
            story_controller_impl_->SetState(StoryState::STOPPED);
          } else {
            story_controller_impl_->state_ = StoryState::STOPPED;
          }

          Done();
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const bool notify_;  // Whether to notify state change; false in DeleteCall.

  FXL_DISALLOW_COPY_AND_ASSIGN(StopCall);
};

class StoryControllerImpl::StopModuleCall : public Operation<> {
 public:
  StopModuleCall(StoryControllerImpl* const story_controller_impl,
                 const fidl::VectorPtr<fidl::StringPtr>& module_path,
                 const std::function<void()>& done)
      : Operation("StoryControllerImpl::StopModuleCall", done),
        story_controller_impl_(story_controller_impl),
        module_path_(module_path.Clone()) {}

 private:
  void Run() override {
    // NOTE(alhaad): We don't use flow tokens here. See NOTE in Kill() to know
    // why.

    // Read the module data.
    auto did_read_data = Future<ModuleDataPtr>::Create();
    operation_queue_.Add(new ReadDataCall<ModuleData>(
        story_controller_impl_->page(), MakeModuleKey(module_path_),
        false /* not_found_is_ok */, XdrModuleData,
        did_read_data->Completer()));

    did_read_data
        ->AsyncMap([this](ModuleDataPtr data) {
          module_data_ = std::move(data);

          // If the module is already marked as stopped, thers's no need to
          // update the module's data.
          if (module_data_->module_stopped) {
            return Future<>::CreateCompleted();
          }

          // Write the module data back, with module_stopped = true, which is a
          // global state shared between machines to track when the module is
          // explicitly stopped.
          module_data_->module_stopped = true;

          std::string key{MakeModuleKey(module_data_->module_path)};
          // TODO(alhaad: This call may never continue if the data we're writing
          // to the ledger is the same as the data already in there as that will
          // not trigger an OnPageChange().
          auto did_write_data = Future<>::Create();
          operation_queue_.Add(new BlockingModuleDataWriteCall(
              story_controller_impl_, std::move(key),
              CloneOptional(module_data_), did_write_data->Completer()));
          return did_write_data;
        })
        ->AsyncMap([this] {
          auto did_kill_module = Future<>::Create();
          operation_queue_.Add(new KillModuleCall(
              story_controller_impl_, std::move(module_data_),
              did_kill_module->Completer()));
          return did_kill_module;
        })
        ->Then([this] {
          // NOTE(alhaad): An interesting flow of control to keep in mind:
          //
          // 1. From ModuleController.Stop() which can only be called from FIDL,
          // we call StoryControllerImpl.StopModule().
          //
          // 2.  StoryControllerImpl.StopModule() pushes StopModuleCall onto the
          // operation queue.
          //
          // 3. When operation becomes current, we write to ledger, block and
          // continue on receiving OnPageChange from ledger.
          //
          // 4. We then call KillModuleCall on a sub operation queue.
          //
          // 5. KillModuleCall will call Teardown() on the same
          // ModuleControllerImpl that had started ModuleController.Stop(). In
          // the callback from Teardown(), it calls done_() (and NOT Done()).
          //
          // 6. done_() in KillModuleCall leads to the next line here, which
          // calls Done() which would call the FIDL callback from
          // ModuleController.Stop().
          //
          // 7. Done() on the next line also deletes this which deletes the
          // still running KillModuleCall, but this is okay because the only
          // thing that was left to do in KillModuleCall was FlowToken going out
          // of scope.
          Done();
        });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::VectorPtr<fidl::StringPtr> module_path_;
  ModuleDataPtr module_data_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StopModuleCall);
};

class StoryControllerImpl::DeleteCall : public Operation<> {
 public:
  DeleteCall(StoryControllerImpl* const story_controller_impl,
             std::function<void()> done)
      : Operation("StoryControllerImpl::DeleteCall", [] {}),
        story_controller_impl_(story_controller_impl),
        done_(std::move(done)) {}

 private:
  void Run() override {
    // No call to Done(), in order to block all further operations on the queue
    // until the instance is deleted.
    operation_queue_.Add(
        new StopCall(story_controller_impl_, false /* notify */, done_));
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned

  // Not the result call of the Operation, because it's invoked without
  // unblocking the operation queue, to prevent subsequent operations from
  // executing until the instance is deleted, which cancels those operations.
  std::function<void()> done_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DeleteCall);
};

class StoryControllerImpl::LedgerNotificationCall : public Operation<> {
 public:
  LedgerNotificationCall(StoryControllerImpl* const story_controller_impl,
                         ModuleDataPtr module_data)
      : Operation("StoryControllerImpl::LedgerNotificationCall", [] {}),
        story_controller_impl_(story_controller_impl),
        module_data_(std::move(module_data)) {}

 private:
  void Run() override {
    FlowToken flow{this};
    if (!story_controller_impl_->IsRunning() ||
        module_data_->module_source != ModuleSource::EXTERNAL) {
      return;
    }

    // Check for existing module at the given path.
    auto* const i =
        story_controller_impl_->FindConnection(module_data_->module_path);
    if (i && module_data_->module_stopped) {
      operation_queue_.Add(new KillModuleCall(
          story_controller_impl_, std::move(module_data_), [flow] {}));
      return;
    } else if (module_data_->module_stopped) {
      // There is no module running, and the ledger change is for a stopped
      // module so do nothing.
      return;
    }

    // We reach this point only if we want to start an external module.
    operation_queue_.Add(new LaunchModuleInShellCall(
        story_controller_impl_, std::move(module_data_),
        nullptr /* module_controller_request */, [flow] {}));
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  ModuleDataPtr module_data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerNotificationCall);
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

    Connection* const anchor = story_controller_impl_->FindAnchor(
        story_controller_impl_->FindConnection(module_path_));
    if (anchor) {
      // Focus modules relative to their anchor module.
      story_controller_impl_->story_shell_->FocusView(
          PathString(module_path_),
          PathString(anchor->module_data->module_path));
    } else {
      // Focus root modules absolutely.
      story_controller_impl_->story_shell_->FocusView(PathString(module_path_),
                                                      nullptr);
    }
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
    story_controller_impl_->story_shell_->DefocusView(PathString(module_path_),
                                                      [] {});
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  const fidl::VectorPtr<fidl::StringPtr> module_path_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DefocusCall);
};

class StoryControllerImpl::ResolveParameterCall
    : public Operation<ResolverParameterConstraintPtr> {
 public:
  ResolveParameterCall(StoryControllerImpl* const story_controller_impl,
                       LinkPathPtr link_path, ResultCall result_call)
      : Operation("StoryControllerImpl::ResolveParameterCall",
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        link_path_(std::move(link_path)) {}

 private:
  void Run() {
    FlowToken flow{this, &result_};
    operation_queue_.Add(new ConnectLinkCall(
        story_controller_impl_, fidl::Clone(link_path_),
        nullptr /* create_link_info */, false /* notify_watchers */,
        link_.NewRequest(), [this, flow] { Cont(flow); }));
  }

  void Cont(FlowToken flow) {
    link_->Get(nullptr /* path */, [this, flow](fidl::StringPtr content) {
      auto link_info = ResolverLinkInfo::New();
      link_info->path = std::move(*link_path_);
      link_info->content_snapshot = std::move(content);

      result_ = ResolverParameterConstraint::New();
      result_->set_link_info(std::move(*link_info));
    });
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;  // not owned
  LinkPathPtr link_path_;
  LinkPtr link_;
  ResolverParameterConstraintPtr result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ResolveParameterCall);
};

class StoryControllerImpl::ResolveModulesCall
    : public Operation<FindModulesResultPtr> {
 public:
  // If |intent| originated from a Module, |requesting_module_path| must be
  // non-null.  Otherwise, it is an error for the |intent| to have any
  // Parameters of type 'link_name' (since a Link with a link name without an
  // associated Module path is impossible to locate).
  ResolveModulesCall(StoryControllerImpl* const story_controller_impl,
                     IntentPtr intent,
                     fidl::VectorPtr<fidl::StringPtr> requesting_module_path,
                     ResultCall result_call)
      : Operation("StoryControllerImpl::ResolveModulesCall",
                  std::move(result_call)),
        story_controller_impl_(story_controller_impl),
        intent_(std::move(intent)),
        requesting_module_path_(std::move(requesting_module_path)) {}

 private:
  void Run() {
    FlowToken flow{this, &result_};

    resolver_query_ = ResolverQuery::New();
    resolver_query_->action = intent_->action.name;
    resolver_query_->handler = intent_->action.handler;

    std::vector<FuturePtr<>> did_create_constraints;
    did_create_constraints.reserve(intent_->parameters->size());

    for (const auto& entry : *intent_->parameters) {
      const auto& name = entry.name;
      const auto& data = entry.data;

      if (name.is_null() && intent_->action.handler.is_null()) {
        // It is not allowed to have a null intent name (left in for backwards
        // compatibility with old code: MI4-736) and rely on action-based
        // resolution.
        // TODO(thatguy): Return an error string.
        FXL_LOG(WARNING) << "A null-named module parameter is not allowed "
                         << "when using Intent.action.name.";
        return;
      }

      if (data.is_json()) {
        auto parameter_constraint = ResolverParameterConstraint::New();
        parameter_constraint->set_json(data.json());

        auto entry = ResolverParameterConstraintEntry::New();
        entry->key = name;
        entry->constraint = std::move(*parameter_constraint);

        resolver_query_->parameter_constraints.push_back(std::move(*entry));

      } else if (data.is_link_name() || data.is_link_path()) {
        // Find the chain for this Module, or use the one that was provided via
        // the data.
        LinkPathPtr link_path;
        if (data.is_link_path()) {
          link_path = CloneOptional(data.link_path());
        } else {
          link_path = story_controller_impl_->GetLinkPathForChainKey(
              requesting_module_path_, data.link_name());
        }

        auto did_resolve_parameter =
            Future<ResolverParameterConstraintPtr>::Create();
        operation_queue_.Add(new ResolveParameterCall(
            story_controller_impl_, std::move(link_path),
            did_resolve_parameter->Completer()));

        auto did_create_constraint = did_resolve_parameter->Then(
            [this, flow, name](ResolverParameterConstraintPtr result) {
              auto entry = ResolverParameterConstraintEntry::New();
              entry->key = name;
              entry->constraint = std::move(*result);
              resolver_query_->parameter_constraints.push_back(
                  std::move(*entry));
            });
        did_create_constraints.push_back(did_create_constraint);

      } else if (data.is_entity_type()) {
        auto parameter_constraint = ResolverParameterConstraint::New();
        parameter_constraint->set_entity_type(data.entity_type().Clone());

        auto entry = ResolverParameterConstraintEntry::New();
        entry->key = name;
        entry->constraint = std::move(*parameter_constraint);
        resolver_query_->parameter_constraints.push_back(std::move(*entry));

      } else if (data.is_entity_reference()) {
        auto parameter_constraint = ResolverParameterConstraint::New();
        parameter_constraint->set_entity_reference(data.entity_reference());

        auto entry = ResolverParameterConstraintEntry::New();
        entry->key = name;
        entry->constraint = std::move(*parameter_constraint);
        resolver_query_->parameter_constraints.push_back(std::move(*entry));
      }
    }

    Future<>::Wait(did_create_constraints)
        ->AsyncMap([this, flow] {
          auto did_find_modules = Future<FindModulesResult>::Create();
          story_controller_impl_->story_provider_impl_->module_resolver()
              ->FindModules(std::move(*resolver_query_), nullptr,
                            did_find_modules->Completer());
          return did_find_modules;
        })
        ->Then([this, flow](FindModulesResult result) {
          result_ = CloneOptional(result);
        });
  }

  OperationQueue operation_queue_;

  StoryControllerImpl* const story_controller_impl_;  // not owned
  const IntentPtr intent_;
  const fidl::VectorPtr<fidl::StringPtr> requesting_module_path_;

  ResolverQueryPtr resolver_query_;
  FindModulesResultPtr result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ResolveModulesCall);
};

// An operation that first performs module resolution with the provided Intent
// and subsequently starts the most appropriate resolved module in the story
// shell.
class StoryControllerImpl::AddIntentCall : public Operation<StartModuleStatus> {
 public:
  AddIntentCall(
      StoryControllerImpl* const story_controller_impl,
      fidl::VectorPtr<fidl::StringPtr> requesting_module_path,
      const std::string& module_name, IntentPtr intent,
      fidl::InterfaceRequest<ModuleController> module_controller_request,
      SurfaceRelationPtr surface_relation,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      const ModuleSource module_source, ResultCall result_call)
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
    FlowToken flow{this, &result_};

    operation_queue_.Add(
        new ResolveModulesCall(story_controller_impl_, CloneOptional(intent_),
                               requesting_module_path_.Clone(),
                               [this, flow](FindModulesResultPtr result) {
                                 AddModuleFromResult(flow, std::move(result));
                               }));
  }

  void AddModuleFromResult(FlowToken flow, FindModulesResultPtr result) {
    if (result->modules->empty()) {
      result_ = StartModuleStatus::NO_MODULES_FOUND;
      return;
    }

    // Add the resulting module to story state.
    const auto& module_result = result->modules->at(0);
    create_chain_info_ = CreateChainInfo::New();
    fidl::Clone(module_result.create_chain_info, create_chain_info_.get());

    module_data_ = ModuleData::New();
    module_data_->module_url = module_result.module_id;
    module_data_->module_path = requesting_module_path_.Clone();
    module_data_->module_path.push_back(module_name_);
    module_data_->module_source = module_source_;
    fidl::Clone(surface_relation_, &module_data_->surface_relation);
    module_data_->module_stopped = false;
    module_data_->intent = std::move(intent_);
    fidl::Clone(module_result.manifest, &module_data_->module_manifest);

    // Initialize the chain, which we need to do to get ChainData, which
    // belongs in |module_data_|.
    operation_queue_.Add(new InitializeChainCall(
        story_controller_impl_, fidl::Clone(module_data_->module_path),
        std::move(create_chain_info_), [this, flow](ChainDataPtr chain_data) {
          WriteModuleData(flow, std::move(chain_data));
        }));
  }

  void WriteModuleData(FlowToken flow, ChainDataPtr chain_data) {
    fidl::Clone(*chain_data, &module_data_->chain_data);
    // Write the module's data.
    operation_queue_.Add(new BlockingModuleDataWriteCall(
        story_controller_impl_, MakeModuleKey(module_data_->module_path),
        fidl::Clone(module_data_), [this, flow] { MaybeLaunchModule(flow); }));
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

    result_ = StartModuleStatus::SUCCESS;
  }

  OperationQueue operation_queue_;
  StoryControllerImpl* const story_controller_impl_;

  // Arguments passed in from the constructor. Some are used to initialize
  // module_data_ in AddModuleFromResult().
  fidl::VectorPtr<fidl::StringPtr> requesting_module_path_;
  const std::string module_name_;
  IntentPtr intent_;
  fidl::InterfaceRequest<ModuleController> module_controller_request_;
  SurfaceRelationPtr surface_relation_;
  fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request_;
  const ModuleSource module_source_;

  // Returned to us from the resolver, and cached here so that InitializeChain()
  // has access to it.
  CreateChainInfoPtr create_chain_info_;

  // Created by AddModuleFromResult, and ultimately written to story state.
  ModuleDataPtr module_data_;

  StartModuleStatus result_{StartModuleStatus::NO_MODULES_FOUND};

  FXL_DISALLOW_COPY_AND_ASSIGN(AddIntentCall);
};

class StoryControllerImpl::StartContainerInShellCall : public Operation<> {
 public:
  StartContainerInShellCall(
      StoryControllerImpl* const story_controller_impl,
      fidl::VectorPtr<fidl::StringPtr> parent_module_path,
      fidl::StringPtr container_name, SurfaceRelationPtr parent_relation,
      fidl::VectorPtr<ContainerLayout> layout,
      fidl::VectorPtr<ContainerRelationEntry> relationships,
      fidl::VectorPtr<ContainerNodePtr> nodes)
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
    std::vector<FuturePtr<StartModuleStatus>> did_add_intents;
    did_add_intents.reserve(nodes_->size());

    for (size_t i = 0; i < nodes_->size(); ++i) {
      auto did_add_intent = Future<StartModuleStatus>::Create();
      auto intent = Intent::New();
      nodes_->at(i)->intent.Clone(intent.get());
      operation_queue_.Add(new AddIntentCall(
          story_controller_impl_, parent_module_path_.Clone(),
          nodes_->at(i)->node_name, std::move(intent),
          nullptr /* module_controller_request */,
          fidl::MakeOptional(
              relation_map_[nodes_->at(i)->node_name]->relationship),
          nullptr /* view_owner_request */, ModuleSource::INTERNAL,
          did_add_intent->Completer()));

      did_add_intents.emplace_back(did_add_intent);
    }

    Future<StartModuleStatus>::Wait(did_add_intents)->Then([this, flow] {
      if (!story_controller_impl_->story_shell_) {
        return;
      }
      auto views = fidl::VectorPtr<modular::ContainerView>::New(nodes_->size());
      for (size_t i = 0; i < nodes_->size(); i++) {
        ContainerView view;
        view.node_name = nodes_->at(i)->node_name;
        view.owner = std::move(node_views_[nodes_->at(i)->node_name]);
        views->at(i) = std::move(view);
      }
      story_controller_impl_->story_shell_->AddContainer(
          container_name_, PathString(parent_module_path_),
          std::move(*parent_relation_), std::move(layout_),
          std::move(relationships_), std::move(views));
    });
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  OperationQueue operation_queue_;
  const fidl::VectorPtr<fidl::StringPtr> parent_module_path_;
  const fidl::StringPtr container_name_;

  SurfaceRelationPtr parent_relation_;
  fidl::VectorPtr<ContainerLayout> layout_;
  fidl::VectorPtr<ContainerRelationEntry> relationships_;
  const fidl::VectorPtr<ContainerNodePtr> nodes_;
  std::map<std::string, ContainerRelationEntryPtr> relation_map_;

  // map of node_name to view_owners
  std::map<fidl::StringPtr, views_v1_token::ViewOwnerPtr> node_views_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartContainerInShellCall);
};

class StoryControllerImpl::StartCall : public Operation<> {
 public:
  StartCall(StoryControllerImpl* const story_controller_impl,
            fidl::InterfaceRequest<views_v1_token::ViewOwner> request)
      : Operation("StoryControllerImpl::StartCall", [] {}),
        story_controller_impl_(story_controller_impl),
        request_(std::move(request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // If the story is running, we do nothing and close the view owner request.
    if (story_controller_impl_->IsRunning()) {
      FXL_LOG(INFO)
          << "StoryControllerImpl::StartCall() while already running: ignored.";
      return;
    }

    story_controller_impl_->StartStoryShell(std::move(request_));

    // Start *all* the root modules, not just the first one, with their
    // respective links.
    operation_queue_.Add(new ReadAllDataCall<ModuleData>(
        story_controller_impl_->page(), kModuleKeyPrefix, XdrModuleData,
        [this, flow](fidl::VectorPtr<ModuleData> data) {
          Cont(flow, std::move(data));
        }));
  }

  void Cont(FlowToken flow, fidl::VectorPtr<ModuleData> data) {
    for (auto& module_data : *data) {
      if (module_data.module_source == ModuleSource::EXTERNAL &&
          !module_data.module_stopped) {
        FXL_CHECK(module_data.intent);
        auto module_data_clone = ModuleData::New();
        fidl::Clone(module_data, module_data_clone.get());
        operation_queue_.Add(new LaunchModuleInShellCall(
            story_controller_impl_, std::move(module_data_clone),
            nullptr /* module_controller_request */, [flow] {}));
      }
    }

    story_controller_impl_->SetState(StoryState::RUNNING);
  }

  StoryControllerImpl* const story_controller_impl_;  // not owned
  fidl::InterfaceRequest<views_v1_token::ViewOwner> request_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartCall);
};

StoryControllerImpl::StoryControllerImpl(
    fidl::StringPtr story_id, LedgerClient* const ledger_client,
    LedgerPageId story_page_id, StoryProviderImpl* const story_provider_impl)
    : PageClient(MakeStoryKey(story_id), ledger_client, story_page_id,
                 kModuleKeyPrefix),
      story_id_(story_id),
      story_provider_impl_(story_provider_impl),
      ledger_client_(ledger_client),
      story_page_id_(std::move(story_page_id)),
      story_scope_(story_provider_impl_->user_scope(),
                   kStoryScopeLabelPrefix + story_id_.get()),
      story_context_binding_(this) {
  auto story_scope = StoryScope::New();
  story_scope->story_id = story_id;
  auto scope = ComponentScope::New();
  scope->set_story_scope(std::move(*story_scope));
  story_provider_impl_->user_intelligence_provider()
      ->GetComponentIntelligenceServices(std::move(*scope),
                                         intelligence_services_.NewRequest());

  story_scope_.AddService<ContextWriter>(
      [this](fidl::InterfaceRequest<ContextWriter> request) {
        intelligence_services_->GetContextWriter(std::move(request));
      });
}

StoryControllerImpl::~StoryControllerImpl() = default;

void StoryControllerImpl::Connect(
    fidl::InterfaceRequest<StoryController> request) {
  bindings_.AddBinding(this, std::move(request));
}

bool StoryControllerImpl::IsRunning() {
  switch (state_) {
    case StoryState::RUNNING:
      return true;
    case StoryState::STOPPED:
      return false;
  }
}

void StoryControllerImpl::StopForDelete(const StopCallback& done) {
  operation_queue_.Add(new DeleteCall(this, done));
}

void StoryControllerImpl::StopForTeardown(const StopCallback& done) {
  operation_queue_.Add(new StopCall(this, false /* notify */, done));
}

StoryState StoryControllerImpl::GetStoryState() const { return state_; }

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
  operation_queue_.Add(new StopModuleCall(this, module_path, done));
}

void StoryControllerImpl::ReleaseModule(
    ModuleControllerImpl* const module_controller_impl) {
  auto f = std::find_if(connections_.begin(), connections_.end(),
                        [module_controller_impl](const Connection& c) {
                          return c.module_controller_impl.get() ==
                                 module_controller_impl;
                        });
  FXL_DCHECK(f != connections_.end());
  f->module_controller_impl.release();
  pending_views_.erase(PathString(f->module_data->module_path));
  connections_.erase(f);
}

fidl::StringPtr StoryControllerImpl::GetStoryId() const { return story_id_; }

void StoryControllerImpl::RequestStoryFocus() {
  story_provider_impl_->RequestStoryFocus(story_id_);
}

void StoryControllerImpl::ConnectLinkPath(
    LinkPathPtr link_path, fidl::InterfaceRequest<Link> request) {
  operation_queue_.Add(new ConnectLinkCall(
      this, std::move(link_path), nullptr /* create_link_info */,
      true /* notify_watchers */, std::move(request), [] {}));
}

LinkPathPtr StoryControllerImpl::GetLinkPathForChainKey(
    const fidl::VectorPtr<fidl::StringPtr>& module_path, fidl::StringPtr key) {
  auto i = std::find_if(chains_.begin(), chains_.end(),
                        [&module_path](const std::unique_ptr<ChainImpl>& ptr) {
                          return ptr->chain_path() == module_path;
                        });

  LinkPathPtr link_path = nullptr;
  if (i != chains_.end()) {
    link_path = (*i)->GetLinkPathForKey(key);
  } else {
    // TODO(MI4-993): It should be an error that is returned to the client for
    // that client to be able to make a request that results in this code path.
    FXL_LOG(WARNING)
        << "Looking for module params on module that doesn't exist: "
        << PathString(module_path);
  }

  if (!link_path) {
    link_path = LinkPath::New();
    link_path->module_path = module_path.Clone();
    link_path->link_name = key;
  }

  return link_path;
}

void StoryControllerImpl::EmbedModule(
    const fidl::VectorPtr<fidl::StringPtr>& parent_module_path,
    fidl::StringPtr module_name, IntentPtr intent,
    fidl::InterfaceRequest<ModuleController> module_controller_request,
    fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
    ModuleSource module_source,
    std::function<void(StartModuleStatus)> callback) {
  operation_queue_.Add(new AddIntentCall(
      this, parent_module_path.Clone(), module_name, std::move(intent),
      std::move(module_controller_request),
      nullptr /* surface_relation */, std::move(view_owner_request),
      std::move(module_source), std::move(callback)));
}

void StoryControllerImpl::StartModule(
    const fidl::VectorPtr<fidl::StringPtr>& parent_module_path,
    fidl::StringPtr module_name, IntentPtr intent,
    fidl::InterfaceRequest<ModuleController> module_controller_request,
    SurfaceRelationPtr surface_relation, ModuleSource module_source,
    std::function<void(StartModuleStatus)> callback) {
  operation_queue_.Add(new AddIntentCall(
      this, parent_module_path.Clone(), module_name, std::move(intent),
      std::move(module_controller_request),
      std::move(surface_relation), nullptr /* view_owner_request */,
      std::move(module_source), std::move(callback)));
}

void StoryControllerImpl::StartContainerInShell(
    const fidl::VectorPtr<fidl::StringPtr>& parent_module_path,
    fidl::StringPtr name, SurfaceRelationPtr parent_relation,
    fidl::VectorPtr<ContainerLayout> layout,
    fidl::VectorPtr<ContainerRelationEntry> relationships,
    fidl::VectorPtr<ContainerNodePtr> nodes) {
  operation_queue_.Add(new StartContainerInShellCall(
      this, parent_module_path.Clone(), name, std::move(parent_relation),
      std::move(layout), std::move(relationships), std::move(nodes)));
}

void StoryControllerImpl::ProcessPendingViews() {
  // NOTE(mesch): As it stands, this machinery to send modules in traversal
  // order to the story shell is N^3 over the lifetime of the story, where N is
  // the number of modules. This function is N^2, and it's called once for each
  // of the N modules. However, N is small, and moreover its scale is limited my
  // much more severe constraints. Eventually, we will address this by changing
  // story shell to be able to accomodate modules out of traversal order.
  if (!story_shell_) {
    return;
  }

  std::vector<fidl::StringPtr> added_keys;

  for (auto& kv : pending_views_) {
    auto* const connection = FindConnection(kv.second.module_path);
    if (!connection) {
      continue;
    }

    auto* const anchor = FindAnchor(connection);
    if (!anchor) {
      continue;
    }

    const auto anchor_view_id = PathString(anchor->module_data->module_path);
    if (!connected_views_.count(anchor_view_id)) {
      continue;
    }

    const auto view_id = PathString(kv.second.module_path);
    story_shell_->ConnectView(std::move(kv.second.view_owner), view_id,
                              anchor_view_id,
                              std::move(kv.second.surface_relation),
                              std::move(kv.second.module_manifest));
    connected_views_.emplace(view_id);

    added_keys.push_back(kv.first);
  }

  if (added_keys.size()) {
    for (auto& key : added_keys) {
      pending_views_.erase(key);
    }
    ProcessPendingViews();
  }
}

void StoryControllerImpl::OnPageChange(const std::string& key,
                                       const std::string& value) {
  auto module_data = ModuleData::New();
  if (!XdrRead(value, &module_data, XdrModuleData)) {
    FXL_LOG(ERROR) << "Unable to parse ModuleData " << key << " " << value;
    return;
  }

  // TODO(mesch,thatguy): We should not have to wait for anything to be written
  // to the ledger. Instead, story graph mutations should be idempotent, and any
  // ledger notification should just trigger the operation it represents, doing
  // nothing if it was done alrady.

  // Check if we already have a blocked operation for this update.
  auto i = std::find_if(blocked_operations_.begin(), blocked_operations_.end(),
                        [&module_data](const auto& p) {
                          return ModuleDataEqual(p.first, *module_data);
                        });
  if (i != blocked_operations_.end()) {
    // For an already blocked operation, we simply continue the operation.
    auto op = i->second;
    blocked_operations_.erase(i);
    op->Continue();
    return;
  }

  // Control reaching here means that this update came from a remote device.
  operation_queue_.Add(
      new LedgerNotificationCall(this, std::move(module_data)));
}

// |StoryController|
void StoryControllerImpl::GetInfo(GetInfoCallback callback) {
  // Synced such that if GetInfo() is called after Start() or Stop(), the state
  // after the previously invoked operation is returned.
  //
  // If this call enters a race with a StoryProvider.DeleteStory() call, it may
  // silently not return or return null, or return the story info before it was
  // deleted, depending on where it gets sequenced in the operation queues of
  // StoryControllerImpl and StoryProviderImpl. The queues do not block each
  // other, however, because the call on the second queue is made in the done
  // callback of the operation on the first queue.
  //
  // This race is normal fidl concurrency behavior.
  operation_queue_.Add(new SyncCall([this, callback] {
    story_provider_impl_->GetStoryInfo(
        story_id_,
        // We capture only |state_| and not |this| because (1) we want the state
        // after SyncCall finishes, not after GetStoryInfo returns (i.e. we want
        // the state after the previous operation before GetInfo(), but not
        // after the operation following GetInfo()), and (2) |this| may have
        // been deleted when GetStoryInfo returned if there was a Delete
        // operation in the queue before GetStoryInfo().
        [state = state_, callback](modular::StoryInfoPtr story_info) {
          callback(std::move(*story_info), state);
        });
  }));
}

// |StoryController|
void StoryControllerImpl::SetInfoExtra(fidl::StringPtr name,
                                       fidl::StringPtr value,
                                       SetInfoExtraCallback callback) {
  story_provider_impl_->SetStoryInfoExtra(story_id_, name, value, callback);
}

// |StoryController|
void StoryControllerImpl::Start(
    fidl::InterfaceRequest<views_v1_token::ViewOwner> request) {
  operation_queue_.Add(new StartCall(this, std::move(request)));
}

// |StoryController|
void StoryControllerImpl::Stop(StopCallback done) {
  operation_queue_.Add(new StopCall(this, true /* notify */, done));
}

// |StoryController|
void StoryControllerImpl::Watch(fidl::InterfaceHandle<StoryWatcher> watcher) {
  auto ptr = watcher.Bind();
  ptr->OnStateChange(state_);
  watchers_.AddInterfacePtr(std::move(ptr));
}

// |StoryController|
void StoryControllerImpl::GetActiveModules(
    fidl::InterfaceHandle<StoryModulesWatcher> watcher,
    GetActiveModulesCallback callback) {
  // We execute this in a SyncCall so that we are sure we don't fall in a crack
  // between a module being created and inserted in the connections collection
  // during some Operation.
  operation_queue_.Add(new SyncCall(fxl::MakeCopyable(
      [this, watcher = std::move(watcher), callback]() mutable {
        if (watcher) {
          auto ptr = watcher.Bind();
          modules_watchers_.AddInterfacePtr(std::move(ptr));
        }

        fidl::VectorPtr<ModuleData> result;
        result.resize(connections_.size());
        for (size_t i = 0; i < connections_.size(); i++) {
          connections_[i].module_data->Clone(&result->at(i));
        }
        callback(std::move(result));
      })));
}

// |StoryController|
void StoryControllerImpl::GetModules(GetModulesCallback callback) {
  operation_queue_.Add(new ReadAllDataCall<ModuleData>(
      page(), kModuleKeyPrefix, XdrModuleData,
      [callback](fidl::VectorPtr<ModuleData> data) {
        callback(std::move(data));
      }));
}

// |StoryController|
void StoryControllerImpl::GetModuleController(
    fidl::VectorPtr<fidl::StringPtr> module_path,
    fidl::InterfaceRequest<ModuleController> request) {
  operation_queue_.Add(new SyncCall(
      fxl::MakeCopyable([this, module_path = std::move(module_path),
                         request = std::move(request)]() mutable {
        for (auto& connection : connections_) {
          if (module_path == connection.module_data->module_path) {
            connection.module_controller_impl->Connect(std::move(request));
            return;
          }
        }

        // Trying to get a controller for a module that is not active just
        // drops the connection request.
      })));
}

// |StoryController|
void StoryControllerImpl::GetActiveLinks(
    fidl::InterfaceHandle<StoryLinksWatcher> watcher,
    GetActiveLinksCallback callback) {
  // We execute this in a SyncCall so that we are sure we don't fall in a crack
  // between a link being created and inserted in the links collection during
  // some Operation. (Right now Links are not created in an Operation, but we
  // don't want to rely on it.)
  operation_queue_.Add(new SyncCall(fxl::MakeCopyable(
      [this, watcher = std::move(watcher), callback]() mutable {
        if (watcher) {
          auto ptr = watcher.Bind();
          links_watchers_.AddInterfacePtr(std::move(ptr));
        }

        // Only active links, i.e. links currently in use by a
        // module, are returned here. Eventually we might want to
        // list all links, but this requires some changes to how
        // links are stored to make it nice. (Right now we need to
        // parse keys, which we don't want to.)
        fidl::VectorPtr<LinkPath> result;
        result.resize(links_.size());
        for (size_t i = 0; i < links_.size(); i++) {
          links_[i]->link_path().Clone(&result->at(i));
        }
        callback(std::move(result));
      })));
}

// |StoryController|
void StoryControllerImpl::GetLink(fidl::VectorPtr<fidl::StringPtr> module_path,
                                  fidl::StringPtr name,
                                  fidl::InterfaceRequest<Link> request) {
  // In the API, a null module path is allowed to represent the empty module
  // path.
  if (module_path.is_null()) {
    module_path.resize(0);
  }

  LinkPathPtr link_path = LinkPath::New();
  link_path->module_path = std::move(module_path);
  link_path->link_name = name;
  ConnectLinkPath(std::move(link_path), std::move(request));
}

void StoryControllerImpl::AddModule(
    fidl::VectorPtr<fidl::StringPtr> parent_module_path,
    fidl::StringPtr module_name, Intent intent,
    SurfaceRelationPtr surface_relation) {
  if (!module_name || module_name->empty()) {
    // TODO(thatguy): When we report errors, make this an error reported back
    // to the client.
    FXL_LOG(FATAL)
        << "StoryController::AddModule(): module_name must not be empty.";
  }

  // AddModule() only adds modules to the story shell. Internally, we use a null
  // SurfaceRelation to mean that the module is embedded, and a non-null
  // SurfaceRelation to indicate that the module is composed by the story shell.
  // If it is null, we set it to the default SurfaceRelation.
  if (!surface_relation) {
    surface_relation = SurfaceRelation::New();
  }

  operation_queue_.Add(new AddIntentCall(
      this, std::move(parent_module_path), module_name, CloneOptional(intent),
      nullptr /* module_controller_request */,
      std::move(surface_relation), nullptr /* view_owner_request */,
      ModuleSource::EXTERNAL, [](StartModuleStatus) {}));
}

void StoryControllerImpl::StartStoryShell(
    fidl::InterfaceRequest<views_v1_token::ViewOwner> request) {
  story_shell_app_ = story_provider_impl_->StartStoryShell(std::move(request));
  story_shell_app_->services().ConnectToService(story_shell_.NewRequest());
  story_shell_->Initialize(story_context_binding_.NewBinding());
}

void StoryControllerImpl::SetState(const StoryState new_state) {
  if (new_state == state_) {
    return;
  }

  state_ = new_state;

  for (auto& i : watchers_.ptrs()) {
    (*i)->OnStateChange(state_);
  }

  story_provider_impl_->NotifyStoryStateChange(story_id_, state_);

  // NOTE(mesch): This gets scheduled on the StoryControllerImpl Operation
  // queue. If the current StoryControllerImpl Operation is part of a
  // DeleteStory Operation of the StoryProviderImpl, then the SetStoryState
  // Operation gets scheduled after the delete of the story is completed, and it
  // will not execute because its queue is deleted beforehand.
  //
  // TODO(mesch): We should execute this inside the containing Operation.

  modular_private::PerDeviceStoryInfoPtr data =
      modular_private::PerDeviceStoryInfo::New();
  data->device_id = story_provider_impl_->device_id();
  data->story_id = story_id_;
  data->timestamp = time(nullptr);
  data->state = state_;

  operation_queue_.Add(
      new WriteDataCall<modular_private::PerDeviceStoryInfo,
                        modular_private::PerDeviceStoryInfoPtr>(
          page(), MakePerDeviceKey(data->device_id), XdrPerDeviceStoryInfo,
          std::move(data), [] {}));
}

void StoryControllerImpl::DisposeLink(LinkImpl* const link) {
  auto f = std::find_if(
      links_.begin(), links_.end(),
      [link](const std::unique_ptr<LinkImpl>& l) { return l.get() == link; });
  FXL_DCHECK(f != links_.end());
  links_.erase(f);
}

bool StoryControllerImpl::IsExternalModule(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  auto* const i = FindConnection(module_path);
  if (!i) {
    return false;
  }

  return i->module_data->module_source == ModuleSource::EXTERNAL;
}

StoryControllerImpl::Connection* StoryControllerImpl::FindConnection(
    const fidl::VectorPtr<fidl::StringPtr>& module_path) {
  for (auto& c : connections_) {
    if (c.module_data->module_path == module_path) {
      return &c;
    }
  }
  return nullptr;
}

StoryControllerImpl::Connection* StoryControllerImpl::FindAnchor(
    Connection* connection) {
  if (!connection) {
    return nullptr;
  }

  auto* anchor =
      FindConnection(ParentModulePath(connection->module_data->module_path));

  // Traverse up until there is a non-embedded module. We recognize non-embedded
  // modules by having a non-null SurfaceRelation. If the root module is there
  // at all, it has a non-null surface relation.
  while (anchor && !anchor->module_data->surface_relation) {
    anchor = FindConnection(ParentModulePath(anchor->module_data->module_path));
  }

  return anchor;
}

void StoryControllerImpl::GetPresentation(
    fidl::InterfaceRequest<presentation::Presentation> request) {
  story_provider_impl_->GetPresentation(story_id_, std::move(request));
}

void StoryControllerImpl::WatchVisualState(
    fidl::InterfaceHandle<StoryVisualStateWatcher> watcher) {
  story_provider_impl_->WatchVisualState(story_id_, std::move(watcher));
}

}  // namespace modular
