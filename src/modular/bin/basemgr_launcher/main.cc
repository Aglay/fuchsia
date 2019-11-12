// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fit/result.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/vfs/cpp/pseudo_file.h>

#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <src/lib/files/glob.h>
#include <src/lib/fxl/command_line.h>
#include <src/lib/fxl/strings/string_printf.h>
#include <src/modular/lib/modular_config/modular_config.h>
#include <src/modular/lib/modular_config/modular_config_constants.h>

constexpr char kBasemgrUrl[] = "fuchsia-pkg://fuchsia.com/basemgr#meta/basemgr.cmx";
constexpr char kBasemgrHubGlob[] = "/hub/c/basemgr.cmx/*";
constexpr char kBasemgrRegex[] = "/basemgr.cmx/(\\d+)";

std::string FindDebugServicesForPath(const char* glob_str, const char* regex_str) {
  glob_t globbuf;
  FXL_CHECK(glob(glob_str, 0, nullptr, &globbuf) == 0);
  std::regex name_regex(regex_str);
  std::string service_path = globbuf.gl_pathv[0];
  std::smatch match;
  FXL_CHECK(std::regex_search(service_path, match, name_regex)) << service_path;
  globfree(&globbuf);
  return service_path;
}

void ShutdownBasemgr() {
  // Get a connection to basemgr in order to shut it down.
  std::string service_path = FindDebugServicesForPath(kBasemgrHubGlob, kBasemgrRegex);

  fuchsia::modular::internal::BasemgrDebugPtr basemgr;
  auto request = basemgr.NewRequest().TakeChannel();
  if (fdio_service_connect(service_path.c_str(), request.get()) != ZX_OK) {
    FXL_LOG(FATAL) << "Could not connect to basemgr service in " << service_path;
  }

  basemgr->Shutdown();
}

std::unique_ptr<vfs::PseudoDir> CreateConfigPseudoDir(std::string config_str) {
  // Read the configuration file in from stdin.
  if (config_str.empty()) {
    std::string line;
    while (getline(std::cin, line)) {
      config_str += line;
    }
  }

  auto dir = std::make_unique<vfs::PseudoDir>();
  dir->AddEntry(modular_config::kStartupConfigFilePath,
                std::make_unique<vfs::PseudoFile>(
                    config_str.length(), [config_str = std::move(config_str)](
                                             std::vector<uint8_t>* out, size_t /*unused*/) {
                      std::copy(config_str.begin(), config_str.end(), std::back_inserter(*out));
                      return ZX_OK;
                    }));
  return dir;
}

std::string GetUsage() {
  return R"(A thin wrapper that takes a config file from stdin and maps it to
/config_override/data/startup.config for a new basemgr instance.

  Usage:

cat myconfig.json | fx shell basemgr_launcher)";
}

/// Parses configurations from command line into a string. Uses default values if no configuration
/// is provided. Processes arguments if they only contain a base shell url, otherwise returns usage.
fit::result<std::string, std::string> GetConfigFromArgs(fxl::CommandLine command_line) {
  auto config_reader = modular::ModularConfigReader(/*config=*/"{}");
  fuchsia::modular::session::BasemgrConfig basemgr_config = config_reader.GetBasemgrConfig();
  fuchsia::modular::session::SessionmgrConfig sessionmgr_config =
      config_reader.GetSessionmgrConfig();

  for (auto opt : command_line.options()) {
    if (opt.name == modular_config::kBaseShell) {
      basemgr_config.mutable_base_shell()->mutable_app_config()->set_url(opt.value);
    } else {
      return fit::error<std::string>(GetUsage());
    }
  }

  std::string config_str =
      modular::ModularConfigReader::GetConfigAsString(&basemgr_config, &sessionmgr_config);
  return fit::ok(config_str);
}

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  bool basemgr_is_running = files::Glob(kBasemgrHubGlob).size() != 0;
  if (basemgr_is_running) {
    ShutdownBasemgr();
  }

  std::string config_str = "";
  if (argc > 1) {
    const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
    auto config = GetConfigFromArgs(std::move(command_line));
    if (config.is_error()) {
      std::cout << config.error().c_str() << std::endl;
      return 1;
    } else {
      config_str = config.value();
    }
  }

  // Create the pseudo directory with our config "file" mapped to
  // kConfigFilename.
  auto config_dir = CreateConfigPseudoDir(config_str);
  fidl::InterfaceHandle<fuchsia::io::Directory> dir_handle;
  config_dir->Serve(fuchsia::io::OPEN_RIGHT_READABLE, dir_handle.NewRequest().TakeChannel());

  // Build a LaunchInfo with the config directory above mapped to
  // /config_override/data.
  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = kBasemgrUrl;
  launch_info.flat_namespace = fuchsia::sys::FlatNamespace::New();
  launch_info.flat_namespace->paths.push_back(modular_config::kOverriddenConfigDir);
  launch_info.flat_namespace->directories.push_back(dir_handle.TakeChannel());

  // Quit the loop when basemgr's out directory has been mounted.
  fidl::InterfacePtr<fuchsia::sys::ComponentController> controller;
  controller.events().OnDirectoryReady = [&controller, &loop] {
    controller->Detach();
    loop.Quit();
  };

  // Launch a basemgr instance with the custom namespace we created above.
  std::unique_ptr<sys::ComponentContext> context = sys::ComponentContext::Create();
  fuchsia::sys::LauncherPtr launcher;
  context->svc()->Connect(launcher.NewRequest());
  launcher->CreateComponent(std::move(launch_info), controller.NewRequest());

  loop.Run();
  return 0;
}
