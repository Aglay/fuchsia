// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod convert;

use {
    crate::{device::IfaceMap, telemetry::convert::*},
    fidl_fuchsia_cobalt::HistogramBucket,
    fidl_fuchsia_wlan_stats as fidl_stats,
    fidl_fuchsia_wlan_stats::MlmeStats::{ApMlmeStats, ClientMlmeStats},
    fuchsia_async as fasync,
    fuchsia_cobalt::CobaltSender,
    fuchsia_zircon as zx,
    fuchsia_zircon::DurationNum,
    futures::prelude::*,
    futures::stream::FuturesUnordered,
    log::{error, warn},
    parking_lot::Mutex,
    std::cmp::PartialOrd,
    std::collections::hash_map::Entry,
    std::collections::HashMap,
    std::default::Default,
    std::ops::Sub,
    std::sync::Arc,
    wlan_common::{
        bss::{BssDescriptionExt, Standard},
        format::MacFmt,
    },
    wlan_metrics_registry::{
        self as metrics,
        NeighborNetworksWlanStandardsCountMetricDimensionWlanStandardType as NeighborNetworksMetricStandardLabel,
    },
    wlan_sme::client::{
        info::{
            ConnectStats, ConnectionLostInfo, ConnectionMilestone, ConnectionPingInfo, ScanStats,
        },
        ConnectFailure, ConnectResult,
    },
};

type StatsRef = Arc<Mutex<fidl_stats::IfaceStats>>;

/// How often to request RSSI stats and dispatcher packet counts from MLME.
const REPORT_PERIOD_MINUTES: i64 = 1;

/// Mapping of WLAN standard type to the corresponding metric label used for Cobalt.
const NEIGHBOR_NETWORKS_STANDARD_MAPPING: [(Standard, NeighborNetworksMetricStandardLabel); 5] = [
    (Standard::Dot11B, NeighborNetworksMetricStandardLabel::_802_11b),
    (Standard::Dot11G, NeighborNetworksMetricStandardLabel::_802_11g),
    (Standard::Dot11A, NeighborNetworksMetricStandardLabel::_802_11a),
    (Standard::Dot11N, NeighborNetworksMetricStandardLabel::_802_11n),
    (Standard::Dot11Ac, NeighborNetworksMetricStandardLabel::_802_11ac),
];

// Export MLME stats to Cobalt every REPORT_PERIOD_MINUTES.
pub async fn report_telemetry_periodically(ifaces_map: Arc<IfaceMap>, mut sender: CobaltSender) {
    // TODO(NET-1386): Make this module resilient to Wlanstack2 downtime.

    let mut last_reported_stats: HashMap<u16, StatsRef> = HashMap::new();
    let mut interval_stream = fasync::Interval::new(REPORT_PERIOD_MINUTES.minutes());
    while let Some(_) = interval_stream.next().await {
        let mut futures = FuturesUnordered::new();
        for (id, iface) in ifaces_map.get_snapshot().iter() {
            let id = *id;
            let iface = Arc::clone(iface);
            let fut = iface.stats_sched.get_stats().map(move |r| (id, iface, r));
            futures.push(fut);
        }

        while let Some((id, _iface, stats_result)) = futures.next().await {
            match stats_result {
                Ok(current_stats) => match last_reported_stats.entry(id) {
                    Entry::Vacant(entry) => {
                        entry.insert(current_stats);
                    }
                    Entry::Occupied(mut value) => {
                        let last_stats = value.get_mut();
                        report_stats(&last_stats.lock(), &current_stats.lock(), &mut sender);
                        let _dropped = std::mem::replace(value.get_mut(), current_stats);
                    }
                },
                Err(e) => {
                    last_reported_stats.remove(&id);
                    error!("Failed to get the stats for iface '{}': {}", id, e);
                }
            };
        }
    }
}

fn report_stats(
    last_stats: &fidl_stats::IfaceStats,
    current_stats: &fidl_stats::IfaceStats,
    sender: &mut CobaltSender,
) {
    report_mlme_stats(&last_stats.mlme_stats, &current_stats.mlme_stats, sender);

    report_dispatcher_stats(&last_stats.dispatcher_stats, &current_stats.dispatcher_stats, sender);
}

fn report_dispatcher_stats(
    last_stats: &fidl_stats::DispatcherStats,
    current_stats: &fidl_stats::DispatcherStats,
    sender: &mut CobaltSender,
) {
    report_dispatcher_packets(
        metrics::DispatcherPacketCountsMetricDimensionPacketType::In,
        get_diff(last_stats.any_packet.in_.count, current_stats.any_packet.in_.count),
        sender,
    );
    report_dispatcher_packets(
        metrics::DispatcherPacketCountsMetricDimensionPacketType::Out,
        get_diff(last_stats.any_packet.out.count, current_stats.any_packet.out.count),
        sender,
    );
    report_dispatcher_packets(
        metrics::DispatcherPacketCountsMetricDimensionPacketType::Dropped,
        get_diff(last_stats.any_packet.drop.count, current_stats.any_packet.drop.count),
        sender,
    );
}

fn report_dispatcher_packets(
    packet_type: metrics::DispatcherPacketCountsMetricDimensionPacketType,
    packet_count: u64,
    sender: &mut CobaltSender,
) {
    sender.log_event_count(
        metrics::DISPATCHER_PACKET_COUNTS_METRIC_ID,
        packet_type as u32,
        0,
        packet_count as i64,
    );
}

fn report_mlme_stats(
    last: &Option<Box<fidl_stats::MlmeStats>>,
    current: &Option<Box<fidl_stats::MlmeStats>>,
    sender: &mut CobaltSender,
) {
    if let (Some(ref last), Some(ref current)) = (last, current) {
        match (last.as_ref(), current.as_ref()) {
            (ClientMlmeStats(last), ClientMlmeStats(current)) => {
                report_client_mlme_stats(&last, &current, sender)
            }
            (ApMlmeStats(_), ApMlmeStats(_)) => {}
            _ => error!("Current MLME stats type is different from the last MLME stats type"),
        };
    }
}

fn report_client_mlme_stats(
    last_stats: &fidl_stats::ClientMlmeStats,
    current_stats: &fidl_stats::ClientMlmeStats,
    sender: &mut CobaltSender,
) {
    report_rssi_stats(
        metrics::CLIENT_ASSOC_RSSI_METRIC_ID,
        &last_stats.assoc_data_rssi,
        &current_stats.assoc_data_rssi,
        sender,
    );
    report_rssi_stats(
        metrics::CLIENT_BEACON_RSSI_METRIC_ID,
        &last_stats.beacon_rssi,
        &current_stats.beacon_rssi,
        sender,
    );

    report_client_mlme_rx_tx_frames(&last_stats, &current_stats, sender);
}

