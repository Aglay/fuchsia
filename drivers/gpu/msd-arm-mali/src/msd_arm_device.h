// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MSD_ARM_DEVICE_H
#define MSD_ARM_DEVICE_H

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/display.h>
#include <ddk/protocol/platform-device.h>

#include "magma_util/macros.h"
#include "magma_util/register_io.h"
#include "magma_util/thread.h"
#include "msd.h"
#include "msd_arm_connection.h"
#include "platform_device.h"
#include "platform_interrupt.h"
#include "platform_semaphore.h"
#include <deque>
#include <list>
#include <mutex>
#include <thread>

class MsdArmDevice : public msd_device_t {
public:
    // Creates a device for the given |device_handle| and returns ownership.
    // If |start_device_thread| is false, then StartDeviceThread should be called
    // to enable device request processing.
    static std::unique_ptr<MsdArmDevice> Create(void* device_handle, bool start_device_thread);

    MsdArmDevice();

    virtual ~MsdArmDevice();

    static MsdArmDevice* cast(msd_device_t* dev)
    {
        DASSERT(dev);
        DASSERT(dev->magic_ == kMagic);
        return static_cast<MsdArmDevice*>(dev);
    }

    bool Init(void* device_handle);
    std::unique_ptr<MsdArmConnection> Open(msd_client_id_t client_id);

private:
#define CHECK_THREAD_IS_CURRENT(x)                                                                 \
    if (x)                                                                                         \
    DASSERT(magma::ThreadIdCheck::IsCurrent(*x))

#define CHECK_THREAD_NOT_CURRENT(x)                                                                \
    if (x)                                                                                         \
    DASSERT(!magma::ThreadIdCheck::IsCurrent(*x))

    RegisterIo* register_io()
    {
        DASSERT(register_io_);
        return register_io_.get();
    }

    void Destroy();
    void StartDeviceThread();
    int DeviceThreadLoop();
    int GpuInterruptThreadLoop();
    int JobInterruptThreadLoop();
    int MmuInterruptThreadLoop();
    bool InitializeInterrupts();
    void EnableInterrupts();
    void DisableInterrupts();

    static const uint32_t kMagic = 0x64657669; //"devi"

    std::thread device_thread_;
    std::unique_ptr<magma::PlatformThreadId> device_thread_id_;
    std::atomic_bool device_thread_quit_flag_{false};

    std::atomic_bool interrupt_thread_quit_flag_{false};
    std::thread gpu_interrupt_thread_;
    std::thread job_interrupt_thread_;
    std::thread mmu_interrupt_thread_;

    std::unique_ptr<magma::PlatformSemaphore> device_request_semaphore_;
    std::mutex device_request_mutex_;

    std::unique_ptr<magma::PlatformDevice> platform_device_;
    std::unique_ptr<RegisterIo> register_io_;
    std::unique_ptr<magma::PlatformInterrupt> gpu_interrupt_;
    std::unique_ptr<magma::PlatformInterrupt> job_interrupt_;
    std::unique_ptr<magma::PlatformInterrupt> mmu_interrupt_;
};

#endif // MSD_ARM_DEVICE_H
