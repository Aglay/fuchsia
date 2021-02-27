// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::{Procedure, ProcedureError as Error, ProcedureMarker, ProcedureRequest};

use crate::{
    at::{AtAgMessage, AtHfMessage, IndicatorStatus},
    protocol::features::{AgFeatures, HfFeatures},
};

/// A singular state within the SLC Initialization Procedure.
pub trait SlcProcedureState {
    /// Returns the next state in the procedure based on the current state and the given
    /// AG `update`.
    /// By default, the state transition will return an error. Implementors should only
    /// implement this for valid transitions.
    fn ag_update(&self, update: AtAgMessage) -> Box<dyn SlcProcedureState> {
        SlcErrorState::unexpected_ag(update)
    }

    /// Returns the next state in the procedure based on the current state and the given
    /// HF `update`.
    /// By default, the state transition will return an error. Implementors should only
    /// implement this for valid transitions.
    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        SlcErrorState::unexpected_hf(update)
    }

    /// Returns the request for this given state.
    fn request(&self) -> ProcedureRequest;

    /// Returns true if this is the final state in the Procedure.
    fn is_terminal(&self) -> bool {
        false
    }
}

/// The relevant state associated with this procedure. This passed among the states, and
/// is populated as the procedure progresses.
#[derive(Clone, Debug, Default)]
struct SlcState {
    ag_features: AgFeatures,
    hf_features: HfFeatures,
    codecs: Option<Vec<u32>>,
    status: IndicatorStatus,
}

impl SlcState {
    /// Returns true if both peers support the Codec Negotiation state.
    fn codec_negotiation(&self) -> bool {
        self.ag_features.contains(AgFeatures::CODEC_NEGOTIATION)
            && self.hf_features.contains(HfFeatures::CODEC_NEGOTIATION)
    }

    /// Returns true if both peers support Three-way calling.
    fn three_way_calling(&self) -> bool {
        self.hf_features.contains(HfFeatures::THREE_WAY_CALLING)
            && self.ag_features.contains(AgFeatures::THREE_WAY_CALLING)
    }

    /// Returns true if both peers support HF Indicators.
    fn hf_indicators(&self) -> bool {
        self.hf_features.contains(HfFeatures::HF_INDICATORS)
            && self.ag_features.contains(AgFeatures::HF_INDICATORS)
    }
}

/// Represents the current state of the Service Level Connection initialization procedure
/// as defined in HFP v1.8 Section 4.2. Provides an interface for driving the procedure
/// given inputs from the AG and HF.
///
/// This procedure is implemented from the perspective of the AG. Namely, outgoing `requests`
/// typically request information about the current state of the AG, to be sent to the remote
/// peer acting as the HF.
/// The state machine of this procedure looks like:
///   1) SlcInitStart
///   2) HfFeatures
///   3) AgFeatures
///   4) (optional) Codecs
///   5) AgSupportedIndicators
///   6) AgIndicatorStatusRequest
///   7) AgIndicatorStatus
///   8) AgIndicatorStatusEnable
///   9) (optional) 3-way support
///   10) (optional) HfSupportedIndicators
///   11) (optional) ListSupportedGenericIndicators
pub struct SlcInitProcedure {
    state: Box<dyn SlcProcedureState>,
}

impl SlcInitProcedure {
    pub fn new() -> Self {
        Self { state: Box::new(SlcInitStart::default()) }
    }

    /// Builds the SLC Initialization Procedure starting at the given `state`.
    #[cfg(test)]
    pub fn new_at_state(state: impl SlcProcedureState + 'static) -> Self {
        Self { state: Box::new(state) }
    }
}

impl Procedure for SlcInitProcedure {
    fn marker(&self) -> ProcedureMarker {
        ProcedureMarker::SlcInitialization
    }

    fn hf_update(&mut self, update: AtHfMessage) -> ProcedureRequest {
        if self.is_terminated() {
            return ProcedureRequest::Error(Error::AlreadyTerminated);
        }

        self.state = self.state.hf_update(update);
        self.state.request()
    }

    fn ag_update(&mut self, update: AtAgMessage) -> ProcedureRequest {
        if self.is_terminated() {
            return ProcedureRequest::Error(Error::AlreadyTerminated);
        }

        self.state = self.state.ag_update(update);
        self.state.request()
    }

    fn is_terminated(&self) -> bool {
        self.state.is_terminal()
    }
}

/// This is the default starting point of the Service Level Connection Initialization procedure.
/// Per HFP v1.8 Section 4.2.1.6, the HF always initiates this procedure.
#[derive(Debug, Default, Clone)]
struct SlcInitStart {
    state: SlcState,
}

