// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <poll.h>

#include "fuchsia/sysmem/cpp/fidl.h"
#include "magma.h"
#include "magma_sysmem.h"
#include "magma_util/dlog.h"
#include "magma_util/macros.h"
#include "platform_buffer.h"
#include "gtest/gtest.h"
#include <array>
#include <thread>

extern "C" {
#include "test_magma_abi.h"
}

class TestConnection {
public:
    TestConnection()
    {
        fd_ = open("/dev/class/gpu/000", O_RDONLY);
        DASSERT(fd_ >= 0);
        magma_create_connection(fd(), &connection_);
    }

    ~TestConnection()
    {
        if (connection_)
            magma_release_connection(connection_);

        close(fd_);
    }

    int fd() { return fd_; }

    void GetDeviceId()
    {
        uint64_t device_id = 0;
        EXPECT_EQ(MAGMA_STATUS_OK, magma_query(fd_, MAGMA_QUERY_DEVICE_ID, &device_id));
        EXPECT_NE(0u, device_id);
    }

    magma_connection_t connection() { return connection_; }

    void Connection() { ASSERT_NE(connection_, nullptr); }

    void Context()
    {
        ASSERT_NE(connection_, nullptr);

        uint32_t context_id[2];

        magma_create_context(connection_, &context_id[0]);
        EXPECT_EQ(magma_get_error(connection_), 0);

        magma_create_context(connection_, &context_id[1]);
        EXPECT_EQ(magma_get_error(connection_), 0);

        magma_release_context(connection_, context_id[0]);
        EXPECT_EQ(magma_get_error(connection_), 0);

        magma_release_context(connection_, context_id[1]);
        EXPECT_EQ(magma_get_error(connection_), 0);

        magma_release_context(connection_, context_id[1]);
        EXPECT_NE(magma_get_error(connection_), 0);
    }

    void NotificationChannelHandle()
    {
        uint32_t handle = magma_get_notification_channel_handle(connection_);
        EXPECT_NE(0u, handle);

        uint32_t handle2 = magma_get_notification_channel_handle(connection_);
        EXPECT_EQ(handle, handle2);
    }

    void WaitNotificationChannel()
    {
        constexpr uint64_t kOneSecondInNs = 1000000000;
        magma_status_t status = magma_wait_notification_channel(connection_, kOneSecondInNs);
        EXPECT_EQ(MAGMA_STATUS_TIMED_OUT, status);
    }

    void ReadNotificationChannel()
    {
        std::array<unsigned char, 1024> buffer;
        uint64_t buffer_size = ~0;
        magma_status_t status = magma_read_notification_channel(connection_, buffer.data(),
                                                                buffer.size(), &buffer_size);
        EXPECT_EQ(MAGMA_STATUS_OK, status);
        EXPECT_EQ(0u, buffer_size);
    }

    void Buffer()
    {
        ASSERT_NE(connection_, nullptr);

        uint64_t size = PAGE_SIZE;
        uint64_t actual_size;
        uint64_t id;

        EXPECT_EQ(magma_create_buffer(connection_, size, &actual_size, &id), 0);
        EXPECT_GE(size, actual_size);
        EXPECT_NE(id, 0u);

        magma_release_buffer(connection_, id);
    }

    void BufferMap()
    {
        ASSERT_NE(connection_, nullptr);

        uint64_t size = PAGE_SIZE;
        uint64_t actual_size;
        uint64_t id;

        EXPECT_EQ(magma_create_buffer(connection_, size, &actual_size, &id), 0);
        EXPECT_NE(id, 0u);

        magma_map_buffer_gpu(connection_, id, 1024, 0, size / PAGE_SIZE, MAGMA_GPU_MAP_FLAG_READ);
        magma_unmap_buffer_gpu(connection_, id, 2048);
        magma_commit_buffer(connection_, id, 100, 100);

        magma_release_buffer(connection_, id);
    }

    void BufferExport(uint32_t* handle_out, uint64_t* id_out)
    {
        ASSERT_NE(connection_, nullptr);

        uint64_t size = PAGE_SIZE;
        magma_buffer_t buffer;

        EXPECT_EQ(magma_create_buffer(connection_, size, &size, &buffer), 0);

        *id_out = magma_get_buffer_id(buffer);

        EXPECT_EQ(magma_export(connection_, buffer, handle_out), 0);
    }

    void BufferImport(uint32_t handle, uint64_t id)
    {
        ASSERT_NE(connection_, nullptr);

        magma_buffer_t buffer;
        EXPECT_EQ(magma_import(connection_, handle, &buffer), 0);
        EXPECT_EQ(magma_get_buffer_id(buffer), id);
    }

    static void BufferImportExport(TestConnection* test1, TestConnection* test2)
    {
        uint32_t handle;
        uint64_t id;
        test1->BufferExport(&handle, &id);
        test2->BufferImport(handle, id);
    }

    void Semaphore(uint32_t count)
    {
        ASSERT_NE(connection_, nullptr);

        std::vector<magma_semaphore_t> semaphore(count);
        for (uint32_t i = 0; i < count; i++) {
            EXPECT_EQ(MAGMA_STATUS_OK, magma_create_semaphore(connection_, &semaphore[i]));
            EXPECT_NE(0u, magma_get_semaphore_id(semaphore[i]));
        }

        auto thread = std::thread([semaphore, wait_all = true] {
            EXPECT_EQ(MAGMA_STATUS_OK, magma_wait_semaphores(semaphore.data(), semaphore.size(),
                                                             UINT64_MAX, wait_all));
            for (uint32_t i = 0; i < semaphore.size(); i++) {
                magma_reset_semaphore(semaphore[i]);
            }
            EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
                      magma_wait_semaphores(semaphore.data(), semaphore.size(), 100, wait_all));
        });

