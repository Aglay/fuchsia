// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These tests run with an external network interface providing default route
// addresses.

#include <arpa/inet.h>
#include <sys/utsname.h>

#include "gtest/gtest.h"

namespace {

// This is the expected derived device name for the mac address
// aa:bb:cc:dd:ee:ff, specified in meta/netstack_external_network_test.cmx
// (see facets.fuchsia.netemul.networks.endpoints[0].mac).
//
// Since this only used on Fuchsia, it is conditionally compiled.
#if defined(__Fuchsia__)
const char kDerivedDeviceName[] = "train-cache-uncle-chill";
#endif

TEST(ExternalNetworkTest, ConnectToNonRoutableINET) {
  int s;
  ASSERT_GE(s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), 0) << strerror(errno);

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;

  // RFC5737#section-3
  //
  // The blocks 192.0.2.0/24 (TEST-NET-1), 198.51.100.0/24 (TEST-NET-2),and
  // 203.0.113.0/24 (TEST-NET-3) are provided for use in documentation.
  ASSERT_EQ(inet_pton(AF_INET, "192.0.2.55", &addr.sin_addr), 1) << strerror(errno);

  addr.sin_port = htons(1337);

  ASSERT_EQ(connect(s, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), -1);
  ASSERT_EQ(errno, EINPROGRESS) << strerror(errno);

  ASSERT_EQ(close(s), 0) << strerror(errno);
}

TEST(ExternalNetworkTest, ConnectToNonRoutableINET6) {
  int s;
  ASSERT_GE(s = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0), 0) << strerror(errno);

  struct sockaddr_in6 addr = {};
  addr.sin6_family = AF_INET6;

  // RFC3849#section-2
  //
  // The prefix allocated for documentation purposes is 2001:DB8::/32.
  ASSERT_EQ(inet_pton(AF_INET6, "2001:db8::55", &addr.sin6_addr), 1) << strerror(errno);

  addr.sin6_port = htons(1337);

  ASSERT_EQ(connect(s, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)), -1);

// If host test env does not support ipv6, the errno is set to ENETUNREACH.
// If host test env does not have a route to the remote, the errno is set to
// EHOSTUNREACH.
// TODO(sshrivy): See if there's a way to detect this in program and assert
// accordingly.
#if defined(__linux__)
  ASSERT_TRUE(errno == EINPROGRESS || errno == ENETUNREACH) << strerror(errno);
#else
  ASSERT_EQ(errno, EHOSTUNREACH) << strerror(errno);
#endif

  ASSERT_EQ(close(s), 0) << strerror(errno);
}

TEST(ExternalNetworkTest, GetHostName) {
  char hostname[HOST_NAME_MAX];
  EXPECT_GE(gethostname(hostname, sizeof(hostname)), 0) << strerror(errno);
#if defined(__Fuchsia__)
  ASSERT_STREQ(hostname, kDerivedDeviceName);
#endif
}

TEST(ExternalNetworkTest, Uname) {
  utsname uts;
  EXPECT_EQ(uname(&uts), 0) << strerror(errno);
#if defined(__Fuchsia__)
  ASSERT_STREQ(uts.nodename, kDerivedDeviceName);
#endif
}

TEST(ExternalNetworkTest, ConnectToRoutableNonexistentINET) {
  int fd;
  ASSERT_GE(fd = socket(AF_INET, SOCK_STREAM, 0), 0) << strerror(errno);

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  // Connect to a routable address to a non-existing remote. This triggers ARP resolution which is
  // expected to fail.
  addr.sin_addr.s_addr = htonl(0xd0e0a0d);
  addr.sin_port = htons(1337);

  EXPECT_EQ(connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), -1);
  // TODO(tamird): match linux. https://github.com/google/gvisor/issues/923.
#if defined(__linux__)
  EXPECT_EQ(errno, ETIMEDOUT) << strerror(errno);
#else
  EXPECT_EQ(errno, EHOSTDOWN) << strerror(errno);
#endif

  EXPECT_EQ(close(fd), 0) << strerror(errno);
}

// Test to ensure UDP send doesn`t error even with ARP timeouts.
// TODO(fxb.dev/35006): Test needs to be extended or replicated to test
// against other transport send errors.
TEST(ExternalNetworkTest, UDPErrSend) {
  int sendfd;
  ASSERT_GE(sendfd = socket(AF_INET, SOCK_DGRAM, 0), 0) << strerror(errno);

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(1337);
  char bytes[64];
  // Assign to a ssize_t variable to avoid compiler warnings for signedness in the EXPECTs below.
  ssize_t len = sizeof(bytes);
  // Precondition sanity check: write completes without error.
  EXPECT_EQ(sendto(sendfd, bytes, len, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
            len)
      << strerror(errno);

  // Send to a routable address to a non-existing remote. This triggers ARP resolution which is
  // expected to fail, but that failure is expected to leave the socket alive. Before the change
  // that added this test, the socket would be incorrectly shut down.
  //
  // TODO(fxb.dev/20716): explicitly validate that ARP resolution failed.
  addr.sin_addr.s_addr = htonl(0xd0e0a0d);
  EXPECT_EQ(sendto(sendfd, bytes, len, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
            len)
      << strerror(errno);

  // Wait for more than the ARP timeout (our current ARP reattempt count is 3 for every other
  // second)
  // TODO(fxb.dev/20716): Read from the ARP config to get the actual configured reattempt count.
  sleep(5);

  // Validate that the socket is still writable from the application side.
  EXPECT_EQ(sendto(sendfd, bytes, len, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
            len)
      << strerror(errno);

  EXPECT_EQ(close(sendfd), 0) << strerror(errno);
}

}  // namespace