fn report_client_mlme_rx_tx_frames(
    last_stats: &fidl_stats::ClientMlmeStats,
    current_stats: &fidl_stats::ClientMlmeStats,
    sender: &mut CobaltSender,
) {
    sender.log_event_count(
        metrics::MLME_RX_TX_FRAME_COUNTS_METRIC_ID,
        metrics::MlmeRxTxFrameCountsMetricDimensionFrameType::Rx as u32,
        0,
        get_diff(last_stats.rx_frame.in_.count, current_stats.rx_frame.in_.count) as i64,
    );
    sender.log_event_count(
        metrics::MLME_RX_TX_FRAME_COUNTS_METRIC_ID,
        metrics::MlmeRxTxFrameCountsMetricDimensionFrameType::Tx as u32,
        0,
        get_diff(last_stats.tx_frame.out.count, current_stats.tx_frame.out.count) as i64,
    );

    sender.log_event_count(
        metrics::MLME_RX_TX_FRAME_BYTES_METRIC_ID,
        metrics::MlmeRxTxFrameBytesMetricDimensionFrameType::Rx as u32,
        0,
        get_diff(last_stats.rx_frame.in_bytes.count, current_stats.rx_frame.in_bytes.count) as i64,
    );
    sender.log_event_count(
        metrics::MLME_RX_TX_FRAME_BYTES_METRIC_ID,
        metrics::MlmeRxTxFrameBytesMetricDimensionFrameType::Tx as u32,
        0,
        get_diff(last_stats.tx_frame.out_bytes.count, current_stats.tx_frame.out_bytes.count)
            as i64,
    );
}

fn report_rssi_stats(
    rssi_metric_id: u32,
    last_stats: &fidl_stats::RssiStats,
    current_stats: &fidl_stats::RssiStats,
    sender: &mut CobaltSender,
) {
    // In the internal stats histogram, hist[x] represents the number of frames
    // with RSSI -x. For the Cobalt representation, buckets from -128 to 0 are
    // used. When data is sent to Cobalt, the concept of index is utilized.
    //
    // Shortly, for Cobalt:
    // Bucket -128 -> index   0
    // Bucket -127 -> index   1
    // ...
    // Bucket    0 -> index 128
    //
    // The for loop below converts the stats internal representation to the
    // Cobalt representation and prepares the histogram that will be sent.

    let mut histogram = Vec::new();
    for bin in 0..current_stats.hist.len() {
        let diff = get_diff(last_stats.hist[bin], current_stats.hist[bin]);
        if diff > 0 {
            let entry = HistogramBucket {
                index: (fidl_stats::RSSI_BINS - (bin as u8) - 1).into(),
                count: diff.into(),
            };
            histogram.push(entry);
        }
    }

    if !histogram.is_empty() {
        sender.log_int_histogram(rssi_metric_id, (), histogram);
    }
}

pub fn report_scan_delay(
    sender: &mut CobaltSender,
    scan_started_time: zx::Time,
    scan_finished_time: zx::Time,
) {
    let delay_micros = (scan_finished_time - scan_started_time).into_micros();
    sender.log_elapsed_time(metrics::SCAN_DELAY_METRIC_ID, (), delay_micros);
}

pub fn report_connection_delay(
    sender: &mut CobaltSender,
    conn_started_time: zx::Time,
    conn_finished_time: zx::Time,
    result: &ConnectResult,
) {
    use wlan_metrics_registry::ConnectionDelayMetricDimensionConnectionResult::{Fail, Success};

    let delay_micros = (conn_finished_time - conn_started_time).into_micros();
    let connection_result_cobalt = match result {
        ConnectResult::Success => Some(Success),
        ConnectResult::Canceled => Some(Fail),
        ConnectResult::Failed(failure) => convert_connect_failure(&failure),
    };

    if let Some(connection_result_cobalt) = connection_result_cobalt {
        sender.log_elapsed_time(
            metrics::CONNECTION_DELAY_METRIC_ID,
            connection_result_cobalt as u32,
            delay_micros,
        );
    }
}

pub fn report_assoc_success_delay(
    sender: &mut CobaltSender,
    assoc_started_time: zx::Time,
    assoc_finished_time: zx::Time,
) {
    let delay_micros = (assoc_finished_time - assoc_started_time).into_micros();
    sender.log_elapsed_time(metrics::ASSOCIATION_DELAY_METRIC_ID, 0, delay_micros);
}

pub fn report_rsna_established_delay(
    sender: &mut CobaltSender,
    rsna_started_time: zx::Time,
    rsna_finished_time: zx::Time,
) {
    let delay_micros = (rsna_finished_time - rsna_started_time).into_micros();
    sender.log_elapsed_time(metrics::RSNA_DELAY_METRIC_ID, 0, delay_micros);
}

pub fn report_neighbor_networks_count(
    sender: &mut CobaltSender,
    bss_count: usize,
    ess_count: usize,
) {
    sender.log_event_count(
        metrics::NEIGHBOR_NETWORKS_COUNT_METRIC_ID,
        metrics::NeighborNetworksCountMetricDimensionNetworkType::Bss as u32,
        0,
        bss_count as i64,
    );
    sender.log_event_count(
        metrics::NEIGHBOR_NETWORKS_COUNT_METRIC_ID,
        metrics::NeighborNetworksCountMetricDimensionNetworkType::Ess as u32,
        0,
        ess_count as i64,
    );
}

pub fn report_standards(
    sender: &mut CobaltSender,
    mut num_bss_by_standard: HashMap<Standard, usize>,
) {
    NEIGHBOR_NETWORKS_STANDARD_MAPPING.iter().for_each(|(standard, label)| {
        let count = match num_bss_by_standard.entry(standard.clone()) {
            Entry::Vacant(_) => 0 as i64,
            Entry::Occupied(e) => *e.get() as i64,
        };
        sender.log_event_count(
            metrics::NEIGHBOR_NETWORKS_WLAN_STANDARDS_COUNT_METRIC_ID,
            label.clone() as u32,
            0,
            count,
        )
    });
}

pub fn report_channels(sender: &mut CobaltSender, num_bss_by_channel: HashMap<u8, usize>) {
    num_bss_by_channel.into_iter().for_each(|(channel, count)| {
        sender.log_event_count(
            metrics::NEIGHBOR_NETWORKS_PRIMARY_CHANNELS_COUNT_METRIC_ID,
            channel as u32,
            0,
            count as i64,
        );
    });
}

