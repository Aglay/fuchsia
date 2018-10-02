// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/symbols/process_symbols.h"

namespace zxdb {

// Provides a ProcessSymbols implementation that just returns empty values for
// everything. Tests can override this to implement the subset of
// functionality they need.
class MockProcessSymbols : public ProcessSymbols {
 public:
  MockProcessSymbols();
  ~MockProcessSymbols() override;

  // ProcessSymbols implementation.
  TargetSymbols* GetTargetSymbols() override;
  std::vector<ModuleSymbolStatus> GetStatus() const override;
  std::vector<Location> ResolveInputLocation(
      const InputLocation& input_location,
      const ResolveOptions& options) const override;
  LineDetails LineDetailsForAddress(uint64_t address) const override;
  bool HaveSymbolsLoadedForModuleAt(uint64_t address) const override;
};

}  // namespace zxdb
