// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context, Error},
    bitflags::bitflags,
    fidl::encoding::Decodable,
    fidl::endpoints::create_request_stream,
    fidl_fuchsia_bluetooth_bredr::*,
    fuchsia_bluetooth::{profile::elem_to_profile_descriptor, types::Uuid},
    fuchsia_syslog::fx_log_info,
    std::fmt::Debug,
};

bitflags! {
    #[allow(dead_code)]
    pub struct AvcrpTargetFeatures: u16 {
        const CATEGORY1         = 1 << 0;
        const CATEGORY2         = 1 << 1;
        const CATEGORY3         = 1 << 2;
        const CATEGORY4         = 1 << 3;
        const PLAYERSETTINGS    = 1 << 4;
        const GROUPNAVIGATION   = 1 << 5;
        const SUPPORTSBROWSING  = 1 << 6;
        const SUPPORTSMULTIPLEMEDIAPLAYERS = 1 << 7;
        const SUPPORTSCOVERART  = 1 << 8;
        // 9-15 Reserved
    }
}

bitflags! {
    #[allow(dead_code)]
    pub struct AvcrpControllerFeatures: u16 {
        const CATEGORY1         = 1 << 0;
        const CATEGORY2         = 1 << 1;
        const CATEGORY3         = 1 << 2;
        const CATEGORY4         = 1 << 3;
        // 4-5 RESERVED
        const SUPPORTSBROWSING  = 1 << 6;
        const SUPPORTSCOVERARTGETIMAGEPROPERTIES = 1 << 7;
        const SUPPORTSCOVERARTGETIMAGE = 1 << 8;
        const SUPPORTSCOVERARTGETLINKEDTHUMBNAIL = 1 << 9;
        // 10-15 RESERVED
    }
}

const SDP_SUPPORTED_FEATURES: u16 = 0x0311;

const AV_REMOTE_TARGET_CLASS: u16 = 0x110c;
const AV_REMOTE_CLASS: u16 = 0x110e;
const AV_REMOTE_CONTROLLER_CLASS: u16 = 0x110f;

/// Make the SDP definition for the AVRCP service.
/// TODO(1346): We need two entries in SDP in the future. One for target and one for controller.
///       We are using using one unified profile for both because of limitations in BrEdr service.
fn make_profile_service_definition() -> ServiceDefinition {
    let service_class_uuids: Vec<fidl_fuchsia_bluetooth::Uuid> = vec![
        Uuid::new16(AV_REMOTE_TARGET_CLASS).into(),
        Uuid::new16(AV_REMOTE_CLASS).into(),
        Uuid::new16(AV_REMOTE_CONTROLLER_CLASS).into(),
    ];
    ServiceDefinition {
        service_class_uuids: Some(service_class_uuids), // AVRCP UUID
        protocol_descriptor_list: Some(vec![
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::L2Cap,
                params: vec![DataElement::Uint16(PSM_AVCTP as u16)],
            },
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::Avctp,
                params: vec![DataElement::Uint16(0x0103)], // Indicate v1.3
            },
        ]),
        profile_descriptors: Some(vec![ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AvRemoteControl,
            major_version: 1,
            minor_version: 6,
        }]),
        additional_attributes: Some(vec![Attribute {
            id: SDP_SUPPORTED_FEATURES, // SDP Attribute "SUPPORTED FEATURES"
            element: DataElement::Uint16(
                AvcrpTargetFeatures::CATEGORY1.bits() | AvcrpTargetFeatures::CATEGORY2.bits(),
            ),
        }]),
        ..ServiceDefinition::new_empty()
    }
}

#[derive(Debug, PartialEq, Hash, Clone, Copy)]
pub struct AvrcpProtocolVersion(pub u8, pub u8);

#[derive(Debug, PartialEq, Clone)]
pub enum AvrcpService {
    Target {
        features: AvcrpTargetFeatures,
        psm: u16,
        protocol_version: AvrcpProtocolVersion,
    },
    Controller {
        features: AvcrpControllerFeatures,
        psm: u16,
        protocol_version: AvrcpProtocolVersion,
    },
}

