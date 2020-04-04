// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <poll.h>

#include <array>
#include <thread>

#if defined(__Fuchsia__)
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>

#include <filesystem>

#include "fuchsia/sysmem/cpp/fidl.h"
#include "magma_sysmem.h"
#include "platform_logger.h"
#include "platform_trace_provider.h"
#endif

#include "gtest/gtest.h"
#include "magma.h"
#include "magma_common_defs.h"

extern "C" {
#include "test_magma_abi.h"
}

namespace {

inline uint64_t page_size() { return sysconf(_SC_PAGESIZE); }

inline constexpr int64_t ms_to_ns(int64_t ms) { return ms * 1000000ull; }
}  // namespace

class TestConnection {
 public:
  static constexpr const char* kDevicePathFuchsia = "/dev/class/gpu";
  static constexpr const char* kDeviceNameLinux = "/dev/dri/renderD128";
  static constexpr const char* kDeviceNameVirt = "/dev/magma0";

#if defined(VIRTMAGMA)
  static std::string device_name() { return kDeviceNameVirt; }
#elif defined(__linux__)
  static std::string device_name() { return kDeviceNameLinux; }
#elif defined(__Fuchsia__)
  static std::string device_name() {
    // Return the first entry in the device path
    for (auto& p : std::filesystem::directory_iterator(kDevicePathFuchsia)) {
      return p.path();
    }
    return "";
  }
#else
#error Unimplemented
#endif

  static bool is_virtmagma() { return device_name() == kDeviceNameVirt; }

