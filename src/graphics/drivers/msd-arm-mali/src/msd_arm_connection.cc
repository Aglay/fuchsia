// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_arm_connection.h"

#include <zircon/compiler.h>

#include <limits>
#include <vector>

#include "address_space.h"
#include "gpu_mapping.h"
#include "magma_arm_mali_types.h"
#include "magma_util/dlog.h"
#include "msd_arm_buffer.h"
#include "msd_arm_context.h"
#include "msd_arm_device.h"
#include "msd_arm_perf_count_pool.h"
#include "msd_arm_semaphore.h"
#include "platform_barriers.h"
#include "platform_logger.h"
#include "platform_semaphore.h"
#include "platform_trace.h"

// This definition of arraysize was stolen from fxl in order to avoid
// a dynamic library dependency on it.
template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];
#define arraysize(array) (sizeof(ArraySizeHelper(array)))

void msd_connection_close(msd_connection_t* connection) {
  delete MsdArmAbiConnection::cast(connection);
}

msd_context_t* msd_connection_create_context(msd_connection_t* abi_connection) {
  auto connection = MsdArmAbiConnection::cast(abi_connection);
  auto context = std::make_unique<MsdArmContext>(connection->ptr());

  return context.release();
}

void msd_context_destroy(msd_context_t* ctx) {
  auto context = static_cast<MsdArmContext*>(ctx);
  auto connection = context->connection().lock();
  if (connection)
    connection->MarkDestroyed();
  delete context;
}

bool MsdArmConnection::ExecuteAtom(
    volatile magma_arm_mali_atom* atom,
    std::deque<std::shared_ptr<magma::PlatformSemaphore>>* semaphores) {
  TRACE_DURATION("magma", "Connection::ExecuteAtom");

  uint8_t atom_number = atom->atom_number;
  if (outstanding_atoms_[atom_number] &&
      outstanding_atoms_[atom_number]->result_code() == kArmMaliResultRunning) {
    MAGMA_LOG(WARNING, "Client %" PRIu64 ": Submitted atom number already in use", client_id_);
    return false;
  }
  uint32_t flags = atom->flags;
  magma_arm_mali_user_data user_data;
  user_data.data[0] = atom->data.data[0];
  user_data.data[1] = atom->data.data[1];
  std::shared_ptr<MsdArmAtom> msd_atom;
  if (flags & kAtomFlagSoftware) {
    if (flags != kAtomFlagSemaphoreSet && flags != kAtomFlagSemaphoreReset &&
        flags != kAtomFlagSemaphoreWait && flags != kAtomFlagSemaphoreWaitAndReset) {
      MAGMA_LOG(WARNING, "Client %" PRIu64 ": Invalid soft atom flags 0x%x", client_id_, flags);
      return false;
    }
    if (semaphores->empty()) {
      MAGMA_LOG(WARNING, "Client %" PRIu64 ": No remaining semaphores", client_id_);
      return false;
    }

    msd_atom = std::make_shared<MsdArmSoftAtom>(shared_from_this(), static_cast<AtomFlags>(flags),
                                                semaphores->front(), atom_number, user_data);
    semaphores->pop_front();
  } else {
    uint32_t slot = flags & kAtomFlagRequireFragmentShader ? 0 : 1;
    if (slot == 0 && (flags & (kAtomFlagRequireComputeShader | kAtomFlagRequireTiler))) {
      MAGMA_LOG(WARNING, "Client %" PRIu64 ": Invalid atom flags 0x%x", client_id_, flags);
      return false;
    }
#if defined(ENABLE_PROTECTED_DEBUG_SWAP_MODE)
    flags ^= kAtomFlagProtected;
#endif
    if ((flags & kAtomFlagProtected) && !owner_->IsProtectedModeSupported()) {
      MAGMA_LOG(WARNING, "Client %" PRIu64 ": Attempting to use protected mode when not supported",
                client_id_);
      return false;
    }

    msd_atom =
        std::make_shared<MsdArmAtom>(shared_from_this(), atom->job_chain_addr, slot, atom_number,
                                     user_data, atom->priority, static_cast<AtomFlags>(flags));

    if (flags & kAtomFlagRequireCycleCounter)
      msd_atom->set_require_cycle_counter();
  }

  {
    // Hold lock for using outstanding_atoms_.
    std::lock_guard<std::mutex> lock(callback_lock_);

    MsdArmAtom::DependencyList dependencies;
    for (size_t i = 0; i < arraysize(atom->dependencies); i++) {
      uint8_t dependency = atom->dependencies[i].atom_number;
      if (dependency) {
        if (!outstanding_atoms_[dependency]) {
          MAGMA_LOG(WARNING,
                    "Client %" PRIu64 ": Dependency on atom that hasn't been submitted yet",
                    client_id_);
          return false;
        }
        auto type = static_cast<ArmMaliDependencyType>(atom->dependencies[i].type);
        if (type != kArmMaliDependencyOrder && type != kArmMaliDependencyData) {
          MAGMA_LOG(WARNING, "Client %" PRIu64 ": Invalid dependency type: %d", client_id_, type);
          return false;
        }
        dependencies.push_back(MsdArmAtom::Dependency{type, outstanding_atoms_[dependency]});
      }
    }
    msd_atom->set_dependencies(dependencies);

    static_assert(arraysize(outstanding_atoms_) - 1 ==
                      std::numeric_limits<decltype(magma_arm_mali_atom::atom_number)>::max(),
                  "outstanding_atoms_ size is incorrect");

    outstanding_atoms_[atom_number] = msd_atom;
  }
  TRACE_FLOW_BEGIN("magma", "atom", msd_atom->trace_nonce());
  owner_->ScheduleAtom(std::move(msd_atom));
  return true;
}

