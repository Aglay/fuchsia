// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    core::{
        pin::Pin,
        task::{Context, Poll},
    },
    fuchsia_bluetooth::types::Channel,
    futures::{
        ready,
        stream::{FusedStream, Stream, StreamExt},
        AsyncWriteExt,
    },
    std::collections::HashMap,
};

use crate::{
    at::{AtAgMessage, AtHfMessage, Parser},
    procedure::{Procedure, ProcedureError, ProcedureMarker, ProcedureRequest},
};

/// A connection between two peers that shares synchronized state and acts as the control plane for
/// HFP. See HFP v1.8, 4.2 for more information.
pub struct ServiceLevelConnection {
    /// The underlying RFCOMM channel connecting the peers.
    channel: Option<Channel>,
    /// Whether the channel has been initialized with the SLCI Procedure.
    initialized: bool,
    /// An AT Command parser instance.
    parser: Parser,
    /// The current active procedures serviced by this SLC.
    procedures: HashMap<ProcedureMarker, Box<dyn Procedure>>,
}

impl ServiceLevelConnection {
    /// Create a new, unconnected `ServiceLevelConnection`.
    pub fn new() -> Self {
        Self {
            initialized: false,
            channel: None,
            parser: Parser::default(),
            procedures: HashMap::new(),
        }
    }

    /// Returns `true` if an active connection exists between the peers.
    pub fn connected(&self) -> bool {
        self.channel.as_ref().map(|ch| !ch.is_terminated()).unwrap_or(false)
    }

    /// Returns `true` if the channel has been initialized - namely the SLCI procedure has
    /// been completed for the connected channel.
    #[cfg(test)]
    fn initialized(&self) -> bool {
        self.connected() && self.initialized
    }

    /// Returns `true` if the provided `procedure` is currently active.
    #[cfg(test)]
    fn is_active(&self, procedure: &ProcedureMarker) -> bool {
        self.procedures.contains_key(procedure)
    }

    /// Connect using the provided `channel`.
    pub fn connect(&mut self, channel: Channel) {
        self.channel = Some(channel);
    }

    /// Sets the channel status to initialized.
    /// Note: This should only be called when the SLCI Procedure has successfully finished
    /// or in testing scenarios.
    fn set_initialized(&mut self) {
        // TODO(fxbug.dev/71643): Save the results of the SLCI procedure here.
        self.initialized = true;
    }

    /// Close the service level connection and reset the state.
    fn reset(&mut self) {
        *self = Self::new();
    }

    pub async fn send_message_to_peer(&mut self, message: AtAgMessage) {
        let bytes = message.into_bytes();
        if let Some(ch) = &mut self.channel {
            log::info!("Sent {:?}", String::from_utf8_lossy(&bytes));
            ch.write_all(&bytes).await.unwrap();
        }
    }

    /// Garbage collects the provided `procedure` and returns true if it has terminated.
    fn check_and_cleanup_procedure(&mut self, procedure: &ProcedureMarker) -> bool {
        let is_terminated = self.procedures.get(procedure).map_or(false, |p| p.is_terminated());
        if is_terminated {
            self.procedures.remove(procedure);

            // Special case of the SLCI Procedure - once this is complete, the channel is
            // considered initialized.
            if *procedure == ProcedureMarker::SlcInitialization {
                self.set_initialized();
            }
        }
        is_terminated
    }

    /// Consume bytes from the peer, producing a parsed AtHfMessage from the bytes and
    /// handling it.
    pub fn receive_data(
        &mut self,
        bytes: Vec<u8>,
    ) -> Result<(ProcedureMarker, ProcedureRequest), ProcedureError> {
        // Parse the byte buffer into a HF message.
        let command = self.parser.parse(&bytes);
        log::info!("Received {:?} from peer", command);

        // Attempt to match the received message to a procedure.
        let procedure_id = self.match_command_to_procedure(&command)?;
        // Progress the procedure with the message.
        let request = self.hf_message(procedure_id, command);
        // Potentially clean up the procedure if this was the last stage. Procedures that
        // have been cleaned up cannot require additional responses, as this would violate
        // the `Procedure::is_terminated()` guarantee.
        if self.check_and_cleanup_procedure(&procedure_id) && request.requires_response() {
            return Err(ProcedureError::UnexpectedRequest);
        }

        // There is special consideration for the SLC Initialization procedure:
        //   - Errors in this procedure are considered fatal. If we encounter an error in this
        //     procedure, we close the underlying RFCOMM channel and let the peer (HF) retry.
        // TODO(fxbug.dev/70591): We should determine the appropriate response to errors in other
        // procedures. It may make sense to shut down the entire SLC for all errors because the
        // service level connection is considered synchronized with the remote peer.
        if procedure_id == ProcedureMarker::SlcInitialization {
            // Errors in this procedure are considered fatal.
            if request.is_err() {
                log::warn!("Error in the SLC Initialization procedure. Closing channel");
                self.reset();
            }
        }

        Ok((procedure_id, request))
    }

