// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/modular_calculator_example.h>
#include <fuchsia/cpp/images.h>
#include <fuchsia/cpp/views_v1.h>
#include <fuchsia/cpp/views_v1_token.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "lib/app/cpp/application_context.h"
#include "lib/app/cpp/connect.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "lib/fxl/time/time_delta.h"
#include "lib/ui/view_framework/base_view.h"
#include "peridot/examples/counter_cpp/store.h"
#include "peridot/lib/fidl/single_service_app.h"

namespace {

constexpr float kBackgroundElevation = 0.f;
constexpr float kSquareElevation = 8.f;
constexpr int kTickRotationDegrees = 45;
constexpr int kAnimationDelayInMs = 50;

constexpr char kModuleName[] = "Module1Impl";

class Module1View : public mozart::BaseView {
 public:
  explicit Module1View(
      modular_example::Store* const store,
      views_v1::ViewManagerPtr view_manager,
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request)
      : BaseView(std::move(view_manager),
                 std::move(view_owner_request),
                 kModuleName),
        store_(store),
        background_node_(session()),
        square_node_(session()) {
    scenic_lib::Material background_material(session());
    background_material.SetColor(0x67, 0x3a, 0xb7, 0xff);  // Deep Purple 500
    background_node_.SetMaterial(background_material);
    parent_node().AddChild(background_node_);

    scenic_lib::Material square_material(session());
    square_material.SetColor(0x00, 0xe6, 0x76, 0xff);  // Green A400
    square_node_.SetMaterial(square_material);
    parent_node().AddChild(square_node_);
  }

  ~Module1View() override = default;

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

  FXL_DISALLOW_COPY_AND_ASSIGN(Module1View);
};

class MultiplierImpl : public modular_calculator_example::Multiplier {
 public:
  MultiplierImpl() = default;

 private:
  // |Multiplier|
  void Multiply(int32_t a, int32_t b, MultiplyCallback result) override {
    result(a * b);
  }

  FXL_DISALLOW_COPY_AND_ASSIGN(MultiplierImpl);
};

// Module implementation that acts as a leaf module. It implements Module.
class Module1App : modular::SingleServiceApp<modular::Module> {
 public:
  explicit Module1App(component::ApplicationContext* const application_context)
      : SingleServiceApp(application_context),
        store_(kModuleName),
        weak_ptr_factory_(this) {
    // TODO(mesch): The callbacks seem to have a sequential relationship.
    // It seems to me there should be a single callback that does all three
    // things in a sequence. Since the result InvalidateScene() happens only
    // (asynchonously) later, the order here really doesn't matter, but it's
    // only accidentally so.
    store_.AddCallback([this] {
      if (view_) {
        view_->InvalidateScene();
      }
    });
    store_.AddCallback([this] { IncrementCounterAction(); });
    store_.AddCallback([this] { CheckForDone(); });
  }

  ~Module1App() override = default;

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
    view_ = std::make_unique<Module1View>(
        &store_,
        application_context()
            ->ConnectToEnvironmentService<views_v1::ViewManager>(),
        std::move(view_owner_request));
  }

  // |Module|
  void Initialize(fidl::InterfaceHandle<modular::ModuleContext> module_context,
                  fidl::InterfaceRequest<component::ServiceProvider>
                      outgoing_services) override {
    FXL_CHECK(outgoing_services.is_valid());

    module_context_.Bind(std::move(module_context));
    modular::LinkPtr link;
    module_context_->GetLink("theOneLink", link.NewRequest());
    store_.Initialize(std::move(link));

    // Provide services to the recipe module.
    outgoing_services_.AddBinding(std::move(outgoing_services));
    outgoing_services_.AddService<modular_calculator_example::Multiplier>(
        [this](fidl::InterfaceRequest<modular_calculator_example::Multiplier> req) {
          multiplier_clients_.AddBinding(&multiplier_service_, std::move(req));
        });

    module_context_->Ready();
  }

  void CheckForDone() {
    if (store_.counter.counter > 10) {
      module_context_->Done();
    }
  }

  void IncrementCounterAction() {
    if (store_.counter.sender == kModuleName || store_.counter.counter > 10) {
      return;
    }

    fxl::WeakPtr<Module1App> module_ptr = weak_ptr_factory_.GetWeakPtr();
    async::PostDelayedTask(
        async_get_default(),
        [this, module_ptr] {
          if (!module_ptr.get() || store_.terminating()) {
            return;
          }

          store_.counter.sender = kModuleName;
          store_.counter.counter += 1;

          FXL_LOG(INFO) << "Module1Impl COUNT " << store_.counter.counter;

          store_.MarkDirty();
          store_.ModelChanged();
        },
        zx::msec(kAnimationDelayInMs));
  }

  // This is a ServiceProvider we expose to our parent (recipe) module, to
  // demonstrate the use of a service exchange.
  fidl::BindingSet<modular_calculator_example::Multiplier> multiplier_clients_;
  MultiplierImpl multiplier_service_;
  component::ServiceNamespace outgoing_services_;

  std::unique_ptr<Module1View> view_;
  modular::ModuleContextPtr module_context_;
  modular_example::Store store_;

  // Note: This should remain the last member so it'll be destroyed and
  // invalidate its weak pointers before any other members are destroyed.
  fxl::WeakPtrFactory<Module1App> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Module1App);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;

  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<Module1App> driver(
      app_context->outgoing().deprecated_services(),
      std::make_unique<Module1App>(app_context.get()),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