magma_status_t msd_context_execute_command_buffer_with_resources(
    struct msd_context_t* ctx, struct magma_system_command_buffer* command_buffer,
    struct magma_system_exec_resource* exec_resources, struct msd_buffer_t** buffers,
    struct msd_semaphore_t** wait_semaphores, struct msd_semaphore_t** signal_semaphores) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t msd_context_execute_immediate_commands(msd_context_t* ctx, uint64_t commands_size,
                                                      void* commands, uint64_t semaphore_count,
                                                      msd_semaphore_t** msd_semaphores) {
  auto context = static_cast<MsdArmContext*>(ctx);
  auto connection = context->connection().lock();
  if (!connection)
    return DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "Connection not valid");

  std::deque<std::shared_ptr<magma::PlatformSemaphore>> semaphores;
  for (size_t i = 0; i < semaphore_count; i++) {
    semaphores.push_back(MsdArmAbiSemaphore::cast(msd_semaphores[i])->ptr());
  }
  uint64_t offset = 0;
  while (offset + sizeof(uint64_t) < commands_size) {
    magma_arm_mali_atom* atom =
        reinterpret_cast<magma_arm_mali_atom*>(static_cast<uint8_t*>(commands) + offset);
    if (atom->size < sizeof(uint64_t)) {
      return DRET_MSG(MAGMA_STATUS_CONTEXT_KILLED, "Atom size must be at least 8");
    }
    // Check for overflow (defined for unsigned types) or too large for buffer.
    if ((offset + atom->size < offset) || offset + atom->size > commands_size) {
      return DRET_MSG(MAGMA_STATUS_CONTEXT_KILLED, "Atom size %ld too large for buffer",
                      atom->size);
    }

    // This check could be changed to allow for backwards compatibility in
    // future versions.
    if (atom->size < sizeof(magma_arm_mali_atom)) {
      return DRET_MSG(MAGMA_STATUS_CONTEXT_KILLED, "Atom size %ld too small", atom->size);
    }

    if (!connection->ExecuteAtom(atom, &semaphores))
      return DRET(MAGMA_STATUS_CONTEXT_KILLED);
    offset += atom->size;
  }

  return MAGMA_STATUS_OK;
}

std::shared_ptr<MsdArmConnection> MsdArmConnection::Create(msd_client_id_t client_id,
                                                           Owner* owner) {
  auto connection = std::shared_ptr<MsdArmConnection>(new MsdArmConnection(client_id, owner));
  if (!connection->Init())
    return DRETP(nullptr, "Couldn't create connection");
  return connection;
}

bool MsdArmConnection::Init() {
  // If coherent memory is supported, use it for page tables to avoid
  // unnecessary cache flushes.
  address_space_ =
      AddressSpace::Create(this, owner_->cache_coherency_status() == kArmMaliCacheCoherencyAce);
  if (!address_space_)
    return DRETF(false, "Couldn't create address space");
  return true;
}

MsdArmConnection::MsdArmConnection(msd_client_id_t client_id, Owner* owner)
    : client_id_(client_id), owner_(owner) {}