  TestConnection() {
    std::string device = device_name();
    EXPECT_FALSE(device.empty()) << " No GPU device";
    if (device.empty())
      return;

#if defined(__Fuchsia__)
    zx::channel server_end, client_end;
    zx::channel::create(0, &server_end, &client_end);

    zx_status_t status = fdio_service_connect(device.c_str(), server_end.release());
    EXPECT_EQ(status, ZX_OK);
    if (status == ZX_OK) {
      EXPECT_EQ(MAGMA_STATUS_OK, magma_device_import(client_end.release(), &device_));
    }
#elif defined(__linux__)
    int fd = open(device.c_str(), O_RDWR);
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      EXPECT_EQ(MAGMA_STATUS_OK, magma_device_import(fd, &device_));
    }
#else
#error Unimplemented
#endif
    if (device_) {
      magma_create_connection2(device_, &connection_);
    }
  }

  ~TestConnection() {
    if (connection_)
      magma_release_connection(connection_);
    if (device_)
      magma_device_release(device_);
    if (fd_ >= 0)
      close(fd_);
  }

  int fd() { return fd_; }

  magma_connection_t connection() { return connection_; }

  void Connection() { ASSERT_NE(connection_, nullptr); }

  void Context() {
    ASSERT_NE(connection_, nullptr);

    uint32_t context_id[2];

    magma_create_context(connection_, &context_id[0]);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_create_context(connection_, &context_id[1]);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_context(connection_, context_id[0]);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_context(connection_, context_id[1]);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_context(connection_, context_id[1]);
    EXPECT_NE(MAGMA_STATUS_OK, magma_get_error(connection_));
  }

  void NotificationChannelHandle() {
    uint32_t handle = magma_get_notification_channel_handle(connection_);
    EXPECT_NE(0u, handle);

    uint32_t handle2 = magma_get_notification_channel_handle(connection_);
    EXPECT_EQ(handle, handle2);
  }

  void WaitNotificationChannel() {
    constexpr uint64_t kOneSecondInNs = 1000000000;
    magma_status_t status = magma_wait_notification_channel(connection_, kOneSecondInNs);
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, status);
  }

  void ReadNotificationChannel() {
    std::array<unsigned char, 1024> buffer;
    uint64_t buffer_size = ~0;
    magma_status_t status =
        magma_read_notification_channel(connection_, buffer.data(), buffer.size(), &buffer_size);
    EXPECT_EQ(MAGMA_STATUS_OK, status);
    EXPECT_EQ(0u, buffer_size);
  }

  void Buffer() {
    ASSERT_NE(connection_, nullptr);

    uint64_t size = page_size();
    uint64_t actual_size;
    uint64_t id;

    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &actual_size, &id));
    EXPECT_GE(size, actual_size);
    EXPECT_NE(id, 0u);

    magma_release_buffer(connection_, id);
  }

  void BufferMap() {
    ASSERT_NE(connection_, nullptr);

    uint64_t size = page_size();
    uint64_t actual_size;
    uint64_t id;

    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &actual_size, &id));
    EXPECT_NE(id, 0u);

    magma_map_buffer_gpu(connection_, id, 1024, 0, size / page_size(), MAGMA_GPU_MAP_FLAG_READ);
    magma_unmap_buffer_gpu(connection_, id, 2048);
    EXPECT_NE(MAGMA_STATUS_OK, magma_get_error(connection_));
    EXPECT_EQ(MAGMA_STATUS_MEMORY_ERROR, magma_commit_buffer(connection_, id, 100, 100));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_error(connection_));

    magma_release_buffer(connection_, id);
  }

  void BufferExport(uint32_t* handle_out, uint64_t* id_out) {
    ASSERT_NE(connection_, nullptr);

    uint64_t size = page_size();
    magma_buffer_t buffer;

    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_buffer(connection_, size, &size, &buffer));

    *id_out = magma_get_buffer_id(buffer);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_export(connection_, buffer, handle_out));
  }

  void BufferReleaseHandle() {
    if (is_virtmagma())
      GTEST_SKIP();  // TODO(fxb/13278)

    uint64_t id;
    uint32_t handle;
    BufferExport(&handle, &id);
    EXPECT_EQ(MAGMA_STATUS_OK, magma_release_buffer_handle(handle));
  }

  void BufferImport(uint32_t handle, uint64_t id) {
    ASSERT_NE(connection_, nullptr);

    magma_buffer_t buffer;
    ASSERT_EQ(MAGMA_STATUS_OK, magma_import(connection_, handle, &buffer));
    EXPECT_EQ(magma_get_buffer_id(buffer), id);
  }

  static void BufferImportExport(TestConnection* test1, TestConnection* test2) {
    if (is_virtmagma())
      GTEST_SKIP();  // TODO(fxb/13278)

    uint32_t handle;
    uint64_t id;
    test1->BufferExport(&handle, &id);
    test2->BufferImport(handle, id);
  }

  void Semaphore(uint32_t count) {
    ASSERT_TRUE(connection_);

    std::vector<magma_semaphore_t> semaphore(count);

    for (uint32_t i = 0; i < count; i++) {
      ASSERT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &semaphore[i]));
      EXPECT_NE(0u, magma_get_semaphore_id(semaphore[i]));
    }

    // Wait for all
    bool wait_all = true;

    magma_signal_semaphore(semaphore[0]);

    constexpr uint32_t kTimeoutMs = 100;
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(count == 1 ? MAGMA_STATUS_OK : MAGMA_STATUS_TIMED_OUT,
              magma_wait_semaphores(semaphore.data(), semaphore.size(), kTimeoutMs, wait_all));
    if (count > 1) {
      // Subtract to allow for rounding errors in magma_wait_semaphores time calculations
      EXPECT_LE(kTimeoutMs - count, std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    }

    for (uint32_t i = 1; i < semaphore.size(); i++) {
      magma_signal_semaphore(semaphore[i]);
    }

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_wait_semaphores(semaphore.data(), semaphore.size(), 0, wait_all));

    for (uint32_t i = 0; i < semaphore.size(); i++) {
      magma_reset_semaphore(semaphore[i]);
    }

    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_wait_semaphores(semaphore.data(), semaphore.size(), 0, wait_all));

    // Wait for one
    wait_all = false;
    start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_wait_semaphores(semaphore.data(), semaphore.size(), kTimeoutMs, wait_all));

    // Subtract to allow for rounding errors in magma_wait_semaphores time calculations
    EXPECT_LE(kTimeoutMs - count, std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());

    magma_signal_semaphore(semaphore[semaphore.size() - 1]);

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_wait_semaphores(semaphore.data(), semaphore.size(), 0, wait_all));

    magma_reset_semaphore(semaphore[semaphore.size() - 1]);

    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
              magma_wait_semaphores(semaphore.data(), semaphore.size(), 0, wait_all));

    for (uint32_t i = 0; i < semaphore.size(); i++) {
      magma_release_semaphore(connection_, semaphore[i]);
    }
  }

  void PollWithNotificationChannel(uint32_t semaphore_count) {
    ASSERT_TRUE(connection_);

    std::vector<magma_poll_item_t> items;

    for (uint32_t i = 0; i < semaphore_count; i++) {
      magma_semaphore_t semaphore;
      ASSERT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &semaphore));

      items.push_back({.semaphore = semaphore,
                       .type = MAGMA_POLL_TYPE_SEMAPHORE,
                       .condition = MAGMA_POLL_CONDITION_SIGNALED});
    }

    items.push_back({
        .handle = magma_get_notification_channel_handle(connection_),
        .type = MAGMA_POLL_TYPE_HANDLE,
        .condition = MAGMA_POLL_CONDITION_READABLE,
    });

    constexpr int64_t kTimeoutNs = ms_to_ns(100);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, magma_poll(items.data(), items.size(), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    magma_signal_semaphore(items[0].semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), items.size(), 0));
    EXPECT_EQ(items[0].result, items[0].condition);
    EXPECT_EQ(items[1].result, 0u);

    magma_reset_semaphore(items[0].semaphore);

    start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, magma_poll(items.data(), items.size(), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    for (uint32_t i = 0; i < semaphore_count; i++) {
      magma_signal_semaphore(items[i].semaphore);
    }

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), items.size(), 0));

    for (uint32_t i = 0; i < items.size(); i++) {
      if (i < items.size() - 1) {
        EXPECT_EQ(items[i].result, items[i].condition);
      } else {
        // Notification channel
        EXPECT_EQ(items[i].result, 0u);
      }
    }

    for (uint32_t i = 0; i < semaphore_count; i++) {
      magma_release_semaphore(connection_, items[i].semaphore);
    }
  }

  void PollWithTestChannel() {
#ifdef __Fuchsia__
    ASSERT_TRUE(connection_);

    zx::channel local, remote;
    ASSERT_EQ(ZX_OK, zx::channel::create(0 /* flags */, &local, &remote));

    magma_semaphore_t semaphore;
    ASSERT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &semaphore));

    std::vector<magma_poll_item_t> items;
    items.push_back({.semaphore = semaphore,
                     .type = MAGMA_POLL_TYPE_SEMAPHORE,
                     .condition = MAGMA_POLL_CONDITION_SIGNALED});
    items.push_back({
        .handle = local.get(),
        .type = MAGMA_POLL_TYPE_HANDLE,
        .condition = MAGMA_POLL_CONDITION_READABLE,
    });

    constexpr int64_t kTimeoutNs = ms_to_ns(100);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, magma_poll(items.data(), items.size(), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    magma_signal_semaphore(semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), items.size(), 0));
    EXPECT_EQ(items[0].result, items[0].condition);
    EXPECT_EQ(items[1].result, 0u);

    magma_reset_semaphore(semaphore);

    start = std::chrono::steady_clock::now();
    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, magma_poll(items.data(), items.size(), kTimeoutNs));
    EXPECT_LE(kTimeoutNs, std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());

    uint32_t dummy;
    EXPECT_EQ(ZX_OK, remote.write(0 /* flags */, &dummy, sizeof(dummy), nullptr /* handles */,
                                  0 /* num_handles*/));

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), items.size(), 0));
    EXPECT_EQ(items[0].result, 0u);
    EXPECT_EQ(items[1].result, items[1].condition);

    magma_signal_semaphore(semaphore);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_poll(items.data(), items.size(), 0));
    EXPECT_EQ(items[0].result, items[0].condition);
    EXPECT_EQ(items[1].result, items[1].condition);