impl AvrcpService {
    pub fn from_attributes(attributes: Vec<Attribute>) -> Option<AvrcpService> {
        let mut features: Option<u16> = None;
        let mut service_uuids: Option<Vec<Uuid>> = None;
        let mut profile: Option<ProfileDescriptor> = None;

        for attr in attributes {
            fx_log_info!("attribute: {:#?} ", attr);
            match attr.id {
                ATTR_SERVICE_CLASS_ID_LIST => {
                    if let DataElement::Sequence(seq) = attr.element {
                        let uuids: Vec<Uuid> = seq
                            .into_iter()
                            .flatten()
                            .filter_map(|item| match *item {
                                DataElement::Uuid(uuid) => Some(uuid.into()),
                                _ => None,
                            })
                            .collect();
                        if uuids.len() > 0 {
                            service_uuids = Some(uuids);
                        }
                    }
                }
                ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST => {
                    if let DataElement::Sequence(profiles) = attr.element {
                        for elem in profiles {
                            let elem = elem.expect("DataElement sequence elements should exist");
                            profile = elem_to_profile_descriptor(&*elem);
                        }
                    }
                }
                SDP_SUPPORTED_FEATURES => {
                    if let DataElement::Uint16(value) = attr.element {
                        features = Some(value);
                    }
                }
                _ => {}
            }
        }

        if service_uuids.is_none() || features.is_none() || profile.is_none() {
            return None;
        }

        let service_uuids = service_uuids.expect("service_uuids should not be none");
        let features = features.expect("features should not be none");
        let profile = profile.expect("profile should not be none");

        if service_uuids.contains(&Uuid::new16(AV_REMOTE_TARGET_CLASS)) {
            if let Some(feature_flags) = AvcrpTargetFeatures::from_bits(features) {
                return Some(AvrcpService::Target {
                    features: feature_flags,
                    psm: PSM_AVCTP as u16, // TODO: Parse this out instead of assuming it's default
                    protocol_version: AvrcpProtocolVersion(
                        profile.major_version,
                        profile.minor_version,
                    ),
                });
            }
        } else if service_uuids.contains(&Uuid::new16(AV_REMOTE_CLASS))
            || service_uuids.contains(&Uuid::new16(AV_REMOTE_CONTROLLER_CLASS))
        {
            if let Some(feature_flags) = AvcrpControllerFeatures::from_bits(features) {
                return Some(AvrcpService::Controller {
                    features: feature_flags,
                    psm: PSM_AVCTP as u16, // TODO: Parse this out instead of assuming it's default
                    protocol_version: AvrcpProtocolVersion(
                        profile.major_version,
                        profile.minor_version,
                    ),
                });
            }
        }
        None
    }
}

pub fn connect_and_advertise(
) -> Result<(ProfileProxy, ConnectionReceiverRequestStream, SearchResultsRequestStream), Error> {
    let profile_svc = fuchsia_component::client::connect_to_service::<ProfileMarker>()
        .context("Failed to connect to Bluetooth profile service")?;

    const SEARCH_ATTRIBUTES: [u16; 5] = [
        ATTR_SERVICE_CLASS_ID_LIST,
        ATTR_PROTOCOL_DESCRIPTOR_LIST,
        ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
        ATTR_ADDITIONAL_PROTOCOL_DESCRIPTOR_LIST,
        SDP_SUPPORTED_FEATURES,
    ];

    let (search_results, search_results_requests) =
        create_request_stream().context("Couldn't create SearchResults")?;

    profile_svc.search(
        ServiceClassProfileIdentifier::AvRemoteControl,
        &SEARCH_ATTRIBUTES,
        search_results,
    )?;

    let (connection_client, connection_requests) =
        create_request_stream().context("Couldn't create ConnectionTarget")?;

    let service_defs = vec![make_profile_service_definition()];
    profile_svc.advertise(
        &mut service_defs.into_iter(),
        SecurityRequirements::new_empty(),
        ChannelParameters::new_empty(),
        connection_client,
    )?;

    fx_log_info!("Advertised Service");

    Ok((profile_svc, connection_requests, search_results_requests))
}
