// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_vsl_device.h"

#include <chrono>
#include <thread>

#include "instructions.h"
#include "magma_util/macros.h"
#include "magma_vendor_queries.h"
#include "msd.h"
#include "msd_vsl_context.h"
#include "platform_barriers.h"
#include "platform_logger.h"
#include "platform_mmio.h"
#include "platform_thread.h"
#include "registers.h"

static constexpr uint32_t kInterruptIndex = 0;

class MsdVslDevice::BatchRequest : public DeviceRequest {
 public:
  BatchRequest(std::unique_ptr<MappedBatch> batch) : batch_(std::move(batch)) {}

 protected:
  magma::Status Process(MsdVslDevice* device) override {
    return device->ProcessBatch(std::move(batch_));
  }

 private:
  std::unique_ptr<MappedBatch> batch_;
};

class MsdVslDevice::InterruptRequest : public DeviceRequest {
 public:
  InterruptRequest() {}

 protected:
  magma::Status Process(MsdVslDevice* device) override { return device->ProcessInterrupt(); }
};

MsdVslDevice::~MsdVslDevice() {
  CHECK_THREAD_NOT_CURRENT(device_thread_id_);

  DisableInterrupts();

  stop_interrupt_thread_ = true;
  if (interrupt_) {
    interrupt_->Signal();
  }
  if (interrupt_thread_.joinable()) {
    interrupt_thread_.join();
    DLOG("Joined interrupt thread");
  }

  stop_device_thread_ = true;

  if (device_request_semaphore_) {
    device_request_semaphore_->Signal();
  }

  if (device_thread_.joinable()) {
    DLOG("joining device thread");
    device_thread_.join();
    DLOG("joined");
  }
}

std::unique_ptr<MsdVslDevice> MsdVslDevice::Create(void* device_handle, bool start_device_thread) {
  auto device = std::make_unique<MsdVslDevice>();

  if (!device->Init(device_handle))
    return DRETP(nullptr, "Failed to initialize device");

  if (start_device_thread)
    device->StartDeviceThread();

  return device;
}

bool MsdVslDevice::Init(void* device_handle) {
  platform_device_ = magma::PlatformDevice::Create(device_handle);
  if (!platform_device_)
    return DRETF(false, "Failed to create platform device");

  std::unique_ptr<magma::PlatformMmio> mmio =
      platform_device_->CpuMapMmio(0, magma::PlatformMmio::CACHE_POLICY_UNCACHED_DEVICE);
  if (!mmio)
    return DRETF(false, "failed to map registers");

  register_io_ = std::make_unique<magma::RegisterIo>(std::move(mmio));

  device_id_ = registers::ChipId::Get().ReadFrom(register_io_.get()).chip_id().get();
  DLOG("Detected vsl chip id 0x%x", device_id_);

  if (device_id_ != 0x7000 && device_id_ != 0x8000)
    return DRETF(false, "Unspported gpu model 0x%x\n", device_id_);

  gpu_features_ = std::make_unique<GpuFeatures>(register_io_.get());
  DLOG("gpu features: 0x%x minor features 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
       gpu_features_->features().reg_value(), gpu_features_->minor_features(0),
       gpu_features_->minor_features(1), gpu_features_->minor_features(2),
       gpu_features_->minor_features(3), gpu_features_->minor_features(4),
       gpu_features_->minor_features(5));
  DLOG("halti5: %d mmu: %d", gpu_features_->halti5(), gpu_features_->has_mmu());

  DLOG(
      "stream count %u register_max %u thread_count %u vertex_cache_size %u shader_core_count "
      "%u pixel_pipes %u vertex_output_buffer_size %u\n",
      gpu_features_->stream_count(), gpu_features_->register_max(), gpu_features_->thread_count(),
      gpu_features_->vertex_cache_size(), gpu_features_->shader_core_count(),
      gpu_features_->pixel_pipes(), gpu_features_->vertex_output_buffer_size());
  DLOG("instruction count %u buffer_size %u num_constants %u varyings_count %u\n",
       gpu_features_->instruction_count(), gpu_features_->buffer_size(),
       gpu_features_->num_constants(), gpu_features_->varyings_count());

  if (!gpu_features_->features().pipe_3d().get())
    return DRETF(false, "Gpu has no 3d pipe: features 0x%x\n",
                 gpu_features_->features().reg_value());

  bus_mapper_ = magma::PlatformBusMapper::Create(platform_device_->GetBusTransactionInitiator());
  if (!bus_mapper_)
    return DRETF(false, "failed to create bus mapper");

  page_table_arrays_ = PageTableArrays::Create(bus_mapper_.get());
  if (!page_table_arrays_)
    return DRETF(false, "failed to create page table arrays");

  // TODO(fxb/43043): Implement and test ringbuffer wrapping.
  const uint32_t kRingbufferSize = magma::page_size();
  auto buffer = MsdVslBuffer::Create(kRingbufferSize, "ring-buffer");
  buffer->platform_buffer()->SetCachePolicy(MAGMA_CACHE_POLICY_UNCACHED);
  ringbuffer_ = std::make_unique<Ringbuffer>(std::move(buffer), 0 /* start_offset */);

  device_request_semaphore_ = magma::PlatformSemaphore::Create();

  Reset();
  if (!HardwareInit()) {
    return DRETF(false, "Failed to initialize hardware");
  }

  return true;
}

