// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_SERVER_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_SERVER_H_

#include <map>

#include "lib/fxl/macros.h"
#include <fuchsia/cpp/network.h>

namespace ledger {

// Base implementation for simulating a cloud server.
class Server {
 public:
  Server();
  virtual ~Server();

  // Serves the given request.
  void Serve(network::URLRequestPtr request,
             std::function<void(network::URLResponsePtr)> callback);

 protected:
  enum class ResponseCode { kOk = 200, kUnauthorized = 401, kNotFound = 404 };

  virtual void HandleGet(network::URLRequestPtr request,
                         std::function<void(network::URLResponsePtr)> callback);
  virtual void HandleGetStream(
      network::URLRequestPtr request,
      std::function<void(network::URLResponsePtr)> callback);
  virtual void HandlePatch(
      network::URLRequestPtr request,
      std::function<void(network::URLResponsePtr)> callback);
  virtual void HandlePost(
      network::URLRequestPtr request,
      std::function<void(network::URLResponsePtr)> callback);
  virtual void HandlePut(network::URLRequestPtr request,
                         std::function<void(network::URLResponsePtr)> callback);

  network::URLResponsePtr BuildResponse(
      const std::string& url,
      ResponseCode code,
      zx::socket body,
      const std::map<std::string, std::string>& headers);

  network::URLResponsePtr BuildResponse(const std::string& url,
                                        ResponseCode code,
                                        std::string body);

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(Server);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIREBASE_TESTING_SERVER_SERVER_H_
