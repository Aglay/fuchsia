// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use eapol;
use failure;
use key::exchange::handshake::fourway::{Config, FourwayHandshakeFrame};
use rsna::{SecAssocResult, VerifiedKeyFrame};

#[derive(Debug, PartialEq)]
pub struct Authenticator {
    pub cfg: Config,
    pub s_nonce: [u8; 32],
}

impl Authenticator {
    pub fn new(cfg: Config) -> Result<Authenticator, failure::Error> {
        Ok(Authenticator {
            cfg,
            s_nonce: [0u8; 32],
        })
    }

    pub fn initiate(&self) -> Result<(), failure::Error> {
        // TODO(hahnr): Send first message of handshake.
        Ok(())
    }

    pub fn on_eapol_key_frame(&self, _frame: FourwayHandshakeFrame) -> SecAssocResult {
        // TODO(hahnr): Implement.
        Ok(vec![])
    }

    pub fn snonce(&self) -> &[u8]{
        &self.s_nonce[..]
    }

    pub fn destroy(self) -> Config {
        self.cfg
    }
}
