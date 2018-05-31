// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_EXAMPLES_TODO_CPP_TODO_H_
#define PERIDOT_EXAMPLES_TODO_CPP_TODO_H_

#include <random>

#include <fuchsia/modular/cpp/fidl.h>
#include <ledger/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include "lib/app/cpp/startup_context.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/macros.h"
#include "peridot/examples/todo_cpp/generator.h"

namespace todo {

using Key = fidl::VectorPtr<uint8_t>;

class TodoApp : public ledger::PageWatcher, fuchsia::modular::Lifecycle {
 public:
  TodoApp(async::Loop* loop);

  // ledger::PageWatcher:
  void OnChange(ledger::PageChange page_change,
                ledger::ResultState result_state,
                OnChangeCallback callback) override;

 private:
  // |modular.Lifecycle|
  void Terminate() override;

  void List(ledger::PageSnapshotPtr snapshot);

  void GetKeys(std::function<void(fidl::VectorPtr<Key>)> callback);

  void AddNew();

  void DeleteOne(fidl::VectorPtr<Key> keys);

  void Act();

  async::Loop* const loop_;
  std::default_random_engine rng_;
  std::normal_distribution<> size_distribution_;
  std::uniform_int_distribution<> delay_distribution_;
  Generator generator_;
  std::unique_ptr<component::StartupContext> context_;
  fidl::InterfacePtr<fuchsia::modular::ModuleContext> module_context_;
  fuchsia::modular::ComponentContextPtr component_context_;
  ledger::LedgerPtr ledger_;
  fidl::Binding<ledger::PageWatcher> page_watcher_binding_;
  ledger::PagePtr page_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TodoApp);
};

}  // namespace todo

#endif  // PERIDOT_EXAMPLES_TODO_CPP_TODO_H_
