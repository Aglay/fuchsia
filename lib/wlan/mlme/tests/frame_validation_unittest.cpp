// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test_data.h"

#include <wlan/mlme/debug.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/wlan.h>

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace wlan {
namespace {

struct TestEmptyHdr : public EmptyHdr {};

struct TestHdrA {
    uint8_t a;
    uint16_t b;
    uint8_t c;
    uint8_t d;

    constexpr size_t len() const { return sizeof(*this); }
} __PACKED;

struct TestHdrB {
    uint8_t a;
    uint8_t b;
    uint8_t c;

    constexpr size_t len() const { return sizeof(*this); }
} __PACKED;

// Dynamic length based on value of field `is_large`.
struct DynamicTestHdr {
    static constexpr size_t kLargeLength = 10;

    bool is_large = false;
    uint8_t b;
    uint8_t c;

    constexpr size_t len() const { return is_large ? kLargeLength : sizeof(*this); }
} __PACKED;

static fbl::unique_ptr<Packet> GetPacket(size_t len) {
    auto buffer = GetBuffer(len);
    auto pkt = fbl::make_unique<Packet>(fbl::move(buffer), len);
    pkt->clear();
    return fbl::move(pkt);
}

size_t no_padding(size_t v) {
    return v;
}

size_t add4BytesPadding(size_t v) {
    return v + 4;
}

void assert_correct_mac_frame_type(FrameType type, const Packet* pkt) {
    bool frame_valid = is_valid_frame_type<MgmtFrameHeader, UnknownBody>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == FrameType::kManagement);

    frame_valid = is_valid_frame_type<DataFrameHeader, UnknownBody>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == FrameType::kData);

    frame_valid = is_valid_frame_type<CtrlFrameHdr, UnknownBody>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == FrameType::kControl);
}

void assert_correct_mgmt_frame_type(ManagementSubtype type, const Packet* pkt) {
    assert_correct_mac_frame_type(FrameType::kManagement, pkt);

    bool frame_valid =
        is_valid_frame_type<MgmtFrameHeader, AssociationRequest>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kAssociationRequest);

    frame_valid =
        is_valid_frame_type<MgmtFrameHeader, AssociationResponse>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kAssociationResponse);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, ProbeRequest>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kProbeRequest);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, ProbeResponse>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kProbeResponse);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, Beacon>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kBeacon);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, Disassociation>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kDisassociation);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, Authentication>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kAuthentication);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, Deauthentication>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kDeauthentication);

    frame_valid = is_valid_frame_type<MgmtFrameHeader, ActionFrame>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ManagementSubtype::kAction);
}

void assert_correct_ctrl_frame_type(ControlSubtype type, const Packet* pkt) {
    assert_correct_mac_frame_type(FrameType::kControl, pkt);

    bool frame_valid = is_valid_frame_type<CtrlFrameHdr, PsPollFrame>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == ControlSubtype::kPsPoll);
}

void assert_correct_data_frame_type(DataSubtype type, const Packet* pkt) {
    assert_correct_mac_frame_type(FrameType::kData, pkt);

    bool frame_valid = is_valid_frame_type<DataFrameHeader, NullDataHdr>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == DataSubtype::kNull || type == DataSubtype::kQosnull);

    frame_valid = is_valid_frame_type<DataFrameHeader, LlcHeader>(pkt->data(), pkt->len());
    ASSERT_EQ(frame_valid, type == DataSubtype::kDataSubtype || type == DataSubtype::kQosdata);
}

Packet WrapInPacket(std::vector<uint8_t> data, Packet::Peer peer = Packet::Peer::kWlan) {
    auto buf = GetBuffer(data.size());
    Packet pkt(fbl::move(buf), data.size());
    pkt.CopyFrom(data.data(), data.size(), 0);
    pkt.set_peer(peer);
    return fbl::move(pkt);
}

TEST(FrameValidation, TestHdrLength) {
    constexpr size_t len = sizeof(TestHdrA);
    auto pkt = GetPacket(sizeof(TestHdrA));

    // Verifies the buffer is at least the size of the header.
    ASSERT_FALSE(is_valid_hdr_length<TestHdrA>(pkt->data(), 0));
    ASSERT_TRUE(is_valid_hdr_length<TestHdrA>(pkt->data(), len));
    ASSERT_TRUE(is_valid_hdr_length<TestHdrA>(pkt->data(), len + 1));
    ASSERT_FALSE(is_valid_hdr_length<TestHdrA>(pkt->data(), len - 1));
}

TEST(FrameValidation, TestHdrLength_IllegalBuffer) {
    constexpr size_t len = sizeof(TestHdrA);
    auto pkt = GetPacket(sizeof(TestHdrA));
    ASSERT_FALSE(is_valid_hdr_length<TestHdrA>(nullptr, len));
}

TEST(FrameValidation, TestEmptyHdrLength) {
    // Empty headers should always be valid, no matter of the buffer's length.
    auto pkt = GetPacket(10);
    ASSERT_TRUE(is_valid_hdr_length<TestEmptyHdr>(pkt->data(), 10));
    ASSERT_TRUE(is_valid_hdr_length<TestEmptyHdr>(pkt->data(), 0));
}

TEST(FrameValidation, TestEmptyHdrLength_IllegalBuffer) {
    auto pkt = GetPacket(10);
    ASSERT_FALSE(is_valid_hdr_length<TestEmptyHdr>(nullptr, 10));
}