fn get_diff<T>(last_stat: T, current_stat: T) -> T
where
    T: Sub<Output = T> + PartialOrd + Default,
{
    if current_stat >= last_stat {
        current_stat - last_stat
    } else {
        Default::default()
    }
}

pub fn log_scan_stats(sender: &mut CobaltSender, scan_stats: &ScanStats, is_join_scan: bool) {
    let (scan_result_dim, error_code_dim) = convert_scan_result(&scan_stats.result);
    let scan_type_dim = convert_scan_type(scan_stats.scan_type);
    let is_join_scan_dim = convert_bool_dim(is_join_scan);
    let client_state_dim = match scan_stats.scan_start_while_connected {
        true => metrics::ScanResultMetricDimensionClientState::Connected,
        false => metrics::ScanResultMetricDimensionClientState::Idle,
    };

    sender.log_event_count(
        metrics::SCAN_RESULT_METRIC_ID,
        [
            scan_result_dim as u32,
            scan_type_dim as u32,
            is_join_scan_dim as u32,
            client_state_dim as u32,
        ],
        // Elapsed time during which the count of event has been gathered. We log 0 since
        // we don't keep track of this.
        0,
        // Log one count of scan result.
        1,
    );

    if let Some(error_code_dim) = error_code_dim {
        sender.log_event_count(
            metrics::SCAN_FAILURE_METRIC_ID,
            [
                error_code_dim as u32,
                scan_type_dim as u32,
                is_join_scan_dim as u32,
                client_state_dim as u32,
            ],
            0,
            1,
        )
    }

    let scan_time = scan_stats.scan_time().into_micros();
    sender.log_elapsed_time(metrics::SCAN_TIME_METRIC_ID, (), scan_time);
    sender.log_elapsed_time(
        metrics::SCAN_TIME_PER_RESULT_METRIC_ID,
        scan_result_dim as u32,
        scan_time,
    );
    sender.log_elapsed_time(
        metrics::SCAN_TIME_PER_SCAN_TYPE_METRIC_ID,
        scan_type_dim as u32,
        scan_time,
    );
    sender.log_elapsed_time(
        metrics::SCAN_TIME_PER_JOIN_OR_DISCOVERY_METRIC_ID,
        is_join_scan_dim as u32,
        scan_time,
    );
    sender.log_elapsed_time(
        metrics::SCAN_TIME_PER_CLIENT_STATE_METRIC_ID,
        client_state_dim as u32,
        scan_time,
    );
}

pub fn log_connect_stats(sender: &mut CobaltSender, connect_stats: &ConnectStats) {
    if let Some(scan_stats) = connect_stats.join_scan_stats() {
        log_scan_stats(sender, &scan_stats, true);
    }

    log_connect_attempts_stats(sender, connect_stats);
    log_connect_result_stats(sender, connect_stats);
    log_time_to_connect_stats(sender, connect_stats);
    log_connection_gap_time_stats(sender, connect_stats);
}

fn log_connect_attempts_stats(sender: &mut CobaltSender, connect_stats: &ConnectStats) {
    // Only log attempts for successful connect. If connect is not successful, or if the expected
    // fields for successful connect attempts are not there, early return.
    match connect_stats.result {
        ConnectResult::Success => (),
        _ => return,
    }
    let is_multi_bss = match &connect_stats.scan_end_stats {
        Some(stats) => stats.bss_count > 1,
        None => return,
    };
    let bss = match &connect_stats.candidate_network {
        Some(bss) => bss,
        None => return,
    };

    use metrics::ConnectionSuccessWithAttemptsBreakdownMetricDimensionAttempts::*;
    let attempts_dim = match connect_stats.attempts {
        0 => {
            warn!("unexpected 0 attempts in connect stats");
            return;
        }
        1 => One,
        2 => Two,
        3 => Three,
        4 => Four,
        5 => Five,
        _ => MoreThanFive,
    };
    let is_multi_bss_dim = convert_bool_dim(is_multi_bss);
    let protection_dim = convert_protection(&bss.get_protection());
    let channel_band_dim = convert_channel_band(bss.chan.primary);

    sender.log_event_count(
        metrics::CONNECTION_ATTEMPTS_METRIC_ID,
        (), // no dimension
        0,
        connect_stats.attempts as i64,
    );
    sender.log_event_count(
        metrics::CONNECTION_SUCCESS_WITH_ATTEMPTS_BREAKDOWN_METRIC_ID,
        [
            attempts_dim as u32,
            is_multi_bss_dim as u32,
            protection_dim as u32,
            channel_band_dim as u32,
        ],
        0,
        1,
    );
}