bool MsdVslDevice::HardwareInit() {
  interrupt_ = platform_device_->RegisterInterrupt(kInterruptIndex);
  if (!interrupt_) {
    return DRETF(false, "Failed to register interrupt");
  }

  {
    auto reg = registers::IrqEnable::Get().FromValue(~0);
    reg.WriteTo(register_io_.get());
  }

  {
    auto reg = registers::SecureAhbControl::Get().ReadFrom(register_io_.get());
    reg.non_secure_access().set(1);
    reg.WriteTo(register_io_.get());
  }

  page_table_arrays_->HardwareInit(register_io_.get());

  page_table_slot_allocator_ = std::make_unique<PageTableSlotAllocator>(page_table_arrays_->size());
  return true;
}

void MsdVslDevice::DisableInterrupts() {
  if (!register_io_) {
    DLOG("Register io was not initialized, skipping disabling interrupts");
    return;
  }
  auto reg = registers::IrqEnable::Get().FromValue(0);
  reg.WriteTo(register_io_.get());
}

void MsdVslDevice::StartDeviceThread() {
  DASSERT(!device_thread_.joinable());
  device_thread_ = std::thread([this] { this->DeviceThreadLoop(); });
  interrupt_thread_ = std::thread([this] { this->InterruptThreadLoop(); });
}

int MsdVslDevice::DeviceThreadLoop() {
  magma::PlatformThreadHelper::SetCurrentThreadName("DeviceThread");

  device_thread_id_ = std::make_unique<magma::PlatformThreadId>();
  CHECK_THREAD_IS_CURRENT(device_thread_id_);

  DLOG("DeviceThreadLoop starting thread 0x%lx", device_thread_id_->id());

  std::unique_ptr<magma::PlatformHandle> profile = platform_device_->GetSchedulerProfile(
      magma::PlatformDevice::kPriorityHigher, "msd-vsl-gc/device-thread");
  if (!profile) {
    return DRETF(false, "Failed to get higher priority");
  }
  if (!magma::PlatformThreadHelper::SetProfile(profile.get())) {
    return DRETF(false, "Failed to set priority");
  }

  std::unique_lock<std::mutex> lock(device_request_mutex_, std::defer_lock);

  while (!stop_device_thread_) {
    // TODO(fxb/44651): add a timeout to detect when the hardware is hung.
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max());
    magma::Status status = device_request_semaphore_->Wait(timeout.count());
    switch (status.get()) {
      case MAGMA_STATUS_OK:
        break;
      default:
        MAGMA_LOG(WARNING, "device_request_semaphore_ Wait failed: %d", status.get());
        DASSERT(false);
        // TODO(fxb/44475): handle wait errors.
    }

    while (true) {
      lock.lock();
      if (!device_request_list_.size()) {
        lock.unlock();
        break;
      }
      auto request = std::move(device_request_list_.front());
      device_request_list_.pop_front();
      lock.unlock();
      request->ProcessAndReply(this);
    }
  }

  DLOG("DeviceThreadLoop exit");
  return 0;
}

void MsdVslDevice::EnqueueDeviceRequest(std::unique_ptr<DeviceRequest> request) {
  std::unique_lock<std::mutex> lock(device_request_mutex_);
  device_request_list_.emplace_back(std::move(request));
  device_request_semaphore_->Signal();
}

