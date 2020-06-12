// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[allow(unused)] // TODO: remove in Kevin's CL (7 total in this file)
use {
    crate::{
        client::types,
        config_management::{Credential, SavedNetworksManager},
    },
    log::{trace, warn},
    std::{
        collections::HashMap,
        sync::{Arc, Mutex, MutexGuard},
    },
};

#[allow(dead_code)] // TODO: remove in Kevin's CL (7 total in this file)
pub struct NetworkSelector {
    saved_network_manager: Arc<SavedNetworksManager>,
    latest_scan_results: Mutex<Vec<types::ScanResult>>,
}

pub trait ScanResultUpdate: Sync + Send {
    fn update_scan_results(&self, scan_results: Vec<types::ScanResult>) -> ();
}

#[cfg(test)] // TODO: remove in Kevin's CL (7 total in this file)
#[derive(Debug, PartialEq)]
struct InternalNetworkData {
    credential: Credential,
    has_ever_connected: bool,
    rssi: Option<i8>,
    compatible: bool,
}

impl NetworkSelector {
    pub fn new(saved_network_manager: Arc<SavedNetworksManager>) -> Self {
        Self { saved_network_manager, latest_scan_results: Mutex::new(Vec::new()) }
    }

    fn take_scan_result_mutex(&self) -> MutexGuard<'_, Vec<types::ScanResult>> {
        match self.latest_scan_results.lock() {
            Ok(scan_result_guard) => scan_result_guard,
            Err(poisoned_guard) => {
                warn!("Mutex was poisoned");
                poisoned_guard.into_inner()
            }
        }
    }

    #[cfg(test)] // TODO: remove in Kevin's CL (7 total in this file)
    /// Insert all saved networks into a hashmap with this module's internal data representation
    fn load_saved_networks(&self) -> HashMap<types::NetworkIdentifier, InternalNetworkData> {
        let mut networks: HashMap<types::NetworkIdentifier, InternalNetworkData> = HashMap::new();
        for saved_network in self.saved_network_manager.get_networks().iter() {
            trace!("Adding saved network to hashmap");
            networks.insert(
                types::NetworkIdentifier {
                    ssid: saved_network.ssid.clone(),
                    type_: saved_network.security_type.into(),
                },
                InternalNetworkData {
                    credential: saved_network.credential.clone(),
                    has_ever_connected: saved_network.has_ever_connected,
                    rssi: None,
                    compatible: false,
                },
            );
        }
        networks
    }

    #[cfg(test)] // TODO: remove in Kevin's CL (7 total in this file)
    /// Augment the networks hash map with data from scan results
    fn augment_networks_with_scan_data(
        &self,
        mut networks: HashMap<types::NetworkIdentifier, InternalNetworkData>,
    ) -> HashMap<types::NetworkIdentifier, InternalNetworkData> {
        let scan_result_guard = self.take_scan_result_mutex();
        for scan_result in &*scan_result_guard {
            if let Some(hashmap_entry) = networks.get_mut(&scan_result.id) {
                // Extract the max RSSI from all the BSS in scan_result.entries
                if let Some(max_rssi) =
                    scan_result.entries.iter().map(|bss| bss.rssi).max_by(|a, b| a.cmp(b))
                {
                    let compatibility =
                        scan_result.compatibility == types::Compatibility::Supported;
                    trace!(
                        "Augmenting network with RSSI {} and compatibility {}",
                        max_rssi,
                        compatibility
                    );
                    hashmap_entry.rssi = Some(max_rssi);
                    hashmap_entry.compatible = compatibility;
                }
            }
        }
        networks
    }

    #[cfg(test)] // TODO: remove in Kevin's CL (7 total in this file)
    pub fn get_best_network(
        &self,
        ignore_list: &Vec<types::NetworkIdentifier>,
    ) -> Option<(types::NetworkIdentifier, Credential)> {
        let networks = self.augment_networks_with_scan_data(self.load_saved_networks());
        find_best_network(&networks, ignore_list)
    }
}

