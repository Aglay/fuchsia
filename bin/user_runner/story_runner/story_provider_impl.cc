// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/story_runner/story_provider_impl.h"

#include <memory>
#include <utility>
#include <vector>

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/zx/time.h>
#include <fuchsia/ui/views_v1/cpp/fidl.h>

#include "lib/fidl/cpp/array.h"
#include "lib/fidl/cpp/interface_handle.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/functional/make_copyable.h"
#include "peridot/bin/device_runner/cobalt/cobalt.h"
#include "peridot/bin/user_runner/focus.h"
#include "peridot/bin/user_runner/presentation_provider.h"
#include "peridot/bin/user_runner/story_runner/link_impl.h"
#include "peridot/bin/user_runner/story_runner/session_storage.h"
#include "peridot/bin/user_runner/story_runner/session_storage_xdr.h"
#include "peridot/bin/user_runner/story_runner/story_controller_impl.h"
#include "peridot/lib/common/names.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/fidl/proxy.h"
#include "peridot/lib/ledger_client/operations.h"
#include "peridot/lib/ledger_client/page_id.h"
#include "peridot/lib/ledger_client/storage.h"
#include "peridot/lib/rapidjson/rapidjson.h"

namespace fuchsia {
namespace modular {

// 1. Ask SessionStorage to create an ID and storage for the new story.
// 2. Optionally add the module in |url| to the story.
class StoryProviderImpl::CreateStoryCall : public Operation<fidl::StringPtr> {
 public:
  CreateStoryCall(SessionStorage* session_storage,
                  StoryProviderImpl* const story_provider_impl,
                  fidl::StringPtr url,
                  fidl::VectorPtr<StoryInfoExtraEntry> extra_info,
                  fidl::StringPtr root_json, ResultCall result_call)
      : Operation("StoryProviderImpl::CreateStoryCall", std::move(result_call)),
        session_storage_(session_storage),
        story_provider_impl_(story_provider_impl),
        extra_info_(std::move(extra_info)),
        start_time_(zx_clock_get(ZX_CLOCK_UTC)) {
    intent_.action.handler = std::move(url);

    if (!root_json.is_null()) {
      fuchsia::modular::IntentParameter param;
      param.name = nullptr;
      param.data.set_json(std::move(root_json));
      intent_.parameters.push_back(std::move(param));
    }
  }

 private:
  void Run() override {
    FlowToken flow{this, &story_id_};

    // Steps:
    // 1) Create the story storage.
    // 2) Set any extra info.
    // 3) If we got an initial module, add it.
    session_storage_->CreateStory(std::move(extra_info_))
        ->WeakThen(GetWeakPtr(), [this, flow](fidl::StringPtr story_id,
                                              ledger::PageId page_id) {
          story_id_ = story_id;
          story_page_id_ = page_id;
          controller_ = std::make_unique<StoryControllerImpl>(
              story_id_, session_storage_->ledger_client(), story_page_id_,
              story_provider_impl_);
          if (intent_.action.handler) {
            controller_->AddModule({} /* parent_module_path */, kRootModuleName,
                                   std::move(intent_),
                                   nullptr /* surface_relation */);
          }

          // We ensure that everything has been written to the story page before
          // this operation is done.
          controller_->Sync([flow] {});

          ReportStoryLaunchTime(zx_clock_get(ZX_CLOCK_UTC) - start_time_);
        });
  }

  SessionStorage* const session_storage_;         // Not owned
  StoryProviderImpl* const story_provider_impl_;  // Not owned
  Intent intent_;
  fidl::VectorPtr<StoryInfoExtraEntry> extra_info_;
  const zx_time_t start_time_;

  std::unique_ptr<StoryControllerImpl> controller_;

  ledger::PageId story_page_id_;
  fidl::StringPtr story_id_;  // This is the result of the Operation.

  // Sub operations run in this queue.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CreateStoryCall);
};

class StoryProviderImpl::DeleteStoryCall : public Operation<> {
 public:
  using StoryControllerImplMap =
      std::map<std::string, struct StoryControllerImplContainer>;
  using PendingDeletion = std::pair<std::string, DeleteStoryCall*>;