int MsdVslDevice::InterruptThreadLoop() {
  magma::PlatformThreadHelper::SetCurrentThreadName("VSL InterruptThread");
  DLOG("VSL Interrupt thread started");

  std::unique_ptr<magma::PlatformHandle> profile = platform_device_->GetSchedulerProfile(
      magma::PlatformDevice::kPriorityHigher, "msd-vsl-gc/vsl-interrupt-thread");
  if (!profile) {
    return DRETF(0, "Failed to get higher priority");
  }
  if (!magma::PlatformThreadHelper::SetProfile(profile.get())) {
    return DRETF(0, "Failed to set priority");
  }

  while (!stop_interrupt_thread_) {
    interrupt_->Wait();

    if (stop_interrupt_thread_) {
      break;
    }

    auto request = std::make_unique<InterruptRequest>();
    auto reply = request->GetReply();
    EnqueueDeviceRequest(std::move(request));
    reply->Wait();
  }
  DLOG("VSL Interrupt thread exiting");
  return 0;
}

magma::Status MsdVslDevice::ProcessInterrupt() {
  CHECK_THREAD_IS_CURRENT(device_thread_id_);

  auto irq_status = registers::IrqAck::Get().ReadFrom(register_io_.get());
  auto mmu_exception = irq_status.mmu_exception().get();
  auto bus_error = irq_status.bus_error().get();
  auto value = irq_status.value().get();
  if (mmu_exception) {
    DMESSAGE("Interrupt thread received mmu_exception");
  }
  if (bus_error) {
    DMESSAGE("Interrupt thread received bus error");
  }
  // Check which bits are set and complete the corresponding event.
  for (unsigned int i = 0; i < kNumEvents; i++) {
    if (value & (1 << i)) {
      if (!CompleteInterruptEvent(i)) {
        DMESSAGE("Failed to complete event %u", i);
      }
    }
  }
  interrupt_->Complete();
  return MAGMA_STATUS_OK;
}

bool MsdVslDevice::AllocInterruptEvent(bool free_on_complete, uint32_t* out_event_id) {
  std::lock_guard<std::mutex> lock(events_mutex_);

  for (uint32_t i = 0; i < kNumEvents; i++) {
    if (!events_[i].allocated) {
      events_[i].allocated = true;
      events_[i].free_on_complete = free_on_complete;
      *out_event_id = i;
      return true;
    }
  }
  return DRETF(false, "No events are currently available");
}

bool MsdVslDevice::FreeInterruptEvent(uint32_t event_id) {
  std::lock_guard<std::mutex> lock(events_mutex_);

  if (event_id >= kNumEvents) {
    return DRETF(false, "Invalid event id %u", event_id);
  }
  if (!events_[event_id].allocated) {
    return DRETF(false, "Event id %u was not allocated", event_id);
  }
  events_[event_id] = {};
  return true;
}

// Writes an event into the end of the ringbuffer.
bool MsdVslDevice::WriteInterruptEvent(uint32_t event_id,
                                       std::unique_ptr<MappedBatch> mapped_batch) {
  std::lock_guard<std::mutex> lock(events_mutex_);

  if (event_id >= kNumEvents) {
    return DRETF(false, "Invalid event id %u", event_id);
  }
  if (!events_[event_id].allocated) {
    return DRETF(false, "Event id %u was not allocated", event_id);
  }
  if (events_[event_id].submitted) {
    return DRETF(false, "Event id %u was already submitted", event_id);
  }
  events_[event_id].submitted = true;
  events_[event_id].mapped_batch = std::move(mapped_batch);
  MiEvent::write(ringbuffer_.get(), event_id);
  return true;
}

bool MsdVslDevice::CompleteInterruptEvent(uint32_t event_id) {
  std::lock_guard<std::mutex> lock(events_mutex_);

  if (event_id >= kNumEvents) {
    return DRETF(false, "Invalid event id %u", event_id);
  }
  if (!events_[event_id].allocated || !events_[event_id].submitted) {
    return DRETF(false, "Cannot complete event %u, allocated %u submitted %u",
                 event_id, events_[event_id].allocated, events_[event_id].submitted);
  }
  bool free_on_complete = events_[event_id].free_on_complete;
  events_[event_id] = {};
  events_[event_id].allocated = !free_on_complete;
  return true;
}

