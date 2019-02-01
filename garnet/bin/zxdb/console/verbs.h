// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/console/command_group.h"
#include "garnet/bin/zxdb/console/switch_record.h"

namespace zxdb {

class Command;
class ConsoleContext;

// Indicates whether a command implies either source or assembly context. This
// can be used by the frontend as a hint for what to show for the next stop.
enum class SourceAffinity {
  // The command applies to source code (e.g. "next").
  kSource,

  // The command applies to assembly code (e.g. "stepi", "disassemble").
  kAssembly,

  // This command does not imply any source or disassembly relation.
  kNone
};

enum class Verb {
  kNone = 0,

  kAspace,
  kAttach,
  kBacktrace,
  kBreak,
  kClear,
  kCls,
  kConnect,
  kContinue,
  kDetach,
  kDisassemble,
  kDisconnect,
  kEdit,
  kFinish,
  kGet,
  kHelp,
  kJump,
  kKill,
  kLibs,
  kList,
  kListProcesses,
  kLocals,
  kMemAnalyze,
  kMemRead,
  kNew,
  kNext,
  kNexti,
  kOpenDump,
  kPause,
  kPrint,
  kQuit,
  kQuitAgent,
  kRegs,
  kRun,
  kSet,
  kStack,
  kStep,
  kStepi,
  kSymInfo,
  kSymNear,
  kSymSearch,
  kSymStat,
  kUntil,

  // Adding a new one? Add in one of the functions GetVerbs() calls.
  kLast  // Not a real verb, keep last.
};

struct VerbRecord {
  // Type for the callback that runs a command.
  using CommandExecutor = std::function<Err(ConsoleContext*, const Command&)>;

  // Executor that is able to receive a callback that it can then pass on.
  using CommandExecutorWithCallback = std::function<Err(
      ConsoleContext*, const Command&, std::function<void(Err)>)>;

  VerbRecord();

  // The help will be referenced by pointer. It is expected to be a static
  // string.
  VerbRecord(CommandExecutor exec, std::initializer_list<std::string> aliases,
             const char* short_help, const char* help, CommandGroup group,
             SourceAffinity source_affinity = SourceAffinity::kNone);
  VerbRecord(CommandExecutorWithCallback exec_cb,
             std::initializer_list<std::string> aliases, const char* short_help,
             const char* help, CommandGroup group,
             SourceAffinity source_affinity = SourceAffinity::kNone);
  ~VerbRecord();

  CommandExecutor exec = nullptr;
  CommandExecutorWithCallback exec_cb = nullptr;

  // These are the user-typed strings that will name this verb. The [0]th one
  // is the canonical name.
  std::vector<std::string> aliases;

  const char* short_help = nullptr;  // One-line help.
  const char* help = nullptr;
  std::vector<SwitchRecord> switches;  // Switches supported by this verb.

  CommandGroup command_group = CommandGroup::kGeneral;
  SourceAffinity source_affinity = SourceAffinity::kNone;
};

// Returns all known verbs. The contents of this map will never change once
// it is called.
const std::map<Verb, VerbRecord>& GetVerbs();

// Converts the given verb to the canonical name.
std::string VerbToString(Verb v);

// Returns the record for the given verb. If the verb is not registered (should
// not happen) or is kNone (this is what noun-only commands use), returns null.
const VerbRecord* GetVerbRecord(Verb verb);

// Returns the mapping from possible inputs to the noun/verb. This is an
// inverted version of the map returned by GetNouns()/GetVerbs();
const std::map<std::string, Verb>& GetStringVerbMap();

// These functions add records for the verbs they support to the given map.
void AppendBreakpointVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendControlVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendMemoryVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendProcessVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendSharedVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendSymbolVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendSystemVerbs(std::map<Verb, VerbRecord>* verbs);
void AppendThreadVerbs(std::map<Verb, VerbRecord>* verbs);

}  // namespace zxdb
