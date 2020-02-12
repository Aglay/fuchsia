// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_FRAME_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_FRAME_H_

#include <zircon/types.h>

#include <list>

#include "sim-sta-ifc.h"
#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/mac_frame.h"

namespace wlan::simulation {

class StationIfc;

class InformationElement {
 public:
  enum SimIEType { IE_TYPE_CSA = 37, IE_TYPE_WPA1 = 221, IE_TYPE_WPA2 = 48 };

  explicit InformationElement() = default;
  virtual ~InformationElement();

  virtual SimIEType IEType() const = 0;
};

// IEEE Std 802.11-2016, 9.4.2.19
class CSAInformationElement : public InformationElement {
 public:
  explicit CSAInformationElement(bool switch_mode, uint8_t new_channel, uint8_t switch_count) {
    channel_switch_mode_ = switch_mode;
    new_channel_number_ = new_channel;
    channel_switch_count_ = switch_count;
  };

  ~CSAInformationElement() override;

  SimIEType IEType() const override;

  bool channel_switch_mode_;
  uint8_t new_channel_number_;
  uint8_t channel_switch_count_;
};

class SimFrame {
 public:
  enum SimFrameType { FRAME_TYPE_MGMT, FRAME_TYPE_CTRL, FRAME_TYPE_DATA };

  SimFrame() = default;
  explicit SimFrame(StationIfc* sender) : sender_(sender){};
  virtual ~SimFrame();

  // Frame type identifier
  virtual SimFrameType FrameType() const = 0;

  StationIfc* sender_;
};

class SimManagementFrame : public SimFrame {
 public:
  enum SimMgmtFrameType {
    FRAME_TYPE_BEACON,
    FRAME_TYPE_PROBE_REQ,
    FRAME_TYPE_PROBE_RESP,
    FRAME_TYPE_ASSOC_REQ,
    FRAME_TYPE_ASSOC_RESP,
    FRAME_TYPE_DISASSOC_REQ
  };

  SimManagementFrame(){};
  explicit SimManagementFrame(StationIfc* sender) : SimFrame(sender){};

  ~SimManagementFrame() override;

  // Frame type identifier
  SimFrameType FrameType() const override;
  // Frame subtype identifier for management frames
  virtual SimMgmtFrameType MgmtFrameType() const = 0;
  void AddCSAIE(const wlan_channel_t& channel, uint8_t channel_switch_count);
  std::shared_ptr<InformationElement> FindIE(InformationElement::SimIEType ie_type) const;
  void RemoveIE(InformationElement::SimIEType);

  std::list<std::shared_ptr<InformationElement>> IEs_;

 private:
  void AddIE(InformationElement::SimIEType ie_type, std::shared_ptr<InformationElement> ie);
};

class SimBeaconFrame : public SimManagementFrame {
 public:
  SimBeaconFrame() = default;
  explicit SimBeaconFrame(StationIfc* sender, const wlan_ssid_t& ssid, const common::MacAddr& bssid)
      : SimManagementFrame(sender), ssid_(ssid), bssid_(bssid){};

  ~SimBeaconFrame() override;

  SimMgmtFrameType MgmtFrameType() const override;

  wlan_ssid_t ssid_;
  common::MacAddr bssid_;
  wlan::CapabilityInfo capability_info_;
};

class SimProbeReqFrame : public SimManagementFrame {
 public:
  SimProbeReqFrame() = default;
  explicit SimProbeReqFrame(StationIfc* sender, const common::MacAddr& src)
      : SimManagementFrame(sender), src_addr_(src){};

  ~SimProbeReqFrame() override;

  SimMgmtFrameType MgmtFrameType() const override;

  common::MacAddr src_addr_;
};

class SimProbeRespFrame : public SimManagementFrame {
 public:
  SimProbeRespFrame() = default;
  explicit SimProbeRespFrame(StationIfc* sender, const common::MacAddr& src,
                             const common::MacAddr& dst, const wlan_ssid_t& ssid)
      : SimManagementFrame(sender), src_addr_(src), dst_addr_(dst), ssid_(ssid){};

  ~SimProbeRespFrame() override;

  SimMgmtFrameType MgmtFrameType() const override;

  common::MacAddr src_addr_;
  common::MacAddr dst_addr_;
  wlan_ssid_t ssid_;
  wlan::CapabilityInfo capability_info_;
};

class SimAssocReqFrame : public SimManagementFrame {
 public:
  SimAssocReqFrame() = default;
  explicit SimAssocReqFrame(StationIfc* sender, const common::MacAddr& src,
                            const common::MacAddr bssid)
      : SimManagementFrame(sender), src_addr_(src), bssid_(bssid){};

  ~SimAssocReqFrame() override;

  SimMgmtFrameType MgmtFrameType() const override;

  common::MacAddr src_addr_;
  common::MacAddr bssid_;
};

class SimAssocRespFrame : public SimManagementFrame {
 public:
  SimAssocRespFrame() = default;
  explicit SimAssocRespFrame(StationIfc* sender, const common::MacAddr& src,
                             const common::MacAddr& dst, const uint16_t status)
      : SimManagementFrame(sender), src_addr_(src), dst_addr_(dst), status_(status){};

  ~SimAssocRespFrame() override;

  SimMgmtFrameType MgmtFrameType() const override;

  common::MacAddr src_addr_;
  common::MacAddr dst_addr_;
  uint16_t status_;
};

class SimDisassocReqFrame : public SimManagementFrame {
 public:
  SimDisassocReqFrame() = default;
  explicit SimDisassocReqFrame(StationIfc* sender, const common::MacAddr& src,
                               const common::MacAddr& dst, const uint16_t reason)
      : SimManagementFrame(sender), src_addr_(src), dst_addr_(dst), reason_(reason){};

  ~SimDisassocReqFrame() override;

  SimMgmtFrameType MgmtFrameType() const override;

  common::MacAddr src_addr_;
  common::MacAddr dst_addr_;
  uint16_t reason_;
};

}  // namespace wlan::simulation

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_FRAME_H_