    /// Matches the incoming HF message to a procedure. Returns the procedure identifier
    /// for the given `command` or an error if the command couldn't be matched.
    pub fn match_command_to_procedure(
        &self,
        command: &AtHfMessage,
    ) -> Result<ProcedureMarker, ProcedureError> {
        // If we haven't initialized the SLC yet, the only valid procedure to match is
        // the SLCI Procedure.
        if !self.initialized {
            return Ok(ProcedureMarker::SlcInitialization);
        }

        // Otherwise, try to match it to a procedure - it must be a non SLCI command since
        // the channel has already been initialized.
        match ProcedureMarker::match_command(command) {
            Ok(ProcedureMarker::SlcInitialization) => {
                log::warn!(
                    "Received unexpected SLCI command after SLC initialization: {:?}",
                    command
                );
                Err(ProcedureError::UnexpectedHf(command.clone()))
            }
            res => res,
        }
    }

    /// Updates the the procedure specified by the `marker` with the received AG `message`.
    /// Initializes the procedure if it is not already in progress.
    /// Returns the request associated with the `message`.
    pub fn ag_message(
        &mut self,
        marker: ProcedureMarker,
        message: AtAgMessage,
    ) -> ProcedureRequest {
        self.procedures.entry(marker).or_insert(marker.initialize()).ag_update(message)
    }

    /// Updates the the procedure specified by the `marker` with the received HF `message`.
    /// Initializes the procedure if it is not already in progress.
    /// Returns the request associated with the `message`.
    pub fn hf_message(
        &mut self,
        marker: ProcedureMarker,
        message: AtHfMessage,
    ) -> ProcedureRequest {
        self.procedures.entry(marker).or_insert(marker.initialize()).hf_update(message)
    }
}

impl Stream for ServiceLevelConnection {
    type Item = Result<(ProcedureMarker, ProcedureRequest), ProcedureError>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if self.is_terminated() {
            panic!("Cannot poll a terminated stream");
        }
        if let Some(channel) = &mut self.channel {
            Poll::Ready(
                ready!(channel.poll_next_unpin(cx)).map(|item| {
                    item.map_or_else(|e| Err(e.into()), |data| self.receive_data(data))
                }),
            )
        } else {
            Poll::Pending
        }
    }
}

