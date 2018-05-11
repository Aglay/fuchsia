// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A Module that serves as the recipe in the example story, i.e. that
// creates other Modules in the story.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/modular_calculator_example.h>

#include "lib/app/cpp/application_context.h"
#include "lib/app/cpp/connect.h"
#include "lib/app_driver/cpp/app_driver.h"
#include <fuchsia/cpp/modular.h>
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/examples/counter_cpp/store.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/single_service_app.h"

namespace {

using modular_calculator_example::Adder;
using modular::to_array;

// JSON data
constexpr char kInitialJson[] =
    "{     \"@type\" : \"http://schema.domokit.org/PingPongPacket\","
    "      \"http://schema.domokit.org/counter\" : 0,"
    "      \"http://schema.org/sender\" : \"RecipeImpl\""
    "}";

// Ledger keys
constexpr char kLedgerCounterKey[] = "counter_key";

// Implementation of the LinkWatcher service that forwards each document
// changed in one Link instance to a second Link instance.
class LinkForwarder : modular::LinkWatcher {
 public:
  LinkForwarder(modular::Link* const src, modular::Link* const dst)
      : src_binding_(this), src_(src), dst_(dst) {
    src_->Watch(src_binding_.NewBinding());
  }

  void Notify(fidl::StringPtr json) override {
    // We receive an initial update when the Link initializes. It's "null"
    // (meaning the value of the json string is the four letters n-u-l-l)
    // if this is a new session, or it has json data if it's a restored session.
    // In either case, it should be ignored, otherwise we can get multiple
    // messages traveling at the same time.
    if (!initial_update_ && json->size() > 0) {
      dst_->Set(nullptr, json);
    }
    initial_update_ = false;
  }

 private:
  fidl::Binding<modular::LinkWatcher> src_binding_;
  modular::Link* const src_;
  modular::Link* const dst_;
  bool initial_update_ = true;

  FXL_DISALLOW_COPY_AND_ASSIGN(LinkForwarder);
};

class ModuleMonitor : modular::ModuleWatcher {
 public:
  ModuleMonitor(modular::ModuleController* const module_client)
      : binding_(this) {
    module_client->Watch(binding_.NewBinding());
  }

  void OnStateChange(modular::ModuleState new_state) override {
    FXL_LOG(INFO) << "RecipeImpl " << new_state;
  }

 private:
  fidl::Binding<modular::ModuleWatcher> binding_;
  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleMonitor);
};

class DeviceMapMonitor : modular::DeviceMapWatcher {
 public:
  DeviceMapMonitor(modular::DeviceMap* const device_map,
                   std::vector<modular::DeviceMapEntry> devices)
      : binding_(this), devices_(std::move(devices)) {
    device_map->WatchDeviceMap(binding_.NewBinding());
  }

  void OnDeviceMapChange(modular::DeviceMapEntry entry) override {
    FXL_LOG(INFO) << "OnDeviceMapChange() " << entry.name << " "
                  << entry.profile;
    for (const auto& device : devices_) {
      if (entry.device_id == device.device_id)
        return;
    }
    FXL_CHECK(false);
  }

 private:
  fidl::Binding<DeviceMapWatcher> binding_;
  std::vector<modular::DeviceMapEntry> devices_;
  FXL_DISALLOW_COPY_AND_ASSIGN(DeviceMapMonitor);
};

class AdderImpl : public modular_calculator_example::Adder {
 public:
  AdderImpl() = default;

 private:
  // |Adder| impl:
  void Add(int32_t a, int32_t b, AddCallback result) override {
    result(a + b);
  }

  FXL_DISALLOW_COPY_AND_ASSIGN(AdderImpl);
};

// Module implementation that acts as a recipe. There is one instance
// per application; the story runner creates new application instances
// to run more module instances.
class RecipeApp : public modular::SingleServiceApp<modular::Module> {
 public:
  RecipeApp(component::ApplicationContext* const application_context)
      : SingleServiceApp(application_context) {}

  ~RecipeApp() override = default;