TEST(FrameValidation, TestDynamicHdrLength) {
    constexpr size_t len = sizeof(DynamicTestHdr);
    auto pkt = GetPacket(DynamicTestHdr::kLargeLength);
    auto hdr = pkt->mut_field<DynamicTestHdr>(0);

    // Dynamically sized header is short and should fit into the buffer.
    hdr->is_large = false;
    ASSERT_TRUE(is_valid_hdr_length<DynamicTestHdr>(pkt->data(), len));

    // Dynamically sized header is large and should not fit into the short, but into the large buffer.
    hdr->is_large = true;
    ASSERT_FALSE(is_valid_hdr_length<DynamicTestHdr>(pkt->data(), len));
    ASSERT_TRUE(is_valid_hdr_length<DynamicTestHdr>(pkt->data(), DynamicTestHdr::kLargeLength));
}

TEST(FrameValidation, TestFrameLength_NoPadding) {
    constexpr size_t len = sizeof(TestHdrA) + sizeof(TestHdrB);
    auto pkt = GetPacket(len);

    // Verifies correct behavior for frames which use no padding.
    bool valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt->data(), len, no_padding);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt->data(), len + 1, no_padding);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt->data(), len - 1, no_padding);
    ASSERT_FALSE(valid_len);

    // Test convenience method.
    valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt.get(), 0);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt.get(), 1);
    ASSERT_FALSE(valid_len);
}

TEST(FrameValidation, TestFrameLength_EmptyBody_NoPadding) {
    constexpr size_t len = sizeof(TestHdrA);
    auto pkt = GetPacket(len);

    // Verifies correct behavior for frames which carry an empty body.
    bool valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt->data(), len, no_padding);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt->data(), len + 1, no_padding);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt->data(), len - 1, no_padding);
    ASSERT_FALSE(valid_len);

    // Test convenience method.
    valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt.get(), 0);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt.get(), 1);
    ASSERT_FALSE(valid_len);
}

TEST(FrameValidation, TestFrameLength_Padding) {
    constexpr size_t len = sizeof(TestHdrA) + 4 + sizeof(TestHdrB);
    auto pkt = GetPacket(len);

    // Verifies correct behavior for frames which use padding.
    bool valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt->data(), len, add4BytesPadding);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt->data(), len + 1, add4BytesPadding);
    ASSERT_TRUE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, TestHdrB>(pkt->data(), len - 1, add4BytesPadding);
    ASSERT_FALSE(valid_len);
}

TEST(FrameValidation, TestFrameLength_EmptyBody_Padding) {
    constexpr size_t len = sizeof(TestHdrA);
    auto pkt = GetPacket(len);

    // Verifies correct behavior for frames which carry an empty body and use padding.
    bool valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt->data(), len, add4BytesPadding);
    ASSERT_FALSE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt->data(), len + 3, add4BytesPadding);
    ASSERT_FALSE(valid_len);
    valid_len = is_valid_frame_length<TestHdrA, UnknownBody>(pkt->data(), len + 4, add4BytesPadding);
    ASSERT_TRUE(valid_len);
}

TEST(FrameValidation, ValidBeaconType) {
    auto pkt = WrapInPacket(kBeaconFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kBeacon, &pkt);
}

TEST(FrameValidation, ValidPsPollFrameType) {
    auto pkt = WrapInPacket(kPsPollFrame);
    assert_correct_ctrl_frame_type(ControlSubtype::kPsPoll, &pkt);
}

TEST(FrameValidation, ValidDeauthFrameType) {
    auto pkt = WrapInPacket(kDeauthFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kDeauthentication, &pkt);
}

TEST(FrameValidation, ValidActionFrameType) {
    auto pkt = WrapInPacket(kActionFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kAction, &pkt);
}

TEST(FrameValidation, ValidProbeRequestFrameType) {
    auto pkt = WrapInPacket(kProbeRequestFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kProbeRequest, &pkt);
}

TEST(FrameValidation, ValidAssocRequestFrameType) {
    auto pkt = WrapInPacket(kAssocReqFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kAssociationRequest, &pkt);
}

TEST(FrameValidation, ValidAssocResponseFrameType) {
    auto pkt = WrapInPacket(kAssocRespFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kAssociationResponse, &pkt);
}

TEST(FrameValidation, ValidAuthFrameType) {
    auto pkt = WrapInPacket(kAuthFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kAuthentication, &pkt);
}

TEST(FrameValidation, ValidDisassocFrameType) {
    auto pkt = WrapInPacket(kDisassocFrame);
    assert_correct_mgmt_frame_type(ManagementSubtype::kDisassociation, &pkt);
}

TEST(FrameValidation, ValidNullDataFrameType) {
    auto pkt = WrapInPacket(kNullDataFrame);
    assert_correct_data_frame_type(DataSubtype::kNull, &pkt);
}

TEST(FrameValidation, ValidQosNullDataFrameType) {
    auto pkt = WrapInPacket(kQosNullDataFrame);
    assert_correct_data_frame_type(DataSubtype::kQosnull, &pkt);
}

TEST(FrameValidation, ValidDataFrameType) {
    auto pkt = WrapInPacket(kDataFrame);
    assert_correct_data_frame_type(DataSubtype::kDataSubtype, &pkt);
}

TEST(FrameValidation, ValidQosDataFrameType) {
auto pkt = WrapInPacket(kQosDataFrame);
assert_correct_data_frame_type(DataSubtype::kQosdata, &pkt);
}

}  // namespace
}  // namespace wlan