  DeleteStoryCall(SessionStorage* session_storage, fidl::StringPtr story_id,
                  StoryControllerImplMap* const story_controller_impls,
                  MessageQueueManager* const message_queue_manager,
                  const bool already_deleted, ResultCall result_call)
      : Operation("StoryProviderImpl::DeleteStoryCall", std::move(result_call)),
        session_storage_(session_storage),
        story_id_(story_id),
        story_controller_impls_(story_controller_impls),
        message_queue_manager_(message_queue_manager),
        already_deleted_(already_deleted) {}

 private:
  void Run() override {
    FlowToken flow{this};

    if (already_deleted_) {
      Teardown(flow);
    } else {
      session_storage_->DeleteStory(story_id_)->WeakThen(
          GetWeakPtr(), [this, flow] { Teardown(flow); });
    }
  }

  void Teardown(FlowToken flow) {
    auto i = story_controller_impls_->find(story_id_);
    if (i == story_controller_impls_->end()) {
      return;
    }

    FXL_DCHECK(i->second.impl != nullptr);
    i->second.impl->StopForDelete([this, flow] { Erase(flow); });
  }

  void Erase(FlowToken flow) {
    // Here we delete the instance from whose operation a result callback was
    // received. Thus we must assume that the callback returns to a method of
    // the instance. If we delete the instance right here, |this| would be
    // deleted not just for the remainder of this function here, but also for
    // the remainder of all functions above us in the callstack, including
    // functions that run as methods of other objects owned by |this| or
    // provided to |this|. To avoid such problems, the delete is invoked
    // through the run loop.
    async::PostTask(async_get_default(), [this, flow] {
      story_controller_impls_->erase(story_id_);
      message_queue_manager_->DeleteNamespace(
          EncodeModuleComponentNamespace(story_id_), [flow] {});

      // TODO(mesch): We must delete the story page too. MI4-1002
    });
  }

 private:
  SessionStorage* const session_storage_;  // Not owned.
  const fidl::StringPtr story_id_;
  StoryControllerImplMap* const story_controller_impls_;
  MessageQueueManager* const message_queue_manager_;
  const bool already_deleted_;  // True if called from OnChange();

  FXL_DISALLOW_COPY_AND_ASSIGN(DeleteStoryCall);
};

// 1. Ensure that the story data in the root page isn't dirty due to a crash
// 2. Retrieve the page specific to this story.
// 3. Return a controller for this story that contains the page pointer.
class StoryProviderImpl::GetControllerCall : public Operation<> {
 public:
  GetControllerCall(StoryProviderImpl* const story_provider_impl,
                    SessionStorage* const session_storage,
                    fidl::StringPtr story_id,
                    fidl::InterfaceRequest<StoryController> request)
      : Operation("StoryProviderImpl::GetControllerCall", [] {}),
        story_provider_impl_(story_provider_impl),
        session_storage_(session_storage),
        story_id_(story_id),
        request_(std::move(request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    // Use the existing controller, if possible.
    // This won't race against itself because it's managed by an operation
    // queue.
    auto i = story_provider_impl_->story_controller_impls_.find(story_id_);
    if (i != story_provider_impl_->story_controller_impls_.end()) {
      i->second.impl->Connect(std::move(request_));
      return;
    }

    session_storage_->GetStoryData(story_id_)->Then(
        [this, flow](modular::internal ::StoryDataPtr story_data) {
          if (!story_data) {
            return;
          }
          struct StoryControllerImplContainer container;
          container.impl = std::make_unique<StoryControllerImpl>(
              story_id_, session_storage_->ledger_client(),
              *story_data->story_page_id, story_provider_impl_);
          container.impl->Connect(std::move(request_));
          container.current_info = CloneOptional(story_data->story_info);
          story_provider_impl_->story_controller_impls_.emplace(
              story_id_, std::move(container));
        });
  }

  StoryProviderImpl* const story_provider_impl_;  // not owned
  SessionStorage* const session_storage_;         // not owned
  const fidl::StringPtr story_id_;
  fidl::InterfaceRequest<StoryController> request_;

  // Sub operations run in this queue.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GetControllerCall);
};

class StoryProviderImpl::StopAllStoriesCall : public Operation<> {
 public:
  StopAllStoriesCall(StoryProviderImpl* const story_provider_impl,
                     ResultCall result_call)
      : Operation("StoryProviderImpl::StopAllStoriesCall",
                  std::move(result_call)),
        story_provider_impl_(story_provider_impl) {}