 private:
  // |Module|
  void Initialize(
      fidl::InterfaceHandle<modular::ModuleContext> module_context,
      fidl::InterfaceRequest<component::ServiceProvider> /*outgoing_services*/)
      override {
    module_context_.Bind(std::move(module_context));
    module_context_->GetLink(nullptr, link_.NewRequest());

    // Read initial Link data. We expect the shell to tell us what it
    // is.
    link_->Get(nullptr, [this](const fidl::StringPtr& json) {
      rapidjson::Document doc;
      doc.Parse(json);
      if (doc.HasParseError()) {
        FXL_LOG(ERROR) << "Recipe Module Link has invalid JSON: " << json;
      } else {
        FXL_LOG(INFO) << "Recipe Module Link: "
                      << modular::JsonValueToPrettyString(doc);
      }
    });

    constexpr char kModule1Link[] = "module1";
    constexpr char kModule2Link[] = "module2";
    module_context_->GetLink(kModule1Link, module1_link_.NewRequest());
    module_context_->GetLink(kModule2Link, module2_link_.NewRequest());

    modular::Intent intent;
    intent.action.handler = "example_module1";
    modular::IntentParameterData parameter_data;
    parameter_data.set_link_name(kModule1Link);
    modular::IntentParameter parameter;
    parameter.name = "theOneLink";
    parameter.data = std::move(parameter_data);
    intent.parameters.push_back(std::move(parameter));
    component::ServiceProviderPtr services_from_module1;
    module_context_->StartModule("module1", std::move(intent),
                                 services_from_module1.NewRequest(),
                                 module1_.NewRequest(), nullptr,
                                 [](const modular::StartModuleStatus&) {});

    // Consume services from Module 1.
    auto multiplier_service =
        component::ConnectToService<modular_calculator_example::Multiplier>(
            services_from_module1.get());
    multiplier_service.set_error_handler([] {
      FXL_CHECK(false)
          << "Uh oh, Connection to Multiplier closed by the module 1.";
    });
    multiplier_service->Multiply(
        4, 4,
        fxl::MakeCopyable([multiplier_service =
                               std::move(multiplier_service)](int32_t result) {
          FXL_CHECK(result == 16);
          FXL_LOG(INFO) << "Incoming Multiplier service: 4 * 4 is 16.";
        }));

    intent = modular::Intent();
    intent.action.handler = "example_module2";
    parameter_data = modular::IntentParameterData();
    parameter_data.set_link_name(kModule2Link);
    parameter = modular::IntentParameter();
    parameter.name = "theOneLink";
    parameter.data = std::move(parameter_data);
    intent.parameters.push_back(std::move(parameter));
    component::ServiceProviderPtr services_from_module2;
    module_context_->StartModule("module2", std::move(intent), nullptr,
                                 module2_.NewRequest(), nullptr,
                                 [](const modular::StartModuleStatus&) {});

    connections_.emplace_back(
        new LinkForwarder(module1_link_.get(), module2_link_.get()));
    connections_.emplace_back(
        new LinkForwarder(module2_link_.get(), module1_link_.get()));

    // Also connect with the root link, to create change notifications
    // the user shell can react on.
    connections_.emplace_back(
        new LinkForwarder(module1_link_.get(), link_.get()));
    connections_.emplace_back(
        new LinkForwarder(module2_link_.get(), link_.get()));

    module_monitors_.emplace_back(new ModuleMonitor(module1_.get()));
    module_monitors_.emplace_back(new ModuleMonitor(module2_.get()));

    module1_link_->Get(nullptr, [this](const fidl::StringPtr& json) {
      if (json == "null") {
        // This must come last, otherwise LinkConnection gets a
        // notification of our own write because of the "send
        // initial values" code.
        std::vector<fidl::StringPtr> segments{modular_example::kJsonSegment,
                                              modular_example::kDocId};
        module1_link_->Set(fidl::VectorPtr<fidl::StringPtr>(segments),
                           kInitialJson);
      } else {
        link_->Get(nullptr, [this](const fidl::StringPtr& json) {
          // There is a possiblity that on re-inflation we start with a
          // deadlocked state such that neither of the child modules make
          // progress. This can happen because there is no synchronization
          // between |LinkForwarder| and |ModuleMonitor|. So we ensure that
          // ping-pong can re-start.
          module1_link_->Set(nullptr, json);
          module2_link_->Set(nullptr, json);
        });
      }
    });

    module_context_->Ready();

    // This snippet of code demonstrates using the module's Ledger. Each time
    // this module is initialized, it updates a counter in the root page.
    // 1. Get the module's ledger.
    module_context_->GetComponentContext(component_context_.NewRequest());
    component_context_->GetLedger(
        module_ledger_.NewRequest(), [this](ledger::Status status) {
          FXL_CHECK(status == ledger::Status::OK);
          // 2. Get the root page of the ledger.
          module_ledger_->GetRootPage(
              module_root_page_.NewRequest(), [this](ledger::Status status) {
                FXL_CHECK(status == ledger::Status::OK);
                // 3. Get a snapshot of the root page.
                module_root_page_->GetSnapshot(
                    page_snapshot_.NewRequest(), nullptr, nullptr,
                    [this](ledger::Status status) {
                      FXL_CHECK(status == ledger::Status::OK);
                      // 4. Read the counter from the root page.
                      page_snapshot_->Get(
                          to_array(kLedgerCounterKey),
                          [this](ledger::Status status,
                                 mem::BufferPtr value) {
                            // 5. If counter doesn't exist, initialize.
                            // Otherwise, increment.
                            if (status == ledger::Status::KEY_NOT_FOUND) {
                              FXL_LOG(INFO) << "No counter in root page. "
                                               "Initializing to 1.";
                              fidl::VectorPtr<uint8_t> data;
                              data.push_back(1);
                              module_root_page_->Put(
                                  to_array(kLedgerCounterKey), std::move(data),
                                  [](ledger::Status status) {
                                    FXL_CHECK(status == ledger::Status::OK);
                                  });
                            } else {
                              FXL_CHECK(status == ledger::Status::OK);
                              std::string counter_data;
                              bool conversion =
                                  fsl::StringFromVmo(*value, &counter_data);
                              FXL_DCHECK(conversion);
                              FXL_LOG(INFO)
                                  << "Retrieved counter from root page: "
                                  << static_cast<uint32_t>(counter_data[0])
                                  << ". Incrementing.";
                              counter_data[0]++;
                              module_root_page_->Put(
                                  to_array(kLedgerCounterKey),
                                  to_array(counter_data),
                                  [](ledger::Status status) {
                                    FXL_CHECK(status == ledger::Status::OK);
                                  });
                            }
                          });
                    });
              });
        });

    device_map_ = application_context()
                      ->ConnectToEnvironmentService<modular::DeviceMap>();

    device_map_->Query([this](fidl::VectorPtr<modular::DeviceMapEntry> devices) {
      FXL_LOG(INFO) << "Devices from device_map_->Query():";
      for (modular::DeviceMapEntry device : devices.take()) {
        FXL_LOG(INFO) << " - " << device.name;
        device_map_entries_.emplace_back(std::move(device));
      }

      device_map_monitor_.reset(new DeviceMapMonitor(
          device_map_.get(), std::move(device_map_entries_)));
      device_map_->SetCurrentDeviceProfile("5");
    });
  }