impl SlcProcedureState for SlcInitStart {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::None
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        // Only the HF request containing its features can continue the SLC initialization process.
        match update {
            AtHfMessage::HfFeatures(f) => Box::new(HfFeaturesReceived {
                state: SlcState { hf_features: f, ..self.state.clone() },
            }),
            m => SlcErrorState::unexpected_hf(m),
        }
    }
}

/// We've received a supported features request from the HF.
#[derive(Debug, Clone)]
struct HfFeaturesReceived {
    state: SlcState,
}

impl SlcProcedureState for HfFeaturesReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::GetAgFeatures {
            response: Box::new(|features: AgFeatures| AtAgMessage::AgFeatures(features)),
        }
    }

    fn ag_update(&self, update: AtAgMessage) -> Box<dyn SlcProcedureState> {
        // Only the AG request containing its features can continue the process.
        match update {
            AtAgMessage::AgFeatures(f) => Box::new(AgFeaturesReceived {
                state: SlcState { ag_features: f, ..self.state.clone() },
            }),
            m => SlcErrorState::unexpected_ag(m),
        }
    }
}

#[derive(Debug, Clone)]
struct AgFeaturesReceived {
    state: SlcState,
}

impl SlcProcedureState for AgFeaturesReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::AgFeatures(self.state.ag_features))
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        // The codec negotiation step of this procedure is optional and is determined by the
        // availability specified in the feature flags of the HF and AG.
        match (update, self.state.codec_negotiation()) {
            (AtHfMessage::HfCodecSup(codecs), true) => Box::new(AvailableCodecsReceived {
                state: SlcState { codecs: Some(codecs), ..self.state.clone() },
            }),
            (AtHfMessage::AgIndSupRequest, false) => {
                Box::new(AgSupportedIndicatorsRequested { state: self.state.clone() })
            }
            (m, _) => SlcErrorState::unexpected_hf(m),
        }
    }
}

/// We've received the available codecs from the HF.
#[derive(Debug, Clone)]
struct AvailableCodecsReceived {
    state: SlcState,
}

impl SlcProcedureState for AvailableCodecsReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::Ok)
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        // Only a HF request for AG indicators can continue the procedure.
        match update {
            AtHfMessage::AgIndSupRequest => {
                Box::new(AgSupportedIndicatorsRequested { state: self.state.clone() })
            }
            m => SlcErrorState::unexpected_hf(m),
        }
    }
}

#[derive(Debug, Clone)]
struct AgSupportedIndicatorsRequested {
    state: SlcState,
}

impl SlcProcedureState for AgSupportedIndicatorsRequested {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::AgSupportedIndicators)
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        // Only a HF request for the current status of the AG indicators will
        // continue the procedure.
        match update {
            AtHfMessage::AgIndStat => {
                Box::new(AgIndicatorStatusRequestReceived { state: self.state.clone() })
            }
            m => SlcErrorState::unexpected_hf(m),
        }
    }
}

#[derive(Debug, Clone)]
struct AgIndicatorStatusRequestReceived {
    state: SlcState,
}

impl SlcProcedureState for AgIndicatorStatusRequestReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::GetAgIndicatorStatus {
            response: Box::new(|status: IndicatorStatus| AtAgMessage::AgIndStat(status)),
        }
    }

    fn ag_update(&self, update: AtAgMessage) -> Box<dyn SlcProcedureState> {
        // Only the current status information from the AG will continue the procedure.
        match update {
            AtAgMessage::AgIndStat(status) => Box::new(AgIndicatorStatusReceived {
                state: SlcState { status, ..self.state.clone() },
            }),
            m => SlcErrorState::unexpected_ag(m),
        }
    }
}

#[derive(Debug, Clone)]
struct AgIndicatorStatusReceived {
    state: SlcState,
}

impl SlcProcedureState for AgIndicatorStatusReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::AgIndStat(self.state.status))
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        // Only a HF request to enable the AG Indicator Status will continue the procedure.
        match update {
            AtHfMessage::HfIndStatusAgEnable => {
                Box::new(AgIndicatorStatusEnableReceived { state: self.state.clone() })
            }
            m => SlcErrorState::unexpected_hf(m),
        }
    }
}

/// The last mandatory step in the procedure. After this, there are optional
/// things that can be received.
#[derive(Debug, Clone)]
struct AgIndicatorStatusEnableReceived {
    state: SlcState,
}