void MsdVslDevice::Reset() {
  DLOG("Reset start");

  auto clock_control = registers::ClockControl::Get().FromValue(0);
  clock_control.isolate_gpu().set(1);
  clock_control.WriteTo(register_io());

  {
    auto reg = registers::SecureAhbControl::Get().FromValue(0);
    reg.reset().set(1);
    reg.WriteTo(register_io_.get());
  }

  std::this_thread::sleep_for(std::chrono::microseconds(100));

  clock_control.soft_reset().set(0);
  clock_control.WriteTo(register_io());

  clock_control.isolate_gpu().set(0);
  clock_control.WriteTo(register_io());

  clock_control = registers::ClockControl::Get().ReadFrom(register_io_.get());

  if (!IsIdle() || !clock_control.idle_3d().get()) {
    MAGMA_LOG(WARNING, "Gpu reset: failed to idle");
  }

  DLOG("Reset complete");
}

bool MsdVslDevice::IsIdle() {
  return registers::IdleState::Get().ReadFrom(register_io_.get()).IsIdle();
}

bool MsdVslDevice::StopRingbuffer() {
  if (IsIdle()) {
    return true;
  }
  // Overwrite the last WAIT with an END.
  uint32_t prev_wait_link = ringbuffer_->SubtractOffset(kWaitLinkDwords * sizeof(uint32_t));
  if (!ringbuffer_->Overwrite32(prev_wait_link, MiEnd::kCommandType)) {
    return DRETF(false, "Failed to overwrite WAIT in ringbuffer");
  }
  return true;
}

bool MsdVslDevice::WaitUntilIdle(uint32_t timeout_ms) {
  auto start = std::chrono::high_resolution_clock::now();
  while (!IsIdle() && std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::high_resolution_clock::now() - start)
                              .count() < timeout_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return IsIdle();
}

bool MsdVslDevice::LoadInitialAddressSpace(std::shared_ptr<AddressSpace> address_space,
                                           uint32_t address_space_index) {
  // Check if we have already configured an address space and enabled the MMU.
  if (page_table_arrays_->IsEnabled(register_io())) {
    return DRETF(false, "MMU already enabled");
  }
  static constexpr uint32_t kPageCount = 1;

  std::unique_ptr<magma::PlatformBuffer> buffer =
      magma::PlatformBuffer::Create(PAGE_SIZE * kPageCount, "address space config");
  if (!buffer) {
    return DRETF(false, "failed to create buffer");
  }

  auto bus_mapping = GetBusMapper()->MapPageRangeBus(buffer.get(), 0, kPageCount);
  if (!bus_mapping) {
    return DRETF(false, "failed to create bus mapping");
  }

  uint32_t* cmd_ptr;
  if (!buffer->MapCpu(reinterpret_cast<void**>(&cmd_ptr))) {
    return DRETF(false, "failed to map command buffer");
  }

  BufferWriter buf_writer(cmd_ptr, buffer->size(), 0);
  auto reg = registers::MmuPageTableArrayConfig::Get().addr();
  MiLoadState::write(&buf_writer, reg, address_space_index);
  MiEnd::write(&buf_writer);

  if (!buffer->UnmapCpu()) {
    return DRETF(false, "failed to unmap cpu");
  }
  if (!buffer->CleanCache(0, PAGE_SIZE * kPageCount, false)) {
    return DRETF(false, "failed to clean buffer cache");
  }

  auto res = SubmitCommandBufferNoMmu(bus_mapping->Get()[0], buf_writer.bytes_written());
  if (!res) {
    return DRETF(false, "failed to submit command buffer");
  }
  constexpr uint32_t kTimeoutMs = 100;
  if (!WaitUntilIdle(kTimeoutMs)) {
    return DRETF(false, "failed to wait for device to be idle");
  }

  page_table_arrays_->Enable(register_io(), true);

  DLOG("Address space loaded, index %u", address_space_index);

  return true;
}