MsdArmConnection::~MsdArmConnection() {
  if (perf_count_manager_) {
    auto* perf_count = performance_counters();
    owner_->RunTaskOnDeviceThread(
        [perf_count_manager = perf_count_manager_, perf_count](MsdArmDevice* device) {
          perf_count->RemoveManager(perf_count_manager.get());
          perf_count->Update();
          return MAGMA_STATUS_OK;
        });
  }

  owner_->DeregisterConnection();
}

static bool access_flags_from_flags(uint64_t mapping_flags, bool cache_coherent,
                                    uint64_t* flags_out) {
  uint64_t access_flags = 0;
  if (mapping_flags & MAGMA_GPU_MAP_FLAG_READ)
    access_flags |= kAccessFlagRead;
  if (mapping_flags & MAGMA_GPU_MAP_FLAG_WRITE)
    access_flags |= kAccessFlagWrite;
  if (!(mapping_flags & MAGMA_GPU_MAP_FLAG_EXECUTE))
    access_flags |= kAccessFlagNoExecute;
  if (mapping_flags & kMagmaArmMaliGpuMapFlagInnerShareable)
    access_flags |= kAccessFlagShareInner;
  if (mapping_flags & kMagmaArmMaliGpuMapFlagBothShareable) {
    if (!cache_coherent)
      return DRETF(false, "Attempting to use cache coherency while disabled.");
    access_flags |= kAccessFlagShareBoth;
  }

  // Protected memory doesn't affect the access flags - instead sysmem should set up the memory
  // controller to ensure everything can be accessed correctly from protected mode.
  if (mapping_flags &
      ~(MAGMA_GPU_MAP_FLAG_READ | MAGMA_GPU_MAP_FLAG_WRITE | MAGMA_GPU_MAP_FLAG_EXECUTE |
        MAGMA_GPU_MAP_FLAG_GROWABLE | kMagmaArmMaliGpuMapFlagInnerShareable |
        kMagmaArmMaliGpuMapFlagBothShareable | kMagmaArmMaliGpuMapFlagProtected))
    return DRETF(false, "Unsupported map flags %lx", mapping_flags);

  if (flags_out)
    *flags_out = access_flags;
  return true;
}

bool MsdArmConnection::AddMapping(std::unique_ptr<GpuMapping> mapping) {
  // The rest of this code assumes that the CPU page size is a multiple of the GPU page size.
  DASSERT(AddressSpace::is_mali_page_aligned(PAGE_SIZE));
  std::lock_guard<std::mutex> lock(address_lock_);
  uint64_t gpu_va = mapping->gpu_va();
  if (!magma::is_page_aligned(gpu_va))
    return DRETF(false, "mapping not page aligned");

  if (mapping->size() == 0)
    return DRETF(false, "empty mapping");

  uint64_t start_page = gpu_va / PAGE_SIZE;
  if (mapping->size() > (1ul << AddressSpace::kVirtualAddressSize))
    return DRETF(false, "size too large");

  uint64_t page_count = magma::round_up(mapping->size(), PAGE_SIZE) / PAGE_SIZE;
  if (start_page + page_count > ((1ul << AddressSpace::kVirtualAddressSize) / PAGE_SIZE))
    return DRETF(false, "virtual address too large");

  auto it = gpu_mappings_.upper_bound(gpu_va);
  if (it != gpu_mappings_.end() && (gpu_va + mapping->size() > it->second->gpu_va()))
    return DRETF(false, "Mapping overlaps existing mapping");
  // Find the mapping with the highest VA that's <= this.
  if (it != gpu_mappings_.begin()) {
    --it;
    // Check if the previous mapping overlaps this.
    if (it->second->gpu_va() + it->second->size() > gpu_va)
      return DRETF(false, "Mapping overlaps existing mapping");
  }
  auto buffer = mapping->buffer().lock();
  DASSERT(buffer);

  if (mapping->page_offset() + page_count > buffer->platform_buffer()->size() / PAGE_SIZE)
    return DRETF(false, "Buffer size %lx too small for map start %lx count %lx",
                 buffer->platform_buffer()->size(), mapping->page_offset(), page_count);

  if (!access_flags_from_flags(
          mapping->flags(), owner_->cache_coherency_status() == kArmMaliCacheCoherencyAce, nullptr))
    return false;

  if (!UpdateCommittedMemory(mapping.get()))
    return false;
  gpu_mappings_[gpu_va] = std::move(mapping);
  return true;
}