impl SlcProcedureState for AgIndicatorStatusEnableReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::Ok)
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        if self.is_terminal() {
            return SlcErrorState::already_terminated();
        }

        // If both parties support three way calling, then we expect the 3-way support message
        // from the HF.
        if self.state.three_way_calling() {
            return match update {
                AtHfMessage::ThreeWaySupport => {
                    Box::new(ThreeWaySupportReceived { state: self.state.clone() })
                }
                m => SlcErrorState::unexpected_hf(m),
            };
        }

        // Otherwise, both parties must be supporting HF Indicators (or else self.is_terminal()
        // would be true).
        match update {
            AtHfMessage::HfIndSup(_, _) => {
                Box::new(HfSupportedIndicatorsReceived { state: self.state.clone() })
            }
            m => SlcErrorState::unexpected_hf(m),
        }
    }

    fn is_terminal(&self) -> bool {
        // We don't continue if neither three way calling nor HF indicators are supported.
        !self.state.three_way_calling() && !self.state.hf_indicators()
    }
}

struct ThreeWaySupportReceived {
    state: SlcState,
}

impl SlcProcedureState for ThreeWaySupportReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::AgThreeWaySupport)
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        if self.is_terminal() {
            return SlcErrorState::already_terminated();
        }

        match update {
            AtHfMessage::HfIndSup(_, _) => {
                Box::new(HfSupportedIndicatorsReceived { state: self.state.clone() })
            }
            m => SlcErrorState::unexpected_hf(m),
        }
    }

    fn is_terminal(&self) -> bool {
        // This is the final state if one or both parties don't support the HF Indicators.
        !self.state.hf_indicators()
    }
}

struct HfSupportedIndicatorsReceived {
    state: SlcState,
}

impl SlcProcedureState for HfSupportedIndicatorsReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::Ok)
    }

    fn hf_update(&self, update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        if self.is_terminal() {
            return SlcErrorState::already_terminated();
        }

        match update {
            AtHfMessage::HfIndAgSup => {
                Box::new(ListSupportedGenericIndicatorsReceived { _state: self.state.clone() })
            }
            m => SlcErrorState::unexpected_hf(m),
        }
    }
}

struct ListSupportedGenericIndicatorsReceived {
    _state: SlcState,
}

impl SlcProcedureState for ListSupportedGenericIndicatorsReceived {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::SendMessage(AtAgMessage::AgSupportedHfSupResp {
            safety: true,
            battery: true,
        })
    }

    fn ag_update(&self, _update: AtAgMessage) -> Box<dyn SlcProcedureState> {
        SlcErrorState::already_terminated()
    }

    fn hf_update(&self, _update: AtHfMessage) -> Box<dyn SlcProcedureState> {
        SlcErrorState::already_terminated()
    }

    fn is_terminal(&self) -> bool {
        // This is the last conditional state.
        true
    }
}

/// Represents the error state for this procedure. Any errors in the SLC
/// Initialization procedure will be considered fatal.
struct SlcErrorState {
    error: Error,
}

impl SlcErrorState {
    /// Builds and returns the error state for an unexpected AG message.
    fn unexpected_ag(m: AtAgMessage) -> Box<dyn SlcProcedureState> {
        Box::new(SlcErrorState { error: Error::UnexpectedAg(m) })
    }

    /// Builds and returns the error state for an unexpected HF message.
    fn unexpected_hf(m: AtHfMessage) -> Box<dyn SlcProcedureState> {
        Box::new(SlcErrorState { error: Error::UnexpectedHf(m) })
    }

    /// Builds and returns the error state for the already terminated error case.
    fn already_terminated() -> Box<dyn SlcProcedureState> {
        Box::new(SlcErrorState { error: Error::AlreadyTerminated })
    }
}

impl SlcProcedureState for SlcErrorState {
    fn request(&self) -> ProcedureRequest {
        ProcedureRequest::Error(self.error.clone())
    }