fn log_connect_result_stats(sender: &mut CobaltSender, connect_stats: &ConnectStats) {
    let oui = connect_stats.candidate_network.as_ref().map(|bss| bss.bssid.to_oui_uppercase(""));
    let result_dim = convert_connection_result(&connect_stats.result);
    sender.with_component().log_event_count::<_, String, _>(
        metrics::CONNECTION_RESULT_METRIC_ID,
        result_dim as u32,
        oui.clone(),
        0,
        1,
    );

    if let ConnectResult::Failed(failure) = &connect_stats.result {
        let fail_at_dim = convert_to_fail_at_dim(failure);
        let timeout_dim = convert_bool_dim(failure.is_timeout());
        sender.with_component().log_event_count::<_, String, _>(
            metrics::CONNECTION_FAILURE_METRIC_ID,
            [fail_at_dim as u32, timeout_dim as u32],
            oui.clone(),
            0,
            1,
        );

        if let ConnectFailure::SelectNetwork(select_network_failure) = failure {
            let error_reason_dim = convert_select_network_failure(&select_network_failure);
            sender.with_component().log_event_count::<_, String, _>(
                metrics::NETWORK_SELECTION_FAILURE_METRIC_ID,
                error_reason_dim as u32,
                oui,
                0,
                1,
            );
        }
    }

    // For the remaining metrics, we expect scan result and candidate network to have been found
    let is_multi_bss = match &connect_stats.scan_end_stats {
        Some(stats) => stats.bss_count > 1,
        None => return,
    };
    let bss = match &connect_stats.candidate_network {
        Some(bss) => bss,
        None => return,
    };
    let oui = bss.bssid.to_oui_uppercase("");

    let is_multi_bss_dim = convert_bool_dim(is_multi_bss);
    let protection_dim = convert_protection(&bss.get_protection());
    let channel_band_dim = convert_channel_band(bss.chan.primary);
    let rssi_dim = convert_rssi(bss.rssi_dbm);
    sender.with_component().log_event_count(
        metrics::CONNECTION_RESULT_POST_NETWORK_SELECTION_METRIC_ID,
        [
            result_dim as u32,
            is_multi_bss_dim as u32,
            protection_dim as u32,
            channel_band_dim as u32,
        ],
        oui.clone(),
        0,
        1,
    );
    sender.with_component().log_event_count(
        metrics::CONNECTION_RESULT_PER_RSSI_METRIC_ID,
        [result_dim as u32, rssi_dim as u32],
        oui.clone(),
        0,
        1,
    );

    match &connect_stats.result {
        ConnectResult::Failed(failure) => match failure {
            ConnectFailure::AuthenticationFailure(code) => {
                let error_code_dim = convert_auth_error_code(*code);
                sender.with_component().log_event_count(
                    metrics::AUTHENTICATION_FAILURE_METRIC_ID,
                    [
                        error_code_dim as u32,
                        is_multi_bss_dim as u32,
                        channel_band_dim as u32,
                        protection_dim as u32,
                    ],
                    oui.clone(),
                    0,
                    1,
                );
                sender.with_component().log_event_count(
                    metrics::AUTHENTICATION_FAILURE_PER_RSSI_METRIC_ID,
                    [error_code_dim as u32, rssi_dim as u32, channel_band_dim as u32],
                    oui,
                    0,
                    1,
                );
            }
            ConnectFailure::AssociationFailure(code) => {
                let error_code_dim = convert_assoc_error_code(*code);
                sender.with_component().log_event_count(
                    metrics::ASSOCIATION_FAILURE_METRIC_ID,
                    [error_code_dim as u32, protection_dim as u32],
                    oui.clone(),
                    0,
                    1,
                );
                sender.with_component().log_event_count(
                    metrics::ASSOCIATION_FAILURE_PER_RSSI_METRIC_ID,
                    [error_code_dim as u32, rssi_dim as u32, channel_band_dim as u32],
                    oui,
                    0,
                    1,
                );
            }
            ConnectFailure::EstablishRsna(..) => {
                sender.with_component().log_event_count(
                    metrics::ESTABLISH_RSNA_FAILURE_METRIC_ID,
                    protection_dim as u32,
                    oui,
                    0,
                    1,
                );
            }
            // Scan failure is already logged as part of scan stats.
            // Select network failure is already logged above.
            _ => (),
        },
        _ => (),
    }
}

fn log_time_to_connect_stats(sender: &mut CobaltSender, connect_stats: &ConnectStats) {
    let connect_result_dim = convert_connection_result(&connect_stats.result);
    let rssi_dim = connect_stats.candidate_network.as_ref().map(|bss| convert_rssi(bss.rssi_dbm));

    let connect_time = connect_stats.connect_time().into_micros();
    sender.log_elapsed_time(metrics::CONNECTION_SETUP_TIME_METRIC_ID, (), connect_time);
    sender.log_elapsed_time(
        metrics::CONNECTION_SETUP_TIME_PER_RESULT_METRIC_ID,
        connect_result_dim as u32,
        connect_time,
    );
    if let Some(connect_time_without_scan) = connect_stats.connect_time_without_scan() {
        sender.log_elapsed_time(
            metrics::CONNECTION_SETUP_TIME_WITHOUT_SCAN_METRIC_ID,
            (),
            connect_time_without_scan.into_micros(),
        );
        sender.log_elapsed_time(
            metrics::CONNECTION_SETUP_TIME_WITHOUT_SCAN_PER_RESULT_METRIC_ID,
            connect_result_dim as u32,
            connect_time_without_scan.into_micros(),
        );
        if let Some(rssi_dim) = rssi_dim {
            sender.log_elapsed_time(
                metrics::CONNECTION_SETUP_TIME_WITHOUT_SCAN_PER_RSSI_METRIC_ID,
                rssi_dim as u32,
                connect_time_without_scan.into_micros(),
            )
        }
    }

    if let Some(time) = connect_stats.connect_queued_time() {
        sender.log_elapsed_time(metrics::CONNECTION_QUEUED_TIME_METRIC_ID, (), time.into_micros());
    }

    if let Some(auth_time) = connect_stats.auth_time() {
        sender.log_elapsed_time(
            metrics::AUTHENTICATION_TIME_METRIC_ID,
            (),
            auth_time.into_micros(),
        );
        if let Some(rssi_dim) = rssi_dim {
            sender.log_elapsed_time(
                metrics::AUTHENTICATION_TIME_PER_RSSI_METRIC_ID,
                rssi_dim as u32,
                auth_time.into_micros(),
            )
        }
    }

    if let Some(assoc_time) = connect_stats.assoc_time() {
        sender.log_elapsed_time(metrics::ASSOCIATION_TIME_METRIC_ID, (), assoc_time.into_micros());
        if let Some(rssi_dim) = rssi_dim {
            sender.log_elapsed_time(
                metrics::ASSOCIATION_TIME_PER_RSSI_METRIC_ID,
                rssi_dim as u32,
                assoc_time.into_micros(),
            )
        }
    }

    if let Some(rsna_time) = connect_stats.rsna_time() {
        sender.log_elapsed_time(
            metrics::ESTABLISH_RSNA_TIME_METRIC_ID,
            (),
            rsna_time.into_micros(),
        );
        if let Some(rssi_dim) = rssi_dim {
            sender.log_elapsed_time(
                metrics::ESTABLISH_RSNA_TIME_PER_RSSI_METRIC_ID,
                rssi_dim as u32,
                rsna_time.into_micros(),
            )
        }
    }
}

pub fn log_connection_gap_time_stats(sender: &mut CobaltSender, connect_stats: &ConnectStats) {
    if connect_stats.result != ConnectResult::Success {
        return;
    }

    let ssid = match &connect_stats.candidate_network {
        Some(bss) => &bss.ssid,
        None => {
            warn!("No candidate_network in successful connect stats");
            return;
        }
    };

    if let Some(previous_disconnect_info) = &connect_stats.previous_disconnect_info {
        let duration = connect_stats.connect_end_at - previous_disconnect_info.disconnect_at;
        let ssids_dim = if ssid == &previous_disconnect_info.ssid {
            metrics::ConnectionGapTimeBreakdownMetricDimensionSsids::SameSsid
        } else {
            metrics::ConnectionGapTimeBreakdownMetricDimensionSsids::DifferentSsids
        };
        let previous_disconnect_cause_dim =
            convert_disconnect_cause(&previous_disconnect_info.disconnect_cause);

        sender.log_elapsed_time(metrics::CONNECTION_GAP_TIME_METRIC_ID, (), duration.into_micros());
        sender.log_elapsed_time(
            metrics::CONNECTION_GAP_TIME_BREAKDOWN_METRIC_ID,
            [ssids_dim as u32, previous_disconnect_cause_dim as u32],
            duration.into_micros(),
        )
    }
}

