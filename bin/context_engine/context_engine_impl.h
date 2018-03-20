// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CONTEXT_ENGINE_CONTEXT_ENGINE_IMPL_H_
#define PERIDOT_BIN_CONTEXT_ENGINE_CONTEXT_ENGINE_IMPL_H_

#include "lib/context/fidl/context_engine.fidl.h"
#include "lib/context/fidl/context_reader.fidl.h"
#include "lib/context/fidl/context_writer.fidl.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "peridot/bin/context_engine/context_repository.h"
#include "peridot/bin/context_engine/debug.h"

namespace modular {
class EntityResolver;
}

namespace maxwell {

class ContextReaderImpl;
class ContextWriterImpl;

class ContextEngineImpl : ContextEngine {
 public:
  // Does not take ownership of |entity_resolver|.
  ContextEngineImpl(modular::EntityResolver* entity_resolver);
  ~ContextEngineImpl() override;

  void AddBinding(f1dl::InterfaceRequest<ContextEngine> request);

  fxl::WeakPtr<ContextDebugImpl> debug();

 private:
  // |ContextEngine|
  void GetWriter(ComponentScopePtr client_info,
                 f1dl::InterfaceRequest<ContextWriter> request) override;

  // |ContextEngine|
  void GetReader(ComponentScopePtr client_info,
                 f1dl::InterfaceRequest<ContextReader> request) override;

  // |ContextEngine|
  void GetContextDebug(f1dl::InterfaceRequest<ContextDebug> request) override;

  modular::EntityResolver* const entity_resolver_;

  ContextRepository repository_;

  f1dl::BindingSet<ContextEngine> bindings_;

  std::vector<std::unique_ptr<ContextReaderImpl>> readers_;
  std::vector<std::unique_ptr<ContextWriterImpl>> writers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ContextEngineImpl);
};

}  // namespace maxwell

#endif  // PERIDOT_BIN_CONTEXT_ENGINE_CONTEXT_ENGINE_IMPL_H_