#else
    GTEST_SKIP();
#endif
  }

  void PollChannelClosed() {
#ifdef __Fuchsia__
    ASSERT_TRUE(connection_);

    zx::channel local, remote;
    ASSERT_EQ(ZX_OK, zx::channel::create(0 /* flags */, &local, &remote));

    std::vector<magma_poll_item_t> items;
    items.push_back({
        .handle = local.get(),
        .type = MAGMA_POLL_TYPE_HANDLE,
        .condition = MAGMA_POLL_CONDITION_READABLE,
    });

    EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, magma_poll(items.data(), items.size(), 0));

    remote.reset();
    EXPECT_EQ(MAGMA_STATUS_CONNECTION_LOST, magma_poll(items.data(), items.size(), 0));
#else
    GTEST_SKIP();
#endif
  }

  void SemaphoreExport(uint32_t* handle_out, uint64_t* id_out) {
    ASSERT_NE(connection_, nullptr);
    magma_semaphore_t semaphore;

    ASSERT_EQ(magma_create_semaphore(connection_, &semaphore), MAGMA_STATUS_OK);
    *id_out = magma_get_semaphore_id(semaphore);
    EXPECT_EQ(magma_export_semaphore(connection_, semaphore, handle_out), MAGMA_STATUS_OK);
  }

  void SemaphoreImport(uint32_t handle, uint64_t id) {
    ASSERT_NE(connection_, nullptr);
    magma_semaphore_t semaphore;

    ASSERT_EQ(magma_import_semaphore(connection_, handle, &semaphore), MAGMA_STATUS_OK);
    EXPECT_EQ(magma_get_semaphore_id(semaphore), id);
  }

  static void SemaphoreImportExport(TestConnection* test1, TestConnection* test2) {
    if (is_virtmagma())
      GTEST_SKIP();  // TODO(fxb/13278)

    uint32_t handle;
    uint64_t id;
    test1->SemaphoreExport(&handle, &id);
    test2->SemaphoreImport(handle, id);
  }

  void ImmediateCommands() {
    uint32_t context_id;
    magma_create_context(connection_, &context_id);
    EXPECT_EQ(magma_get_error(connection_), 0);

    magma_inline_command_buffer inline_command_buffer{};
    magma_execute_immediate_commands2(connection_, context_id, 0, &inline_command_buffer);
    EXPECT_EQ(magma_get_error(connection_), 0);

    magma_release_context(connection_, context_id);
    EXPECT_EQ(magma_get_error(connection_), 0);
  }

  void ImageFormat() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    fuchsia::sysmem::SingleBufferSettings buffer_settings;
    buffer_settings.has_image_format_constraints = true;
    buffer_settings.image_format_constraints.pixel_format.type =
        fuchsia::sysmem::PixelFormatType::NV12;
    buffer_settings.image_format_constraints.min_bytes_per_row = 128;
    buffer_settings.image_format_constraints.bytes_per_row_divisor = 256;
    buffer_settings.image_format_constraints.min_coded_height = 64;
    fidl::Encoder encoder(fidl::Encoder::NO_HEADER);
    encoder.Alloc(
        fidl::EncodingInlineSize<fuchsia::sysmem::SingleBufferSettings, fidl::Encoder>(&encoder));
    buffer_settings.Encode(&encoder, 0);
    std::vector<uint8_t> encoded_bytes = encoder.TakeBytes();
    size_t real_size = encoded_bytes.size();
    // Add an extra byte to ensure the size is correct.
    encoded_bytes.push_back(0);
    magma_buffer_format_description_t description;
    ASSERT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_description(encoded_bytes.data(), real_size, &description));
    magma_image_plane_t planes[4];
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_plane_info_with_size(description, 128u, 64u, planes));

    EXPECT_EQ(256u, planes[0].bytes_per_row);
    EXPECT_EQ(0u, planes[0].byte_offset);
    EXPECT_EQ(256u, planes[1].bytes_per_row);
    EXPECT_EQ(256u * 64u, planes[1].byte_offset);
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_plane_info_with_size(description, 128u, 64u, planes));
    EXPECT_EQ(256u, planes[0].bytes_per_row);
    EXPECT_EQ(0u, planes[0].byte_offset);
    EXPECT_EQ(256u, planes[1].bytes_per_row);
    EXPECT_EQ(256u * 64u, planes[1].byte_offset);
    magma_buffer_format_description_release(description);
    EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, magma_get_buffer_format_description(
                                             encoded_bytes.data(), real_size + 1, &description));
    EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, magma_get_buffer_format_description(
                                             encoded_bytes.data(), real_size - 1, &description));
