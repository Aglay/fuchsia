// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/algorithm.h>
#include <lib/media/codec_impl/codec_buffer.h>
#include <lib/media/codec_impl/codec_impl.h>
#include <lib/media/codec_impl/codec_port.h>
#include <zircon/assert.h>

namespace {

void BarrierAfterFlush() {
#if defined(__aarch64__)
  // According to the ARMv8 ARM K11.5.4 it's better to use DSB instead of DMB
  // for ordering with respect to MMIO (DMB is ok if all agents are just
  // observing memory). The system shareability domain is used because that's
  // the only domain the video decoder is guaranteed to be in. SY is used
  // instead of LD or ST because section B2.3.5 says that the barrier needs both
  // read and write access types to be effective with regards to cache
  // operations.
  asm __volatile__("dsb sy");
#elif defined(__x86_64__)
  // This is here just in case we both (a) don't need to flush cache on x86 due to cache coherent
  // DMA (CLFLUSH not needed), and (b) we have code using non-temporal stores or "string
  // operations" whose surrounding code didn't itself take care of doing an SFENCE.  After returning
  // from this function, we may write to MMIO to start DMA - we want any previous (program order)
  // non-temporal stores to be visible to HW before that MMIO write that starts DMA.  The MFENCE
  // instead of SFENCE is mainly paranoia, though one could hypothetically create HW that starts or
  // continues DMA based on an MMIO read (please don't), in which case MFENCE might be needed here
  // before that read.
  asm __volatile__("mfence");
#else
  ZX_PANIC("codec_buffer.cc missing BarrierAfterFlush() impl for this platform");
#endif
}

}  // namespace

CodecBuffer::CodecBuffer(CodecImpl* parent, CodecPort port, fuchsia::media::StreamBuffer buffer, bool is_secure)
    : parent_(parent), port_(port), buffer_(std::move(buffer)), is_secure_(is_secure) {
  // nothing else to do here
}

CodecBuffer::~CodecBuffer() {
  zx_status_t status;
  if (is_mapped_) {
    ZX_DEBUG_ASSERT(buffer_base_);
    uintptr_t unmap_address = fbl::round_down(reinterpret_cast<uintptr_t>(base()), ZX_PAGE_SIZE);
    size_t unmap_len = fbl::round_up(reinterpret_cast<uintptr_t>(base() + size()), ZX_PAGE_SIZE) - unmap_address;
    status =
        zx::vmar::root_self()->unmap(unmap_address, unmap_len);
    if (status != ZX_OK) {
      parent_->FailFatalLocked(
          "CodecBuffer::~CodecBuffer() failed to unmap() Buffer - status: %d", status);
    }
    buffer_base_ = nullptr;
    is_mapped_ = false;
  }
  if (pinned_) {
    status = pinned_.unpin();
    if (status != ZX_OK) {
      parent_->FailFatalLocked("CodecBuffer::~CodecBuffer() failed unpin() - status: %d", status);
    }
  }
}

bool CodecBuffer::Map() {
  ZX_DEBUG_ASSERT(buffer_.has_data());
  ZX_DEBUG_ASSERT(buffer_.data().is_vmo());
  const fuchsia::media::StreamBufferDataVmo& data_vmo = buffer_.data().vmo();
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_handle());
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_usable_size());
  ZX_DEBUG_ASSERT(!is_secure_);
  // Map the VMO in the local address space.
  uintptr_t tmp;
  zx_vm_option_t flags = ZX_VM_PERM_READ;
  if (port_ == kOutputPort) {
    flags |= ZX_VM_PERM_WRITE;
  }
  // We must page-align the mapping (since HW can only map at page granularity).  This means the
  // mapping may include up to ZX_PAGE_SIZE - 1 bytes before vmo_usable_start, and up to
  // ZX_PAGE_SIZE - 1 bytes after vmo_usable_start + vmo_usable_size.  The usage of the mapping is
  // expected to stay within CodecBuffer::base() to CodecBuffer::base() + vmo_usable_size.
  uint64_t vmo_offset = fbl::round_down(data_vmo.vmo_usable_start(), ZX_PAGE_SIZE);
  size_t len =
      fbl::round_up(data_vmo.vmo_usable_start() + data_vmo.vmo_usable_size(), ZX_PAGE_SIZE) -
      vmo_offset;
  zx_status_t res =
      zx::vmar::root_self()->map(0, data_vmo.vmo_handle(), vmo_offset, len, flags, &tmp);
  if (res != ZX_OK) {
    printf("Failed to map %zu byte buffer vmo (res %d)\n", data_vmo.vmo_usable_size(),
          res);
    return false;
  }
  buffer_base_ = reinterpret_cast<uint8_t*>(tmp + (data_vmo.vmo_usable_start() % ZX_PAGE_SIZE));
  is_mapped_ = true;
  return true;
}

void CodecBuffer::FakeMap(uint8_t* fake_map_addr) {
  ZX_DEBUG_ASSERT(buffer_.has_data());
  ZX_DEBUG_ASSERT(buffer_.data().is_vmo());
  const fuchsia::media::StreamBufferDataVmo& data_vmo = buffer_.data().vmo();
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_handle());
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_usable_size());
  ZX_DEBUG_ASSERT(reinterpret_cast<uintptr_t>(fake_map_addr) % ZX_PAGE_SIZE == 0);
  buffer_base_ = fake_map_addr + (data_vmo.vmo_usable_start() % ZX_PAGE_SIZE);
  ZX_DEBUG_ASSERT(!is_mapped_);
}

