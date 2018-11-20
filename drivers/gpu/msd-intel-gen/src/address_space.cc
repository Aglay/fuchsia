// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "address_space.h"
#include "gtt.h"

std::unique_ptr<GpuMapping> AddressSpace::MapBufferGpu(std::shared_ptr<AddressSpace> address_space,
                                                       std::shared_ptr<MsdIntelBuffer> buffer,
                                                       uint64_t offset, uint64_t length)
{
    DASSERT(address_space);
    DASSERT(buffer);

    length = address_space->GetMappedSize(length);

    if (!magma::is_page_aligned(offset))
        return DRETP(nullptr, "offset (0x%lx) not page aligned", offset);

    if (offset + length > buffer->platform_buffer()->size())
        return DRETP(nullptr, "offset (0x%lx) + length (0x%lx) > buffer size (0x%lx)", offset,
                     length, buffer->platform_buffer()->size());

    if (length > address_space->Size())
        return DRETP(nullptr, "length (0x%lx) > address space size (0x%lx)", length,
                     address_space->Size());

    uint64_t align_pow2;
    if (!magma::get_pow2(PAGE_SIZE, &align_pow2))
        return DRETP(nullptr, "alignment is not power of 2");

    // Casting to uint8_t below
    DASSERT((align_pow2 & ~0xFF) == 0);
    DASSERT(magma::is_page_aligned(length));

    gpu_addr_t gpu_addr;
    if (!address_space->Alloc(length, static_cast<uint8_t>(align_pow2), &gpu_addr))
        return DRETP(nullptr, "failed to allocate gpu address");

    DLOG("MapBufferGpu offset 0x%lx length 0x%lx allocated gpu_addr 0x%lx", offset, length,
         gpu_addr);

    uint64_t page_offset = offset / PAGE_SIZE;
    uint32_t page_count = length / PAGE_SIZE;

    std::unique_ptr<magma::PlatformBusMapper::BusMapping> bus_mapping;

    if (address_space->type() == ADDRESS_SPACE_PPGTT) {
        bus_mapping = address_space->owner_->GetBusMapper()->MapPageRangeBus(
            buffer->platform_buffer(), page_offset, page_count);
        if (!bus_mapping)
            return DRETP(nullptr, "failed to bus map the page range");

        if (!address_space->Insert(gpu_addr, bus_mapping.get()))
            return DRETP(nullptr, "failed to insert into address_space");

    } else {
        if (!static_cast<Gtt*>(address_space.get())
                 ->GlobalGttInsert(gpu_addr, buffer->platform_buffer(), page_offset, page_count))
            return DRETP(nullptr, "failed to insert into address_space");
    }

    return std::unique_ptr<GpuMapping>(
        new GpuMapping(address_space, buffer, offset, length, gpu_addr, std::move(bus_mapping)));
}

std::shared_ptr<GpuMapping>
AddressSpace::GetSharedGpuMapping(std::shared_ptr<AddressSpace> address_space,
                                  std::shared_ptr<MsdIntelBuffer> buffer, uint64_t offset,
                                  uint64_t length)
{
    DASSERT(address_space);
    DASSERT(buffer);

    auto range = address_space->mappings_by_buffer_.equal_range(buffer->platform_buffer());
    for (auto iter = range.first; iter != range.second; iter++) {
        auto& mapping = iter->second->second;
        if (mapping->offset() == offset && mapping->length() == GetMappedSize(length))
            return mapping;
    }

    std::shared_ptr<GpuMapping> mapping =
        AddressSpace::MapBufferGpu(address_space, buffer, offset, length);
    if (!mapping)
        return DRETP(nullptr, "Couldn't map buffer");

    if (!address_space->AddMapping(mapping))
        return DRETP(nullptr, "Couldn't add mapping");

    return mapping;
}

bool AddressSpace::AddMapping(std::shared_ptr<GpuMapping> gpu_mapping)
{
    auto iter = mappings_.upper_bound(gpu_mapping->gpu_addr());
    if (iter != mappings_.end() &&
        (gpu_mapping->gpu_addr() + gpu_mapping->length() > iter->second->gpu_addr()))
        return DRETF(false, "Mapping overlaps existing mapping");
    // Find the mapping with the highest VA that's <= this.
    if (iter != mappings_.begin()) {
        --iter;
        // Check if the previous mapping overlaps this.
        if (iter->second->gpu_addr() + iter->second->length() > gpu_mapping->gpu_addr())
            return DRETF(false, "Mapping overlaps existing mapping");
    }

    std::pair<map_container_t::iterator, bool> result =
        mappings_.insert({gpu_mapping->gpu_addr(), gpu_mapping});
    DASSERT(result.second);

    mappings_by_buffer_.insert({gpu_mapping->buffer()->platform_buffer(), result.first});

    return true;
}

void AddressSpace::ReleaseBuffer(magma::PlatformBuffer* buffer, uint32_t* released_count_out)
{
    *released_count_out = 0;
    auto range = mappings_by_buffer_.equal_range(buffer);
    for (auto iter = range.first; iter != range.second;) {
        mappings_.erase(iter->second);
        iter = mappings_by_buffer_.erase(iter);
        *released_count_out += 1;
    }
}
