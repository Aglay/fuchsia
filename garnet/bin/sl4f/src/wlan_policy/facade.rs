// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{common_utils::common::macros::with_line, wlan_policy::types::ClientStateSummary},
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_wlan_common as fidl_common, fidl_fuchsia_wlan_policy as fidl_policy,
    fuchsia_component::client::connect_to_service,
    fuchsia_syslog::macros::*,
    futures::TryStreamExt,
    parking_lot::RwLock,
    std::{
        collections::HashSet,
        fmt::{self, Debug},
    },
};

struct InnerWlanPolicyFacade {
    controller: Option<fidl_policy::ClientControllerProxy>,
    update_listener: Option<fidl_policy::ClientStateUpdatesRequestStream>,
}

#[derive(Debug)]
pub struct WlanPolicyFacade {
    inner: RwLock<InnerWlanPolicyFacade>,
}

impl Debug for InnerWlanPolicyFacade {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let update_listener = if self.update_listener.is_some() {
            "Some(ClientStateUpdatesRequestStream)"
        } else {
            "None"
        }
        .to_string();

        f.debug_struct("InnerWlanPolicyFacade")
            .field("controller", &self.controller)
            .field("update_listener", &update_listener)
            .finish()
    }
}

impl WlanPolicyFacade {
    pub fn new() -> Result<WlanPolicyFacade, Error> {
        Ok(Self {
            inner: RwLock::new(InnerWlanPolicyFacade { controller: None, update_listener: None }),
        })
    }

    /// Client controller needs to be created once per server before the client controller API
    /// can be used. This will get the client conrtoller if has not already been initialized, if it
    /// has been initialize it does nothing.
    pub fn create_client_controller(&self) -> Result<(), Error> {
        let tag = "WlanPolicyFacade::create_client_controller";
        let mut inner = self.inner.write();
        if let Some(controller) = inner.controller.as_ref() {
            fx_log_info!(tag: &with_line!(tag), "Current client controller: {:?}", controller);
        } else {
            // Get controller
            fx_log_info!(tag: &with_line!(tag), "Setting new client controller");
            let (controller, update_stream) = Self::init_client_controller().map_err(|e| {
                fx_log_info!(tag: &with_line!(tag), "Error getting client controller: {}", e);
                format_err!("Error getting client,controller: {}", e)
            })?;
            inner.controller = Some(controller);
            // Do not set value if it has already been set by getting updates.
            if inner.update_listener.is_none() {
                inner.update_listener = Some(update_stream);
            }
        }
        Ok(())
    }

    /// This also returns the stream for listener updates that is created in the process of
    /// creating the client controller.
    fn init_client_controller() -> Result<
        (fidl_policy::ClientControllerProxy, fidl_policy::ClientStateUpdatesRequestStream),
        Error,
    > {
        let provider = connect_to_service::<fidl_policy::ClientProviderMarker>()?;
        let (controller, req) =
            fidl::endpoints::create_proxy::<fidl_policy::ClientControllerMarker>()?;
        let (update_sink, update_stream) =
            fidl::endpoints::create_request_stream::<fidl_policy::ClientStateUpdatesMarker>()?;
        provider.get_controller(req, update_sink)?;
        Ok((controller, update_stream))
    }

    /// Creates a listener update stream for getting status updates. This might be used to set the
    /// facade's listener update stream if it wasn't set by creating the client controller.
    fn init_listener() -> Result<fidl_policy::ClientStateUpdatesRequestStream, Error> {
        let listener = connect_to_service::<fidl_policy::ClientListenerMarker>()?;
        let (client_end, server_end) =
            fidl::endpoints::create_endpoints::<fidl_policy::ClientStateUpdatesMarker>().unwrap();
        listener.get_listener(client_end)?;
        let server_stream = server_end.into_stream()?;
        Ok(server_stream)
    }

    /// Request a scan and return the list of network names found, or an error if one occurs.
    pub async fn scan_for_networks(&self) -> Result<Vec<String>, Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        // Policy will send results back through this iterator
        let (iter, server) =
            fidl::endpoints::create_proxy::<fidl_policy::ScanResultIteratorMarker>()
                .context("failed to create iterator")?;
        // Request a scan from policy
        controller.scan_for_networks(server)?;

