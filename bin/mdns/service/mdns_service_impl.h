// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_MDNS_SERVICE_MDNS_SERVICE_IMPL_H_
#define GARNET_BIN_MDNS_SERVICE_MDNS_SERVICE_IMPL_H_

#include <unordered_map>

#include <fuchsia/mdns/cpp/fidl.h>
#include <lib/fit/function.h>

#include "garnet/bin/mdns/service/mdns.h"
#include "garnet/bin/media/util/fidl_publisher.h"
#include "lib/component/cpp/startup_context.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/macros.h"

namespace mdns {

class MdnsServiceImpl : public fuchsia::mdns::MdnsService {
 public:
  MdnsServiceImpl(component::StartupContext* startup_context);

  ~MdnsServiceImpl() override;

  // MdnsService implementation.
  void ResolveHostName(std::string host_name, uint32_t timeout_ms,
                       ResolveHostNameCallback callback) override;

  void SubscribeToService(
      std::string service_name,
      fidl::InterfaceRequest<fuchsia::mdns::MdnsServiceSubscription>
          subscription_request) override;

  void PublishServiceInstance(std::string service_name,
                              std::string instance_name, uint16_t port,
                              fidl::VectorPtr<std::string> text,
                              PublishServiceInstanceCallback callback) override;

  void UnpublishServiceInstance(std::string service_name,
                                std::string instance_name) override;

  void AddResponder(std::string service_name, std::string instance_name,
                    fidl::InterfaceHandle<fuchsia::mdns::MdnsResponder>
                        responder_handle) override;

  void SetSubtypes(std::string service_name, std::string instance_name,
                   std::vector<std::string> subtypes) override;

  void ReannounceInstance(std::string service_name,
                          std::string instance_name) override;

  void SetVerbose(bool value) override;

 private:
  class Subscriber : public Mdns::Subscriber,
                     public fuchsia::mdns::MdnsServiceSubscription {
   public:
    Subscriber(
        fidl::InterfaceRequest<fuchsia::mdns::MdnsServiceSubscription> request,
        fit::closure deleter);

    ~Subscriber() override;

    // Mdns::Subscriber implementation:
    void InstanceDiscovered(const std::string& service,
                            const std::string& instance,
                            const inet::SocketAddress& v4_address,
                            const inet::SocketAddress& v6_address,
                            const std::vector<std::string>& text) override;

    void InstanceChanged(const std::string& service,
                         const std::string& instance,
                         const inet::SocketAddress& v4_address,
                         const inet::SocketAddress& v6_address,
                         const std::vector<std::string>& text) override;

    void InstanceLost(const std::string& service,
                      const std::string& instance) override;

    void UpdatesComplete() override;

    // MdnsServiceSubscription implementation.
    void GetInstances(uint64_t version_last_seen,
                      GetInstancesCallback callback) override;

   private:
    fidl::Binding<fuchsia::mdns::MdnsServiceSubscription> binding_;
    media::FidlPublisher<GetInstancesCallback> instances_publisher_;
    std::unordered_map<std::string, fuchsia::mdns::MdnsServiceInstancePtr>
        instances_by_name_;

    FXL_DISALLOW_COPY_AND_ASSIGN(Subscriber);
  };

  // Publisher for PublishServiceInstance.
  class SimplePublisher : public Mdns::Publisher {
   public:
    SimplePublisher(inet::IpPort port, fidl::VectorPtr<std::string> text,
                    PublishServiceInstanceCallback callback);

   private:
    // Mdns::Publisher implementation.
    void ReportSuccess(bool success) override;

    void GetPublication(bool query, const std::string& subtype,
                        fit::function<void(std::unique_ptr<Mdns::Publication>)>
                            callback) override;

    inet::IpPort port_;
    std::vector<std::string> text_;
    PublishServiceInstanceCallback callback_;

    FXL_DISALLOW_COPY_AND_ASSIGN(SimplePublisher);
  };

  // Publisher for AddResponder.
  class ResponderPublisher : public Mdns::Publisher {
   public:
    ResponderPublisher(fuchsia::mdns::MdnsResponderPtr responder,
                       fit::closure deleter);

    // Mdns::Publisher implementation.
    void ReportSuccess(bool success) override;

    void GetPublication(bool query, const std::string& subtype,
                        fit::function<void(std::unique_ptr<Mdns::Publication>)>
                            callback) override;

    fuchsia::mdns::MdnsResponderPtr responder_;

    FXL_DISALLOW_COPY_AND_ASSIGN(ResponderPublisher);
  };

  // Starts the service.
  void Start();

  component::StartupContext* startup_context_;
  fidl::BindingSet<fuchsia::mdns::MdnsService> bindings_;
  mdns::Mdns mdns_;
  size_t next_subscriber_id_ = 0;
  std::unordered_map<size_t, std::unique_ptr<Subscriber>> subscribers_by_id_;
  std::unordered_map<std::string, std::unique_ptr<Mdns::Publisher>>
      publishers_by_instance_full_name_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MdnsServiceImpl);
};

}  // namespace mdns

#endif  // GARNET_BIN_MDNS_SERVICE_MDNS_SERVICE_IMPL_H_