impl ScanResultUpdate for NetworkSelector {
    fn update_scan_results(&self, scan_results: Vec<types::ScanResult>) {
        let mut scan_result_guard = self.take_scan_result_mutex();
        *scan_result_guard = scan_results;
    }
}

#[cfg(test)] // TODO: remove in Kevin's CL (7 total in this file)
/// Find the best network in the given hashmap
fn find_best_network(
    networks: &HashMap<types::NetworkIdentifier, InternalNetworkData>,
    ignore_list: &Vec<types::NetworkIdentifier>,
) -> Option<(types::NetworkIdentifier, Credential)> {
    networks
        .iter()
        .filter(|(id, data)| {
            // Filter out networks that are incompatible
            if !data.compatible {
                trace!("Network is incompatible, filtering");
                return false;
            };
            // Filter out networks not present in scan results
            if data.rssi.is_none() {
                trace!("RSSI not present, filtering");
                return false;
            };
            // Filter out networks we've been told to ignore
            if ignore_list.contains(id) {
                trace!("Network is ignored, filtering");
                return false;
            }
            true
        })
        .max_by(|(_, data_a), (_, data_b)| {
            // Sort by RSSI
            let rssi_a = data_a.rssi.unwrap();
            let rssi_b = data_b.rssi.unwrap();
            rssi_a.partial_cmp(&rssi_b).unwrap()
            // TODO(53999): add in some weighing for network denials
        })
        .map(|(id, data)| (id.clone(), data.credential.clone()))
}

#[cfg(test)]
mod tests {
    use {
        super::*, crate::util::logger::set_logger_for_test, fidl_fuchsia_wlan_sme as fidl_sme,
        fuchsia_async as fasync, std::sync::Arc,
    };

    struct TestValues {
        network_selector: Arc<NetworkSelector>,
        saved_network_manager: Arc<SavedNetworksManager>,
    }

    fn test_setup(exec: &mut fasync::Executor) -> TestValues {
        set_logger_for_test();

        // setup modules
        let saved_network_manager =
            Arc::new(exec.run_singlethreaded(SavedNetworksManager::new_for_test()).unwrap());
        let network_selector = Arc::new(NetworkSelector::new(Arc::clone(&saved_network_manager)));

        TestValues { network_selector, saved_network_manager }
    }

    #[test]
    fn saved_networks_are_loaded() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup(&mut exec);
        let network_selector = test_values.network_selector;

        // check there are 0 saved networks to start with
        let networks = network_selector.load_saved_networks();
        assert_eq!(networks.len(), 0);

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        // insert some new saved networks
        test_values
            .saved_network_manager
            .store(test_id_1.clone().into(), credential_1.clone())
            .unwrap();
        test_values
            .saved_network_manager
            .store(test_id_2.clone().into(), credential_2.clone())
            .unwrap();

        // mark the first one as having connected
        test_values.saved_network_manager.record_connect_result(
            test_id_1.clone().into(),
            &credential_1.clone(),
            fidl_sme::ConnectResultCode::Success,
        );