uint64_t CodecBuffer::lifetime_ordinal() const {
  ZX_DEBUG_ASSERT(buffer_.has_buffer_lifetime_ordinal());
  return buffer_.buffer_lifetime_ordinal();
}

uint32_t CodecBuffer::index() const {
  ZX_DEBUG_ASSERT(buffer_.has_buffer_index());
  return buffer_.buffer_index();
}

uint8_t* CodecBuffer::base() const {
  ZX_DEBUG_ASSERT(buffer_base_ && "Shouldn't be using if buffer was not mapped.");
  return buffer_base_;
}

zx_paddr_t CodecBuffer::physical_base() const {
  // Must call Pin() first.
  ZX_DEBUG_ASSERT(pinned_);
  // Else we'll need a different method that can deal with scattered pages.  For now we don't need
  // that.
  ZX_DEBUG_ASSERT(is_known_contiguous_);
  return contiguous_paddr_base_;
}

size_t CodecBuffer::size() const {
  ZX_DEBUG_ASSERT(buffer_.has_data());
  ZX_DEBUG_ASSERT(buffer_.data().is_vmo());
  ZX_DEBUG_ASSERT(buffer_.data().vmo().has_vmo_usable_size());
  return buffer_.data().vmo().vmo_usable_size();
}

const zx::vmo& CodecBuffer::vmo() const {
  ZX_ASSERT(buffer_.has_data());
  ZX_ASSERT(buffer_.data().is_vmo());
  return buffer_.data().vmo().vmo_handle();
}

uint64_t CodecBuffer::offset() const {
  ZX_ASSERT(buffer_.has_data());
  ZX_ASSERT(buffer_.data().is_vmo());
  ZX_ASSERT(buffer_.data().vmo().has_vmo_usable_start());
  return buffer_.data().vmo().vmo_usable_start();
}

const fuchsia::media::StreamBuffer& CodecBuffer::codec_buffer() const { return buffer_; }

void CodecBuffer::SetVideoFrame(std::weak_ptr<VideoFrame> video_frame) const {
  video_frame_ = video_frame;
}

std::weak_ptr<VideoFrame> CodecBuffer::video_frame() const { return video_frame_; }

zx_status_t CodecBuffer::Pin() {
  ZX_DEBUG_ASSERT(buffer_.has_data());
  ZX_DEBUG_ASSERT(buffer_.data().is_vmo());
  fuchsia::media::StreamBufferDataVmo& data_vmo = buffer_.mutable_data()->vmo();
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_handle());
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_usable_start());
  ZX_DEBUG_ASSERT(data_vmo.has_vmo_usable_size());

  zx::vmo* vmo = data_vmo.mutable_vmo_handle();
  zx_info_vmo_t info;
  zx_status_t status = vmo->get_info(ZX_INFO_VMO, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    return status;
  }
  if (!(info.flags & ZX_INFO_VMO_CONTIGUOUS)) {
    // Not supported yet.
    return ZX_ERR_NOT_SUPPORTED;
  }
  // We could potentially know this via the BufferCollectionInfo_2, but checking the VMO directly
  // also works fine.
  is_known_contiguous_ = true;

  // We must page-align the pin (since pining is page granularity).  This means the pin may include
  // up to ZX_PAGE_SIZE - 1 bytes before vmo_usable_start, and up to ZX_PAGE_SIZE - 1 bytes after
  // vmo_usable_start + vmo_usable_size. The usage of the pin is expected to stay within
  // CodecBuffer::base() to CodecBuffer::base() + vmo_usable_size.
  uint64_t pin_offset = fbl::round_down(data_vmo.vmo_usable_start(), ZX_PAGE_SIZE);
  uint64_t pin_size =
      fbl::round_up(data_vmo.vmo_usable_start() + data_vmo.vmo_usable_size(), ZX_PAGE_SIZE) -
      pin_offset;

  uint32_t options = ZX_BTI_CONTIGUOUS | ZX_BTI_PERM_READ;
  if (port_ == kOutputPort) {
    options |= ZX_BTI_PERM_WRITE;
  }

  zx_paddr_t paddr;
  status = parent_->Pin(options, *vmo, pin_offset, pin_size, &paddr, 1, &pinned_);
  if (status != ZX_OK) {
    return status;
  }
  // Include the low-order bits of vmo_usable_start() in contiguous_paddr_base_ so that the paddr
  // at contiguous_paddr_base_ points (physical) at the byte at offset vmo_usable_start() within
  // *vmo.
  contiguous_paddr_base_ = paddr + (data_vmo.vmo_usable_start() % ZX_PAGE_SIZE);
  return ZX_OK;
}

bool CodecBuffer::is_pinned() const {
  return !!pinned_;
}

zx_status_t CodecBuffer::CacheFlush(uint32_t offset, uint32_t length) const {
  ZX_DEBUG_ASSERT(!is_secure_);
  zx_status_t status;
  if (is_mapped_) {
    status = zx_cache_flush(base() + offset, length, ZX_CACHE_FLUSH_DATA);
  } else {
    const fuchsia::media::StreamBufferDataVmo& data_vmo = buffer_.data().vmo();
    status = vmo().op_range(
        ZX_VMO_OP_CACHE_CLEAN, data_vmo.vmo_usable_start() + offset, length, nullptr, 0);
  }
  BarrierAfterFlush();
  return status;
}