        // Get results and check for scan error. Get the next chunk of results until we get an
        // error or empty list, which indicates the end of results.
        let mut scan_results = HashSet::new();
        loop {
            let results = iter.get_next().await?.map_err(|e| format_err!("{:?}", e))?;
            if results.is_empty() {
                break;
            }

            // For now, just return the names of the scanned networks.
            let results = Self::stringify_scan_results(results);
            scan_results.extend(results);
        }
        Ok(scan_results.into_iter().collect())
    }

    /// Connect to a network through the policy layer. The network muct have been saved first.
    /// Returns an error if the connect command was not recieved, otherwise it returns a boolean
    /// representing whether the connection was successful. Until listener updates are used, it
    /// will return false.
    /// NOTE - this will trigger a connection but will not yet return whether connection happened.
    /// # Arguments:
    /// * `target_ssid': The SSID (network name) that we want to connect to.
    /// * `type`: Security type should be a string of the security type, either "none", "wep",
    ///           "wpa", "wpa2" or "wpa3", matching the policy API's defined security types, case
    ///           doesn't matter.
    /// TODO(46928): Use listener upates
    pub async fn connect(
        &self,
        target_ssid: Vec<u8>,
        type_: fidl_policy::SecurityType,
    ) -> Result<bool, Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        let mut network_id = fidl_policy::NetworkIdentifier { ssid: target_ssid, type_ };
        match controller.connect(&mut network_id).await.expect("Connect: failed to connect") {
            fidl_common::RequestStatus::Acknowledged => {
                // For now we will just check whether our request is acknowledged.
                return Ok(false);
            }
            fidl_common::RequestStatus::RejectedNotSupported => bail!("RejectedNotSupported"),
            _ => {
                bail!("Scan: failed to send request");
            }
        }
    }

    /// Forget the specified saved network. Doesn't do anything if network not saved.
    /// # Arguments:
    /// * `target_ssid`:  The SSID (network name) that we want to forget.
    /// * `type`: the security type of the network. It should be a string, either "none", "wep",
    ///           "wpa", "wpa2" or "wpa3", matching the policy API's defined security types. Target
    ///           password can be password, PSK, or none, represented by empty string.
    /// * `credential`: the password or other credential of the network we want to forget.
    pub async fn remove_network(
        &self,
        target_ssid: Vec<u8>,
        type_: fidl_policy::SecurityType,
        credential: fidl_policy::Credential,
    ) -> Result<(), Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        let config = fidl_policy::NetworkConfig {
            id: Some(fidl_policy::NetworkIdentifier { ssid: target_ssid, type_ }),
            credential: Some(credential),
        };
        controller
            .remove_network(config)
            .await
            .map_err(|err| format_err!("{:?}", err))? // FIDL error
            .map_err(|err| format_err!("{:?}", err)) // network config change error
    }

    /// Send the request to the policy layer to start making client connections.
    pub async fn start_client_connections(&self) -> Result<(), Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        let req_status = controller.start_client_connections().await?;
        if fidl_common::RequestStatus::Acknowledged == req_status {
            Ok(())
        } else {
            bail!("{:?}", req_status);
        }
    }

    /// Wait for and return a client update. If this is the first update gotten from the facade,
    /// it will get an immedate status. After that, it will wait for a change and return a status
    /// when there has been a change since the last call to get_update. This call will hang if
    /// if there are no updates.
    pub async fn get_update(&self) -> Result<ClientStateSummary, Error> {
        // Initialize the update listener if it has not been initialized.
        let mut inner_guard = self.inner.write();
        if inner_guard.update_listener.is_none() {
            inner_guard.update_listener = Some(Self::init_listener()?);
        }

        // Listener should be set above if not already set.
        let update_listener = inner_guard
            .update_listener
            .as_mut()
            .ok_or(format_err!("failed to set update listener of facade"))?;

        if let Some(update_request) = update_listener.try_next().await? {
            let update = update_request.into_on_client_state_update();
            let (update, responder) = match update {
                Some((update, responder)) => (update, responder),
                None => return Err(format_err!("Client provider produced invalid update.")),
            };
            // Ack the update.
            responder.send().map_err(|e| format_err!("failed to ack update: {}", e))?;
            Ok(update.into())
        } else {
            Err(format_err!("update listener's next update is None"))
        }
    }

    /// Send the request to the policy layer to stop making client connections.
    pub async fn stop_client_connections(&self) -> Result<(), Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        let req_status = controller.stop_client_connections().await?;
        if fidl_common::RequestStatus::Acknowledged == req_status {
            Ok(())
        } else {
            bail!("{:?}", req_status);
        }
    }

    /// Save the specified network.
    /// # Arguments:
    /// * `target_ssid`:  The SSID (network name) that we want to save.
    /// * `type`: the security type of the network. It should be a string, either "none", "wep",
    ///           "wpa", "wpa2" or "wpa3", matching the policy API's defined security types. Target
    ///           password can be password, PSK, or none, represented by empty string
    /// * `credential`: the password or other credential of the network we want to remember.
    pub async fn save_network(
        &self,
        target_ssid: Vec<u8>,
        type_: fidl_policy::SecurityType,
        credential: fidl_policy::Credential,
    ) -> Result<(), Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        let network_id = fidl_policy::NetworkIdentifier { ssid: target_ssid.clone(), type_: type_ };

        controller
            .save_network(fidl_policy::NetworkConfig {
                id: Some(network_id),
                credential: Some(credential),
            })
            .await?
            .map_err(|e| format_err!("{:?}", e))
    }

    // Get a list of the saved networks.
    pub async fn get_saved_networks(&self) -> Result<Vec<String>, Error> {
        let inner_guard = self.inner.read();
        let controller = inner_guard
            .controller
            .as_ref()
            .ok_or(format_err!("client controller has not been initialized"))?;

        // Policy will send configs back through this iterator
        let (iter, server) =
            fidl::endpoints::create_proxy::<fidl_policy::NetworkConfigIteratorMarker>()
                .context("failed to create iterator")?;
        controller
            .get_saved_networks(server)
            .map_err(|e| format_err!("Get saved networks: fidl error {:?}", e))?;

        // get each config from the stream as they become available
        let mut networks = vec![];
        loop {
            let cfgs = iter.get_next().await?;
            if cfgs.is_empty() {
                break;
            }
            networks.extend(cfgs);
        }

        Ok(Self::stringify_networks(networks))
    }

    fn stringify_scan_results(results: Vec<fidl_policy::ScanResult>) -> Vec<String> {
        results
            .into_iter()
            .filter_map(|result| result.id)
            .map(|id| String::from_utf8_lossy(&id.ssid).into_owned())
            .collect()
    }

    // Convert a list of network configs to a list of strings to pass back through SL4F. For now
    // just use the SSIDs.
    fn stringify_networks(networks: Vec<fidl_policy::NetworkConfig>) -> Vec<String> {
        networks
            .into_iter()
            .filter_map(|result| result.id)
            .map(|id| String::from_utf8_lossy(&id.ssid).into_owned())
            .collect()
    }
}