  modular::LinkPtr link_;
  modular::ModuleContextPtr module_context_;

  // This is a ServiceProvider we expose to one of our child modules, to
  // demonstrate the use of a service exchange.
  fidl::BindingSet<modular_calculator_example::Adder> adder_clients_;
  AdderImpl adder_service_;
  component::ServiceNamespace outgoing_services_;

  // The following ledger interfaces are stored here to make life-time
  // management easier when chaining together lambda callbacks.
  modular::ComponentContextPtr component_context_;
  ledger::LedgerPtr module_ledger_;
  ledger::PagePtr module_root_page_;
  ledger::PageSnapshotPtr page_snapshot_;

  modular::ModuleControllerPtr module1_;
  modular::LinkPtr module1_link_;

  modular::ModuleControllerPtr module2_;
  modular::LinkPtr module2_link_;

  std::vector<std::unique_ptr<LinkForwarder>> connections_;
  std::vector<std::unique_ptr<ModuleMonitor>> module_monitors_;

  modular::DeviceMapPtr device_map_;
  std::vector<modular::DeviceMapEntry> device_map_entries_;
  std::unique_ptr<DeviceMapMonitor> device_map_monitor_;

  FXL_DISALLOW_COPY_AND_ASSIGN(RecipeApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;

  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::AppDriver<RecipeApp> driver(
      app_context->outgoing().deprecated_services(),
      std::make_unique<RecipeApp>(app_context.get()),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
