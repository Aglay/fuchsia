// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/network/mdns/service/instance_responder.h"

#include <lib/zx/time.h>

#include <gtest/gtest.h>

#include "src/connectivity/network/mdns/service/mdns_names.h"
#include "src/connectivity/network/mdns/service/test/agent_test.h"

namespace mdns {
namespace test {

class InstanceResponderTest : public AgentTest, public Mdns::Publisher {
 public:
  InstanceResponderTest() {}

 protected:
  static const inet::IpPort kPort;
  static const std::string kServiceName;
  static const std::string kOtherServiceName;
  static const std::string kInstanceName;
  static constexpr size_t kMaxSenderAddresses = 64;

  static std::string service_full_name() { return MdnsNames::LocalServiceFullName(kServiceName); }

  static std::string instance_full_name() {
    return MdnsNames::LocalInstanceFullName(kInstanceName, kServiceName);
  }

  // Expects that the agent has not called |ReportSuccess|.
  void ExpectNoReportSuccessCall() { EXPECT_FALSE(report_success_parameter_.has_value()); }

  // Expects that the agent has not called |GetPublication|.
  void ExpectNoGetPublicationCall() { EXPECT_TRUE(get_publication_calls_.empty()); }

  // Expects that the agent has called |GetPublication| with the given parameters. Returns the
  // callback passed to |GetPublication|.
  fit::function<void(std::unique_ptr<Mdns::Publication>)> ExpectGetPublicationCall(
      bool query, const std::string& subtype,
      const std::vector<inet::SocketAddress>& source_addresses);

  // Expects that nothing else has happened.
  void ExpectNoOther() override;

  // Expects a sequence of announcements made after startup.
  void ExpectAnnouncements();

  // Expects a single announcement (a 'GetPublication' call and subsequent publication).
  void ExpectAnnouncement();

  // Expects a single publication.
  void ExpectPublication();

 private:
  struct GetPublicationCall {
    bool query_;
    const std::string subtype_;
    std::vector<inet::SocketAddress> source_addresses_;
    fit::function<void(std::unique_ptr<Mdns::Publication>)> callback_;
  };

  // |Mdns::Publisher| implementation.
  void ReportSuccess(bool success) override { report_success_parameter_.emplace(success); }

  void GetPublication(bool query, const std::string& subtype,
                      const std::vector<inet::SocketAddress>& source_addresses,
                      fit::function<void(std::unique_ptr<Mdns::Publication>)> callback) override {
    get_publication_calls_.push(GetPublicationCall{.query_ = query,
                                                   .subtype_ = subtype,
                                                   .source_addresses_ = source_addresses,
                                                   .callback_ = std::move(callback)});
  }

  std::optional<bool> report_success_parameter_;
  std::queue<GetPublicationCall> get_publication_calls_;
};

const inet::IpPort InstanceResponderTest::kPort = inet::IpPort::From_uint16_t(2525);
const std::string InstanceResponderTest::kServiceName = "_test._tcp.";
const std::string InstanceResponderTest::kOtherServiceName = "_other._tcp.";
const std::string InstanceResponderTest::kInstanceName = "testinstance";

fit::function<void(std::unique_ptr<Mdns::Publication>)>
InstanceResponderTest::ExpectGetPublicationCall(
    bool query, const std::string& subtype,
    const std::vector<inet::SocketAddress>& source_addresses) {
  EXPECT_FALSE(get_publication_calls_.empty());
  EXPECT_EQ(query, get_publication_calls_.front().query_);
  EXPECT_EQ(subtype, get_publication_calls_.front().subtype_);
  auto callback = std::move(get_publication_calls_.front().callback_);
  EXPECT_EQ(source_addresses, get_publication_calls_.front().source_addresses_);
  EXPECT_NE(nullptr, callback);
  get_publication_calls_.pop();
  return callback;
}

void InstanceResponderTest::ExpectNoOther() {
  AgentTest::ExpectNoOther();
  ExpectNoReportSuccessCall();
  ExpectNoGetPublicationCall();
}

void InstanceResponderTest::ExpectAnnouncements() {
  ExpectAnnouncement();
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));
  ExpectAnnouncement();
  ExpectPostTaskForTimeAndInvoke(zx::sec(2), zx::sec(2));
  ExpectAnnouncement();
  ExpectPostTaskForTimeAndInvoke(zx::sec(4), zx::sec(4));
  ExpectAnnouncement();
  ExpectNoOther();
}

