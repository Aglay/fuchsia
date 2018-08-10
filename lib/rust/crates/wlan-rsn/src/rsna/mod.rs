// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use akm::Akm;
use bytes::Bytes;
use cipher::Cipher;
use eapol;
use failure;
use Error;
use rsne::Rsne;
use key::exchange::Key;

pub mod esssa;
#[cfg(test)]
pub mod test_util;

#[derive(Debug, Clone, PartialEq)]
pub struct NegotiatedRsne {
    pub group_data: Cipher,
    pub pairwise: Cipher,
    pub akm: Akm,
    pub mic_size: u16,
}

impl NegotiatedRsne {

    pub fn from_rsne(rsne: &Rsne) -> Result<NegotiatedRsne, failure::Error> {
        ensure!(rsne.group_data_cipher_suite.is_some(), Error::InvalidNegotiatedRsne);
        let group_data = rsne.group_data_cipher_suite.as_ref().unwrap();

        ensure!(rsne.pairwise_cipher_suites.len() == 1, Error::InvalidNegotiatedRsne);
        let pairwise = &rsne.pairwise_cipher_suites[0];

        ensure!(rsne.akm_suites.len() == 1, Error::InvalidNegotiatedRsne);
        let akm = &rsne.akm_suites[0];

        let mic_size = akm.mic_bytes();
        ensure!(mic_size.is_some(), Error::InvalidNegotiatedRsne);
        let mic_size = mic_size.unwrap();

        Ok(NegotiatedRsne{
            group_data: group_data.clone(),
            pairwise: pairwise.clone(),
            akm: akm.clone(),
            mic_size,
        })
    }

    pub fn to_full_rsne(&self) -> Rsne {
        let mut s_rsne = Rsne::new();
        s_rsne.group_data_cipher_suite = Some(self.group_data.clone());
        s_rsne.pairwise_cipher_suites = vec![self.pairwise.clone()];
        s_rsne.akm_suites = vec![self.akm.clone()];
        s_rsne
    }
}

// EAPOL Key frames carried in this struct must comply with IEEE Std 802.11-2016, 12.7.2.
// TODO(hahnr): Enforce verification by making this struct only constructable through proper
// validation.
pub struct VerifiedKeyFrame<'a> {
    pub frame: &'a eapol::KeyFrame,
    pub kd_plaintext: Bytes,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Role {
    Authenticator,
    Supplicant,
}

#[derive(Debug, PartialEq)]
pub enum SecAssocStatus {
    // TODO(hahnr): Rather than reporting wrong password as a status, report it as an error.
    WrongPassword,
    EssSaEstablished,
}

#[derive(Debug, PartialEq)]
pub enum SecAssocUpdate {
    TxEapolKeyFrame(eapol::KeyFrame),
    Key(Key),
    Status(SecAssocStatus),
}

pub type SecAssocResult = Result<Vec<SecAssocUpdate>, failure::Error>;

#[cfg(test)]
mod tests {
    use super::*;
    use bytes::Bytes;
    use akm::{self, Akm};
    use cipher::{self, Cipher};
    use rsne::Rsne;
    use suite_selector::OUI;

    #[test]
    fn test_negotiated_rsne_from_rsne() {
        let rsne = make_rsne(Some(cipher::GCMP_256), vec![cipher::CCMP_128], vec![akm::PSK]);
        NegotiatedRsne::from_rsne(&rsne).expect("error, could not create negotiated RSNE");

        let rsne = make_rsne(None, vec![cipher::CCMP_128], vec![akm::PSK]);
        NegotiatedRsne::from_rsne(&rsne).expect_err("error, created negotiated RSNE");

        let rsne = make_rsne(Some(cipher::CCMP_128), vec![], vec![akm::PSK]);
        NegotiatedRsne::from_rsne(&rsne).expect_err("error, created negotiated RSNE");

        let rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::CCMP_128], vec![]);
        NegotiatedRsne::from_rsne(&rsne).expect_err("error, created negotiated RSNE");
    }

    #[test]
    fn test_to_rsne() {
        let rsne = make_rsne(Some(cipher::CCMP_128), vec![cipher::CCMP_128], vec![akm::PSK]);
        let negotiated_rsne = NegotiatedRsne::from_rsne(&rsne)
            .expect("error, could not create negotiated RSNE")
            .to_full_rsne();
       assert_eq!(negotiated_rsne, rsne);
    }

    fn make_cipher(suite_type: u8) -> cipher::Cipher {
        cipher::Cipher { oui: Bytes::from(&OUI[..]), suite_type }
    }

    fn make_akm(suite_type: u8) -> akm::Akm {
        akm::Akm { oui: Bytes::from(&OUI[..]), suite_type }
    }

    fn make_rsne(data: Option<u8>, pairwise: Vec<u8>, akms: Vec<u8>) -> Rsne {
        let mut rsne = Rsne::new();
        rsne.group_data_cipher_suite = data.map(make_cipher);
        rsne.pairwise_cipher_suites = pairwise.into_iter().map(make_cipher).collect();
        rsne.akm_suites = akms.into_iter().map(make_akm).collect();
        rsne
    }

}