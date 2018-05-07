// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/images.h>
#include <fuchsia/cpp/views_v1.h>
#include <fuchsia/cpp/views_v1_token.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "lib/ui/view_framework/base_view.h"
#include "peridot/examples/counter_cpp/store.h"
#include "peridot/lib/fidl/single_service_app.h"

namespace {

constexpr float kBackgroundElevation = 0.f;
constexpr float kSquareElevation = 8.f;
constexpr int kTickRotationDegrees = 45;
constexpr int kAnimationDelayInMs = 50;

constexpr char kModuleName[] = "Module2Impl";

class Module2View : public mozart::BaseView {
 public:
  explicit Module2View(
      modular_example::Store* const store,
      views_v1::ViewManagerPtr view_manager,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request)
      : BaseView(std::move(view_manager),
                 std::move(view_owner_request),
                 "Module2Impl"),
        store_(store),
        background_node_(session()),
        square_node_(session()) {
    scenic_lib::Material background_material(session());
    background_material.SetColor(0x67, 0x3a, 0xb7, 0xff);  // Deep Purple 500
    background_node_.SetMaterial(background_material);
    parent_node().AddChild(background_node_);

    scenic_lib::Material square_material(session());
    square_material.SetColor(0x29, 0x79, 0xff, 0xff);  // Blue A400
    square_node_.SetMaterial(square_material);
    parent_node().AddChild(square_node_);
  }

  ~Module2View() override = default;

 private:
  // Copied from
  // https://fuchsia.googlesource.com/garnet/+/master/examples/ui/spinning_square/spinning_square_view.cc
  // |BaseView|:
  void OnSceneInvalidated(
      images::PresentationInfo /*presentation_info*/) override {
    if (!has_logical_size()) {
      return;
    }

    const float center_x = logical_size().width * .5f;
    const float center_y = logical_size().height * .5f;
    const float square_size =
        std::min(logical_size().width, logical_size().height) * .6f;
    const float angle =
        kTickRotationDegrees * store_->counter.counter * M_PI * 2;

    scenic_lib::Rectangle background_shape(session(), logical_size().width,
                                           logical_size().height);
    background_node_.SetShape(background_shape);
    background_node_.SetTranslation(
        (float[]){center_x, center_y, kBackgroundElevation});

    scenic_lib::Rectangle square_shape(session(), square_size, square_size);
    square_node_.SetShape(square_shape);
    square_node_.SetTranslation(
        (float[]){center_x, center_y, kSquareElevation});
    square_node_.SetRotation(
        (float[]){0.f, 0.f, sinf(angle * .5f), cosf(angle * .5f)});
  }

  modular_example::Store* const store_;
  scenic_lib::ShapeNode background_node_;
  scenic_lib::ShapeNode square_node_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Module2View);
};

// Module implementation that acts as a leaf module. It implements Module.
class Module2App : public modular::SingleServiceApp<modular::Module> {
 public:
  explicit Module2App(component::ApplicationContext* const application_context)
      : SingleServiceApp(application_context),
        store_(kModuleName),
        weak_ptr_factory_(this) {
    store_.AddCallback([this] {
      if (view_) {
        view_->InvalidateScene();
      }
    });
    store_.AddCallback([this] { IncrementCounterAction(); });
  }

  ~Module2App() override = default;

  // |SingleServiceApp|
  void Terminate(std::function<void()> done) override {
    store_.Stop();
    done();
  }

 private:
  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      fidl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    view_ = std::make_unique<Module2View>(
        &store_,
        application_context()
            ->ConnectToEnvironmentService<views_v1::ViewManager>(),
        std::move(view_owner_request));
  }

  // |Module|
  void Initialize(
      fidl::InterfaceHandle<modular::ModuleContext> module_context,
      fidl::InterfaceRequest<component::ServiceProvider> /*outgoing_services*/)
      override {
    module_context_.Bind(std::move(module_context));
    modular::LinkPtr link;
    module_context_->GetLink("theOneLink", link.NewRequest());
    store_.Initialize(std::move(link));

    module_context_->Ready();
  }

  void IncrementCounterAction() {
    if (store_.counter.sender == kModuleName || store_.counter.counter > 11) {
      return;
    }

    fxl::WeakPtr<Module2App> module_ptr = weak_ptr_factory_.GetWeakPtr();
    async::PostDelayedTask(
        async_get_default(),
        [this, module_ptr] {
          if (!module_ptr.get() || store_.terminating()) {
            return;
          }

          store_.counter.sender = kModuleName;
          store_.counter.counter += 1;

          FXL_LOG(INFO) << "Module2Impl COUNT " << store_.counter.counter;

          store_.MarkDirty();
          store_.ModelChanged();
        },
        zx::msec(kAnimationDelayInMs));
  }

  std::unique_ptr<Module2View> view_;
  fidl::InterfacePtr<modular::ModuleContext> module_context_;
  modular_example::Store store_;

  // Note: This should remain the last member so it'll be destroyed and
  // invalidate its weak pointers before any other members are destroyed.
  fxl::WeakPtrFactory<Module2App> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Module2App);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;

  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<Module2App> driver(
      app_context->outgoing().deprecated_services(),
      std::make_unique<Module2App>(app_context.get()),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
