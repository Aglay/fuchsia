// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{DoneSignaler, TestEvent, TestEventSender},
    anyhow::{bail, Context, Error},
    async_trait::async_trait,
    component_events::injectors::ProtocolInjector,
    fidl_fuchsia_feedback as fcrash,
    futures::{SinkExt, StreamExt},
    log::*,
    std::sync::Arc,
};

const REPORT_PROGRAM_NAME: &str = "triage_detect";

/// FakeCrashReporter can be injected to capture Detect's crash report requests.
pub struct FakeCrashReporter {
    event_sender: TestEventSender,
    done_signaler: DoneSignaler,
}

impl FakeCrashReporter {
    pub fn new(
        event_sender: TestEventSender,
        done_signaler: DoneSignaler,
    ) -> Arc<FakeCrashReporter> {
        Arc::new(FakeCrashReporter { event_sender, done_signaler })
    }
}

fn evaluate_report(report: &fcrash::CrashReport) -> Result<String, Error> {
    let fcrash::CrashReport { program_name, crash_signature, .. } = report;
    if program_name != &Some(REPORT_PROGRAM_NAME.to_string()) {
        bail!(
            "Crash report program name should be {} but it was {:?}",
            REPORT_PROGRAM_NAME,
            program_name
        );
    }
    match crash_signature {
        Some(signature) => return Ok(signature.to_string()),
        None => bail!("Crash report crash signature was None"),
    }
}

#[async_trait]
impl ProtocolInjector for FakeCrashReporter {
    type Marker = fcrash::CrashReporterMarker;

    async fn serve(
        self: Arc<Self>,
        mut request_stream: fcrash::CrashReporterRequestStream,
    ) -> Result<(), Error> {
        let mut event_sender = self.event_sender.clone();
        loop {
            match request_stream.next().await {
                Some(Ok(fcrash::CrashReporterRequest::File { report, responder })) => {
                    match evaluate_report(&report) {
                        Ok(signature) => {
                            info!("Received crash report: {}", signature);
                            event_sender.send(Ok(TestEvent::CrashReport(signature))).await.unwrap();
                            responder
                                .send(&mut Ok(()))
                                .context("failed to send response to client")?;
                        }
                        Err(problem) => {
                            error!("Problem in report: {}", problem);
                            event_sender.send(Err(problem)).await.unwrap();
                            self.done_signaler.signal_done().await;
                        }
                    }
                }
                Some(Err(e)) => {
                    self.done_signaler.signal_done().await;
                    bail!("{}", e);
                }
                None => break,
            }
        }
        Ok(())
    }
}
