// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/console/console_main.h"

#include <cstdlib>

#include "garnet/bin/zxdb/client/session.h"
#include "garnet/bin/zxdb/client/setting_schema_definition.h"
#include "garnet/bin/zxdb/console/actions.h"
#include "garnet/bin/zxdb/console/command_line_options.h"
#include "garnet/bin/zxdb/console/console.h"
#include "garnet/bin/zxdb/console/output_buffer.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/developer/debug/ipc/debug/debug.h"
#include "src/developer/debug/shared/buffered_fd.h"
#include "src/developer/debug/shared/message_loop_poll.h"
#include "src/developer/debug/zxdb/common/string_util.h"

namespace zxdb {

namespace {

// Loads any actions specified on the command line into the vector.
Err SetupActions(const CommandLineOptions& options,
                 std::vector<Action>* actions) {
  if (options.connect) {
    std::string cmd = "connect " + *options.connect;
    actions->push_back(Action("Connect", [cmd](const Action&, const Session&,
                                               Console* console) {
      console->ProcessInputLine(cmd.c_str(), ActionFlow::PostActionCallback);
    }));
  }

  if (options.run) {
    std::string cmd = "run " + *options.run;
    actions->push_back(Action("Run", [cmd](const Action&, const Session&,
                                           Console* console) {
      console->ProcessInputLine(cmd.c_str(), ActionFlow::PostActionCallback);
    }));
  }

  if (options.script_file) {
    Err err = ScriptFileToActions(*options.script_file, actions);
    if (err.has_error())
      return err;
  }

  return Err();
}

void ScheduleActions(zxdb::Session& session, zxdb::Console& console,
                     std::vector<zxdb::Action> actions) {
  auto callback = [&](zxdb::Err err) {
    std::string msg;
    if (!err.has_error()) {
      msg = "All actions were executed successfully.";
    } else if (err.type() == zxdb::ErrType::kCanceled) {
      msg = "Action processing was cancelled.";
    } else {
      msg = fxl::StringPrintf("Error executing actions: %s", err.msg().c_str());
    }
    // Go into interactive mode.
    console.Init();
  };

  // This will add the actions to the MessageLoop and oversee that all the
  // actions run or the flow is interrupted if one of them fails.
  // Actions run on a singleton ActionFlow instance.
  zxdb::ActionFlow& flow = zxdb::ActionFlow::Singleton();
  flow.ScheduleActions(std::move(actions), &session, &console, callback);
}

}  // namespace

int ConsoleMain(int argc, const char* argv[]) {
  CommandLineOptions options;
  std::vector<std::string> params;
  Err err = ParseCommandLine(argc, argv, &options, &params);
  if (err.has_error()) {
    fprintf(stderr, "%s", err.msg().c_str());
    return 1;
  }

  std::vector<zxdb::Action> actions;
  err = SetupActions(options, &actions);
  if (err.has_error()) {
    fprintf(stderr, "%s", err.msg().c_str());
    return 1;
  }

  debug_ipc::MessageLoopPoll loop;
  loop.Init();

  // This scope forces all the objects to be destroyed before the Cleanup()
  // call which will mark the message loop as not-current.
  {
    debug_ipc::BufferedFD buffer;

    // Route data from buffer -> session.
    Session session;
    buffer.set_data_available_callback(
        [&session]() { session.OnStreamReadable(); });

    if (options.debug_mode) {
      session.system().settings().SetBool(ClientSettings::System::kDebugMode,
                                          true);
    }

    Console console(&session);
    if (options.quit_agent_on_quit) {
      session.system().settings().SetBool(
          ClientSettings::System::kQuitAgentOnExit, true);
    }

    // Save command-line switches ----------------------------------------------

    // Symbol paths
    std::vector<std::string> paths;
    std::vector<std::string> repo_paths;
    // At this moment, the build index has all the "default" paths.
    BuildIDIndex& build_id_index =
        session.system().GetSymbols()->build_id_index();
    for (const auto& build_id_file : build_id_index.build_id_files())
      paths.push_back(build_id_file);
    for (const auto& source : build_id_index.sources())
      paths.push_back(source);

    // We add the options paths given paths.
    paths.insert(paths.end(), options.symbol_paths.begin(),
                 options.symbol_paths.end());
    repo_paths.insert(repo_paths.end(), options.symbol_repo_paths.begin(),
                      options.symbol_repo_paths.end());

    // Let the user's home folder be the default symbol repo.
    if (const char* home = std::getenv("HOME")) {
      repo_paths.emplace_back(home);
    }

    // Adding it to the settings will trigger the loading of the symbols.
    // Redundant adds are ignored.
    session.system().settings().SetList(ClientSettings::System::kSymbolPaths,
                                        std::move(paths));
    session.system().settings().SetList(
        ClientSettings::System::kSymbolRepoPaths, std::move(repo_paths));

    if (!actions.empty()) {
      ScheduleActions(session, console, std::move(actions));
    } else {
      // Interactive mode is the default mode.
      console.Init();

      // Tip for connecting when run interactively.
      OutputBuffer help;
      help.Append(Syntax::kWarning, "👉 ");
      help.Append(
          Syntax::kComment,
          "Please \"connect <ip>:<port>\" matching what you passed to\n   "
          "\"run debug_agent --port=<port>\" on the target system. Or try "
          "\"help\".");
      console.Output(help);
    }

    loop.Run();
  }

  loop.Cleanup();

  return 0;
}

}  // namespace zxdb
