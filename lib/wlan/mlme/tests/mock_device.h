// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_WLAN_MLME_TESTS_MOCK_DEVICE_H_
#define GARNET_LIB_WLAN_MLME_TESTS_MOCK_DEVICE_H_

#include <lib/timekeeper/test_clock.h>
#include <wlan/mlme/device_interface.h>
#include <wlan/mlme/mlme.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/service.h>
#include <wlan/mlme/timer.h>

#include <fuchsia/wlan/minstrel/cpp/fidl.h>

#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

#include "test_timer.h"

namespace wlan {

static constexpr uint8_t kClientAddress[] = {0x94, 0x3C, 0x49, 0x49, 0x9F, 0x2D};

namespace {

// TODO(hahnr): Support for failing various device calls.
struct MockDevice : public DeviceInterface {
   public:
    using PacketList = std::vector<fbl::unique_ptr<Packet>>;
    using KeyList = std::vector<wlan_key_config_t>;

    MockDevice() : sta_assoc_ctx_{} {
        state = fbl::AdoptRef(new DeviceState);
        common::MacAddr addr(kClientAddress);
        state->set_address(addr);

        auto info = &wlanmac_info.ifc_info;
        memcpy(info->mac_addr, kClientAddress, 6);
        info->driver_features = 0;
        info->mac_role = WLAN_MAC_ROLE_CLIENT;
        info->num_bands = 1;
        info->bands[0] = {
            .basic_rates = {12, 24, 48, 54, 96, 108},
            .supported_channels =
                {
                    .base_freq = 2407,
                    .channels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},
                },
            .ht_supported = false,
            .vht_supported = false,
        };
        state->set_channel(wlan_channel_t{.cbw = CBW20, .primary = 1});
    }

    // DeviceInterface implementation.

    zx_status_t GetTimer(uint64_t id, fbl::unique_ptr<Timer>* timer) override final {
        *timer = CreateTimer(id);
        return ZX_OK;
    }

    fbl::unique_ptr<Timer> CreateTimer(uint64_t id) {
        return fbl::make_unique<TestTimer>(id, &clock_);
    }

    zx_status_t DeliverEthernet(Span<const uint8_t> eth_frame) override final {
        eth_queue.push_back({eth_frame.cbegin(), eth_frame.cend()});
        return ZX_OK;
    }

    zx_status_t SendWlan(fbl::unique_ptr<Packet> packet, CBW cbw, PHY phy,
                         uint32_t flags) override final {
        wlan_queue.push_back(std::move(packet));
        return ZX_OK;
    }

    zx_status_t SendService(Span<const uint8_t> span) override final {
        std::vector<uint8_t> msg(span.cbegin(), span.cend());
        svc_queue.push_back(msg);
        return ZX_OK;
    }

    zx_status_t SetChannel(wlan_channel_t chan) override final {
        state->set_channel(chan);
        return ZX_OK;
    }

    zx_status_t SetStatus(uint32_t status) override final {
        state->set_online(status == 1);
        return ZX_OK;
    }

    zx_status_t ConfigureBss(wlan_bss_config_t* cfg) override final {
        if (!cfg) {
            bss_cfg.reset();
        } else {
            // Copy config which might get freed by the MLME before the result was verified.
            bss_cfg.reset(new wlan_bss_config_t);
            memcpy(bss_cfg.get(), cfg, sizeof(wlan_bss_config_t));
        }
        return ZX_OK;
    }

    zx_status_t ConfigureBeacon(fbl::unique_ptr<Packet> packet) override final {
        beacon = std::move(packet);
        return ZX_OK;
    }

    zx_status_t EnableBeaconing(wlan_bcn_config_t* bcn_cfg) override final {
        beaconing_enabled = (bcn_cfg != nullptr);
        return ZX_OK;
    }

    zx_status_t SetKey(wlan_key_config_t* cfg) override final {
        keys.push_back(*cfg);
        return ZX_OK;
    }

    zx_status_t StartHwScan(const wlan_hw_scan_config_t* scan_config) override {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t ConfigureAssoc(wlan_assoc_ctx_t* assoc_ctx) override final {
        sta_assoc_ctx_ = *assoc_ctx;
        return ZX_OK;
    }
    zx_status_t ClearAssoc(const common::MacAddr& peer_addr) override final {
        std::memset(&sta_assoc_ctx_, 0, sizeof(sta_assoc_ctx_));
        return ZX_OK;
    }

    fbl::RefPtr<DeviceState> GetState() override final { return state; }

    const wlanmac_info_t& GetWlanInfo() const override final { return wlanmac_info; }

    zx_status_t GetMinstrelPeers(::fuchsia::wlan::minstrel::Peers* peers_fidl) override final {
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_status_t GetMinstrelStats(const common::MacAddr& addr,
                                 ::fuchsia::wlan::minstrel::Peer* resp) override final {
        return ZX_ERR_NOT_SUPPORTED;
    }

    // Convenience methods.

    void AdvanceTime(zx::duration duration) { clock_.Set(zx::time() + duration); }

    void SetTime(zx::time time) { clock_.Set(time); }

    zx::time GetTime() { return clock_.Now(); }

    wlan_channel_t GetChannel() { return state->channel(); }

    uint16_t GetChannelNumber() { return state->channel().primary; }

    // kNoOrdinal means return the first message as <T> even though it might not be of type T.
    template <typename T>
    std::vector<MlmeMsg<T>> GetServiceMsgs(uint32_t ordinal = MlmeMsg<T>::kNoOrdinal) {
        std::vector<MlmeMsg<T>> ret;
        for (auto iter = svc_queue.begin(); iter != svc_queue.end(); ++iter) {
            auto msg = MlmeMsg<T>::Decode(*iter, ordinal);
            if (msg.has_value()) {
                ret.emplace_back(std::move(msg.value()));
                iter->clear();
            }
        }
        svc_queue.erase(
            std::remove_if(svc_queue.begin(), svc_queue.end(), [](auto& i) { return i.empty(); }),
            svc_queue.end());
        return ret;
    }

    std::vector<std::vector<uint8_t>> GetEthPackets() {
        std::vector<std::vector<uint8_t>> tmp;
        tmp.swap(eth_queue);
        return tmp;
    }

    PacketList GetWlanPackets() { return std::move(wlan_queue); }

    KeyList GetKeys() { return keys; }

    const wlan_assoc_ctx_t* GetStationAssocContext(void) { return &sta_assoc_ctx_; }

    bool AreQueuesEmpty() { return wlan_queue.empty() && svc_queue.empty() && eth_queue.empty(); }

    fbl::RefPtr<DeviceState> state;
    wlanmac_info_t wlanmac_info;
    PacketList wlan_queue;
    std::vector<std::vector<uint8_t>> svc_queue;
    std::vector<std::vector<uint8_t>> eth_queue;
    fbl::unique_ptr<wlan_bss_config_t> bss_cfg;
    KeyList keys;
    fbl::unique_ptr<Packet> beacon;
    bool beaconing_enabled;
    wlan_assoc_ctx_t sta_assoc_ctx_;

   private:
    timekeeper::TestClock clock_;
};
}  // namespace
}  // namespace wlan

#endif  // GARNET_LIB_WLAN_MLME_TESTS_MOCK_DEVICE_H_
