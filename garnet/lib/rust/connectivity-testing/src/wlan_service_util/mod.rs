// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_wlan_device_service::{DestroyIfaceRequest, DeviceServiceProxy},
    fuchsia_syslog::fx_log_info,
    fuchsia_zircon as zx,
};

pub mod ap;
pub mod client;

// Helper methods for calling wlan_service fidl methods
pub async fn get_iface_list(wlan_svc: &DeviceServiceProxy) -> Result<Vec<u16>, Error> {
    let ifaces = wlan_svc.list_ifaces().await.context("Error getting iface list")?.ifaces;
    Ok(ifaces.into_iter().map(|i| i.iface_id).collect())
}

/// Returns the list of Phy IDs for this system.
///
/// # Arguments
/// * `wlan_svc`: a DeviceServiceProxy
pub async fn get_phy_list(wlan_svc: &DeviceServiceProxy) -> Result<Vec<u16>, Error> {
    let phys = wlan_svc.list_phys().await.context("Error getting phy list")?.phys;
    Ok(phys.into_iter().map(|p| p.phy_id).collect())
}

pub async fn destroy_iface(wlan_svc: &DeviceServiceProxy, iface_id: u16) -> Result<(), Error> {
    let mut req = DestroyIfaceRequest { iface_id };

    let response = wlan_svc.destroy_iface(&mut req).await.context("Error destroying iface")?;
    zx::Status::ok(response).context("Destroy iface returned non-OK status")?;
    Ok(fx_log_info!("Destroyed iface {:?}", iface_id))
}

pub async fn get_wlan_mac_addr(
    wlan_svc: &DeviceServiceProxy,
    iface_id: u16,
) -> Result<[u8; 6], Error> {
    let (_status, resp) = wlan_svc.query_iface(iface_id).await?;
    Ok(resp.ok_or(format_err!("No valid iface response"))?.mac_addr)
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl_fuchsia_wlan_device::MacRole,
        fidl_fuchsia_wlan_device_service::{
            DeviceServiceMarker, DeviceServiceRequest, DeviceServiceRequestStream,
            QueryIfaceResponse,
        },
        fuchsia_async::Executor,
        futures::{task::Poll, StreamExt},
        pin_utils::pin_mut,
        wlan_common::assert_variant,
    };

    fn fake_iface_query_response(
        mac_addr: [u8; 6],
        role: fidl_fuchsia_wlan_device::MacRole,
    ) -> QueryIfaceResponse {
        QueryIfaceResponse { role, id: 0, phy_id: 0, phy_assigned_id: 0, mac_addr }
    }

    pub fn respond_to_query_iface_request(
        exec: &mut Executor,
        req_stream: &mut DeviceServiceRequestStream,
        role: fidl_fuchsia_wlan_device::MacRole,
        fake_mac_addr: Option<[u8; 6]>,
    ) {
        use fuchsia_zircon::sys::{ZX_ERR_NOT_FOUND, ZX_OK};

        let req = exec.run_until_stalled(&mut req_stream.next());
        let responder = assert_variant !(
            req,
            Poll::Ready(Some(Ok(DeviceServiceRequest::QueryIface{iface_id : _, responder})))
            => responder);
        if let Some(mac) = fake_mac_addr {
            let mut response = fake_iface_query_response(mac, role);
            responder
                .send(ZX_OK, Some(&mut response))
                .expect("sending fake response with mac address");
        } else {
            responder.send(ZX_ERR_NOT_FOUND, None).expect("sending fake response with none")
        }
    }

    #[test]
    fn test_get_wlan_mac_addr_ok() {
        let (mut exec, proxy, mut req_stream) = crate::setup_fake_service::<DeviceServiceMarker>();
        let mac_addr_fut = get_wlan_mac_addr(&proxy, 0);
        pin_mut!(mac_addr_fut);

        assert_variant!(exec.run_until_stalled(&mut mac_addr_fut), Poll::Pending);

        respond_to_query_iface_request(
            &mut exec,
            &mut req_stream,
            MacRole::Client,
            Some([1, 2, 3, 4, 5, 6]),
        );

        let mac_addr = exec.run_singlethreaded(&mut mac_addr_fut).expect("should get a mac addr");
        assert_eq!(mac_addr, [1, 2, 3, 4, 5, 6]);
    }

    #[test]
    fn test_get_wlan_mac_addr_not_found() {
        let (mut exec, proxy, mut req_stream) = crate::setup_fake_service::<DeviceServiceMarker>();
        let mac_addr_fut = get_wlan_mac_addr(&proxy, 0);
        pin_mut!(mac_addr_fut);

        assert_variant!(exec.run_until_stalled(&mut mac_addr_fut), Poll::Pending);

        respond_to_query_iface_request(&mut exec, &mut req_stream, MacRole::Client, None);

        let err = exec.run_singlethreaded(&mut mac_addr_fut).expect_err("should be an error");
        assert_eq!("No valid iface response", format!("{}", err));
    }

    #[test]
    fn test_get_wlan_mac_addr_service_interrupted() {
        let (mut exec, proxy, req_stream) = crate::setup_fake_service::<DeviceServiceMarker>();
        let mac_addr_fut = get_wlan_mac_addr(&proxy, 0);
        pin_mut!(mac_addr_fut);

        assert_variant!(exec.run_until_stalled(&mut mac_addr_fut), Poll::Pending);

        // Simulate service not being available by closing the channel
        std::mem::drop(req_stream);

        let err = exec.run_singlethreaded(&mut mac_addr_fut).expect_err("should be an error");
        assert!(format!("{}", err).contains("PEER_CLOSED"));
    }
}