        // check these networks were loaded
        let mut expected_hashmap = HashMap::new();
        expected_hashmap.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: None,
                compatible: false,
            },
        );
        expected_hashmap.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: false,
                rssi: None,
                compatible: false,
            },
        );
        let networks = network_selector.load_saved_networks();
        assert_eq!(networks, expected_hashmap);
    }

    #[test]
    fn scan_results_are_stored() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup(&mut exec);
        let network_selector = test_values.network_selector;

        // check there are 0 scan results to start with
        let guard = network_selector.latest_scan_results.lock().unwrap();
        assert_eq!(guard.len(), 0);
        drop(guard);

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };

        // provide some new scan results
        let mock_scan_results = vec![
            types::ScanResult {
                id: test_id_1.clone(),
                entries: vec![
                    types::Bss {
                        bssid: [0, 1, 2, 3, 4, 5],
                        rssi: -14,
                        frequency: 2400,
                        timestamp_nanos: 0,
                    },
                    types::Bss {
                        bssid: [6, 7, 8, 9, 10, 11],
                        rssi: -10,
                        frequency: 2410,
                        timestamp_nanos: 1,
                    },
                    types::Bss {
                        bssid: [0, 1, 2, 3, 4, 5],
                        rssi: -20,
                        frequency: 2400,
                        timestamp_nanos: 0,
                    },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                id: test_id_2.clone(),
                entries: vec![types::Bss {
                    bssid: [20, 30, 40, 50, 60, 70],
                    rssi: -15,
                    frequency: 2400,
                    timestamp_nanos: 0,
                }],
                compatibility: types::Compatibility::DisallowedNotSupported,
            },
        ];
        network_selector.update_scan_results(mock_scan_results.clone());

        // check that the scan results are stored
        let guard = network_selector.latest_scan_results.lock().unwrap();
        assert_eq!(*guard, mock_scan_results);
    }

    #[test]
    fn scan_results_used_to_augment_hashmap() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup(&mut exec);
        let network_selector = test_values.network_selector;

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        // create the saved networks hashmap
        let mut saved_networks = HashMap::new();
        saved_networks.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: None,
                compatible: false,
            },
        );
        saved_networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: false,
                rssi: None,
                compatible: false,
            },
        );

        // store some scan results
        let mock_scan_results = vec![
            types::ScanResult {
                id: test_id_1.clone(),
                entries: vec![
                    types::Bss {
                        bssid: [0, 1, 2, 3, 4, 5],
                        rssi: -14,
                        frequency: 2400,
                        timestamp_nanos: 0,
                    },
                    types::Bss {
                        bssid: [6, 7, 8, 9, 10, 11],
                        rssi: -10,
                        frequency: 2410,
                        timestamp_nanos: 1,
                    },
                    types::Bss {
                        bssid: [0, 1, 2, 3, 4, 5],
                        rssi: -20,
                        frequency: 2400,
                        timestamp_nanos: 0,
                    },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                id: test_id_2.clone(),
                entries: vec![types::Bss {
                    bssid: [20, 30, 40, 50, 60, 70],
                    rssi: -15,
                    frequency: 2400,
                    timestamp_nanos: 0,
                }],
                compatibility: types::Compatibility::DisallowedNotSupported,
            },
        ];
        network_selector.update_scan_results(mock_scan_results.clone());

        // build our expected result
        let mut expected_result = HashMap::new();
        expected_result.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: Some(-10),  // strongest RSSI of all the bss
                compatible: true, // compatible
            },
        );
        expected_result.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: false,
                rssi: Some(-15),
                compatible: false, // DisallowedNotSupported
            },
        );

        // validate the function works
        let result = network_selector.augment_networks_with_scan_data(saved_networks);
        assert_eq!(result, expected_result);
    }

    #[test]
    fn find_best_network_sorts_by_rssi() {
        // build network hashmap
        let mut networks = HashMap::new();
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());
        networks.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: Some(-10),
                compatible: true,
            },
        );
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: Some(-15),
                compatible: true,
            },
        );

        // stronger network returned
        assert_eq!(
            find_best_network(&networks, &vec![]).unwrap(),
            (test_id_1.clone(), credential_1.clone())
        );

        // make the other network stronger
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: Some(-5),
                compatible: true,
            },
        );

        // stronger network returned
        assert_eq!(
            find_best_network(&networks, &vec![]).unwrap(),
            (test_id_2.clone(), credential_2.clone())
        );
    }

    #[test]
    fn find_best_network_incompatible() {
        // build network hashmap
        let mut networks = HashMap::new();
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());
        networks.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: Some(1),
                compatible: true,
            },
        );
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: Some(2),
                compatible: true,
            },
        );

        // stronger network returned
        assert_eq!(
            find_best_network(&networks, &vec![]).unwrap(),
            (test_id_2.clone(), credential_2.clone())
        );

        // mark it as incompatible
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: Some(2),
                compatible: false,
            },
        );

        // other network returned
        assert_eq!(
            find_best_network(&networks, &vec![]).unwrap(),
            (test_id_1.clone(), credential_1.clone())
        );
    }

    #[test]
    fn find_best_network_no_rssi() {
        // build network hashmap
        let mut networks = HashMap::new();
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());
        networks.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: None, // No RSSI
                compatible: true,
            },
        );
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: None, // No RSSI
                compatible: true,
            },
        );

        // no network returned
        assert!(find_best_network(&networks, &vec![]).is_none());

        // add RSSI
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: Some(20),
                compatible: true,
            },
        );
        assert_eq!(
            find_best_network(&networks, &vec![]).unwrap(),
            (test_id_2.clone(), credential_2.clone())
        );
    }

    #[test]
    fn find_best_network_ignore_list() {
        // build network hashmap
        let mut networks = HashMap::new();
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());
        networks.insert(
            test_id_1.clone(),
            InternalNetworkData {
                credential: credential_1.clone(),
                has_ever_connected: true,
                rssi: Some(1),
                compatible: true,
            },
        );
        networks.insert(
            test_id_2.clone(),
            InternalNetworkData {
                credential: credential_2.clone(),
                has_ever_connected: true,
                rssi: Some(2),
                compatible: true,
            },
        );

        // stronger network returned
        assert_eq!(
            find_best_network(&networks, &vec![]).unwrap(),
            (test_id_2.clone(), credential_2.clone())
        );

        // ignore the stronger network, other network returned
        assert_eq!(
            find_best_network(&networks, &vec![test_id_2.clone()]).unwrap(),
            (test_id_1.clone(), credential_1.clone())
        );
    }

    #[test]
    fn get_best_network_end_to_end() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup(&mut exec);
        let network_selector = test_values.network_selector;

        // create some identifiers
        let test_id_1 = types::NetworkIdentifier {
            ssid: "foo".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa3,
        };
        let credential_1 = Credential::Password("foo_pass".as_bytes().to_vec());
        let test_id_2 = types::NetworkIdentifier {
            ssid: "bar".as_bytes().to_vec(),
            type_: types::SecurityType::Wpa,
        };
        let credential_2 = Credential::Password("bar_pass".as_bytes().to_vec());

        // insert some new saved networks
        test_values
            .saved_network_manager
            .store(test_id_1.clone().into(), credential_1.clone())
            .unwrap();
        test_values
            .saved_network_manager
            .store(test_id_2.clone().into(), credential_2.clone())
            .unwrap();

        // mark them as having connected
        test_values.saved_network_manager.record_connect_result(
            test_id_1.clone().into(),
            &credential_1.clone(),
            fidl_sme::ConnectResultCode::Success,
        );
        test_values.saved_network_manager.record_connect_result(
            test_id_2.clone().into(),
            &credential_2.clone(),
            fidl_sme::ConnectResultCode::Success,
        );

        // provide some new scan results
        let mock_scan_results = vec![
            types::ScanResult {
                id: test_id_1.clone(),
                entries: vec![
                    types::Bss {
                        bssid: [0, 1, 2, 3, 4, 5],
                        rssi: -14,
                        frequency: 2400,
                        timestamp_nanos: 0,
                    },
                    types::Bss {
                        bssid: [6, 7, 8, 9, 10, 11],
                        rssi: -10,
                        frequency: 2410,
                        timestamp_nanos: 1,
                    },
                    types::Bss {
                        bssid: [0, 1, 2, 3, 4, 5],
                        rssi: -20,
                        frequency: 2400,
                        timestamp_nanos: 0,
                    },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                id: test_id_2.clone(),
                entries: vec![types::Bss {
                    bssid: [20, 30, 40, 50, 60, 70],
                    rssi: -15,
                    frequency: 2400,
                    timestamp_nanos: 0,
                }],
                compatibility: types::Compatibility::Supported,
            },
        ];
        network_selector.update_scan_results(mock_scan_results.clone());

        // Check that we pick a network
        assert_eq!(
            network_selector.get_best_network(&vec![]).unwrap(),
            (test_id_1.clone(), credential_1.clone())
        );

        // Ignore that network, check that we pick the other one
        assert_eq!(
            network_selector.get_best_network(&vec![test_id_1.clone()]).unwrap(),
            (test_id_2.clone(), credential_2.clone())
        );
    }
}