#endif
  }

  void Sysmem(bool use_format_modifier) {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    magma_sysmem_connection_t connection;
    zx::channel local_endpoint, server_endpoint;
    EXPECT_EQ(ZX_OK, zx::channel::create(0u, &local_endpoint, &server_endpoint));
    EXPECT_EQ(ZX_OK,
              fdio_service_connect("/svc/fuchsia.sysmem.Allocator", server_endpoint.release()));
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_sysmem_connection_import(local_endpoint.release(), &connection));

    magma_buffer_collection_t collection;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_collection_import(connection, ZX_HANDLE_INVALID, &collection));

    magma_buffer_format_constraints_t buffer_constraints{};

    buffer_constraints.count = 1;
    buffer_constraints.usage = 0;
    buffer_constraints.secure_permitted = false;
    buffer_constraints.secure_required = false;
    buffer_constraints.cpu_domain_supported = true;
    magma_sysmem_buffer_constraints_t constraints;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_constraints_create(connection, &buffer_constraints, &constraints));

    // Create a set of basic 512x512 RGBA image constraints.
    magma_image_format_constraints_t image_constraints{};
    image_constraints.image_format = MAGMA_FORMAT_R8G8B8A8;
    image_constraints.has_format_modifier = use_format_modifier;
    image_constraints.format_modifier = use_format_modifier ? MAGMA_FORMAT_MODIFIER_LINEAR : 0;
    image_constraints.width = 512;
    image_constraints.height = 512;
    image_constraints.layers = 1;
    image_constraints.bytes_per_row_divisor = 1;
    image_constraints.min_bytes_per_row = 0;

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_constraints_set_format(connection, constraints, 0, &image_constraints));

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_buffer_collection_set_constraints(connection, collection, constraints));

    // Buffer should be allocated now.
    magma_buffer_format_description_t description;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_sysmem_get_description_from_collection(connection, collection, &description));

    uint32_t buffer_count;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_count(description, &buffer_count));
    EXPECT_EQ(1u, buffer_count);
    magma_bool_t is_secure;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_is_secure(description, &is_secure));
    EXPECT_FALSE(is_secure);

    magma_bool_t has_format_modifier;
    uint64_t format_modifier;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_format_modifier(description, &has_format_modifier,
                                                                &format_modifier));
    if (has_format_modifier) {
      EXPECT_EQ(MAGMA_FORMAT_MODIFIER_LINEAR, format_modifier);
    }

    magma_image_plane_t planes[4];
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_plane_info_with_size(description, 512u, 512u, planes));
    EXPECT_EQ(512 * 4u, planes[0].bytes_per_row);
    EXPECT_EQ(0u, planes[0].byte_offset);
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_get_buffer_format_plane_info_with_size(description, 512, 512, planes));
    EXPECT_EQ(512 * 4u, planes[0].bytes_per_row);
    EXPECT_EQ(0u, planes[0].byte_offset);

    uint32_t handle;
    uint32_t offset;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_sysmem_get_buffer_handle_from_collection(
                                   connection, collection, 0, &handle, &offset));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_release_buffer_handle(handle));

    magma_buffer_collection_release(connection, collection);
    magma_buffer_constraints_release(connection, constraints);
    magma_sysmem_connection_release(connection);
