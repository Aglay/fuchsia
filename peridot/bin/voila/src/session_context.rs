// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_modular_internal::{SessionContextRequest, SessionContextRequestStream};
use futures::prelude::*;
use log::warn;

/// Service injected by Voila for sessionmgr.
pub struct SessionContext {}

impl SessionContext {
    // Handles a single incoming client request.
    async fn handle_request(&self, request: SessionContextRequest) -> Result<(), (fidl::Error)> {
        match request {
            SessionContextRequest::Logout { control_handle: _ } => {}
            SessionContextRequest::GetPresentation { presentation: _, control_handle: _ } => {}
        }
        Ok(())
    }

    /// Asynchronously handles the supplied stream of requests.
    pub async fn handle_requests_from_stream(
        &self,
        mut stream: SessionContextRequestStream,
    ) -> Result<(), fidl::Error> {
        while let Some(req) = await!(stream.try_next())? {
            await!(self.handle_request(req)).unwrap_or_else(|err| {
                warn!("Error handling SessionContextRequest: {:?}", err);
            });
        }
        Ok(())
    }
}