    fn is_terminal(&self) -> bool {
        // The procedure should be considered terminated in the error state.
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use matches::assert_matches;

    #[test]
    fn supported_features_received_transition_to_codec_negotiation() {
        let state = SlcState {
            hf_features: HfFeatures::CODEC_NEGOTIATION,
            ag_features: AgFeatures::CODEC_NEGOTIATION,
            ..SlcState::default()
        };
        let mut procedure = SlcInitProcedure::new_at_state(AgFeaturesReceived { state });
        let update = AtHfMessage::HfCodecSup(Vec::new());
        // Both parties support codec negotiation, so upon receiving the Codec HF message, we
        // expect to successfully transition to the codec state and the resulting event
        // should be an Ack to the codecs.
        let event = procedure.hf_update(update);
        assert_matches!(event, ProcedureRequest::SendMessage(AtAgMessage::Ok));
    }

    #[test]
    fn supported_features_received_transition_unexpected_update() {
        let state = SlcState {
            hf_features: HfFeatures::CODEC_NEGOTIATION,
            ag_features: AgFeatures::CODEC_NEGOTIATION,
            ..SlcState::default()
        };
        let mut procedure = SlcInitProcedure::new_at_state(AgFeaturesReceived { state });
        let update = AtHfMessage::AgIndSupRequest;
        // Both parties support codec negotiation, but we receive an invalid HF message.
        assert_matches!(procedure.hf_update(update), ProcedureRequest::Error(_));
    }

    #[test]
    fn supported_features_received_transition_with_no_codec_support() {
        // HF doesn't support codec negotiation.
        let state = SlcState {
            hf_features: HfFeatures::NR_EC,
            ag_features: AgFeatures::CODEC_NEGOTIATION,
            ..SlcState::default()
        };
        let mut procedure = SlcInitProcedure::new_at_state(AgFeaturesReceived { state });

        // Since one party doesn't support codec negotiation, we expect the next update to
        // be a request for the AG supported indicators.
        let update = AtHfMessage::AgIndSupRequest;
        assert_matches!(
            procedure.hf_update(update),
            ProcedureRequest::SendMessage(AtAgMessage::AgSupportedIndicators)
        );
    }

    #[test]
    fn supported_features_received_transition_unexpected_update_with_no_codec_support() {
        // AG doesn't support codec negotiation.
        let state = SlcState {
            hf_features: HfFeatures::CODEC_NEGOTIATION,
            ag_features: AgFeatures::NR_EC,
            ..SlcState::default()
        };
        let mut procedure = SlcInitProcedure::new_at_state(AgFeaturesReceived { state });
        let update = AtHfMessage::HfCodecSup(Vec::new());
        // One party doesn't support codec negotiation, so it is an error if the HF sends
        // a codec negotiation AT message.
        assert_matches!(procedure.hf_update(update), ProcedureRequest::Error(_));
    }

    /// Validates the entire mandatory state machine for the SLCI Procedure, see
    /// Section 4.2.1.6 of HFP v1.8 for the mandatory steps. We can trigger the mandatory
    /// sequence of operations by specifying the lack of codec support, 3-way calling, and
    /// HF indicators in either the AG or HF.
    #[test]
    fn validate_mandatory_procedure_state_machine() {
        let mut slc_proc = SlcInitProcedure::new();
        assert_matches!(slc_proc.marker(), ProcedureMarker::SlcInitialization);
        assert!(!slc_proc.is_terminated());

        // Because the HF and AG don't both support the optional feature flags,
        // we don't expect to trigger any of the conditional state transitions in the
        // procedure.
        let hf_features = HfFeatures::CODEC_NEGOTIATION;
        let ag_features = AgFeatures::IN_BAND_RING;

        // First update should be an HF Feature request.
        let update1 = AtHfMessage::HfFeatures(hf_features);
        assert_matches!(slc_proc.hf_update(update1), ProcedureRequest::GetAgFeatures { .. });

        // Next update should be an AG Feature response.
        let update2 = AtAgMessage::AgFeatures(ag_features);
        assert_matches!(slc_proc.ag_update(update2), ProcedureRequest::SendMessage(_));

        // Since the AG doesn't support codec negotiation (see `ag_features`), we expect to
        // skip to the Hf Indicator support stage.
        let update3 = AtHfMessage::AgIndSupRequest;
        assert_matches!(slc_proc.hf_update(update3), ProcedureRequest::SendMessage(_));

        // We then expect the HF to request the indicator status which will result
        // in the procedure asking the AG for the status.
        let update4 = AtHfMessage::AgIndStat;
        assert_matches!(slc_proc.hf_update(update4), ProcedureRequest::GetAgIndicatorStatus { .. });
        let status = IndicatorStatus::default();
        let update5 = AtAgMessage::AgIndStat(status);
        assert_matches!(slc_proc.ag_update(update5), ProcedureRequest::SendMessage(_));

        // Lastly, the HF should request to enable the indicator status update on the AG.
        let update6 = AtHfMessage::HfIndStatusAgEnable;
        assert_matches!(slc_proc.hf_update(update6), ProcedureRequest::SendMessage(_));

        // Since both the AG and HF don't support 3-way calling and HF-indicators flags, we
        // expect the procedure to be terminated.
        assert!(slc_proc.is_terminated());
    }

    /// Validates the entire state machine, including optional states, for the SLCI Procedure.
    /// See HFP v1.8 Section 4.2.1.6 for the complete state diagram.
    #[test]
    fn validate_optional_procedure_state_machine() {
        let mut slc_proc = SlcInitProcedure::new();
        let hf_features = HfFeatures::all();
        let ag_features = AgFeatures::all();

        // First update should be an HF Feature request.
        let update1 = AtHfMessage::HfFeatures(hf_features);
        assert_matches!(slc_proc.hf_update(update1), ProcedureRequest::GetAgFeatures { .. });

        // Next update should be an AG Feature response.
        let update2 = AtAgMessage::AgFeatures(ag_features);
        assert_matches!(slc_proc.ag_update(update2), ProcedureRequest::SendMessage(_));

        let update3 = AtHfMessage::HfCodecSup(vec![]);
        assert_matches!(slc_proc.hf_update(update3), ProcedureRequest::SendMessage(_));

        let update4 = AtHfMessage::AgIndSupRequest;
        assert_matches!(slc_proc.hf_update(update4), ProcedureRequest::SendMessage(_));

        let update5 = AtHfMessage::AgIndStat;
        assert_matches!(slc_proc.hf_update(update5), ProcedureRequest::GetAgIndicatorStatus { .. });

        let status = IndicatorStatus::default();
        let update6 = AtAgMessage::AgIndStat(status);
        assert_matches!(slc_proc.ag_update(update6), ProcedureRequest::SendMessage(_));

        let update7 = AtHfMessage::HfIndStatusAgEnable;
        assert_matches!(slc_proc.hf_update(update7), ProcedureRequest::SendMessage(_));

        // Optional
        let update8 = AtHfMessage::ThreeWaySupport;
        assert_matches!(slc_proc.hf_update(update8), ProcedureRequest::SendMessage(_));
        // Optional
        let update9 = AtHfMessage::HfIndSup(true, true);
        assert_matches!(slc_proc.hf_update(update9), ProcedureRequest::SendMessage(_));
        // Optional
        let update10 = AtHfMessage::HfIndAgSup;
        assert_matches!(slc_proc.hf_update(update10), ProcedureRequest::SendMessage(_));

        assert!(slc_proc.is_terminated());
    }

    #[test]
    fn unexpected_at_event_results_in_error() {
        let mut slc_proc = SlcInitProcedure::new();

        // We don't expect this AT command to be received in the starting
        // state of the SLC Initialization Procedure.
        let unexpected_update1 = AtHfMessage::AgIndStat;
        assert_matches!(slc_proc.hf_update(unexpected_update1), ProcedureRequest::Error(_));

        // Jump to a different state and test an unexpected update.
        let state = SlcState { hf_features: HfFeatures::CODEC_NEGOTIATION, ..SlcState::default() };
        slc_proc = SlcInitProcedure::new_at_state(HfFeaturesReceived { state });
        let unexpected_update2 = AtHfMessage::CurrentCalls;
        assert_matches!(slc_proc.hf_update(unexpected_update2), ProcedureRequest::Error(_));
    }

    /// Validates the result of the is_terminated() check on various input flags.
    // TODO: We should probably do some sort of comprehensive list of all permutations
    // of input flags.
    #[test]
    fn check_is_terminated_on_last_mandatory_step() {
        let mut state = AgIndicatorStatusEnableReceived { state: SlcState::default() };

        // HF and AG both support 3-way calling - shouldn't be done.
        state.state.hf_features.set(HfFeatures::THREE_WAY_CALLING, true);
        state.state.ag_features.set(AgFeatures::THREE_WAY_CALLING, true);
        assert!(!state.is_terminal());

        // HF/AG both support 3-way calling and HF-indicators - shouldn't be done.
        state.state.hf_features.set(HfFeatures::HF_INDICATORS, true);
        state.state.ag_features.set(AgFeatures::HF_INDICATORS, true);
        assert!(!state.is_terminal());

        // HF/AG both support only HF-indicators - shouldn't be done.
        state.state.hf_features.set(HfFeatures::THREE_WAY_CALLING, false);
        state.state.ag_features.set(AgFeatures::THREE_WAY_CALLING, false);
        assert!(!state.is_terminal());

        // HF supports 3-way calling / HF-indicators, but AG doesn't - should be done
        // since AG doesn't support anything.
        state.state = SlcState::default();
        state.state.hf_features.set(HfFeatures::THREE_WAY_CALLING, true);
        state.state.hf_features.set(HfFeatures::HF_INDICATORS, true);
        assert!(state.is_terminal());
    }
}