bool MsdArmConnection::RemoveMapping(uint64_t gpu_va) {
  std::lock_guard<std::mutex> lock(address_lock_);
  auto it = gpu_mappings_.find(gpu_va);
  if (it == gpu_mappings_.end())
    return DRETF(false, "Mapping not found");

  recently_removed_mappings_.push_front(
      std::make_pair<uint64_t>(it->second->gpu_va(), it->second->size()));
  while (recently_removed_mappings_.size() > kMaxStoredRemovedMappings) {
    recently_removed_mappings_.pop_back();
  }

  address_space_->Clear(it->second->gpu_va(), it->second->size());
  gpu_mappings_.erase(gpu_va);
  return true;
}

// CommitMemoryForBuffer or PageInAddress will hold address_lock_ before calling this, but that's
// impossible to specify for the thread safety analysis.
bool MsdArmConnection::UpdateCommittedMemory(GpuMapping* mapping) __TA_NO_THREAD_SAFETY_ANALYSIS {
  uint64_t access_flags = 0;
  if (!access_flags_from_flags(mapping->flags(),
                               owner_->cache_coherency_status() == kArmMaliCacheCoherencyAce,
                               &access_flags))
    return false;

  auto buffer = mapping->buffer().lock();
  DASSERT(buffer);

  Region committed_region = buffer->committed_region();
  Region mapping_region =
      Region::FromStartAndLength(mapping->page_offset(), mapping->size() / PAGE_SIZE);

  committed_region.Intersect(mapping_region);

  // If the current set of bus mappings contain pages that are not in the region, we need to throw
  // them out and make a new bus mapping.
  if (!committed_region.Contains(mapping->committed_region_in_buffer())) {
    auto regions_to_clear =
        mapping->committed_region_in_buffer().SubtractWithSplit(committed_region);
    for (auto region : regions_to_clear) {
      if (region.empty())
        continue;
      address_space_->Clear(
          mapping->gpu_va() + (region.start() - mapping->page_offset()) * PAGE_SIZE,
          region.length() * PAGE_SIZE);
    }
    // Technically if there's an IOMMU the new mapping might be at a different address, so we'd need
    // to update the GPU address space to represent that. However, on current systems (amlogic) that
    // doesn't happen.
    // TODO(fxbug.dev/32763): Shrink existing PMTs when that's supported.
    std::unique_ptr<magma::PlatformBusMapper::BusMapping> bus_mapping;
    if (committed_region.length() > 0) {
      bus_mapping = owner_->GetBusMapper()->MapPageRangeBus(
          buffer->platform_buffer(), committed_region.start(), committed_region.length());
      if (!bus_mapping)
        return DRETF(false, "Couldn't allocate new bus mapping");
    }
    mapping->ReplaceBusMappings(std::move(bus_mapping));
    return true;
  }

  std::vector<Region> new_regions;

  auto regions = committed_region.SubtractWithSplit(mapping->committed_region_in_buffer());
  for (Region region : regions) {
    if (!region.empty())
      new_regions.push_back(region);
  }

  if (new_regions.empty()) {
    // Sometimes an access to a growable region that was just grown can fault.  Unlock the MMU
    // if that's detected so the access can be retried.
    if (committed_region.length() > 0)
      address_space_->Unlock();
    return true;
  }

  for (auto& region : new_regions) {
    std::unique_ptr<magma::PlatformBusMapper::BusMapping> bus_mapping =
        owner_->GetBusMapper()->MapPageRangeBus(buffer->platform_buffer(), region.start(),
                                                region.length());
    if (!bus_mapping)
      return DRETF(false, "Couldn't pin region 0x%lx to 0x%lx", region.start(), region.length());

    magma_cache_policy_t cache_policy;
    magma_status_t status = buffer->platform_buffer()->GetCachePolicy(&cache_policy);
    if (!(mapping->flags() & kMagmaArmMaliGpuMapFlagBothShareable) &&
        (status != MAGMA_STATUS_OK || cache_policy == MAGMA_CACHE_POLICY_CACHED)) {
      // Flushing the region must happen after the region is mapped to the bus, as otherwise
      // the backing memory may not exist yet.
      if (!buffer->EnsureRegionFlushed(region.start() * PAGE_SIZE, region.end() * PAGE_SIZE))
        return DRETF(false, "EnsureRegionFlushed failed");
    }

    // Ensure mapping isn't put into the page table until the cache flush
    // above completed.
    magma::barriers::WriteBarrier();

    uint64_t offset_in_mapping = (region.start() - mapping->page_offset()) * PAGE_SIZE;

    if (!address_space_->Insert(mapping->gpu_va() + offset_in_mapping, bus_mapping.get(),
                                region.start() * PAGE_SIZE, region.length() * PAGE_SIZE,
                                access_flags)) {
      return DRETF(false, "Pages can't be inserted into address space");
    }

    mapping->AddBusMapping(std::move(bus_mapping));
  }

  return true;
}