 private:
  void Run() override {
    FlowToken flow{this};

    for (auto& it : story_provider_impl_->story_controller_impls_) {
      // Each callback has a copy of |flow| which only goes out-of-scope
      // once the story corresponding to |it| stops.
      //
      // TODO(mesch): If a DeleteCall is executing in front of
      // StopForTeardown(), then the StopCall in StopForTeardown() never
      // executes because the StoryController instance is deleted after the
      // DeleteCall finishes. This will then block unless it runs in a
      // timeout.
      it.second.impl->StopForTeardown([this, story_id = it.first, flow] {
        // It is okay to erase story_id because story provider binding has
        // been closed and this callback cannot be invoked synchronously.
        story_provider_impl_->story_controller_impls_.erase(story_id);
      });
    }
  }

  StoryProviderImpl* const story_provider_impl_;  // not owned

  FXL_DISALLOW_COPY_AND_ASSIGN(StopAllStoriesCall);
};

class StoryProviderImpl::StopStoryShellCall : public Operation<> {
 public:
  StopStoryShellCall(StoryProviderImpl* const story_provider_impl,
                     ResultCall result_call)
      : Operation("StoryProviderImpl::StopStoryShellCall",
                  std::move(result_call)),
        story_provider_impl_(story_provider_impl) {}

 private:
  void Run() override {
    FlowToken flow{this};
    if (story_provider_impl_->preloaded_story_shell_) {
      // Calling Teardown() below will branch |flow| into normal and timeout
      // paths. |flow| must go out of scope when either of the paths
      // finishes.
      FlowTokenHolder branch{flow};
      story_provider_impl_->preloaded_story_shell_->story_shell_app->Teardown(
          kBasicTimeout,
          [branch] { std::unique_ptr<FlowToken> flow = branch.Continue(); });
    }
  }

  StoryProviderImpl* const story_provider_impl_;  // not owned

  FXL_DISALLOW_COPY_AND_ASSIGN(StopStoryShellCall);
};

struct StoryProviderImpl::LinkPeer {
  std::unique_ptr<LedgerClient> ledger;
  std::unique_ptr<LinkImpl> link;
};

class StoryProviderImpl::GetLinkPeerCall : public Operation<> {
 public:
  GetLinkPeerCall(StoryProviderImpl* const impl,
                  SessionStorage* const session_storage,
                  fidl::StringPtr story_id,
                  fidl::VectorPtr<fidl::StringPtr> module_path,
                  fidl::StringPtr link_name,
                  fidl::InterfaceRequest<Link> request)
      : Operation("StoryProviderImpl::GetLinkPeerCall", [] {}),
        impl_(impl),
        session_storage_(session_storage),
        story_id_(story_id),
        module_path_(std::move(module_path)),
        link_name_(link_name),
        request_(std::move(request)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    session_storage_->GetStoryData(story_id_)->Then(
        [this, flow](modular::internal ::StoryDataPtr story_data) {
          if (!story_data) {
            // The InterfaceRequest<Link> will go out of scope, and the channel
            // closed with an error.
            return;
          }
          auto link_peer = std::make_unique<LinkPeer>();

          link_peer->ledger =
              session_storage_->ledger_client()->GetLedgerClientPeer();

          auto link_path = LinkPath::New();
          link_path->module_path = module_path_.Clone();
          link_path->link_name = link_name_;

          link_peer->link = std::make_unique<LinkImpl>(
              link_peer->ledger.get(), CloneStruct(*story_data->story_page_id),
              std::move(*link_path), nullptr);

          link_peer->link->Connect(std::move(request_));

          impl_->link_peers_.emplace_back(std::move(link_peer));

          // TODO(mesch): Set an orphaned handler so that link peers get dropped
          // earlier than at logout.
        });
  }

  StoryProviderImpl* const impl_;          // not owned
  SessionStorage* const session_storage_;  // not owned
  const fidl::StringPtr story_id_;
  const fidl::VectorPtr<fidl::StringPtr> module_path_;
  const fidl::StringPtr link_name_;
  fidl::InterfaceRequest<Link> request_;