bool MsdVslDevice::SubmitCommandBufferNoMmu(uint64_t bus_addr, uint32_t length,
                                            uint16_t* prefetch_out) {
  if (bus_addr & 0xFFFFFFFF00000000ul)
    return DRETF(false, "Can't submit address > 32 bits without mmu: 0x%08lx", bus_addr);

  uint32_t prefetch = magma::round_up(length, sizeof(uint64_t)) / sizeof(uint64_t);
  if (prefetch & 0xFFFF0000)
    return DRETF(false, "Can't submit length %u (prefetch 0x%x)", length, prefetch);

  prefetch &= 0xFFFF;
  if (prefetch_out) {
    *prefetch_out = prefetch;
  }

  DLOG("Submitting buffer at bus addr 0x%lx", bus_addr);

  auto reg_cmd_addr = registers::FetchEngineCommandAddress::Get().FromValue(0);
  reg_cmd_addr.addr().set(bus_addr & 0xFFFFFFFF);

  auto reg_cmd_ctrl = registers::FetchEngineCommandControl::Get().FromValue(0);
  reg_cmd_ctrl.enable().set(1);
  reg_cmd_ctrl.prefetch().set(prefetch);

  auto reg_sec_cmd_ctrl = registers::SecureCommandControl::Get().FromValue(0);
  reg_sec_cmd_ctrl.enable().set(1);
  reg_sec_cmd_ctrl.prefetch().set(prefetch);

  reg_cmd_addr.WriteTo(register_io_.get());
  reg_cmd_ctrl.WriteTo(register_io_.get());
  reg_sec_cmd_ctrl.WriteTo(register_io_.get());

  return true;
}

bool MsdVslDevice::StartRingbuffer(std::shared_ptr<AddressSpace> address_space) {
  if (!IsIdle()) {
    return true;  // Already running and looping on WAIT-LINK.
  }
  bool res = ringbuffer_->Map(address_space);
  if (!res) {
    return DRETF(res, "Could not map ringbuffer");
  }
  uint64_t rb_gpu_addr;
  res = ringbuffer_->GetGpuAddress(&rb_gpu_addr);
  if (!res) {
    return DRETF(res, "Could not get ringbuffer gpu address");
  }

  const uint16_t kRbPrefetch = 2;
  // Write the initial WAIT-LINK to the ringbuffer. The LINK points back to the WAIT,
  // and will keep looping until the WAIT is replaced with a LINK on command buffer submission.
  uint32_t wait_gpu_addr = rb_gpu_addr + ringbuffer_->tail();
  MiWait::write(ringbuffer_.get());
  MiLink::write(ringbuffer_.get(), kRbPrefetch, wait_gpu_addr);

  auto reg_cmd_addr = registers::FetchEngineCommandAddress::Get().FromValue(0);
  reg_cmd_addr.addr().set(static_cast<uint32_t>(wait_gpu_addr));

  auto reg_cmd_ctrl = registers::FetchEngineCommandControl::Get().FromValue(0);
  reg_cmd_ctrl.enable().set(1);
  reg_cmd_ctrl.prefetch().set(kRbPrefetch);

  auto reg_sec_cmd_ctrl = registers::SecureCommandControl::Get().FromValue(0);
  reg_sec_cmd_ctrl.enable().set(1);
  reg_sec_cmd_ctrl.prefetch().set(kRbPrefetch);

  reg_cmd_addr.WriteTo(register_io_.get());
  reg_cmd_ctrl.WriteTo(register_io_.get());
  reg_sec_cmd_ctrl.WriteTo(register_io_.get());
  return true;
}

bool MsdVslDevice::AddRingbufferWaitLink() {
  uint64_t rb_gpu_addr;
  bool res = ringbuffer_->GetGpuAddress(&rb_gpu_addr);
  if (!res) {
    return DRETF(false, "Failed to get ringbuffer gpu address");
  }
  uint32_t wait_gpu_addr = rb_gpu_addr + ringbuffer_->tail();
  MiWait::write(ringbuffer_.get());
  MiLink::write(ringbuffer_.get(), 2 /* prefetch */, wait_gpu_addr);
  return true;
}

