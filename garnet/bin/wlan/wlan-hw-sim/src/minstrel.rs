// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{simulation_tests::*, *},
    failure::format_err,
    fidl_fuchsia_wlan_service as fidl_wlan_service, fidl_fuchsia_wlan_tap as wlantap,
    fuchsia_app as app, fuchsia_async as fasync,
    futures::{channel::mpsc, poll, select},
    pin_utils::pin_mut,
    std::collections::HashMap,
};

// Remedy for FLK-24 (DNO-389)
// Refer to |KMinstrelUpdateIntervalForHwSim| in //garnet/drivers/wlan/wlan/device.cpp
const MINSTREL_DATA_FRAME_INTERVAL_NANOS: i64 = 4_000_000;

const BSS_MINSTL: [u8; 6] = [0x6d, 0x69, 0x6e, 0x73, 0x74, 0x0a];
const SSID_MINSTREL: &[u8] = b"minstrel";

pub fn test_rate_selection() {
    let mut exec = fasync::Executor::new().expect("error creating executor");
    let wlan_service = app::client::connect_to_service::<fidl_wlan_service::WlanMarker>()
        .expect("Error connecting to wlan service");
    let mut helper = test_utils::TestHelper::begin_test(
        &mut exec,
        wlantap::WlantapPhyConfig { quiet: true, ..create_wlantap_config() },
    );

    loop_until_iface_is_found(&mut exec, &wlan_service, &mut helper);

    let phy = helper.proxy();
    connect(&mut exec, &wlan_service, &phy, &mut helper, SSID_MINSTREL, &BSS_MINSTL);

    let beacon_sender_fut = beacon_sender(&BSS_MINSTL, SSID_MINSTREL, &phy).fuse();
    pin_mut!(beacon_sender_fut);

    let (sender, mut receiver) = mpsc::channel(1);
    let eth_sender_fut = eth_sender(&mut receiver).fuse();
    pin_mut!(eth_sender_fut);

    let test_fut = async {
        select! {
            _ = eth_sender_fut => Ok(()),
            _ = beacon_sender_fut => Err(format_err!("beacon sender should not have exited")),
        }
    };
    pin_mut!(test_fut);

    // Simulated hardware supports 8 ERP tx vectors with idx 129 to 136, both inclusive.
    // (see `fn send_association_response(...)`)
    const MUST_USE_IDX: &[u16] = &[129, 130, 131, 132, 133, 134, 136]; // Missing 135 is OK.
    const ALL_SUPPORTED_IDX: &[u16] = &[129, 130, 131, 132, 133, 134, 135, 136];
    const ERP_STARTING_IDX: u16 = 129;
    const MAX_SUCCESSFUL_IDX: u16 = 130;
    // Only the lowest ones can succeed in the simulated environment.
    let will_succeed = |idx| ERP_STARTING_IDX <= idx && idx <= MAX_SUCCESSFUL_IDX;
    let is_done = |hm: &HashMap<u16, u64>| {
        // Due to its randomness, Minstrel may skip 135. But the other 7 must be present.
        if hm.keys().len() < MUST_USE_IDX.len() {
            return false;
        }
        // safe to unwrap below because there are at least 7 entries
        let max_key = hm.keys().max_by_key(|k| hm[&k]).unwrap();
        let second_largest =
            hm.iter().map(|(k, v)| if k == max_key { 0 } else { *v }).max().unwrap();
        let max_val = hm[&max_key];
        if max_val % 100 == 0 {
            println!("{:?}", hm);
        }
        // One tx vector has become dominant as the number of data frames transmitted with it
        // is at least 15 times as large as anyone else. 15 is the number of non-probing data
        // frames between 2 consecutive probing data frames.
        const NORMAL_FRAMES_PER_PROBE: u64 = 15;
        max_val >= NORMAL_FRAMES_PER_PROBE * second_largest
    };
    let mut hm = HashMap::new();
    helper
        .run(
            &mut exec,
            30.seconds(),
            "verify rate selection is working",
            |event| {
                handle_rate_selection_event(
                    event,
                    &phy,
                    &BSS_MINSTL,
                    &mut hm,
                    will_succeed,
                    is_done,
                    sender.clone(),
                );
            },
            test_fut,
        )
        .expect("running main future");

    let total = hm.values().sum::<u64>();
    let others = total - hm[&MAX_SUCCESSFUL_IDX];
    println!("{:#?}\ntotal: {}, others: {}", hm, total, others);
    let mut tx_vec_idx_seen: Vec<_> = hm.keys().cloned().collect();
    tx_vec_idx_seen.sort();
    if tx_vec_idx_seen.len() == MUST_USE_IDX.len() {
        // 135 may not be attempted due to randomness and it is OK.
        assert_eq!(&tx_vec_idx_seen[..], MUST_USE_IDX);
    } else {
        assert_eq!(&tx_vec_idx_seen[..], ALL_SUPPORTED_IDX);
    }
    let most_frequently_used_idx = hm.keys().max_by_key(|k| hm[&k]).unwrap();
    println!(
        "If the test fails due to QEMU slowness outside of the scope of WLAN(FLK-24, \
         DNO-389). Try increasing |MINSTREL_DATA_FRAME_INTERVAL_NANOS| above."
    );
    assert_eq!(most_frequently_used_idx, &MAX_SUCCESSFUL_IDX);
}