bool MsdArmConnection::PageInMemory(uint64_t address) {
  std::lock_guard<std::mutex> lock(address_lock_);
  if (gpu_mappings_.empty())
    return false;

  auto it = gpu_mappings_.upper_bound(address);
  if (it == gpu_mappings_.begin())
    return false;
  --it;
  GpuMapping& mapping = *it->second.get();
  DASSERT(address >= mapping.gpu_va());
  if (address >= mapping.gpu_va() + mapping.size()) {
    MAGMA_LOG(WARNING,
              "Address 0x%lx is unmapped. Closest lower mapping is at 0x%lx, size 0x%lx (offset "
              "would be 0x%lx), flags 0x%lx",
              address, mapping.gpu_va(), mapping.size(), address - mapping.gpu_va(),
              mapping.flags());
    uint32_t i = 0;
    for (auto x : recently_removed_mappings_) {
      if (address >= x.first && address < x.first + x.second) {
        MAGMA_LOG(WARNING, "Found in part of mapping 0x%lx length 0x%lx found at index %d", x.first,
                  x.second, i);
      }
      i++;
    }
    return false;
  }
  if (!(mapping.flags() & MAGMA_GPU_MAP_FLAG_GROWABLE)) {
    Region committed_region = mapping.committed_region();
    MAGMA_LOG(WARNING,
              "Address 0x%lx at offset 0x%lx in non-growable mapping at 0x%lx, size 0x%lx, pinned "
              "region start offset 0x%lx, pinned region length 0x%lx "
              "flags 0x%lx",
              address, address - mapping.gpu_va(), mapping.gpu_va(), mapping.size(),
              committed_region.start() * PAGE_SIZE, committed_region.length() * PAGE_SIZE,
              mapping.flags());
    return false;
  }

  auto buffer = mapping.buffer().lock();
  DASSERT(buffer);

  // TODO(fxbug.dev/13028): Look into growing the buffer on a different thread.

  // Try to grow in units of 64 pages to avoid needing to fault too often.
  constexpr uint64_t kPagesToGrow = 64;
  constexpr uint64_t kCacheLineSize = 64;
  uint64_t offset_needed = address - mapping.gpu_va() + kCacheLineSize - 1;

  // Don't shrink the amount being committed if there's a race and the
  // client committed more memory between when the fault happened and this
  // code.
  uint64_t committed_page_count =
      std::max(buffer->committed_page_count(),
               magma::round_up(offset_needed, PAGE_SIZE * kPagesToGrow) / PAGE_SIZE);
  committed_page_count =
      std::min(committed_page_count,
               buffer->platform_buffer()->size() / PAGE_SIZE - buffer->start_committed_pages());

  // The MMU command to update the page tables should automatically cause
  // the atom to continue executing.
  return buffer->CommitPageRange(buffer->start_committed_pages(), committed_page_count);
}

bool MsdArmConnection::CommitMemoryForBuffer(MsdArmBuffer* buffer, uint64_t page_offset,
                                             uint64_t page_count) {
  std::lock_guard<std::mutex> lock(address_lock_);
  return buffer->CommitPageRange(page_offset, page_count);
}

bool MsdArmConnection::SetCommittedPagesForBuffer(MsdArmBuffer* buffer, uint64_t page_offset,
                                                  uint64_t page_count) {
  std::lock_guard<std::mutex> lock(address_lock_);
  return buffer->SetCommittedPages(page_offset, page_count);
}

bool MsdArmConnection::DecommitMemoryForBuffer(MsdArmBuffer* buffer, uint64_t page_offset,
                                               uint64_t page_count) {
  std::lock_guard<std::mutex> lock(address_lock_);
  return buffer->DecommitPageRange(page_offset, page_count);
}

void MsdArmConnection::SetNotificationCallback(msd_connection_notification_callback_t callback,
                                               void* token) {
  std::lock_guard<std::mutex> lock(callback_lock_);
  callback_ = callback;
  token_ = token;
}