bool MsdVslDevice::LinkRingbuffer(uint32_t wait_link_offset, uint32_t gpu_addr,
                                  uint32_t dest_prefetch) {
  DASSERT(ringbuffer_->IsOffsetPopulated(wait_link_offset));
  // We can assume the instruction was written as 8 contiguous bytes.
  DASSERT(ringbuffer_->IsOffsetPopulated(wait_link_offset + sizeof(uint32_t)));

  // Replace the penultimate WAIT (before the newly added one) with a LINK to the command buffer.
  // We will first modify the second dword which specifies the address,
  // as the hardware may be executing at the address of the current WAIT.
  ringbuffer_->Overwrite32(wait_link_offset + sizeof(uint32_t), gpu_addr);
  magma::barriers::Barrier();
  ringbuffer_->Overwrite32(wait_link_offset, MiLink::kCommandType | dest_prefetch);
  magma::barriers::Barrier();
  return true;
}

bool MsdVslDevice::WriteLinkCommand(magma::PlatformBuffer* buf, uint32_t length,
                                    uint16_t link_prefetch, uint32_t link_addr) {
  // Check if we have enough space for the LINK command.
  uint32_t link_instr_size = kInstructionDwords * sizeof(uint32_t);

  if (buf->size() < length + link_instr_size) {
    return DRETF(false, "Buffer does not have %d free bytes for ringbuffer LINK", link_instr_size);
  }

  uint32_t* buf_cpu_addr;
  bool res = buf->MapCpu(reinterpret_cast<void**>(&buf_cpu_addr));
  if (!res) {
    return DRETF(false, "Failed to map command buffer");
  }

  BufferWriter buf_writer(buf_cpu_addr, buf->size(), length);
  MiLink::write(&buf_writer, link_prefetch, link_addr);
  if (!buf->UnmapCpu()) {
    return DRETF(false, "Failed to unmap command buffer");
  }
  return true;
}

// When submitting a command buffer, we modify the following:
//  1) add a LINK from the command buffer to the end of the ringbuffer
//  2) add an EVENT and WAIT-LINK pair to the end of the ringbuffer
//  3) modify the penultimate WAIT in the ringbuffer to LINK to the command buffer
bool MsdVslDevice::SubmitCommandBuffer(std::shared_ptr<AddressSpace> address_space,
                                       uint32_t address_space_index,
                                       magma::PlatformBuffer* buf,
                                       std::unique_ptr<MappedBatch> mapped_batch,
                                       uint32_t event_id, uint16_t* prefetch_out) {
  // Check if we have loaded an address space and enabled the MMU.
  if (!page_table_arrays_->IsEnabled(register_io())) {
    if (!LoadInitialAddressSpace(address_space, address_space_index)) {
      return DRETF(false, "Failed to load initial address space");
    }
  }
  // Check if we have started the ringbuffer WAIT-LINK loop.
  if (IsIdle()) {
    if (!StartRingbuffer(address_space)) {
      return DRETF(false, "Failed to start ringbuffer");
    }
  }
  // Check if we need to switch address spaces.
  auto mapped_address_space = ringbuffer_->GetMappedAddressSpace().lock();
  // TODO(fxb/43718): support switching address spaces.
  // We will need to keep the previous address space alive until the switch is completed
  // by the hardware.
  if (!mapped_address_space || (mapped_address_space.get() != address_space.get())) {
    return DRETF(false, "Switching ringbuffer contexts not yet supported");
  }
  uint64_t rb_gpu_addr;
  bool res = ringbuffer_->GetGpuAddress(&rb_gpu_addr);
  if (!res) {
    return DRETF(false, "Failed to get ringbuffer gpu address");
  }
  uint32_t gpu_addr = mapped_batch->GetGpuAddress();
  uint32_t length = magma::round_up(mapped_batch->GetLength(), sizeof(uint64_t));

  // Number of new commands to be added to the ringbuffer - EVENT WAIT LINK.
  const uint16_t kRbPrefetch = 3;
  uint32_t prev_wait_link = ringbuffer_->SubtractOffset(kWaitLinkDwords * sizeof(uint32_t));

  if (buf) {
    // Write a LINK at the end of the command buffer that links back to the ringbuffer.
    if (!WriteLinkCommand(buf, length, kRbPrefetch,
                          static_cast<uint32_t>(rb_gpu_addr + ringbuffer_->tail()))) {
      return DRETF(false, "Failed to write LINK from command buffer to ringbuffer");
    }
    // Increment the command buffer length to account for the LINK command size.
    length += (kInstructionDwords * sizeof(uint32_t));
  } else {
    // If there is no command buffer, we link directly to the new ringbuffer commands.
    gpu_addr = rb_gpu_addr + ringbuffer_->tail();
    length = kRbPrefetch * sizeof(uint64_t);
  }

  uint32_t prefetch = magma::round_up(length, sizeof(uint64_t)) / sizeof(uint64_t);
  if (prefetch & 0xFFFF0000)
    return DRETF(false, "Can't submit length %u (prefetch 0x%x)", length, prefetch);

  if (prefetch_out) {
    *prefetch_out = prefetch & 0xFFFF;
  }

  // Write the new commands to the end of the ringbuffer.
  // Add an EVENT to the end to the ringbuffer.
  if (!WriteInterruptEvent(event_id, std::move(mapped_batch))) {
    return DRETF(false, "Failed to write interrupt event %u\n", event_id);
  }
  // Add a new WAIT-LINK to the end of the ringbuffer.
  if (!AddRingbufferWaitLink()) {
    return DRETF(false, "Failed to add WAIT-LINK to ringbuffer");
  }

  DLOG("Submitting buffer at gpu addr 0x%x", gpu_addr);

  if (!LinkRingbuffer(prev_wait_link, gpu_addr, prefetch)) {
    return DRETF(false, "Failed to link ringbuffer");
  }
  return true;
}