void InstanceResponderTest::ExpectAnnouncement() {
  ExpectGetPublicationCall(false, "", {})(Mdns::Publication::Create(kPort));
  ExpectPublication();
}

void InstanceResponderTest::ExpectPublication() {
  auto message = ExpectOutboundMessage(addresses().multicast_reply());

  auto resource = ExpectResource(message.get(), MdnsResourceSection::kAnswer, service_full_name(),
                                 DnsType::kPtr, DnsClass::kIn, false);
  EXPECT_EQ(instance_full_name(), resource->ptr_.pointer_domain_name_.dotted_string_);

  resource = ExpectResource(message.get(), MdnsResourceSection::kAdditional, instance_full_name(),
                            DnsType::kSrv);
  EXPECT_EQ(0, resource->srv_.priority_);
  EXPECT_EQ(0, resource->srv_.weight_);
  EXPECT_EQ(kPort, resource->srv_.port_);
  EXPECT_EQ(kHostFullName, resource->srv_.target_.dotted_string_);

  resource = ExpectResource(message.get(), MdnsResourceSection::kAdditional, instance_full_name(),
                            DnsType::kTxt);
  EXPECT_TRUE(resource->txt_.strings_.empty());

  ExpectAddressPlaceholder(message.get(), MdnsResourceSection::kAdditional);

  ExpectNoOtherQuestionOrResource(message.get());
}

// Tests initial startup of the responder.
TEST_F(InstanceResponderTest, Startup) {
  InstanceResponder under_test(this, kServiceName, kInstanceName, this);
  SetAgent(under_test);

  under_test.Start(kHostFullName, addresses());
  ExpectAnnouncements();
}

// Tests that multicast sends are rate-limited.
TEST_F(InstanceResponderTest, MulticastRateLimit) {
  InstanceResponder under_test(this, kServiceName, kInstanceName, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kHostFullName, addresses());
  ExpectAnnouncements();

  ReplyAddress sender_address0(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100));
  ReplyAddress sender_address1(
      inet::SocketAddress(192, 168, 1, 2, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100));

  // First question.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address0);
  ExpectGetPublicationCall(true, "",
                           {sender_address0.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Second question - answer should be delayed 1s.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address0);
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));
  ExpectGetPublicationCall(true, "",
                           {sender_address0.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTimeAndInvoke(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Third question - no delay.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address0);
  ExpectGetPublicationCall(true, "",
                           {sender_address0.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Fourth and fifth questions - one answer, delayed 1s.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address0);
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address1);
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));
  ExpectGetPublicationCall(true, "",
                           {sender_address0.socket_address(), sender_address1.socket_address()})(
      Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTimeAndInvoke(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that source addresses are limited to pertinent queries.
TEST_F(InstanceResponderTest, SourceAddresses) {
  InstanceResponder under_test(this, kServiceName, kInstanceName, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kHostFullName, addresses());
  ExpectAnnouncements();

  ReplyAddress sender_address0(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100));
  ReplyAddress sender_address1(
      inet::SocketAddress(192, 168, 1, 2, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100));

  // Irrelevant question.
  under_test.ReceiveQuestion(
      DnsQuestion(MdnsNames::LocalServiceFullName(kOtherServiceName), DnsType::kPtr),
      addresses().multicast_reply(), sender_address0);

  // Pertient question.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address1);

  // Expect only pertinent sender address.
  ExpectGetPublicationCall(true, "",
                           {sender_address1.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that at most 64 source addresses are sent.
TEST_F(InstanceResponderTest, SourceAddressLimit) {
  InstanceResponder under_test(this, kServiceName, kInstanceName, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kHostFullName, addresses());
  ExpectAnnouncements();

  ReplyAddress sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100));

  // First question.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             addresses().multicast_reply(), sender_address);

  // Expect one sender address.
  ExpectGetPublicationCall(true, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Second question asked 65 times.
  for (size_t i = 0; i <= kMaxSenderAddresses; ++i) {
    under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                               addresses().multicast_reply(), sender_address);
  }
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));

  // Expect 64 sender addresses.
  ExpectGetPublicationCall(true, "",
                           {sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address()})(
      Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTimeAndInvoke(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

}  // namespace test
}  // namespace mdns
