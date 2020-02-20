// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "h264_multi_decoder.h"

#include <lib/media/codec_impl/codec_buffer.h>

#include <cmath>

#include "registers.h"
#include "util.h"

using InitFlagReg = AvScratch2;
using HeadPaddingReg = AvScratch3;
using H264DecodeModeReg = AvScratch4;
using H264DecodeSeqInfo = AvScratch5;
using NalSearchCtl = AvScratch9;
using H264AuxAddr = AvScratchC;
using H264DecodeSizeReg = AvScratchE;
using H264AuxDataSize = AvScratchH;
using FrameCounterReg = AvScratchI;
using DpbStatusReg = AvScratchJ;
using LmemDumpAddr = AvScratchL;
using DebugReg1 = AvScratchM;
using DebugReg2 = AvScratchN;

using H264DecodeInfo = M4ControlReg;

// AvScratch1
class StreamInfo : public TypedRegisterBase<DosRegisterIo, StreamInfo, uint32_t> {
 public:
  DEF_FIELD(7, 0, width_in_mbs);
  DEF_FIELD(23, 8, total_mbs);

  static auto Get() { return AddrType(0x09c1 * 4); }
};

// AvScratch2
class SequenceInfo : public TypedRegisterBase<DosRegisterIo, SequenceInfo, uint32_t> {
 public:
  DEF_BIT(0, aspect_ratio_info_present_flag);
  DEF_BIT(1, timing_info_present_flag);
  DEF_BIT(4, pic_struct_present_flag);

  // relatively lower-confidence vs. other bits - not confirmed
  DEF_BIT(6, fixed_frame_rate_flag);

  DEF_FIELD(14, 13, chroma_format_idc);
  DEF_BIT(15, frame_mbs_only_flag);
  DEF_FIELD(23, 16, aspect_ratio_idc);

  static auto Get() { return AddrType(0x09c2 * 4); }
};

// AvScratchB
class StreamInfo2 : public TypedRegisterBase<DosRegisterIo, StreamInfo2, uint32_t> {
 public:
  DEF_FIELD(7, 0, level_idc);
  DEF_FIELD(15, 8, max_reference_size);

  static auto Get() { return AddrType(0x09cb * 4); }
};

// AvScratchF
class CodecSettings : public TypedRegisterBase<DosRegisterIo, CodecSettings, uint32_t> {
 public:
  DEF_BIT(1, trickmode_i);
  DEF_BIT(2, zeroed0);
  DEF_BIT(3, drop_b_frames);
  DEF_BIT(4, error_recovery_mode);
  DEF_BIT(5, zeroed1);
  DEF_BIT(6, ip_frames_only);
  DEF_BIT(7, disable_fast_poc);

  static auto Get() { return AddrType(0x09cf * 4); }
};

enum DecodeMode {
  // Mode where multiple streams can be decoded, and input doesn't have to be
  // broken into frame-sized chunks.
  kDecodeModeMultiStreamBased = 0x2
};

// Actions written by CPU into DpbStatusReg to tell the firmware what to do.
enum H264Action {
  // Start searching for the head of a frame to decode.
  kH264ActionSearchHead = 0xf0,

  // Done responding to a config request.
  kH264ActionConfigDone = 0xf2,

  // Decode the first slice in a new picture.
  kH264ActionDecodeNewpic = 0xf3,
};

// Actions written by the firmware into DpbStatusReg before an interrupt to tell
// the CPU what to do.
enum H264Status {
  // Configure the DPB.
  kH264ConfigRequest = 0x11,

  // Out of input data, so get more.
  kH264DataRequest = 0x12,

  // Initialize the current set of reference frames and output buffer to be
  // decoded into.
  kH264SliceHeadDone = 0x1,

  // Store the current frame into the DPB, or output it.
  kH264PicDataDone = 0x2,
};

H264MultiDecoder::H264MultiDecoder(Owner* owner, Client* client)
    : VideoDecoder(owner, client, /*is_secure=*/false) {}

H264MultiDecoder::~H264MultiDecoder() {
  if (owner_->IsDecoderCurrent(this)) {
    owner_->core()->StopDecoding();
    owner_->core()->WaitForIdle();
  }
}

zx_status_t H264MultiDecoder::Initialize() {
  zx_status_t status = InitializeBuffers();
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to initialize buffers");
    return status;
  }

  return InitializeHardware();
}