#endif
  }

  void TracingInit() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    zx::channel local_endpoint, server_endpoint;
    EXPECT_EQ(ZX_OK, zx::channel::create(0u, &local_endpoint, &server_endpoint));
    EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.tracing.provider.Registry",
                                          server_endpoint.release()));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_initialize_tracing(local_endpoint.release()));

    if (magma::PlatformTraceProvider::Get())
      EXPECT_TRUE(magma::PlatformTraceProvider::Get()->IsInitialized());
#endif
  }

  void LoggingInit() {
#if !defined(__Fuchsia__)
    GTEST_SKIP();
#else
    zx::channel local_endpoint, server_endpoint;
    EXPECT_EQ(ZX_OK, zx::channel::create(0u, &local_endpoint, &server_endpoint));
    EXPECT_EQ(ZX_OK,
              fdio_service_connect("/svc/fuchsia.logger.LogSink", server_endpoint.release()));
    EXPECT_EQ(MAGMA_STATUS_OK, magma_initialize_logging(local_endpoint.release()));
    EXPECT_TRUE(magma::PlatformLogger::IsInitialized());
    MAGMA_LOG(INFO, "LoggingInit test complete");
#endif
  }

  void GetDeviceIdImported() {
    uint64_t device_id = 0;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_query2(device_, MAGMA_QUERY_DEVICE_ID, &device_id));
    EXPECT_NE(0u, device_id);
  }

  void GetVendorIdImported() {
    uint64_t vendor_id = 0;
    EXPECT_EQ(MAGMA_STATUS_OK, magma_query2(device_, MAGMA_QUERY_VENDOR_ID, &vendor_id));
    EXPECT_NE(0u, vendor_id);
  }

  void GetMinimumMappableAddressImported() {
    uint64_t address;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_query2(device_, MAGMA_QUERY_MINIMUM_MAPPABLE_ADDRESS, &address));
  }

  void QueryReturnsBufferImported() {
    uint32_t handle_out = 0;
    // Drivers shouldn't allow this value to be queried through this entrypoint.
    EXPECT_NE(MAGMA_STATUS_OK,
              magma_query_returns_buffer2(device_, MAGMA_QUERY_DEVICE_ID, &handle_out));
    EXPECT_EQ(0u, handle_out);
  }

  void QueryTestRestartSupported() {
    uint64_t is_supported = 0;
    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_query2(device_, MAGMA_QUERY_IS_TEST_RESTART_SUPPORTED, &is_supported));
    // We don't care about the value of |is_supported|, just that the query returns ok.
  }

 private:
  int fd_ = -1;
  magma_device_t device_ = 0;
  magma_connection_t connection_ = 0;
};

