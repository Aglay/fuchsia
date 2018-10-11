// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/console/console_main.h"

#include "garnet/bin/zxdb/client/session.h"
#include "garnet/bin/zxdb/console/actions.h"
#include "garnet/bin/zxdb/console/command_line_options.h"
#include "garnet/bin/zxdb/console/command_line_options.h"
#include "garnet/bin/zxdb/console/console.h"
#include "garnet/bin/zxdb/console/output_buffer.h"
#include "garnet/lib/debug_ipc/helper/buffered_fd.h"
#include "garnet/lib/debug_ipc/helper/message_loop_poll.h"
#include "garnet/public/lib/fxl/command_line.h"
#include "garnet/public/lib/fxl/strings/string_printf.h"

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

    Console console(&session);

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
          "\"debug_agent --port=<port>\" on the target system. Or try "
          "\"help\".");
      console.Output(std::move(help));
    }

    loop.Run();
  }

  loop.Cleanup();
  return 0;
}

}  // namespace