void MsdArmConnection::SendNotificationData(MsdArmAtom* atom, ArmMaliResultCode result_code) {
  std::lock_guard<std::mutex> lock(callback_lock_);
  // It may already have been destroyed on the main thread.
  if (!token_)
    return;

  msd_notification_t notification = {.type = MSD_CONNECTION_NOTIFICATION_CHANNEL_SEND};
  static_assert(sizeof(magma_arm_mali_status) <= MSD_CHANNEL_SEND_MAX_SIZE,
                "notification too large");
  notification.u.channel_send.size = sizeof(magma_arm_mali_status);

  auto status = reinterpret_cast<magma_arm_mali_status*>(notification.u.channel_send.data);
  status->result_code = result_code;
  status->atom_number = atom->atom_number();
  status->data = atom->user_data();

  callback_(token_, &notification);
}

void MsdArmConnection::MarkDestroyed() {
  owner_->SetCurrentThreadToDefaultPriority();
  owner_->CancelAtoms(shared_from_this());

  std::lock_guard<std::mutex> lock(callback_lock_);
  if (!token_)
    return;

  msd_notification_t notification = {.type = MSD_CONNECTION_NOTIFICATION_CHANNEL_SEND};
  static_assert(sizeof(magma_arm_mali_status) <= MSD_CHANNEL_SEND_MAX_SIZE,
                "notification too large");
  notification.u.channel_send.size = sizeof(magma_arm_mali_status);

  auto status = reinterpret_cast<magma_arm_mali_status*>(notification.u.channel_send.data);
  status->result_code = kArmMaliResultTerminated;
  status->atom_number = {};
  status->data = {};

  callback_(token_, &notification);

  // Don't send any completion messages after termination.
  token_ = 0;
}

void MsdArmConnection::SendPerfCounterNotification(msd_notification_t* notification) {
  std::lock_guard<std::mutex> lock(callback_lock_);
  if (!token_)
    return;
  callback_(token_, notification);
}

bool MsdArmConnection::GetVirtualAddressFromPhysical(uint64_t address,
                                                     uint64_t* virtual_address_out) {
  std::lock_guard<std::mutex> lock(address_lock_);
  uint64_t page_address = address & ~(PAGE_SIZE - 1);
  for (auto& mapping : gpu_mappings_) {
    for (const std::unique_ptr<magma::PlatformBusMapper::BusMapping>& bus_mapping :
         mapping.second->bus_mappings()) {
      const std::vector<uint64_t>& page_list = bus_mapping->Get();
      for (uint32_t i = 0; i < page_list.size(); i++) {
        if (page_address == page_list[i]) {
          // Offset in bytes from the start of the vmo.
          uint64_t buffer_offset = (i + bus_mapping->page_offset()) * PAGE_SIZE;
          // Offset in bytes of the start of the mapping from the start of the
          // vmo.
          uint64_t mapping_offset = mapping.second->page_offset() * PAGE_SIZE;
          // The bus mapping shouldn't contain memory outside the gpu
          // offset.
          DASSERT(buffer_offset >= mapping_offset);
          uint64_t offset_in_page = address - page_address;
          *virtual_address_out =
              mapping.second->gpu_va() + buffer_offset - mapping_offset + offset_in_page;
          // Only return one virtual address.
          return true;
        }
      }
    }
  }
  return false;
}

magma_status_t MsdArmConnection::EnablePerformanceCounters(std::vector<uint64_t> flags) {
  bool start_managing = false;
  if (!perf_count_manager_) {
    perf_count_manager_ = std::make_shared<ConnectionPerfCountManager>();
    start_managing = true;
  }
  auto* perf_count = performance_counters();
  auto reply = owner_->RunTaskOnDeviceThread([perf_count_manager = perf_count_manager_, perf_count,
                                              flags = std::move(flags),
                                              start_managing](MsdArmDevice* device) {
    perf_count_manager->enabled_performance_counters_ = std::move(flags);
    if (start_managing) {
      if (!perf_count->AddManager(perf_count_manager.get()))
        return MAGMA_STATUS_INTERNAL_ERROR;
    }
    perf_count->Update();
    return MAGMA_STATUS_OK;
  });

  if (!start_managing) {
    // The call task can't fail, so return true immediately.
    return MAGMA_STATUS_OK;
  }
  // Wait so we can return the status of whether it succeeded or not.
  return reply->Wait().get();
}

magma_status_t MsdArmConnection::DumpPerformanceCounters(std::shared_ptr<MsdArmPerfCountPool> pool,
                                                         uint32_t trigger_id) {
  auto* perf_count = performance_counters();
  owner_->RunTaskOnDeviceThread([pool, perf_count, trigger_id](MsdArmDevice* device) {
    perf_count->AddClient(pool.get());
    pool->AddTriggerId(trigger_id);
    perf_count->TriggerRead();
    return MAGMA_STATUS_OK;
  });
  return MAGMA_STATUS_OK;
}