class TestConnectionWithContext : public TestConnection {
 public:
  TestConnectionWithContext() { magma_create_context(connection(), &context_id_); }

  ~TestConnectionWithContext() { magma_release_context(connection(), context_id_); }

  uint32_t context_id() { return context_id_; }

  void ExecuteCommandBufferWithResources(uint32_t resource_count) {
    magma_system_command_buffer command_buffer = {.num_resources = resource_count};
    magma_system_exec_resource resources[resource_count];

    memset(resources, 0, sizeof(magma_system_exec_resource) * resource_count);
    magma_execute_command_buffer_with_resources(connection(), context_id(), &command_buffer,
                                                resources, nullptr);

    // Command buffer is mostly zeros, so we expect an error here
    EXPECT_NE(MAGMA_STATUS_OK, magma_get_error(connection()));
  }

  void ExecuteCommandBufferNoResources() {
    magma_system_command_buffer command_buffer = {.num_resources = 0};
    magma_execute_command_buffer_with_resources(connection(), context_id(), &command_buffer,
                                                nullptr /* resources */, nullptr);
  }

 private:
  uint32_t context_id_;
};

// NOTE: LoggingInit is first so other tests may use logging.
TEST(MagmaAbi, LoggingInit) {
  TestConnection test;
  test.LoggingInit();
}