        for (uint32_t i = 0; i < semaphore.size(); i++) {
            usleep(10 * 1000);
            magma_signal_semaphore(semaphore[i]);
        }
        thread.join();

        thread = std::thread([semaphore, wait_all = false] {
            EXPECT_EQ(MAGMA_STATUS_OK, magma_wait_semaphores(semaphore.data(), semaphore.size(),
                                                             UINT64_MAX, wait_all));
            for (uint32_t i = 0; i < semaphore.size(); i++) {
                magma_reset_semaphore(semaphore[i]);
            }
            EXPECT_EQ(MAGMA_STATUS_TIMED_OUT,
                      magma_wait_semaphores(semaphore.data(), semaphore.size(), 100, wait_all));
        });

        usleep(10 * 1000);
        magma_signal_semaphore(semaphore[semaphore.size() - 1]);
        thread.join();

        for (uint32_t i = 0; i < semaphore.size(); i++) {
            magma_release_semaphore(connection_, semaphore[i]);
        }
    }

    void SemaphoreExport(uint32_t* handle_out, uint64_t* id_out)
    {
        ASSERT_NE(connection_, nullptr);
        magma_semaphore_t semaphore;

        EXPECT_EQ(magma_create_semaphore(connection_, &semaphore), MAGMA_STATUS_OK);
        *id_out = magma_get_semaphore_id(semaphore);
        EXPECT_EQ(magma_export_semaphore(connection_, semaphore, handle_out), MAGMA_STATUS_OK);
    }

    void SemaphoreImport(uint32_t handle, uint64_t id)
    {
        ASSERT_NE(connection_, nullptr);
        magma_semaphore_t semaphore;

        EXPECT_EQ(magma_import_semaphore(connection_, handle, &semaphore), MAGMA_STATUS_OK);
        EXPECT_EQ(magma_get_semaphore_id(semaphore), id);
    }

    static void SemaphoreImportExport(TestConnection* test1, TestConnection* test2)
    {
        uint32_t handle;
        uint64_t id;
        test1->SemaphoreExport(&handle, &id);
        test2->SemaphoreImport(handle, id);
    }

    void ImageFormat()
    {
        fuchsia::sysmem::SingleBufferSettings buffer_settings;
        buffer_settings.has_image_format_constraints = true;
        buffer_settings.image_format_constraints.pixel_format.type =
            fuchsia::sysmem::PixelFormatType::NV12;
        buffer_settings.image_format_constraints.min_bytes_per_row = 128;
        buffer_settings.image_format_constraints.bytes_per_row_divisor = 256;
        buffer_settings.image_format_constraints.min_coded_height = 64;
        fidl::Encoder encoder(fidl::Encoder::NO_HEADER);
        encoder.Alloc(fidl::CodingTraits<fuchsia::sysmem::SingleBufferSettings>::encoded_size);
        buffer_settings.Encode(&encoder, 0);
        std::vector<uint8_t> encoded_bytes = encoder.TakeBytes();
        magma_buffer_format_description_t description;
        ASSERT_EQ(MAGMA_STATUS_OK, magma_get_buffer_format_description(
                                       encoded_bytes.data(), encoded_bytes.size(), &description));
        magma_image_plane_t planes[4];
        EXPECT_EQ(MAGMA_STATUS_OK, magma_get_buffer_format_plane_info(description, planes));

        EXPECT_EQ(256u, planes[0].bytes_per_row);
        EXPECT_EQ(0u, planes[0].byte_offset);
        EXPECT_EQ(256u, planes[1].bytes_per_row);
        EXPECT_EQ(256u * 64u, planes[1].byte_offset);
        magma_buffer_format_description_release(description);
        EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS,
                  magma_get_buffer_format_description(encoded_bytes.data(),
                                                      encoded_bytes.size() + 1, &description));
        EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS,
                  magma_get_buffer_format_description(encoded_bytes.data(),
                                                      encoded_bytes.size() - 1, &description));
    }

private:
    int fd_ = -1;
    magma_connection_t connection_;
};

TEST(MagmaAbi, DeviceId)
{
    TestConnection test;
    test.GetDeviceId();
}

TEST(MagmaAbi, Buffer)
{
    TestConnection test;
    test.Buffer();
}

TEST(MagmaAbi, Connection)
{
    TestConnection test;
    test.Connection();
}

TEST(MagmaAbi, Context)
{
    TestConnection test;
    test.Context();
}

TEST(MagmaAbi, NotificationChannelHandle)
{
    TestConnection test;
    test.NotificationChannelHandle();
}

TEST(MagmaAbi, ReadNotificationChannel)
{
    TestConnection test;
    test.ReadNotificationChannel();
}

TEST(MagmaAbi, BufferMap)
{
    TestConnection test;
    test.BufferMap();
}

TEST(MagmaAbi, BufferImportExport)
{
    TestConnection test1;
    TestConnection test2;
    TestConnection::BufferImportExport(&test1, &test2);
}

TEST(MagmaAbi, Semaphore)
{
    TestConnection test;
    test.Semaphore(1);
    test.Semaphore(2);
    test.Semaphore(3);
}

TEST(MagmaAbi, SemaphoreImportExport)
{
    TestConnection test1;
    TestConnection test2;
    TestConnection::SemaphoreImportExport(&test1, &test2);
}

TEST(MagmaAbi, ImageFormat)
{
    TestConnection test;
    test.ImageFormat();
}

TEST(MagmaAbi, FromC) { EXPECT_TRUE(test_magma_abi_from_c()); }