magma_status_t MsdArmConnection::ReleasePerformanceCounterBufferPool(
    std::shared_ptr<MsdArmPerfCountPool> pool) {
  auto* perf_count = performance_counters();
  auto reply = owner_->RunTaskOnDeviceThread([pool, perf_count](MsdArmDevice* device) {
    pool->set_valid(false);
    perf_count->RemoveClient(pool.get());
    return MAGMA_STATUS_OK;
  });

  // Wait for the set_valid to be processed to ensure that no more notifications will be sent about
  // the performance counter pool.
  return reply->Wait().get();
}

magma_status_t MsdArmConnection::AddPerformanceCounterBufferOffsetToPool(
    std::shared_ptr<MsdArmPerfCountPool> pool, std::shared_ptr<MsdArmBuffer> buffer,
    uint64_t buffer_id, uint64_t buffer_offset, uint64_t buffer_size) {
  owner_->RunTaskOnDeviceThread(
      [pool, buffer, buffer_id, buffer_offset, buffer_size](MsdArmDevice* device) {
        pool->AddBuffer(buffer, buffer_id, buffer_offset, buffer_size);
        return MAGMA_STATUS_OK;
      });
  return MAGMA_STATUS_OK;
}

magma_status_t MsdArmConnection::RemovePerformanceCounterBufferFromPool(
    std::shared_ptr<MsdArmPerfCountPool> pool, std::shared_ptr<MsdArmBuffer> buffer) {
  auto reply = owner_->RunTaskOnDeviceThread([pool, buffer](MsdArmDevice* device) {
    pool->RemoveBuffer(buffer);
    return MAGMA_STATUS_OK;
  });
  // Wait for the buffer to be removed to ensure that in-flight operations won't continue to use the
  // buffer.
  return reply->Wait().get();
}

magma_status_t msd_connection_map_buffer_gpu(msd_connection_t* abi_connection,
                                             msd_buffer_t* abi_buffer, uint64_t gpu_va,
                                             uint64_t page_offset, uint64_t page_count,
                                             uint64_t flags) {
  TRACE_DURATION("magma", "msd_connection_map_buffer_gpu", "page_count", page_count);
  MsdArmConnection* connection = MsdArmAbiConnection::cast(abi_connection)->ptr().get();

  auto mapping =
      std::make_unique<GpuMapping>(gpu_va, page_offset, page_count * PAGE_SIZE, flags, connection,
                                   MsdArmAbiBuffer::cast(abi_buffer)->base_ptr());
  if (!connection->AddMapping(std::move(mapping)))
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "AddMapping failed");
  return MAGMA_STATUS_OK;
}

magma_status_t msd_connection_unmap_buffer_gpu(msd_connection_t* abi_connection,
                                               msd_buffer_t* buffer, uint64_t gpu_va) {
  TRACE_DURATION("magma", "msd_connection_unmap_buffer_gpu");
  if (!MsdArmAbiConnection::cast(abi_connection)->ptr()->RemoveMapping(gpu_va))
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "RemoveMapping failed");
  return MAGMA_STATUS_OK;
}

magma_status_t msd_connection_commit_buffer(msd_connection_t* abi_connection,
                                            msd_buffer_t* abi_buffer, uint64_t page_offset,
                                            uint64_t page_count) {
  MsdArmConnection* connection = MsdArmAbiConnection::cast(abi_connection)->ptr().get();
  if (!connection->SetCommittedPagesForBuffer(MsdArmAbiBuffer::cast(abi_buffer)->base_ptr().get(),
                                              page_offset, page_count))
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "CommitMemoryForBuffer failed");
  return MAGMA_STATUS_OK;
}

magma_status_t msd_connection_buffer_range_op(msd_connection_t* abi_connection,
                                              msd_buffer_t* abi_buffer, uint32_t options,
                                              uint64_t start_offset, uint64_t length) {
  MsdArmConnection* connection = MsdArmAbiConnection::cast(abi_connection)->ptr().get();
  MsdArmBuffer* buffer = MsdArmAbiBuffer::cast(abi_buffer)->base_ptr().get();
  if (options == MAGMA_BUFFER_RANGE_OP_POPULATE_TABLES) {
    if (!connection->CommitMemoryForBuffer(buffer, start_offset / magma::page_size(),
                                           length / magma::page_size()))
      return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "CommitMemoryForBuffer failed");
  } else if (options == MAGMA_BUFFER_RANGE_OP_DEPOPULATE_TABLES) {
    if (!connection->DecommitMemoryForBuffer(buffer, start_offset / magma::page_size(),
                                             length / magma::page_size()))
      return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "CommitMemoryForBuffer failed");
  } else {
    return DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "Invalid options %d", options);
  }
  return MAGMA_STATUS_OK;
}