magma::Status MsdVslDevice::SubmitBatch(std::unique_ptr<MappedBatch> batch) {
  DLOG("SubmitBatch");
  CHECK_THREAD_NOT_CURRENT(device_thread_id_);

  EnqueueDeviceRequest(std::make_unique<BatchRequest>(std::move(batch)));
  return MAGMA_STATUS_OK;
}

magma::Status MsdVslDevice::ProcessBatch(std::unique_ptr<MappedBatch> batch) {
  CHECK_THREAD_IS_CURRENT(device_thread_id_);

  auto context = batch->GetContext().lock();
  if (!context) {
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "No context for batch %lu, IsCommandBuffer=%d",
                 batch->GetBatchBufferId(), batch->IsCommandBuffer());
  }
  auto address_space = context->exec_address_space();

  uint32_t event_id;
  if (!AllocInterruptEvent(true /* free_on_complete */, &event_id)) {
    // TODO(fxb/39354): queue the buffer to try again after an interrupt completes.
    return DRET_MSG(MAGMA_STATUS_UNIMPLEMENTED, "No events remaining");
  }
  magma::PlatformBuffer* buf = nullptr;
  if (batch->IsCommandBuffer()) {
    // TODO(fxb/39354): handle command buffers.
    return DRET_MSG(MAGMA_STATUS_UNIMPLEMENTED, "Command buffers not yet handled");
  }
  if (!SubmitCommandBuffer(address_space, address_space->page_table_array_slot(), buf,
                           std::move(batch), event_id, nullptr /* prefetch_out */)) {
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Failed to submit command buffer");
  }

  return MAGMA_STATUS_OK;
}

std::unique_ptr<MsdVslConnection> MsdVslDevice::Open(msd_client_id_t client_id) {
  uint32_t page_table_array_slot;
  if (!page_table_slot_allocator_->Alloc(&page_table_array_slot))
    return DRETP(nullptr, "couldn't allocate page table slot");

  auto address_space = AddressSpace::Create(this, page_table_array_slot);
  if (!address_space)
    return DRETP(nullptr, "failed to create address space");

  page_table_arrays_->AssignAddressSpace(page_table_array_slot, address_space.get());

  return std::make_unique<MsdVslConnection>(this, std::move(address_space), client_id);
}

magma_status_t MsdVslDevice::ChipIdentity(magma_vsl_gc_chip_identity* out_identity) {
  if (device_id() != 0x8000) {
    // TODO(fxb/37962): Read hardcoded values from features database instead.
    return DRET_MSG(MAGMA_STATUS_UNIMPLEMENTED, "unhandled device id 0x%x", device_id());
  }
  memset(out_identity, 0, sizeof(*out_identity));
  out_identity->chip_model = device_id();
  out_identity->chip_revision =
      registers::Revision::Get().ReadFrom(register_io_.get()).chip_revision().get();
  out_identity->chip_date =
      registers::ChipDate::Get().ReadFrom(register_io_.get()).chip_date().get();

  out_identity->stream_count = gpu_features_->stream_count();
  out_identity->pixel_pipes = gpu_features_->pixel_pipes();
  out_identity->resolve_pipes = 0x0;
  out_identity->instruction_count = gpu_features_->instruction_count();
  out_identity->num_constants = gpu_features_->num_constants();
  out_identity->varyings_count = gpu_features_->varyings_count();
  out_identity->gpu_core_count = 0x1;

  out_identity->product_id =
      registers::ProductId::Get().ReadFrom(register_io_.get()).product_id().get();
  out_identity->chip_flags = 0x4;
  out_identity->eco_id = registers::EcoId::Get().ReadFrom(register_io_.get()).eco_id().get();
  out_identity->customer_id =
      registers::CustomerId::Get().ReadFrom(register_io_.get()).customer_id().get();
  return MAGMA_STATUS_OK;
}

