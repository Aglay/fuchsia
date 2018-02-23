// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <map>
#include <memory>

#include "garnet/bin/media/flog_service/flog_directory.h"
#include "garnet/bin/media/util/factory_service_base.h"
#include "garnet/bin/media/util/incident.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/media/fidl/flog/flog.fidl.h"

namespace flog {

// FlogService implementation.
class FlogServiceImpl : public FactoryServiceBase<FlogServiceImpl>,
                        public FlogService {
 public:
  FlogServiceImpl(std::unique_ptr<app::ApplicationContext> application_context);
  ~FlogServiceImpl() override;

  // FlogService implementation.
  void CreateLogger(f1dl::InterfaceRequest<FlogLogger> logger,
                    const f1dl::String& label) override;

  void GetLogDescriptions(const GetLogDescriptionsCallback& callback) override;

  void CreateReader(f1dl::InterfaceRequest<FlogReader> reader,
                    uint32_t log_id) override;

  void DeleteLog(uint32_t log_id) override;

  void DeleteAllLogs() override;

 private:
  f1dl::BindingSet<FlogService> bindings_;
  Incident ready_;
  uint32_t last_allocated_log_id_ = 0;
  std::unique_ptr<std::map<uint32_t, std::string>> log_labels_by_id_;
  std::shared_ptr<FlogDirectory> directory_;
};

}  // namespace flog