  // Sub operations run in this queue.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GetLinkPeerCall);
};

StoryProviderImpl::StoryProviderImpl(
    Scope* const user_scope, std::string device_id,
    SessionStorage* const session_storage, AppConfig story_shell,
    const ComponentContextInfo& component_context_info,
    FocusProviderPtr focus_provider,
    UserIntelligenceProvider* const user_intelligence_provider,
    ModuleResolver* const module_resolver,
    PresentationProvider* const presentation_provider, const bool test)
    : user_scope_(user_scope),
      session_storage_(session_storage),
      device_id_(std::move(device_id)),
      story_shell_(std::move(story_shell)),
      test_(test),
      component_context_info_(component_context_info),
      user_intelligence_provider_(user_intelligence_provider),
      module_resolver_(module_resolver),
      presentation_provider_(presentation_provider),
      focus_provider_(std::move(focus_provider)),
      focus_watcher_binding_(this),
      weak_factory_(this) {
  session_storage_->set_on_story_deleted(
      [weak_ptr = weak_factory_.GetWeakPtr()](fidl::StringPtr story_id) {
        if (!weak_ptr)
          return;
        weak_ptr->OnStoryStorageDeleted(std::move(story_id));
      });
  session_storage_->set_on_story_updated(
      [weak_ptr = weak_factory_.GetWeakPtr()](
          fidl::StringPtr story_id, modular::internal ::StoryData story_data) {
        if (!weak_ptr)
          return;
        weak_ptr->OnStoryStorageUpdated(std::move(story_id),
                                        std::move(story_data));
      });

  focus_provider_->Watch(focus_watcher_binding_.NewBinding());
  if (!test_) {
    // As an optimization, since app startup time is long, we optimistically
    // load a story shell instance even if there are no stories that need it
    // yet. This can reduce the time to first frame.
    MaybeLoadStoryShellDelayed();
  }
}

StoryProviderImpl::~StoryProviderImpl() = default;

void StoryProviderImpl::Connect(fidl::InterfaceRequest<StoryProvider> request) {
  bindings_.AddBinding(this, std::move(request));
}

void StoryProviderImpl::StopAllStories(const std::function<void()>& callback) {
  operation_queue_.Add(new StopAllStoriesCall(this, callback));
}

void StoryProviderImpl::Teardown(const std::function<void()>& callback) {
  // Closing all binding to this instance ensures that no new messages come
  // in, though previous messages need to be processed. The stopping of
  // stories is done on |operation_queue_| since that must strictly happen
  // after all pending messgages have been processed.
  bindings_.CloseAll();
  operation_queue_.Add(new StopAllStoriesCall(this, [] {}));
  operation_queue_.Add(new StopStoryShellCall(this, callback));
}

// |StoryProvider|
void StoryProviderImpl::Watch(
    fidl::InterfaceHandle<StoryProviderWatcher> watcher) {
  auto watcher_ptr = watcher.Bind();
  for (const auto& item : story_controller_impls_) {
    const auto& container = item.second;
    watcher_ptr->OnChange(CloneStruct(*container.current_info),
                          container.impl->GetStoryState());
  }
  watchers_.AddInterfacePtr(std::move(watcher_ptr));
}

// |StoryProvider|
void StoryProviderImpl::Duplicate(
    fidl::InterfaceRequest<StoryProvider> request) {
  Connect(std::move(request));
}

std::unique_ptr<AppClient<Lifecycle>> StoryProviderImpl::StartStoryShell(
    fidl::InterfaceRequest<fuchsia::ui::views_v1_token::ViewOwner> request) {
  MaybeLoadStoryShell();

  auto preloaded_story_shell = std::move(preloaded_story_shell_);
  auto app_client = std::move(preloaded_story_shell->story_shell_app);

  proxies_.Connect(std::move(preloaded_story_shell->story_shell_view),
                   std::move(request));

  // Kickoff another StoryShell, to make it faster for next story. We
  // optimize even further by delaying the loading of the next story shell
  // instance by waiting a few seconds.
  if (!test_) {
    MaybeLoadStoryShellDelayed();
  }

  return app_client;
}

void StoryProviderImpl::MaybeLoadStoryShellDelayed() {
  async::PostDelayedTask(
      async_get_default(),
      [weak_this = weak_factory_.GetWeakPtr()] {
        if (weak_this) {
          weak_this->operation_queue_.Add(new SyncCall([weak_this] {
            if (weak_this) {
              weak_this->MaybeLoadStoryShell();
            }
          }));
        }
      },
      zx::sec(5));
}

void StoryProviderImpl::MaybeLoadStoryShell() {
  if (preloaded_story_shell_) {
    return;
  }

  auto story_shell_app = std::make_unique<AppClient<Lifecycle>>(
      user_scope_->GetLauncher(), CloneStruct(story_shell_));

  // CreateView must be called in order to get the Flutter application to
  // run

  fuchsia::ui::views_v1::ViewProviderPtr view_provider;
  story_shell_app->services().ConnectToService(view_provider.NewRequest());

  fuchsia::ui::views_v1_token::ViewOwnerPtr story_shell_view;
  view_provider->CreateView(story_shell_view.NewRequest(), nullptr);

  preloaded_story_shell_ =
      std::make_unique<StoryShellConnection>(StoryShellConnection{
          std::move(story_shell_app), std::move(story_shell_view)});
}

// |StoryProvider|
void StoryProviderImpl::CreateStory(fidl::StringPtr module_url,
                                    CreateStoryCallback callback) {
  FXL_LOG(INFO) << "CreateStory() " << module_url;
  operation_queue_.Add(new CreateStoryCall(session_storage_, this, module_url,
                                           nullptr /* extra_info */,
                                           nullptr /* root_json */, callback));
}

// |StoryProvider|
void StoryProviderImpl::CreateStoryWithInfo(
    fidl::StringPtr module_url, fidl::VectorPtr<StoryInfoExtraEntry> extra_info,
    fidl::StringPtr root_json, CreateStoryWithInfoCallback callback) {
  FXL_LOG(INFO) << "CreateStoryWithInfo() " << module_url << " " << root_json;
  operation_queue_.Add(new CreateStoryCall(session_storage_, this, module_url,
                                           std::move(extra_info),
                                           std::move(root_json), callback));
}

// |StoryProvider|
void StoryProviderImpl::DeleteStory(fidl::StringPtr story_id,
                                    DeleteStoryCallback callback) {
  operation_queue_.Add(
      new DeleteStoryCall(session_storage_, story_id, &story_controller_impls_,
                          component_context_info_.message_queue_manager,
                          false /* already_deleted */, callback));
}

// |StoryProvider|
void StoryProviderImpl::GetStoryInfo(fidl::StringPtr story_id,
                                     GetStoryInfoCallback callback) {
  auto on_run = Future<>::Create();
  auto done =
      on_run
          ->AsyncMap([this, story_id] {
            return session_storage_->GetStoryData(story_id);
          })
          ->Map(
              [](modular::internal ::StoryDataPtr story_data) -> StoryInfoPtr {
                if (!story_data) {
                  return nullptr;
                }
                return fidl::MakeOptional(std::move(story_data->story_info));
              });
  operation_queue_.Add(WrapFutureAsOperation(
      on_run, done, callback, "StoryProviderImpl::GetStoryInfo"));
}

// Called by StoryControllerImpl on behalf of ModuleContextImpl
void StoryProviderImpl::RequestStoryFocus(fidl::StringPtr story_id) {
  FXL_LOG(INFO) << "RequestStoryFocus() " << story_id;
  focus_provider_->Request(story_id);
}

void StoryProviderImpl::NotifyStoryStateChange(fidl::StringPtr story_id,
                                               const StoryState story_state) {
  auto i = story_controller_impls_.find(story_id);

  if (i == story_controller_impls_.end()) {
    // If this call arrives while DeleteStory() is in progress, the story
    // controller might already be gone from here.
    return;
  }

  const StoryInfo* const story_info = i->second.current_info.get();
  NotifyStoryWatchers(story_info, story_state);
}

// |StoryProvider|
void StoryProviderImpl::GetController(
    fidl::StringPtr story_id, fidl::InterfaceRequest<StoryController> request) {
  operation_queue_.Add(new GetControllerCall(this, session_storage_, story_id,
                                             std::move(request)));
}

// |StoryProvider|
void StoryProviderImpl::PreviousStories(PreviousStoriesCallback callback) {
  auto on_run = Future<>::Create();
  auto done =
      on_run->AsyncMap([this] { return session_storage_->GetAllStoryData(); })
          ->Map([](fidl::VectorPtr<modular::internal ::StoryData>
                       all_story_data) {
            FXL_DCHECK(all_story_data);
            auto result = fidl::VectorPtr<fuchsia::modular::StoryInfo>::New(0);

            for (auto& story_data : *all_story_data) {
              result.push_back(std::move(story_data.story_info));
            }
            return result;
          });
  operation_queue_.Add(WrapFutureAsOperation(
      on_run, done, callback, "StoryProviderImpl::PreviousStories"));
}

// |StoryProvider|
void StoryProviderImpl::RunningStories(RunningStoriesCallback callback) {
  auto stories = fidl::VectorPtr<fidl::StringPtr>::New(0);
  for (const auto& impl_container : story_controller_impls_) {
    if (impl_container.second.impl->IsRunning()) {
      stories.push_back(impl_container.second.impl->GetStoryId());
    }
  }
  callback(std::move(stories));
}

void StoryProviderImpl::OnStoryStorageUpdated(
    fidl::StringPtr story_id, modular::internal ::StoryData story_data) {
  // HACK(jimbe) We don't have the page and it's expensive to get it, so
  // just mark it as STOPPED. We know it's not running or we'd have a
  // StoryController.
  //
  // If we have a StoryControllerImpl for this story id, update our cached
  // StoryInfo.
  StoryState state = StoryState::STOPPED;
  auto i = story_controller_impls_.find(story_data.story_info.id);
  if (i != story_controller_impls_.end()) {
    state = i->second.impl->GetStoryState();
    i->second.current_info = CloneOptional(story_data.story_info);
  }

  NotifyStoryWatchers(&story_data.story_info, state);
}

void StoryProviderImpl::OnStoryStorageDeleted(fidl::StringPtr story_id) {
  for (const auto& i : watchers_.ptrs()) {
    (*i)->OnDelete(story_id);
  }

  // NOTE: DeleteStoryCall is used here, as well as in DeleteStory(). In this
  // case, either another device deleted the story, or we did and the Ledger is
  // now notifying us. In this case, we pass |already_deleted = true| so that we
  // don't ask to delete the story data again.
  operation_queue_.Add(
      new DeleteStoryCall(session_storage_, story_id, &story_controller_impls_,
                          component_context_info_.message_queue_manager,
                          true /* already_deleted */, [] {}));
}

// |FocusWatcher|
void StoryProviderImpl::OnFocusChange(FocusInfoPtr info) {
  if (info->device_id.get() != device_id_) {
    return;
  }

  if (info->focused_story_id.is_null()) {
    return;
  }

  auto i = story_controller_impls_.find(info->focused_story_id.get());
  if (i == story_controller_impls_.end()) {
    FXL_LOG(ERROR) << "Story controller not found for focused story "
                   << info->focused_story_id;
    return;
  }

  // Last focus time is recorded in the ledger, and story provider watchers
  // are notified through watching SessionStorage.
  auto on_run = Future<>::Create();
  // TODO(thatguy): WeakThen() here is an attempt to fix a non-determinstic
  // crash that appeared to be happening in the Then() lambda.
  auto done = on_run->WeakThen(weak_factory_.GetWeakPtr(),
                               [this, story_id = info->focused_story_id] {
                                 session_storage_->UpdateLastFocusedTimestamp(
                                     story_id, zx_clock_get(ZX_CLOCK_UTC));
                               });
  std::function<void()> callback = [] {};
  operation_queue_.Add(WrapFutureAsOperation(
      on_run, done, callback, "StoryProviderImpl::OnFocusChange"));
}

void StoryProviderImpl::NotifyStoryWatchers(const StoryInfo* const story_info,
                                            const StoryState story_state) {
  for (const auto& i : watchers_.ptrs()) {
    (*i)->OnChange(CloneStruct(*story_info), story_state);
  }
}

void StoryProviderImpl::GetLinkPeer(
    fidl::StringPtr story_id, fidl::VectorPtr<fidl::StringPtr> module_path,
    fidl::StringPtr link_name, fidl::InterfaceRequest<Link> request) {
  operation_queue_.Add(new GetLinkPeerCall(this, session_storage_, story_id,
                                           std::move(module_path), link_name,
                                           std::move(request)));
}

void StoryProviderImpl::GetPresentation(
    fidl::StringPtr story_id,
    fidl::InterfaceRequest<presentation::Presentation> request) {
  presentation_provider_->GetPresentation(std::move(story_id),
                                          std::move(request));
}

void StoryProviderImpl::WatchVisualState(
    fidl::StringPtr story_id,
    fidl::InterfaceHandle<StoryVisualStateWatcher> watcher) {
  presentation_provider_->WatchVisualState(std::move(story_id),
                                           std::move(watcher));
}

}  // namespace modular
}  // namespace fuchsia