magma_status_t MsdVslDevice::ChipOption(magma_vsl_gc_chip_option* out_option) {
  if (device_id() != 0x8000) {
    // TODO(fxb/37962): Read hardcoded values from features database instead.
    return DRET_MSG(MAGMA_STATUS_UNIMPLEMENTED, "unhandled device id 0x%x", device_id());
  }
  memset(out_option, 0, sizeof(*out_option));
  out_option->gpu_profiler = false;
  out_option->allow_fast_clear = false;
  out_option->power_management = false;
  out_option->enable_mmu = true;
  out_option->compression = kVslGcCompressionOptionNone;
  out_option->usc_l1_cache_ratio = 0;
  out_option->secure_mode = kVslGcSecureModeNormal;
  return MAGMA_STATUS_OK;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

msd_connection_t* msd_device_open(msd_device_t* device, msd_client_id_t client_id) {
  auto connection = MsdVslDevice::cast(device)->Open(client_id);
  if (!connection)
    return DRETP(nullptr, "failed to create connection");
  return new MsdVslAbiConnection(std::move(connection));
}

void msd_device_destroy(msd_device_t* device) { delete MsdVslDevice::cast(device); }

magma_status_t msd_device_query(msd_device_t* device, uint64_t id, uint64_t* value_out) {
  switch (id) {
    case MAGMA_QUERY_VENDOR_ID:
      // VK_VENDOR_ID_VIV
      *value_out = 0x10001;
      return MAGMA_STATUS_OK;

    case MAGMA_QUERY_DEVICE_ID:
      *value_out = MsdVslDevice::cast(device)->device_id();
      return MAGMA_STATUS_OK;

    case MAGMA_QUERY_IS_TOTAL_TIME_SUPPORTED:
      *value_out = 0;
      return MAGMA_STATUS_OK;
  }
  return DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "unhandled id %" PRIu64, id);
}

static magma_status_t DataToBuffer(const char* name, void* data, uint64_t size,
                                   uint32_t* buffer_out) {
  std::unique_ptr<magma::PlatformBuffer> buffer = magma::PlatformBuffer::Create(size, name);
  if (!buffer) {
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Failed to allocate buffer");
  }
  if (!buffer->Write(data, 0, size)) {
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Failed to write result to buffer");
  }
  if (!buffer->duplicate_handle(buffer_out)) {
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Failed to duplicate handle");
  }
  return MAGMA_STATUS_OK;
}

magma_status_t msd_device_query_returns_buffer(msd_device_t* device, uint64_t id,
                                               uint32_t* buffer_out) {
  switch (id) {
    case kMsdVslVendorQueryChipIdentity: {
      magma_vsl_gc_chip_identity result;
      magma_status_t status = MsdVslDevice::cast(device)->ChipIdentity(&result);
      if (status != MAGMA_STATUS_OK) {
        return status;
      }
      return DataToBuffer("chip_identity", &result, sizeof(result), buffer_out);
    }
    case kMsdVslVendorQueryChipOption: {
      magma_vsl_gc_chip_option result;
      magma_status_t status = MsdVslDevice::cast(device)->ChipOption(&result);
      if (status != MAGMA_STATUS_OK) {
        return status;
      }
      return DataToBuffer("chip_option", &result, sizeof(result), buffer_out);
    }
    default:
      return DRET_MSG(MAGMA_STATUS_UNIMPLEMENTED, "unhandled id %" PRIu64, id);
  }
}

void msd_device_dump_status(msd_device_t* device, uint32_t dump_type) {}
