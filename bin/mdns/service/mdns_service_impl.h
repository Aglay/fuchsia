// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <unordered_map>

#include "garnet/bin/mdns/service/mdns.h"
#include "garnet/bin/media/util/fidl_publisher.h"
#include "lib/app/cpp/application_context.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fxl/macros.h"
#include "lib/mdns/fidl/mdns.fidl.h"

namespace mdns {

class MdnsServiceImpl : public MdnsService {
 public:
  MdnsServiceImpl(app::ApplicationContext* application_context);

  ~MdnsServiceImpl() override;

  // MdnsService implementation.
  void ResolveHostName(const fidl::String& host_name,
                       uint32_t timeout_ms,
                       const ResolveHostNameCallback& callback) override;

  void SubscribeToService(const fidl::String& service_name,
                          fidl::InterfaceRequest<MdnsServiceSubscription>
                              subscription_request) override;

  void PublishServiceInstance(
      const fidl::String& service_name,
      const fidl::String& instance_name,
      uint16_t port,
      fidl::Array<fidl::String> text,
      const PublishServiceInstanceCallback& callback) override;

  void UnpublishServiceInstance(const fidl::String& service_name,
                                const fidl::String& instance_name) override;

  void AddResponder(
      const fidl::String& service_name,
      const fidl::String& instance_name,
      fidl::InterfaceHandle<MdnsResponder> responder_handle) override;

  void SetSubtypes(const fidl::String& service_name,
                   const fidl::String& instance_name,
                   fidl::Array<fidl::String> subtypes) override;

  void ReannounceInstance(const fidl::String& service_name,
                          const fidl::String& instance_name) override;

  void SetVerbose(bool value) override;

 private:
  class MdnsServiceSubscriptionImpl : public MdnsServiceSubscription {
   public:
    MdnsServiceSubscriptionImpl(MdnsServiceImpl* owner,
                                const std::string& service_name);

    ~MdnsServiceSubscriptionImpl() override;

    void AddBinding(
        fidl::InterfaceRequest<MdnsServiceSubscription> subscription_request);

    // Sets a callback for a in-proc party. This is used by |NetConnectorImpl|
    // to discover Fuchsia devices.
    void SetCallback(const Mdns::ServiceInstanceCallback& callback) {
      callback_ = callback;
    }

    // MdnsServiceSubscription implementation.
    void GetInstances(uint64_t version_last_seen,
                      const GetInstancesCallback& callback) override;

   private:
    MdnsServiceImpl* owner_;
    std::shared_ptr<MdnsAgent> agent_;
    fidl::BindingSet<MdnsServiceSubscription> bindings_;
    Mdns::ServiceInstanceCallback callback_ = nullptr;
    media::FidlPublisher<GetInstancesCallback> instances_publisher_;
    std::unordered_map<std::string, MdnsServiceInstancePtr> instances_by_name_;

    FXL_DISALLOW_COPY_AND_ASSIGN(MdnsServiceSubscriptionImpl);
  };

  // Publisher for PublishServiceInstance.
  class SimplePublisher : public Mdns::Publisher {
   public:
    SimplePublisher(IpPort port,
                    fidl::Array<fidl::String> text,
                    const PublishServiceInstanceCallback& callback);

   private:
    // Mdns::Publisher implementation.
    void ReportSuccess(bool success) override;

    void GetPublication(
        bool query,
        const std::string& subtype,
        const std::function<void(std::unique_ptr<Mdns::Publication>)>& callback)
        override;

    IpPort port_;
    std::vector<std::string> text_;
    PublishServiceInstanceCallback callback_;

    FXL_DISALLOW_COPY_AND_ASSIGN(SimplePublisher);
  };

  // Publisher for AddResponder.
  class ResponderPublisher : public Mdns::Publisher {
   public:
    ResponderPublisher(MdnsResponderPtr responder, const fxl::Closure& deleter);

    // Mdns::Publisher implementation.
    void ReportSuccess(bool success) override;

    void GetPublication(
        bool query,
        const std::string& subtype,
        const std::function<void(std::unique_ptr<Mdns::Publication>)>& callback)
        override;

    MdnsResponderPtr responder_;

    FXL_DISALLOW_COPY_AND_ASSIGN(ResponderPublisher);
  };

  // Starts the service.
  void Start();

  app::ApplicationContext* application_context_;
  fidl::BindingSet<MdnsService> bindings_;
  mdns::Mdns mdns_;
  std::unordered_map<std::string, std::unique_ptr<MdnsServiceSubscriptionImpl>>
      subscriptions_by_service_name_;
  std::unordered_map<std::string, std::unique_ptr<Mdns::Publisher>>
      publishers_by_instance_full_name_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MdnsServiceImpl);
};

}  // namespace mdns