fn handle_rate_selection_event<F, G>(
    event: wlantap::WlantapPhyEvent,
    phy: &wlantap::WlantapPhyProxy,
    bssid: &[u8; 6],
    hm: &mut HashMap<u16, u64>,
    should_succeed: F,
    is_done: G,
    mut sender: mpsc::Sender<()>,
) where
    F: Fn(u16) -> bool,
    G: Fn(&HashMap<u16, u64>) -> bool,
{
    match event {
        wlantap::WlantapPhyEvent::Tx { args } => {
            let frame_ctrl = get_frame_ctrl(&args.packet.data);
            if frame_ctrl.typ() == mac_frames::FrameControlType::Data as u16 {
                let tx_vec_idx = args.packet.info.tx_vector_idx;
                send_tx_status_report(*bssid, tx_vec_idx, should_succeed(tx_vec_idx), phy)
                    .expect("Error sending tx_status report");
                let count = hm.entry(tx_vec_idx).or_insert(0);
                *count += 1;
                if *count == 1 {
                    println!("new tx_vec_idx: {} at #{}", tx_vec_idx, hm.values().sum::<u64>());
                }
                if is_done(hm) {
                    sender.try_send(()).expect("Indicating test successful");
                }
            }
        }
        _ => {}
    }
}

async fn eth_sender(receiver: &mut mpsc::Receiver<()>) -> Result<(), failure::Error> {
    let mut client = await!(create_eth_client(&HW_MAC_ADDR))
        .expect("cannot create ethernet client")
        .expect(&format!("ethernet client not found {:?}", &HW_MAC_ADDR));

    let mut buf: Vec<u8> = vec![];
    eth_frames::write_eth_header(
        &mut buf,
        &eth_frames::EthHeader {
            dst: BSSID,
            src: HW_MAC_ADDR,
            eth_type: eth_frames::EtherType::Ipv4 as u16,
        },
    )
    .expect("Error creating fake ethernet frame");

    let mut timer_stream = fasync::Interval::new(MINSTREL_DATA_FRAME_INTERVAL_NANOS.nanos());
    let mut client_stream = client.get_stream().fuse();
    'eth_sender: loop {
        select! {
            event = client_stream.next() => {
                if let Some(Ok(ethernet::Event::StatusChanged)) = event {
                    println!("status changed to: {:?}", await!(client.get_status()).unwrap());
                }
            },
            () = timer_stream.select_next_some() => { client.send(&buf); },
        }
        if let Ok(Some(())) = receiver.try_next() {
            break 'eth_sender;
        }
    }
    // Send any packets that are still in the buffer
    let _ = poll!(client_stream.next());
    Ok(())
}

fn create_wlan_tx_status_entry(tx_vec_idx: u16) -> wlantap::WlanTxStatusEntry {
    fidl_fuchsia_wlan_tap::WlanTxStatusEntry { tx_vec_idx: tx_vec_idx, attempts: 1 }
}

fn send_tx_status_report(
    bssid: [u8; 6],
    tx_vec_idx: u16,
    is_successful: bool,
    proxy: &wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    use fidl_fuchsia_wlan_tap::WlanTxStatus;

    let mut ts = WlanTxStatus {
        peer_addr: bssid,
        success: is_successful,
        tx_status_entries: [
            create_wlan_tx_status_entry(tx_vec_idx),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
        ],
    };
    proxy.report_tx_status(0, &mut ts)?;
    Ok(())
}

async fn beacon_sender<'a>(
    bssid: &'a [u8; 6],
    ssid: &'a [u8],
    phy: &'a wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    let mut beacon_timer_stream = fasync::Interval::new(102_400_000.nanos());
    while let Some(_) = await!(beacon_timer_stream.next()) {
        send_beacon(&mut vec![], &CHANNEL, bssid, ssid, &phy).unwrap();
    }
    Ok(())
}