TEST(MagmaAbi, DeviceId) {
  TestConnection test;
  test.GetDeviceIdImported();
}

TEST(MagmaAbi, VendorId) {
  TestConnection test;
  test.GetVendorIdImported();
}

TEST(MagmaAbi, MinimumMappableAddress) {
  TestConnection test;
  test.GetMinimumMappableAddressImported();
}

TEST(MagmaAbi, QueryReturnsBuffer) {
  TestConnection test;
  test.QueryReturnsBufferImported();
}

TEST(MagmaAbi, QueryTestRestartSupported) {
  TestConnection test;
  test.QueryTestRestartSupported();
}

TEST(MagmaAbi, TracingInit) {
  TestConnection test;
  test.TracingInit();
}

TEST(MagmaAbi, Buffer) {
  TestConnection test;
  test.Buffer();
}

TEST(MagmaAbi, Connection) {
  TestConnection test;
  test.Connection();
}

TEST(MagmaAbi, Context) {
  TestConnection test;
  test.Context();
}

TEST(MagmaAbi, NotificationChannelHandle) {
  TestConnection test;
  test.NotificationChannelHandle();
}

TEST(MagmaAbi, ReadNotificationChannel) {
  TestConnection test;
  test.ReadNotificationChannel();
}

TEST(MagmaAbi, BufferMap) {
  TestConnection test;
  test.BufferMap();
}

TEST(MagmaAbi, BufferReleaseHandle) {
  TestConnection test;
  test.BufferReleaseHandle();
}

TEST(MagmaAbi, BufferImportExport) {
  TestConnection test1;
  TestConnection test2;
  TestConnection::BufferImportExport(&test1, &test2);
}

TEST(MagmaAbi, Semaphore) {
  TestConnection test;
  test.Semaphore(1);
  test.Semaphore(2);
  test.Semaphore(3);
}

TEST(MagmaAbi, SemaphoreImportExport) {
  TestConnection test1;
  TestConnection test2;
  TestConnection::SemaphoreImportExport(&test1, &test2);
}

TEST(MagmaAbi, ImmediateCommands) { TestConnection().ImmediateCommands(); }

TEST(MagmaAbi, PollWithNotificationChannel) {
  TestConnection().PollWithNotificationChannel(1);
  TestConnection().PollWithNotificationChannel(2);
  TestConnection().PollWithNotificationChannel(3);
}

TEST(MagmaAbi, PollWithTestChannel) { TestConnection().PollWithTestChannel(); }

TEST(MagmaAbi, PollChannelClosed) { TestConnection().PollChannelClosed(); }

TEST(MagmaAbi, ImageFormat) {
  TestConnection test;
  test.ImageFormat();
}

TEST(MagmaAbi, Sysmem) {
  TestConnection test;
  test.Sysmem(false);
}

TEST(MagmaAbi, SysmemLinearFormatModifier) {
  TestConnection test;
  test.Sysmem(true);
}

TEST(MagmaAbi, FromC) { EXPECT_TRUE(test_magma_abi_from_c(TestConnection::device_name().c_str())); }

TEST(MagmaAbi, ExecuteCommandBufferWithResources) {
  TestConnectionWithContext().ExecuteCommandBufferWithResources(5);
}

TEST(MagmaAbi, ExecuteCommandBufferNoResources) {
  TestConnectionWithContext().ExecuteCommandBufferNoResources();
}

TEST(MagmaAbiPerf, ExecuteCommandBufferWithResources) {
  if (TestConnection::is_virtmagma())
    GTEST_SKIP();

  TestConnectionWithContext test;

  auto start = std::chrono::steady_clock::now();
  constexpr uint32_t kTestIterations = 10000;
  for (uint32_t test_iter = kTestIterations; test_iter; --test_iter) {
    test.ExecuteCommandBufferWithResources(10);
  }

  auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start);

  printf("ExecuteCommandBufferWithResources: avg duration %lld ns\n",
         duration.count() / kTestIterations);
}