zx_status_t H264MultiDecoder::LoadSecondaryFirmware(const uint8_t* data, uint32_t firmware_size) {
  // For some reason, some portions of the firmware aren't loaded into the
  // hardware directly, but are kept in main memory.
  constexpr uint32_t kSecondaryFirmwareSize = 4 * 1024;
  // Some sections of the input firmware are copied into multiple places in the output buffer, and 1
  // part of the output buffer seems to be unused.
  constexpr uint32_t kFirmwareSectionCount = 9;
  constexpr uint32_t kSecondaryFirmwareBufferSize = kSecondaryFirmwareSize * kFirmwareSectionCount;
  constexpr uint32_t kBufferAlignShift = 16;
  {
    zx_status_t status = io_buffer_init_aligned(&secondary_firmware_, owner_->bti()->get(),
                                                kSecondaryFirmwareBufferSize, kBufferAlignShift,
                                                IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (status != ZX_OK) {
      DECODE_ERROR("Failed to make second firmware buffer: %d", status);
      return status;
    }
    SetIoBufferName(&secondary_firmware_, "H264SecondaryFirmware");

    auto addr = static_cast<uint8_t*>(io_buffer_virt(&secondary_firmware_));
    // The secondary firmware is in a different order in the file than the main
    // firmware expects it to have.
    memcpy(addr + 0, data + 0x4000, kSecondaryFirmwareSize);                // header
    memcpy(addr + 0x1000, data + 0x2000, kSecondaryFirmwareSize);           // data
    memcpy(addr + 0x2000, data + 0x6000, kSecondaryFirmwareSize);           // mmc
    memcpy(addr + 0x3000, data + 0x3000, kSecondaryFirmwareSize);           // list
    memcpy(addr + 0x4000, data + 0x5000, kSecondaryFirmwareSize);           // slice
    memcpy(addr + 0x5000, data + 0x4000, kSecondaryFirmwareSize);           // main
    memcpy(addr + 0x5000 + 0x2000, data + 0x2000, kSecondaryFirmwareSize);  // data copy 2
    memcpy(addr + 0x5000 + 0x3000, data + 0x5000, kSecondaryFirmwareSize);  // slice copy 2
    ZX_DEBUG_ASSERT(0x5000 + 0x3000 + kSecondaryFirmwareSize == kSecondaryFirmwareBufferSize);
  }
  io_buffer_cache_flush(&secondary_firmware_, 0, kSecondaryFirmwareBufferSize);
  return ZX_OK;
}

constexpr uint32_t kAuxBufPrefixSize = 16 * 1024;
constexpr uint32_t kAuxBufSuffixSize = 0;

zx_status_t H264MultiDecoder::InitializeBuffers() {
  constexpr uint32_t kBufferAlignment = 1 << 16;
  constexpr uint32_t kCodecDataSize = 0x200000;
  auto codec_data_create_result =
      InternalBuffer::CreateAligned("H264MultiCodecData", &owner_->SysmemAllocatorSyncPtr(),
                                    owner_->bti(), kCodecDataSize, kBufferAlignment, is_secure(),
                                    /*is_writable=*/true, /*is_mapping_needed*/ false);
  if (!codec_data_create_result.is_ok()) {
    LOG(ERROR, "Failed to make codec data buffer - status: %d", codec_data_create_result.error());
    return codec_data_create_result.error();
  }
  codec_data_.emplace(codec_data_create_result.take_value());

  // Aux buf seems to be used for reading SEI data.
  constexpr uint32_t kAuxBufSize = kAuxBufPrefixSize + kAuxBufSuffixSize;
  auto aux_buf_create_result =
      InternalBuffer::CreateAligned("H264AuxBuf", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
                                    kAuxBufSize, kBufferAlignment, /*is_secure=*/false,
                                    /*is_writable=*/true, /*is_mapping_needed*/ false);
  if (!aux_buf_create_result.is_ok()) {
    LOG(ERROR, "Failed to make aux buffer - status: %d", aux_buf_create_result.error());
    return aux_buf_create_result.error();
  }
  aux_buf_.emplace(aux_buf_create_result.take_value());

  // Lmem is used to dump the AMRISC's local memory, which is needed for updating the DPB.
  constexpr uint32_t kLmemBufSize = 4096;
  auto lmem_create_result =
      InternalBuffer::CreateAligned("H264AuxBuf", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
                                    kLmemBufSize, kBufferAlignment, /*is_secure=*/false,
                                    /*is_writable=*/true, /*is_mapping_needed*/ true);
  if (!lmem_create_result.is_ok()) {
    LOG(ERROR, "Failed to make lmem buffer - status: %d", lmem_create_result.error());
    return lmem_create_result.error();
  }
  lmem_.emplace(lmem_create_result.take_value());

  return ZX_OK;
}

void H264MultiDecoder::ResetHardware() {
  DosSwReset0::Get().FromValue(0).set_vdec_mc(1).set_vdec_iqidct(1).set_vdec_vld_part(1).WriteTo(
      owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  DosSwReset0::Get().FromValue(0).set_vdec_mc(1).set_vdec_iqidct(1).set_vdec_vld_part(1).WriteTo(
      owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  DosSwReset0::Get().FromValue(0).set_vdec_pic_dc(1).set_vdec_dblk(1).WriteTo(owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  auto temp = PowerCtlVld::Get().ReadFrom(owner_->dosbus());
  temp.set_reg_value(temp.reg_value() | (1 << 9) | (1 << 6));
  temp.WriteTo(owner_->dosbus());
}

zx_status_t H264MultiDecoder::InitializeHardware() {
  if (is_secure()) {
    DECODE_ERROR("is_secure() == true not yet supported by H264MultiDecoder");
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t status =
      owner_->SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kVdec, is_secure());
  if (status != ZX_OK)
    return status;
  FirmwareBlob::FirmwareType firmware_type = FirmwareBlob::FirmwareType::kDec_H264_Multi_Gxm;

  // Don't use the TEE to load the firmware, since the version we're using on astro and sherlock
  // doesn't support H264_Multi_Gxm.
  uint8_t* data;
  uint32_t firmware_size;
  status = owner_->firmware_blob()->GetFirmwareData(firmware_type, &data, &firmware_size);
  if (status != ZX_OK)
    return status;
  status = owner_->core()->LoadFirmware(data, firmware_size);
  if (status != ZX_OK)
    return status;
  status = LoadSecondaryFirmware(data, firmware_size);
  if (status != ZX_OK)
    return status;
  BarrierAfterFlush();  // After secondary_firmware_ cache is flushed to RAM.

  ResetHardware();
  AvScratchG::Get()
      .FromValue(truncate_to_32(io_buffer_phys(&secondary_firmware_)))
      .WriteTo(owner_->dosbus());

  PscaleCtrl::Get().FromValue(0).WriteTo(owner_->dosbus());
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  VdecAssistMbox1Mask::Get().FromValue(1).WriteTo(owner_->dosbus());
  {
    auto temp = MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus()).set_nv12_output(true);
    temp.set_reg_value(temp.reg_value() | (0xbf << 24));
    temp.WriteTo(owner_->dosbus());
    temp.set_reg_value(temp.reg_value() & ~(0xbf << 24));
    temp.WriteTo(owner_->dosbus());
  }
  MdecPicDcMuxCtrl::Get().ReadFrom(owner_->dosbus()).set_bit31(0).WriteTo(owner_->dosbus());
  MdecExtIfCfg0::Get().FromValue(0).WriteTo(owner_->dosbus());
  MdecPicDcThresh::Get().FromValue(0x404038aa).WriteTo(owner_->dosbus());

  // Signal that the DPB hasn't been initialized yet.
  // TODO(fxb/13483): Initialize DPB when this is called a second time.
  AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch9::Get().FromValue(0).WriteTo(owner_->dosbus());
  DpbStatusReg::Get().FromValue(0).WriteTo(owner_->dosbus());

  FrameCounterReg::Get().FromValue(0).WriteTo(owner_->dosbus());

  constexpr uint32_t kBufferStartAddressOffset = 0x1000000;
  constexpr uint32_t kDcacReadMargin = 64 * 1024;
  uint32_t buffer_offset =
      truncate_to_32(codec_data_->phys_base()) - kBufferStartAddressOffset + kDcacReadMargin;
  AvScratch8::Get().FromValue(buffer_offset).WriteTo(owner_->dosbus());

  CodecSettings::Get()
      .ReadFrom(owner_->dosbus())
      .set_drop_b_frames(0)
      .set_zeroed0(0)
      .set_error_recovery_mode(1)
      .set_zeroed1(0)
      .set_ip_frames_only(0)
      .WriteTo(owner_->dosbus());

  LmemDumpAddr::Get().FromValue(truncate_to_32(lmem_->phys_base())).WriteTo(owner_->dosbus());
  DebugReg1::Get().FromValue(0).WriteTo(owner_->dosbus());
  DebugReg2::Get().FromValue(0).WriteTo(owner_->dosbus());
  H264DecodeInfo::Get().FromValue(1 << 13).WriteTo(owner_->dosbus());
  // TODO(fxb/13483): Use real values.
  constexpr uint32_t kBytesToDecode = 2000;
  H264DecodeSizeReg::Get().FromValue(kBytesToDecode).WriteTo(owner_->dosbus());
  ViffBitCnt::Get().FromValue(kBytesToDecode * 8).WriteTo(owner_->dosbus());

  H264AuxAddr::Get().FromValue(truncate_to_32(aux_buf_->phys_base())).WriteTo(owner_->dosbus());
  H264AuxDataSize::Get()
      .FromValue(((kAuxBufPrefixSize / 16) << 16) | (kAuxBufSuffixSize / 16))
      .WriteTo(owner_->dosbus());
  H264DecodeModeReg::Get().FromValue(kDecodeModeMultiStreamBased).WriteTo(owner_->dosbus());
  H264DecodeSeqInfo::Get().FromValue(0).WriteTo(owner_->dosbus());
  HeadPaddingReg::Get().FromValue(0).WriteTo(owner_->dosbus());
  // TODO(fxb/13483): Set to 1 on second initialization.
  InitFlagReg::Get().FromValue(0).WriteTo(owner_->dosbus());

  // TODO(fxb/13483): Set to 1 when SEI is supported.
  NalSearchCtl::Get().FromValue(0).WriteTo(owner_->dosbus());
  return ZX_OK;
}

void H264MultiDecoder::UpdateDecodeSize() {
  // For now, just use the decode size from InitializeHardware.
  owner_->core()->StartDecoding();
  DpbStatusReg::Get().FromValue(kH264ActionSearchHead).WriteTo(owner_->dosbus());
}

void H264MultiDecoder::ConfigureDpb() {
  // StreamInfo AKA AvScratch1.
  auto stream_info = StreamInfo::Get().ReadFrom(owner_->dosbus());
  // SequenceInfo AKA AvScratch2.
  auto sequence_info = SequenceInfo::Get().ReadFrom(owner_->dosbus());
  uint32_t mb_width = stream_info.width_in_mbs();
  if (!mb_width && stream_info.total_mbs())
    mb_width = 256;
  if (!mb_width) {
    // Not returning ZX_ERR_IO_DATA_INTEGRITY, because this isn't an explicit
    // integrity check.
    return;
  }
  uint32_t mb_height = stream_info.total_mbs() / mb_width;
  DLOG("Got width: %d height: %d, mbs_only %d info: %x\n", mb_width, mb_height,
       sequence_info.frame_mbs_only_flag(), stream_info.reg_value());
  auto info2 = StreamInfo2::Get().ReadFrom(owner_->dosbus());
  DLOG("Size: %d bits: %d\n", H264DecodeSizeReg::Get().ReadFrom(owner_->dosbus()).reg_value(),
       ViffBitCnt::Get().ReadFrom(owner_->dosbus()).reg_value());

  constexpr uint32_t kReferenceBufMargin = 4;
  next_max_reference_size_ = info2.max_reference_size() + kReferenceBufMargin;
  zx::bti bti;
  zx_status_t status = owner_->bti()->duplicate(ZX_RIGHT_SAME_RIGHTS, &bti);
  if (status != ZX_OK) {
    DECODE_ERROR("bti duplicate failed, status: %d\n", status);
    return;
  }
  constexpr uint32_t kMacroblockSize = 16;
  // TODO(fxb/13483): Calculate real values, taking into account more sequence
  // info.
  NalSearchCtl::Get().FromValue(0).WriteTo(owner_->dosbus());
  display_width_ = mb_width * kMacroblockSize;
  display_height_ = mb_width * kMacroblockSize;
  uint32_t min_frame_count = 22;
  uint32_t max_frame_count = 24;
  uint32_t coded_width = mb_width * kMacroblockSize;
  uint32_t coded_height = mb_height * kMacroblockSize;
  uint32_t stride = coded_width;
  bool has_sar = false;
  uint32_t sar_width = 0;
  uint32_t sar_height = 0;
  client_->InitializeFrames(std::move(bti), min_frame_count, max_frame_count, coded_width,
                            coded_height, stride, display_width_, display_height_, has_sar,
                            sar_width, sar_height);

  mb_width_ = mb_width;
  mb_height_ = mb_height;
}

// This struct contains parameters for the current frame that are dumped from
// lmem
struct HardwareRenderParams {
  uint16_t data[0x400];

  static constexpr uint32_t kNalUnitType = 0x80;
  static constexpr uint32_t kNalRefIdc = 0x81;
  static constexpr uint32_t kSliceType = 0x82;
  static constexpr uint32_t kLog2MaxFrameNum = 0x83;
  static constexpr uint32_t kPicOrderCntType = 0x85;
  static constexpr uint32_t kLog2MaxPicOrderCntLsb = 0x86;
  static constexpr uint32_t kEntropyCodingModeFlag = 0x8d;
  static constexpr uint32_t kProfileIdcMmco = 0xe7;

  // offset to dpb_max_buffer_frame.
  static constexpr uint32_t kDpbStructStart = 0x100 + 24 * 8;
  static constexpr uint32_t kPicOrderCntLsb = kDpbStructStart + 14;
  static constexpr uint32_t kDeltaPicOrderCntBottom0 = kDpbStructStart + 19;
  static constexpr uint32_t kDeltaPicOrderCntBottom1 = kDpbStructStart + 20;

  // Read a pair of entries starting at |offset| as a 32-bit number.
  uint32_t Read32(uint32_t offset) {
    // Little endian.
    return data[offset] | (static_cast<uint32_t>(data[offset + 1]) << 16);
  }

  void ReadFromLmem(InternalBuffer* lmem) {
    lmem->CacheFlushInvalidate(0, sizeof(data));
    uint16_t* input_params = reinterpret_cast<uint16_t*>(lmem->virt_base());

    // Convert from middle-endian.
    for (uint32_t i = 0; i < fbl::count_of(data); i += 4) {
      for (uint32_t j = 0; j < 4; j++) {
        data[i + j] = input_params[i + (3 - j)];
      }
    }
  }
};

void H264MultiDecoder::HandleSliceHeadDone() {
  // Setup reference frames and output buffers before decoding.
  HardwareRenderParams params;
  params.ReadFromLmem(&*lmem_);
  DLOG("NAL unit type: %d\n", params.data[HardwareRenderParams::kNalUnitType]);
  DLOG("NAL ref_idc: %d\n", params.data[HardwareRenderParams::kNalRefIdc]);
  DLOG("NAL slice_type: %d\n", params.data[HardwareRenderParams::kSliceType]);
  DLOG("pic order cnt type: %d\n", params.data[HardwareRenderParams::kPicOrderCntType]);
  DLOG("log2_max_frame_num: %d\n", params.data[HardwareRenderParams::kLog2MaxFrameNum]);
  DLOG("log2_max_pic_order_cnt: %d\n", params.data[HardwareRenderParams::kLog2MaxPicOrderCntLsb]);
  DLOG("entropy coding mode flag: %d\n", params.data[HardwareRenderParams::kEntropyCodingModeFlag]);
  DLOG("profile idc mmc0: %d\n", params.data[HardwareRenderParams::kProfileIdcMmco]);
  current_frame_ = &video_frames_[0];

  // Calculate the pic order count. This currently is good enough for the first
  // frame of bear.h264.
  // TODO(fxb/13483): Implement other types of calculations.
  ZX_DEBUG_ASSERT(params.data[HardwareRenderParams::kPicOrderCntType] == 0);
  uint32_t prevPicOrderCntMsb = 0;
  uint32_t prevPicOrderCntLsb = 0;
  uint32_t pic_order_cnt_lsb = params.data[HardwareRenderParams::kPicOrderCntLsb];
  uint32_t PicOrderCntMsb;
  uint32_t MaxPicOrderCntLsb = 1 << params.data[HardwareRenderParams::kLog2MaxPicOrderCntLsb];
  // H.264 8.2.1.1
  if (pic_order_cnt_lsb < prevPicOrderCntLsb &&
      (prevPicOrderCntLsb - pic_order_cnt_lsb >= (MaxPicOrderCntLsb / 2))) {
    PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;
  } else if (pic_order_cnt_lsb > prevPicOrderCntLsb &&
             (pic_order_cnt_lsb - prevPicOrderCntLsb > (MaxPicOrderCntLsb / 2))) {
    PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;
  } else {
    PicOrderCntMsb = prevPicOrderCntMsb;
  }

  uint32_t TopFieldOrderCnt = PicOrderCntMsb + pic_order_cnt_lsb;
  // Assume field_pic_flag = 0
  uint32_t BottomFieldOrderCnt =
      TopFieldOrderCnt + params.Read32(HardwareRenderParams::kDeltaPicOrderCntBottom0);
  uint32_t FramePicOrderCnt = std::min(TopFieldOrderCnt, BottomFieldOrderCnt);
  DLOG("Got frame pic order cnt: %d, lsb %d\n", FramePicOrderCnt, pic_order_cnt_lsb);

  H264CurrentPocIdxReset::Get().FromValue(0).WriteTo(owner_->dosbus());
  H264CurrentPoc::Get().FromValue(FramePicOrderCnt).WriteTo(owner_->dosbus());
  H264CurrentPoc::Get().FromValue(TopFieldOrderCnt).WriteTo(owner_->dosbus());
  H264CurrentPoc::Get().FromValue(BottomFieldOrderCnt).WriteTo(owner_->dosbus());

  CurrCanvasCtrl::Get()
      .FromValue(0)
      .set_canvas_index(current_frame_->index)
      .WriteTo(owner_->dosbus());
  // Unclear if reading from the register is actually necessary, or if this
  // would always be the same as above.
  uint32_t curr_canvas_index =
      CurrCanvasCtrl::Get().ReadFrom(owner_->dosbus()).lower_canvas_index();
  RecCanvasCtrl::Get().FromValue(curr_canvas_index).WriteTo(owner_->dosbus());
  DbkrCanvasCtrl::Get().FromValue(curr_canvas_index).WriteTo(owner_->dosbus());
  DbkwCanvasCtrl::Get().FromValue(curr_canvas_index).WriteTo(owner_->dosbus());

  // TODO(fxb/13483): BUFFER INFO data
  //
  // TODO(fxb/13483): Offset for multiple slices in same picture.

  H264CoMbWrAddr::Get()
      .FromValue(truncate_to_32(current_frame_->reference_mv_buffer.phys_base()))
      .WriteTo(owner_->dosbus());

  // TODO(fxb/13483) Initialize colocate mv read

  // TODO(fxb/13483): new slice, same pic:
  DpbStatusReg::Get().FromValue(kH264ActionDecodeNewpic).WriteTo(owner_->dosbus());
}

void H264MultiDecoder::HandlePicDataDone() {
  ZX_DEBUG_ASSERT(current_frame_);
  // TODO(fxb/13483): Get PTS
  // TODO(fxb/13483): Output frame only when past max_num_reorder_frames (or equivalent).
  client_->OnFrameReady(current_frame_->frame);
  // TODO(fxb/13483): store in DPB
  current_frame_ = nullptr;
  DpbStatusReg::Get().FromValue(kH264ActionSearchHead).WriteTo(owner_->dosbus());
}

void H264MultiDecoder::HandleInterrupt() {
  // Clear interrupt
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  uint32_t decode_status = DpbStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value();
  DLOG("Got H264MultiDecoder::HandleInterrupt, decode status: %x", decode_status);
  switch (decode_status) {
    case kH264ConfigRequest: {
      DpbStatusReg::Get().FromValue(kH264ActionConfigDone).WriteTo(owner_->dosbus());
      ConfigureDpb();
      break;
    }
    case kH264DataRequest:
      DECODE_ERROR("Got unhandled data request");
      break;
    case kH264SliceHeadDone: {
      HandleSliceHeadDone();
      break;
    }
    case kH264PicDataDone: {
      HandlePicDataDone();
      break;
    }
  }
}

void H264MultiDecoder::ReturnFrame(std::shared_ptr<VideoFrame> frame) {
  DLOG("Not implemented: %s\n", __func__);
}

void H264MultiDecoder::CallErrorHandler() { DLOG("Not implemented: %s\n", __func__); }

void H264MultiDecoder::InitializedFrames(std::vector<CodecFrame> frames, uint32_t coded_width,
                                         uint32_t coded_height, uint32_t stride) {
  uint32_t frame_count = frames.size();
  video_frames_.clear();
  for (uint32_t i = 0; i < frame_count; ++i) {
    auto frame = std::make_shared<VideoFrame>();
    // While we'd like to pass in IO_BUFFER_CONTIG, since we know the VMO was
    // allocated with zx_vmo_create_contiguous(), the io_buffer_init_vmo()
    // treats that flag as an invalid argument, so instead we have to pretend as
    // if it's a non-contiguous VMO, then validate that the VMO is actually
    // contiguous later in aml_canvas_config() called by
    // owner_->ConfigureCanvas() below.
    assert(frames[i].codec_buffer_spec.has_data());
    assert(frames[i].codec_buffer_spec.data().is_vmo());
    assert(frames[i].codec_buffer_spec.data().vmo().has_vmo_handle());
    zx_status_t status = io_buffer_init_vmo(
        &frame->buffer, owner_->bti()->get(),
        frames[i].codec_buffer_spec.data().vmo().vmo_handle().get(), 0, IO_BUFFER_RW);
    if (status != ZX_OK) {
      DECODE_ERROR("Failed to io_buffer_init_vmo() for frame - status: %d\n", status);
      OnFatalError();
      return;
    }
    io_buffer_cache_flush(&frame->buffer, 0, io_buffer_size(&frame->buffer, 0));

    BarrierAfterFlush();

    frame->hw_width = coded_width;
    frame->hw_height = coded_height;
    frame->coded_width = coded_width;
    frame->coded_height = coded_height;
    frame->stride = stride;
    frame->uv_plane_offset = stride * coded_height;
    frame->display_width = display_width_;
    frame->display_height = display_height_;
    frame->index = i;

    // can be nullptr
    frame->codec_buffer = frames[i].codec_buffer_ptr;
    if (frames[i].codec_buffer_ptr) {
      frames[i].codec_buffer_ptr->SetVideoFrame(frame);
    }

    // The ConfigureCanvas() calls validate that the VMO is physically
    // contiguous, regardless of how the VMO was created.
    auto y_canvas =
        owner_->ConfigureCanvas(&frame->buffer, 0, frame->stride, frame->coded_height, 0, 0);
    auto uv_canvas = owner_->ConfigureCanvas(&frame->buffer, frame->uv_plane_offset, frame->stride,
                                             frame->coded_height / 2, 0, 0);
    if (!y_canvas || !uv_canvas) {
      OnFatalError();
      return;
    }

    AncNCanvasAddr::Get(i)
        .FromValue((uv_canvas->index() << 16) | (uv_canvas->index() << 8) | (y_canvas->index()))
        .WriteTo(owner_->dosbus());
    constexpr uint32_t kMvRefDataSizePerMb = 96;
    uint32_t colocated_buffer_size =
        fbl::round_up(mb_width_ * mb_height_ * kMvRefDataSizePerMb, ZX_PAGE_SIZE);

    auto create_result =
        InternalBuffer::Create("H264ReferenceMvs", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
                               colocated_buffer_size, is_secure_,
                               /*is_writable=*/true, /*is_mapping_needed*/ false);
    if (!create_result.is_ok()) {
      LOG(ERROR, "Couldn't allocate reference mv buffer - status: %d", create_result.error());
      OnFatalError();
      return;
    }

    video_frames_.push_back({i, std::move(frame), std::move(y_canvas), std::move(uv_canvas),
                             create_result.take_value()});
  }
  AvScratch0::Get()
      .FromValue((next_max_reference_size_ << 24) | (frame_count << 16) | (frame_count << 8))
      .WriteTo(owner_->dosbus());
}

bool H264MultiDecoder::CanBeSwappedIn() {
  DLOG("Not implemented: %s\n", __func__);
  return true;
}

bool H264MultiDecoder::CanBeSwappedOut() const {
  DLOG("Not implemented: %s\n", __func__);
  return false;
}

void H264MultiDecoder::SetSwappedOut() { DLOG("Not implemented: %s\n", __func__); }

void H264MultiDecoder::SwappedIn() { DLOG("Not implemented: %s\n", __func__); }

void H264MultiDecoder::OnFatalError() {
  if (!fatal_error_) {
    fatal_error_ = true;
    client_->OnError();
  }
}