void msd_connection_set_notification_callback(msd_connection_t* abi_connection,
                                              msd_connection_notification_callback_t callback,
                                              void* token) {
  MsdArmAbiConnection::cast(abi_connection)->ptr()->SetNotificationCallback(callback, token);
}

void msd_connection_release_buffer(msd_connection_t* abi_connection, msd_buffer_t* abi_buffer) {}

magma_status_t msd_connection_enable_performance_counters(msd_connection_t* abi_connection,
                                                          const uint64_t* counters,
                                                          uint64_t counter_count) {
  auto connection = MsdArmAbiConnection::cast(abi_connection)->ptr();
  return connection->EnablePerformanceCounters(
      std::vector<uint64_t>(counters, counters + counter_count));
}

magma_status_t msd_connection_create_performance_counter_buffer_pool(
    struct msd_connection_t* connection, uint64_t pool_id, struct msd_perf_count_pool** pool_out) {
  auto pool =
      std::make_shared<MsdArmPerfCountPool>(MsdArmAbiConnection::cast(connection)->ptr(), pool_id);
  auto abi_pool = std::make_unique<MsdArmAbiPerfCountPool>(std::move(pool));
  *pool_out = abi_pool.release();
  return MAGMA_STATUS_OK;
}

magma_status_t msd_connection_release_performance_counter_buffer_pool(
    struct msd_connection_t* abi_connection, struct msd_perf_count_pool* abi_pool) {
  auto pool = MsdArmAbiPerfCountPool::cast(abi_pool)->ptr();
  auto connection = MsdArmAbiConnection::cast(abi_connection)->ptr();
  auto result = connection->ReleasePerformanceCounterBufferPool(pool);
  delete MsdArmAbiPerfCountPool::cast(abi_pool);
  return result;
}

magma_status_t msd_connection_dump_performance_counters(struct msd_connection_t* abi_connection,
                                                        struct msd_perf_count_pool* abi_pool,
                                                        uint32_t trigger_id) {
  auto pool = MsdArmAbiPerfCountPool::cast(abi_pool);
  return MsdArmAbiConnection::cast(abi_connection)
      ->ptr()
      ->DumpPerformanceCounters(pool->ptr(), trigger_id);
}

magma_status_t msd_connection_clear_performance_counters(struct msd_connection_t* connection,
                                                         const uint64_t* counters,
                                                         uint64_t counter_count) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t msd_connection_add_performance_counter_buffer_offset_to_pool(
    struct msd_connection_t* abi_connection, struct msd_perf_count_pool* abi_pool,
    struct msd_buffer_t* abi_buffer, uint64_t buffer_id, uint64_t buffer_offset,
    uint64_t buffer_size) {
  auto pool = MsdArmAbiPerfCountPool::cast(abi_pool);
  auto buffer = MsdArmAbiBuffer::cast(abi_buffer);
  uint64_t real_buffer_size = buffer->base_ptr()->platform_buffer()->size();

  if (buffer_offset > real_buffer_size || (real_buffer_size - buffer_offset) < buffer_size) {
    return DRET_MSG(MAGMA_STATUS_INVALID_ARGS,
                    "Invalid buffer size %lu offset %lu for buffer size %lu", buffer_size,
                    buffer_offset, real_buffer_size);
  }

  return MsdArmAbiConnection::cast(abi_connection)
      ->ptr()
      ->AddPerformanceCounterBufferOffsetToPool(pool->ptr(), buffer->base_ptr(), buffer_id,
                                                buffer_offset, buffer_size);

  return MAGMA_STATUS_OK;
}

magma_status_t msd_connection_remove_performance_counter_buffer_from_pool(
    struct msd_connection_t* abi_connection, struct msd_perf_count_pool* abi_pool,
    struct msd_buffer_t* abi_buffer) {
  auto pool = MsdArmAbiPerfCountPool::cast(abi_pool);
  auto buffer = MsdArmAbiBuffer::cast(abi_buffer);

  return MsdArmAbiConnection::cast(abi_connection)
      ->ptr()
      ->RemovePerformanceCounterBufferFromPool(pool->ptr(), buffer->base_ptr());
}