impl FusedStream for ServiceLevelConnection {
    fn is_terminated(&self) -> bool {
        !self.connected()
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            at::IndicatorStatus,
            protocol::features::{AgFeatures, HfFeatures},
        },
        fuchsia_async as fasync,
        fuchsia_bluetooth::types::Channel,
        futures::io::AsyncWriteExt,
        matches::assert_matches,
    };

    fn create_and_connect_slc() -> (ServiceLevelConnection, Channel) {
        let mut slc = ServiceLevelConnection::new();
        let (local, remote) = Channel::create();
        slc.connect(local);

        (slc, remote)
    }

    #[fasync::run_until_stalled(test)]
    async fn connected_state_before_and_after_connect() {
        let mut slc = ServiceLevelConnection::new();
        assert!(!slc.connected());
        let (_left, right) = Channel::create();
        slc.connect(right);
        assert!(slc.connected());
    }

    #[fasync::run_until_stalled(test)]
    async fn slc_stream_produces_items() {
        let (mut slc, mut remote) = create_and_connect_slc();

        remote.write_all(b"AT+BRSF=0\r").await.unwrap();

        let expected_marker = ProcedureMarker::SlcInitialization;

        let (actual_marker, actual_request) = match slc.next().await {
            Some(Ok((m, r))) => (m, r),
            x => panic!("Unexpected stream item: {:?}", x),
        };
        // The BRSF should start the SLCI procedure.
        assert_eq!(actual_marker, expected_marker);
        assert_matches!(actual_request, ProcedureRequest::GetAgFeatures { .. });
    }

    #[fasync::run_until_stalled(test)]
    async fn slc_stream_terminated() {
        let (mut slc, remote) = create_and_connect_slc();

        drop(remote);

        assert_matches!(slc.next().await, None);
        assert!(!slc.connected());
        assert!(slc.is_terminated());
    }

    #[fasync::run_until_stalled(test)]
    async fn unexpected_command_before_initialization_closes_channel() {
        let (mut slc, remote) = create_and_connect_slc();

        // Peer sends an unexpected AT command.
        let unexpected = format!("AT+CIND=\r").into_bytes();
        let _ = remote.as_ref().write(&unexpected);

        {
            match slc.next().await {
                Some(Ok((_, ProcedureRequest::Error(_)))) => {}
                x => panic!("Expected Error Request but got: {:?}", x),
            }
        }

        // Channel should be disconnected now.
        assert!(!slc.connected());
    }

    async fn expect_outgoing_message_to_peer(slc: &mut ServiceLevelConnection) {
        match slc.next().await {
            Some(Ok((_, ProcedureRequest::SendMessage(_)))) => {}
            x => panic!("Expected SendMessage but got: {:?}", x),
        }
    }

    // TODO(fxbug.dev/71412): Migrate regex-based AT to library definitions.
    #[fasync::run_until_stalled(test)]
    async fn completing_slc_init_procedure_initializes_channel() {
        let (mut slc, remote) = create_and_connect_slc();
        let slci_marker = ProcedureMarker::SlcInitialization;
        assert!(!slc.initialized());
        assert!(!slc.is_active(&slci_marker));

        // Peer sends us HF features.
        let features = HfFeatures::THREE_WAY_CALLING;
        let command1 = format!("AT+BRSF={}\r", features.bits()).into_bytes();
        let _ = remote.as_ref().write(&command1);
        let response_fn1 = {
            match slc.next().await {
                Some(Ok((_, ProcedureRequest::GetAgFeatures { response }))) => response,
                x => panic!("Expected GetAgFeatures but got: {:?}", x),
            }
        };
        // At this point, the SLC Initialization procedure should be in progress.
        assert!(slc.is_active(&slci_marker));
        // Simulate local response with AG Features - expect these to be sent to the peer.
        let features = AgFeatures::empty();
        let next_request = slc.ag_message(slci_marker, response_fn1(features));
        assert_matches!(next_request, ProcedureRequest::SendMessage(_));

        // Peer sends us an HF supported indicators request - expect an outgoing message
        // to the peer.
        let command2 = format!("AT+CIND=?\r").into_bytes();
        let _ = remote.as_ref().write(&command2);
        expect_outgoing_message_to_peer(&mut slc).await;

        // We then expect the HF to request the indicator status which will result
        // in the procedure asking the AG for the status.
        let command3 = format!("AT+CIND?\r").into_bytes();
        let _ = remote.as_ref().write(&command3);
        let response_fn2 = {
            match slc.next().await {
                Some(Ok((_, ProcedureRequest::GetAgIndicatorStatus { response }))) => response,
                x => panic!("Expected GetAgFeatures but got: {:?}", x),
            }
        };
        // Simulate local response with AG status - expect this to go to the peer.
        let status = IndicatorStatus::default();
        let next_request = slc.ag_message(slci_marker, response_fn2(status));
        assert_matches!(next_request, ProcedureRequest::SendMessage(_));

        // We then expect HF to request enabling Ind Status update in the AG - expect an outgoing
        // message to the peer.
        let command4 = format!("AT+CMER\r").into_bytes();
        let _ = remote.as_ref().write(&command4);
        expect_outgoing_message_to_peer(&mut slc).await;

        // The SLC should be considered initialized and the SLCI Procedure garbage collected.
        assert!(slc.initialized());
        assert!(!slc.is_active(&slci_marker));
    }

    // TODO(fxbug.dev/71412): Migrate regex-based AT to library definitions.
    #[test]
    fn slci_command_after_initialization_returns_error() {
        let _exec = fasync::Executor::new().unwrap();
        let (mut slc, _remote) = create_and_connect_slc();
        // Bypass the SLCI procedure by setting the channel to initialized.
        slc.set_initialized();

        // Receiving an AT command associated with the SLCI procedure thereafter should
        // be an error.
        let cmd1 = AtHfMessage::HfFeatures(HfFeatures::empty());
        assert_matches!(
            slc.match_command_to_procedure(&cmd1),
            Err(ProcedureError::UnexpectedHf(_))
        );
        let cmd2 = AtHfMessage::AgIndStat;
        assert_matches!(
            slc.match_command_to_procedure(&cmd2),
            Err(ProcedureError::UnexpectedHf(_))
        );
    }
}