pub fn log_connection_ping(sender: &mut CobaltSender, info: &ConnectionPingInfo) {
    for milestone in ConnectionMilestone::all().iter() {
        if info.connected_duration() >= milestone.min_duration() {
            let dur_dim = convert_connected_milestone(milestone);
            sender.log_event(metrics::CONNECTION_UPTIME_PING_METRIC_ID, dur_dim as u32);
        }
    }

    if info.reaches_new_milestone() {
        let dur_dim = convert_connected_milestone(&info.get_milestone());
        sender.log_event_count(
            metrics::CONNECTION_COUNT_BY_DURATION_METRIC_ID,
            dur_dim as u32,
            0,
            1,
        );
    }
}

pub fn log_connection_lost(sender: &mut CobaltSender, info: &ConnectionLostInfo) {
    use metrics::LostConnectionCountMetricDimensionConnectedTime::*;

    let duration_dim = match &info.connected_duration {
        x if x < &1.minutes() => LessThanOneMinute,
        x if x < &10.minutes() => LessThanTenMinutes,
        x if x < &30.minutes() => LessThanThirtyMinutes,
        x if x < &1.hour() => LessThanOneHour,
        x if x < &3.hours() => LessThanThreeHours,
        x if x < &6.hours() => LessThanSixHours,
        _ => AtLeastSixHours,
    };
    let rssi_dim = convert_rssi(info.last_rssi);

    sender.with_component().log_event_count(
        metrics::LOST_CONNECTION_COUNT_METRIC_ID,
        [duration_dim as u32, rssi_dim as u32],
        info.bssid.to_oui_uppercase(""),
        0,
        1,
    );
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            device::{self, IfaceDevice},
            mlme_query_proxy::MlmeQueryProxy,
            stats_scheduler::{self, StatsRequest},
        },
        fidl::endpoints::create_proxy,
        fidl_fuchsia_cobalt::{CobaltEvent, CountEvent, EventPayload},
        fidl_fuchsia_wlan_common as fidl_common,
        fidl_fuchsia_wlan_mlme::{self as fidl_mlme, MlmeMarker},
        fidl_fuchsia_wlan_stats::{Counter, DispatcherStats, IfaceStats, PacketCounter},
        futures::channel::mpsc,
        maplit::hashset,
        pin_utils::pin_mut,
        std::collections::HashSet,
        wlan_common::assert_variant,
        wlan_sme::client::{
            info::{
                ConnectStats, ConnectionLostInfo, DisconnectCause, PreviousDisconnectInfo,
                ScanEndStats, ScanResult, ScanStartStats, SupplicantProgress,
            },
            ConnectFailure, ConnectResult, EstablishRsnaFailure, SelectNetworkFailure,
        },
    };

    const IFACE_ID: u16 = 1;

    #[test]
    fn test_report_telemetry_periodically() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");

        let (ifaces_map, stats_requests) = fake_iface_map();
        let (cobalt_sender, mut cobalt_receiver) = fake_cobalt_sender();

        let telemetry_fut = report_telemetry_periodically(Arc::new(ifaces_map), cobalt_sender);
        pin_mut!(telemetry_fut);

        // Schedule the first stats request
        let _ = exec.run_until_stalled(&mut telemetry_fut);
        assert!(exec.wake_next_timer().is_some());
        let _ = exec.run_until_stalled(&mut telemetry_fut);

        // Provide stats response
        let mut nth_req = 0;
        let mut stats_server = stats_requests.for_each(move |req| {
            nth_req += 1;
            future::ready(req.reply(fake_iface_stats(nth_req)))
        });
        let _ = exec.run_until_stalled(&mut stats_server);

        // TODO(WLAN-1113): For some reason, telemetry skips logging the first stats response
        // Schedule the next stats request
        let _ = exec.run_until_stalled(&mut telemetry_fut);
        assert!(exec.wake_next_timer().is_some());
        let _ = exec.run_until_stalled(&mut telemetry_fut);

        // Provide stats response
        let _ = exec.run_until_stalled(&mut stats_server);

        // Verify that stats are sent to Cobalt
        let _ = exec.run_until_stalled(&mut telemetry_fut);
        let mut expected_metrics = vec![
            CobaltEvent {
                metric_id: metrics::DISPATCHER_PACKET_COUNTS_METRIC_ID,
                event_codes: vec![0], // in
                component: None,
                payload: event_count(1),
            },
            CobaltEvent {
                metric_id: metrics::DISPATCHER_PACKET_COUNTS_METRIC_ID,
                event_codes: vec![1], // out
                component: None,
                payload: event_count(2),
            },
            CobaltEvent {
                metric_id: metrics::DISPATCHER_PACKET_COUNTS_METRIC_ID,
                event_codes: vec![2], // dropped
                component: None,
                payload: event_count(3),
            },
            CobaltEvent {
                metric_id: metrics::MLME_RX_TX_FRAME_COUNTS_METRIC_ID,
                event_codes: vec![0], // rx
                component: None,
                payload: event_count(1),
            },
            CobaltEvent {
                metric_id: metrics::MLME_RX_TX_FRAME_COUNTS_METRIC_ID,
                event_codes: vec![1], // tx
                component: None,
                payload: event_count(2),
            },
            CobaltEvent {
                metric_id: metrics::MLME_RX_TX_FRAME_BYTES_METRIC_ID,
                event_codes: vec![0], // rx
                component: None,
                payload: event_count(4),
            },
            CobaltEvent {
                metric_id: metrics::MLME_RX_TX_FRAME_BYTES_METRIC_ID,
                event_codes: vec![1], // tx
                component: None,
                payload: event_count(5),
            },
            CobaltEvent {
                metric_id: metrics::CLIENT_ASSOC_RSSI_METRIC_ID,
                event_codes: vec![],
                component: None,
                payload: EventPayload::IntHistogram(vec![HistogramBucket { index: 128, count: 1 }]),
            },
            CobaltEvent {
                metric_id: metrics::CLIENT_BEACON_RSSI_METRIC_ID,
                event_codes: vec![],
                component: None,
                payload: EventPayload::IntHistogram(vec![HistogramBucket { index: 128, count: 1 }]),
            },
        ];
        while let Ok(Some(event)) = cobalt_receiver.try_next() {
            let index = expected_metrics.iter().position(|e| *e == event);
            assert!(index.is_some(), "unexpected event: {:?}", event);
            expected_metrics.remove(index.unwrap());
        }
        assert!(expected_metrics.is_empty(), "some metrics not logged: {:?}", expected_metrics);
    }

    #[test]
    fn test_log_connect_stats_success() {
        let (mut cobalt_sender, mut cobalt_receiver) = fake_cobalt_sender();
        log_connect_stats(&mut cobalt_sender, &fake_connect_stats());

        let mut expected_metrics = hashset! {
            metrics::CONNECTION_ATTEMPTS_METRIC_ID,
            metrics::CONNECTION_SUCCESS_WITH_ATTEMPTS_BREAKDOWN_METRIC_ID,
            metrics::CONNECTION_RESULT_METRIC_ID,
            metrics::CONNECTION_RESULT_POST_NETWORK_SELECTION_METRIC_ID,
            metrics::CONNECTION_RESULT_PER_RSSI_METRIC_ID,
            metrics::SCAN_RESULT_METRIC_ID,
            metrics::CONNECTION_SETUP_TIME_METRIC_ID,
            metrics::CONNECTION_SETUP_TIME_PER_RESULT_METRIC_ID,
            metrics::CONNECTION_SETUP_TIME_WITHOUT_SCAN_METRIC_ID,
            metrics::CONNECTION_SETUP_TIME_WITHOUT_SCAN_PER_RESULT_METRIC_ID,
            metrics::CONNECTION_SETUP_TIME_WITHOUT_SCAN_PER_RSSI_METRIC_ID,
            metrics::SCAN_TIME_METRIC_ID,
            metrics::SCAN_TIME_PER_RESULT_METRIC_ID,
            metrics::SCAN_TIME_PER_SCAN_TYPE_METRIC_ID,
            metrics::SCAN_TIME_PER_JOIN_OR_DISCOVERY_METRIC_ID,
            metrics::SCAN_TIME_PER_CLIENT_STATE_METRIC_ID,
            metrics::AUTHENTICATION_TIME_METRIC_ID,
            metrics::AUTHENTICATION_TIME_PER_RSSI_METRIC_ID,
            metrics::ASSOCIATION_TIME_METRIC_ID,
            metrics::ASSOCIATION_TIME_PER_RSSI_METRIC_ID,
            metrics::ESTABLISH_RSNA_TIME_METRIC_ID,
            metrics::ESTABLISH_RSNA_TIME_PER_RSSI_METRIC_ID,
            metrics::CONNECTION_QUEUED_TIME_METRIC_ID,
            metrics::CONNECTION_GAP_TIME_METRIC_ID,
            metrics::CONNECTION_GAP_TIME_BREAKDOWN_METRIC_ID,
        };
        while let Ok(Some(event)) = cobalt_receiver.try_next() {
            assert!(expected_metrics.contains(&event.metric_id), "unexpected event: {:?}", event);
            expected_metrics.remove(&event.metric_id);
        }
        assert!(expected_metrics.is_empty(), "some metrics not logged: {:?}", expected_metrics);
    }

    #[test]
    fn test_log_connect_stats_scan_failure() {
        // Note: This mock is not completely correct (e.g. we would not expect time stats for
        //       later steps to be filled out if connect fails at scan), but for our testing
        //       purpose, it's sufficient. The same applies for other connect stats failure
        //       test cases.
        let connect_stats = ConnectStats {
            result: ConnectFailure::ScanFailure(fidl_mlme::ScanResultCodes::InvalidArgs).into(),
            scan_end_stats: Some(ScanEndStats {
                scan_end_at: now(),
                result: ScanResult::Failed(fidl_mlme::ScanResultCodes::InvalidArgs),
                bss_count: 1,
            }),
            ..fake_connect_stats()
        };

        let expected_metrics_subset = hashset! {
            metrics::CONNECTION_RESULT_METRIC_ID,
            metrics::CONNECTION_FAILURE_METRIC_ID,
            metrics::SCAN_RESULT_METRIC_ID,
            metrics::SCAN_FAILURE_METRIC_ID,
        };
        // These metrics are only logged when connection attempt succeeded.
        let unexpected_metrics = hashset! {
            metrics::CONNECTION_ATTEMPTS_METRIC_ID,
            metrics::CONNECTION_SUCCESS_WITH_ATTEMPTS_BREAKDOWN_METRIC_ID,
        };
        test_metric_subset(&connect_stats, expected_metrics_subset, unexpected_metrics);
    }

    #[test]
    fn test_log_connect_stats_select_network_failure() {
        let connect_stats = ConnectStats {
            result: SelectNetworkFailure::NoCompatibleNetwork.into(),
            ..fake_connect_stats()
        };
        let expected_metrics_subset = hashset! {
            metrics::CONNECTION_RESULT_METRIC_ID,
            metrics::CONNECTION_FAILURE_METRIC_ID,
            metrics::NETWORK_SELECTION_FAILURE_METRIC_ID,
        };
        test_metric_subset(&connect_stats, expected_metrics_subset, hashset! {});
    }

    #[test]
    fn test_log_connect_stats_auth_failure() {
        let connect_stats = ConnectStats {
            result: ConnectFailure::AuthenticationFailure(
                fidl_mlme::AuthenticateResultCodes::Refused,
            )
            .into(),
            ..fake_connect_stats()
        };
        let expected_metrics_subset = hashset! {
            metrics::CONNECTION_RESULT_METRIC_ID,
            metrics::CONNECTION_FAILURE_METRIC_ID,
            metrics::AUTHENTICATION_FAILURE_METRIC_ID,
        };
        test_metric_subset(&connect_stats, expected_metrics_subset, hashset! {});
    }

    #[test]
    fn test_log_connect_stats_assoc_failure() {
        let connect_stats = ConnectStats {
            result: ConnectFailure::AssociationFailure(
                fidl_mlme::AssociateResultCodes::RefusedReasonUnspecified,
            )
            .into(),
            ..fake_connect_stats()
        };
        let expected_metrics_subset = hashset! {
            metrics::CONNECTION_RESULT_METRIC_ID,
            metrics::CONNECTION_FAILURE_METRIC_ID,
            metrics::ASSOCIATION_FAILURE_METRIC_ID,
        };
        test_metric_subset(&connect_stats, expected_metrics_subset, hashset! {});
    }

    #[test]
    fn test_log_connect_stats_establish_rsna_failure() {
        let connect_stats = ConnectStats {
            result: EstablishRsnaFailure::OverallTimeout.into(),
            ..fake_connect_stats()
        };
        let expected_metrics_subset = hashset! {
            metrics::CONNECTION_RESULT_METRIC_ID,
            metrics::CONNECTION_FAILURE_METRIC_ID,
            metrics::ESTABLISH_RSNA_FAILURE_METRIC_ID,
        };
        test_metric_subset(&connect_stats, expected_metrics_subset, hashset! {});
    }

    #[test]
    fn test_log_connection_ping() {
        use metrics::ConnectionCountByDurationMetricDimensionConnectedTime::*;

        let (mut cobalt_sender, mut cobalt_receiver) = fake_cobalt_sender();
        let start = now();
        let ping = ConnectionPingInfo::first_connected(start);
        log_connection_ping(&mut cobalt_sender, &ping);
        assert_ping_metrics(&mut cobalt_receiver, &[Connected as u32]);
        assert_variant!(cobalt_receiver.try_next(), Ok(Some(event)) => {
            assert_eq!(event.metric_id, metrics::CONNECTION_COUNT_BY_DURATION_METRIC_ID);
            assert_eq!(event.event_codes, vec![Connected as u32])
        });

        let ping = ping.next_ping(start + 1.minute());
        log_connection_ping(&mut cobalt_sender, &ping);
        assert_ping_metrics(&mut cobalt_receiver, &[Connected as u32, ConnectedOneMinute as u32]);
        assert_variant!(cobalt_receiver.try_next(), Ok(Some(event)) => {
            assert_eq!(event.metric_id, metrics::CONNECTION_COUNT_BY_DURATION_METRIC_ID);
            assert_eq!(event.event_codes, vec![ConnectedOneMinute as u32])
        });

        let ping = ping.next_ping(start + 3.minutes());
        log_connection_ping(&mut cobalt_sender, &ping);
        assert_ping_metrics(&mut cobalt_receiver, &[Connected as u32, ConnectedOneMinute as u32]);
        // check that CONNECTION_COUNT_BY_DURATION isn't logged since new milestone is not reached
        assert_variant!(cobalt_receiver.try_next(), Ok(None) | Err(..));

        let ping = ping.next_ping(start + 10.minutes());
        log_connection_ping(&mut cobalt_sender, &ping);
        assert_ping_metrics(
            &mut cobalt_receiver,
            &[Connected as u32, ConnectedOneMinute as u32, ConnectedTenMinute as u32],
        );
        assert_variant!(cobalt_receiver.try_next(), Ok(Some(event)) => {
            assert_eq!(event.metric_id, metrics::CONNECTION_COUNT_BY_DURATION_METRIC_ID);
            assert_eq!(event.event_codes, vec![ConnectedTenMinute as u32])
        });
    }

    fn assert_ping_metrics(cobalt_receiver: &mut mpsc::Receiver<CobaltEvent>, milestones: &[u32]) {
        for milestone in milestones {
            assert_variant!(cobalt_receiver.try_next(), Ok(Some(event)) => {
                assert_eq!(event.metric_id, metrics::CONNECTION_UPTIME_PING_METRIC_ID);
                assert_eq!(event.event_codes, vec![*milestone]);
            });
        }
    }

    #[test]
    fn test_log_connection_lost_info() {
        let (mut cobalt_sender, mut cobalt_receiver) = fake_cobalt_sender();
        let connection_lost_info = ConnectionLostInfo {
            connected_duration: 30.seconds(),
            bssid: [1u8; 6],
            last_rssi: -90,
        };
        log_connection_lost(&mut cobalt_sender, &connection_lost_info);

        assert_variant!(cobalt_receiver.try_next(), Ok(Some(event)) => {
            assert_eq!(event.metric_id, metrics::LOST_CONNECTION_COUNT_METRIC_ID);
        });
    }

    fn test_metric_subset(
        connect_stats: &ConnectStats,
        mut expected_metrics_subset: HashSet<u32>,
        unexpected_metrics: HashSet<u32>,
    ) {
        let (mut cobalt_sender, mut cobalt_receiver) = fake_cobalt_sender();
        log_connect_stats(&mut cobalt_sender, connect_stats);

        while let Ok(Some(event)) = cobalt_receiver.try_next() {
            assert!(
                !unexpected_metrics.contains(&event.metric_id),
                "unexpected event: {:?}",
                event
            );
            if expected_metrics_subset.contains(&event.metric_id) {
                expected_metrics_subset.remove(&event.metric_id);
            }
        }
        assert!(
            expected_metrics_subset.is_empty(),
            "some metrics not logged: {:?}",
            expected_metrics_subset
        );
    }

    fn now() -> zx::Time {
        zx::Time::get(zx::ClockId::Monotonic)
    }

    fn fake_connect_stats() -> ConnectStats {
        let now = now();
        ConnectStats {
            connect_start_at: now,
            connect_end_at: now,
            scan_start_stats: Some(ScanStartStats {
                scan_start_at: now,
                scan_type: fidl_mlme::ScanTypes::Passive,
                scan_start_while_connected: false,
            }),
            scan_end_stats: Some(ScanEndStats {
                scan_end_at: now,
                result: ScanResult::Success,
                bss_count: 1,
            }),
            auth_start_at: Some(now),
            auth_end_at: Some(now),
            assoc_start_at: Some(now),
            assoc_end_at: Some(now),
            rsna_start_at: Some(now),
            rsna_end_at: Some(now),
            supplicant_error: None,
            supplicant_progress: Some(SupplicantProgress {
                pmksa_established: true,
                ptksa_established: true,
                gtksa_established: true,
                esssa_established: true,
            }),
            num_rsna_key_frame_exchange_timeout: 0,
            result: ConnectResult::Success,
            candidate_network: Some(fake_bss_description()),
            attempts: 1,
            last_ten_failures: vec![],
            previous_disconnect_info: Some(PreviousDisconnectInfo {
                ssid: fake_bss_description().ssid,
                disconnect_cause: DisconnectCause::Manual,
                disconnect_at: now - 10.seconds(),
            }),
        }
    }

    fn fake_bss_description() -> fidl_mlme::BssDescription {
        fidl_mlme::BssDescription {
            bssid: [7, 1, 2, 77, 53, 8],
            ssid: b"foo".to_vec(),
            bss_type: fidl_mlme::BssTypes::Infrastructure,
            beacon_period: 100,
            dtim_period: 100,
            timestamp: 0,
            local_time: 0,
            cap: wlan_common::mac::CapabilityInfo(0).0,
            rates: vec![],
            country: None,
            rsne: None,
            vendor_ies: None,

            rcpi_dbmh: 0,
            rsni_dbh: 0,

            ht_cap: None,
            ht_op: None,
            vht_cap: None,
            vht_op: None,
            chan: fidl_common::WlanChan {
                primary: 1,
                secondary80: 0,
                cbw: fidl_common::Cbw::Cbw20,
            },
            rssi_dbm: 0,
            snr_db: 0,
        }
    }

    fn event_count(count: i64) -> EventPayload {
        EventPayload::EventCount(CountEvent { period_duration_micros: 0, count })
    }

    fn fake_iface_stats(nth_req: u64) -> IfaceStats {
        IfaceStats {
            dispatcher_stats: DispatcherStats {
                any_packet: fake_packet_counter(nth_req),
                mgmt_frame: fake_packet_counter(nth_req),
                ctrl_frame: fake_packet_counter(nth_req),
                data_frame: fake_packet_counter(nth_req),
            },
            mlme_stats: Some(Box::new(ClientMlmeStats(fidl_stats::ClientMlmeStats {
                svc_msg: fake_packet_counter(nth_req),
                data_frame: fake_packet_counter(nth_req),
                mgmt_frame: fake_packet_counter(nth_req),
                tx_frame: fake_packet_counter(nth_req),
                rx_frame: fake_packet_counter(nth_req),
                assoc_data_rssi: fake_rssi(nth_req),
                beacon_rssi: fake_rssi(nth_req),
                noise_floor_histograms: fake_noise_floor_histograms(),
                rssi_histograms: fake_rssi_histograms(),
                rx_rate_index_histograms: fake_rx_rate_index_histograms(),
                snr_histograms: fake_snr_histograms(),
            }))),
        }
    }

    fn fake_packet_counter(nth_req: u64) -> PacketCounter {
        PacketCounter {
            in_: Counter { count: 1 * nth_req, name: "in".to_string() },
            out: Counter { count: 2 * nth_req, name: "out".to_string() },
            drop: Counter { count: 3 * nth_req, name: "drop".to_string() },
            in_bytes: Counter { count: 4 * nth_req, name: "in_bytes".to_string() },
            out_bytes: Counter { count: 5 * nth_req, name: "out_bytes".to_string() },
            drop_bytes: Counter { count: 6 * nth_req, name: "drop_bytes".to_string() },
        }
    }

    fn fake_rssi(nth_req: u64) -> fidl_stats::RssiStats {
        fidl_stats::RssiStats { hist: vec![nth_req] }
    }

    fn fake_antenna_id() -> Option<Box<fidl_stats::AntennaId>> {
        Some(Box::new(fidl_stats::AntennaId { freq: fidl_stats::AntennaFreq::Antenna5G, index: 0 }))
    }

    fn fake_noise_floor_histograms() -> Vec<fidl_stats::NoiseFloorHistogram> {
        vec![fidl_stats::NoiseFloorHistogram {
            hist_scope: fidl_stats::HistScope::PerAntenna,
            antenna_id: fake_antenna_id(),
            // Noise floor bucket_index 165 indicates -90 dBm.
            noise_floor_samples: vec![fidl_stats::HistBucket {
                bucket_index: 165,
                num_samples: 10,
            }],
            invalid_samples: 0,
        }]
    }

    fn fake_rssi_histograms() -> Vec<fidl_stats::RssiHistogram> {
        vec![fidl_stats::RssiHistogram {
            hist_scope: fidl_stats::HistScope::PerAntenna,
            antenna_id: fake_antenna_id(),
            // RSSI bucket_index 225 indicates -30 dBm.
            rssi_samples: vec![fidl_stats::HistBucket { bucket_index: 225, num_samples: 10 }],
            invalid_samples: 0,
        }]
    }

    fn fake_rx_rate_index_histograms() -> Vec<fidl_stats::RxRateIndexHistogram> {
        vec![fidl_stats::RxRateIndexHistogram {
            hist_scope: fidl_stats::HistScope::PerAntenna,
            antenna_id: fake_antenna_id(),
            // Rate bucket_index 74 indicates HT BW40 MCS 14 SGI, which is 802.11n 270 Mb/s.
            // Rate bucket_index 75 indicates HT BW40 MCS 15 SGI, which is 802.11n 300 Mb/s.
            rx_rate_index_samples: vec![
                fidl_stats::HistBucket { bucket_index: 74, num_samples: 5 },
                fidl_stats::HistBucket { bucket_index: 75, num_samples: 5 },
            ],
            invalid_samples: 0,
        }]
    }

    fn fake_snr_histograms() -> Vec<fidl_stats::SnrHistogram> {
        vec![fidl_stats::SnrHistogram {
            hist_scope: fidl_stats::HistScope::PerAntenna,
            antenna_id: fake_antenna_id(),
            // Signal to noise ratio bucket_index 60 indicates 60 dB.
            snr_samples: vec![fidl_stats::HistBucket { bucket_index: 60, num_samples: 10 }],
            invalid_samples: 0,
        }]
    }

    fn fake_iface_map() -> (IfaceMap, impl Stream<Item = StatsRequest>) {
        let (ifaces_map, _watcher) = IfaceMap::new();
        let (iface_device, stats_requests) = fake_iface_device();
        ifaces_map.insert(IFACE_ID, iface_device);
        (ifaces_map, stats_requests)
    }

    fn fake_iface_device() -> (IfaceDevice, impl Stream<Item = StatsRequest>) {
        let (sme_sender, _sme_receiver) = mpsc::unbounded();
        let (stats_sched, stats_requests) = stats_scheduler::create_scheduler();
        let (proxy, _server) = create_proxy::<MlmeMarker>().expect("Error creating proxy");
        let mlme_query = MlmeQueryProxy::new(proxy);
        let device_info = fake_device_info();
        let iface_device = IfaceDevice {
            phy_ownership: device::PhyOwnership { phy_id: 0, phy_assigned_id: 0 },
            sme_server: device::SmeServer::Client(sme_sender),
            stats_sched,
            mlme_query,
            device_info,
        };
        (iface_device, stats_requests)
    }

    fn fake_device_info() -> fidl_mlme::DeviceInfo {
        fidl_mlme::DeviceInfo {
            role: fidl_mlme::MacRole::Client,
            bands: vec![],
            mac_addr: [0xAC; 6],
            driver_features: vec![],
            qos_capable: false,
        }
    }

    fn fake_cobalt_sender() -> (CobaltSender, mpsc::Receiver<CobaltEvent>) {
        const BUFFER_SIZE: usize = 100;
        let (sender, receiver) = mpsc::channel(BUFFER_SIZE);
        (CobaltSender::new(sender), receiver)
    }
}
