// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(dead_code)]
use failure::{self, bail};
use std::fmt;

// IEEE Std 802.11-2016, Annex E
// Note the distinction of index for primary20 and index for center frequency.
// Fuchsia OS minimizes the use of the notion of center frequency,
// with following exceptions:
// - Cbw80P80's secondary frequency segment
// - Frequency conversion at device drivers
pub type MHz = u16;
pub const BASE_FREQ_2GHZ: MHz = 2407;
pub const BASE_FREQ_5GHZ: MHz = 5000;

pub const INVALID_CHAN_IDX: u8 = 0;

/// Channel bandwidth. Cbw80P80 requires the specification of
/// channel index corresponding to the center frequency
/// of the secondary consecutive frequency segment.
#[derive(Clone, Copy, Debug, Ord, PartialOrd, Eq, PartialEq)]
pub enum Cbw {
    Cbw20,
    Cbw40, // Same as Cbw40Above
    Cbw40Below,
    Cbw80,
    Cbw160,
    Cbw80P80 { secondary80: u8 },
}

/// A short list of IEEE WLAN PHY.
#[derive(Clone, Copy, Debug, Ord, PartialOrd, Eq, PartialEq)]
pub enum Phy {
    Hr,  // IEEE 802.11b, used for DSSS, HR/DSSS, ERP-DSSS/CCK
    Erp, // IEEE 802.11a/g, used for ERP-OFDM
    Ht,  // IEEE 802.11n
    Vht, // IEEE 802.11ac
    Hew, // IEEE 802.11ax
}

/// A Channel defines the frequency spectrum to be used for radio synchronization.
/// See for sister definitions in FIDL and C/C++
///  - //garnet/lib/wlan/protocol/wlan/protocol/info.h |struct wlan_channel_t|
///  - //garnet/public/fidl/fuchsia.wlan.mlme/wlan_mlme.fidl |struct WlanChan|
#[derive(Clone, Copy, Debug, Ord, PartialOrd, Eq, PartialEq)]
pub struct Channel {
    // TODO(porce): Augment with country and band
    pub primary: u8,
    pub cbw: Cbw,
}

// Fuchsia's short CBW notation. Not IEEE standard.
impl fmt::Display for Cbw {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Cbw::Cbw20 => write!(f, ""),       // Vanilla plain 20 MHz bandwidth
            Cbw::Cbw40 => write!(f, "+"),      // SCA, often denoted by "+1"
            Cbw::Cbw40Below => write!(f, "-"), // SCB, often denoted by "-1",
            Cbw::Cbw80 => write!(f, "V"),      // VHT 80 MHz (V from VHT)
            Cbw::Cbw160 => write!(f, "W"),     // VHT 160 MHz (as Wide as V + V ;) )
            Cbw::Cbw80P80 { secondary80 } => write!(f, "+{}P", secondary80), // VHT 80Plus80 (not often obvious, but P is the first alphabet)
        }
    }
}

impl fmt::Display for Channel {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}{}", self.primary, self.cbw)
    }
}

// TODO(porce): Convert from FIDL's WlanChan

impl Channel {
    pub fn new(primary: u8, cbw: Cbw) -> Self {
        Channel { primary, cbw }
    }

    // Weak validity test w.r.t the 2 GHz band primary channel only
    fn is_primary_2ghz(&self) -> bool {
        let p = self.primary;
        p >= 1 && p <= 11
    }

    // Weak validity test w.r.t the 5 GHz band primary channel only
    fn is_primary_5ghz(&self) -> bool {
        let p = self.primary;
        match p {
            36..=64 => (p - 36) % 4 == 0,
            100..=144 => (p - 100) % 4 == 0,
            149..=165 => (p - 149) % 4 == 0,
            _ => false,
        }
    }

    // Weak validity test w.r.t the primary channel only in any band.
    fn is_primary_valid(&self) -> bool {
        self.is_primary_2ghz() || self.is_primary_5ghz()
    }

    fn get_band_start_freq(&self) -> Result<MHz, failure::Error> {
        if self.is_primary_2ghz() {
            Ok(BASE_FREQ_2GHZ)
        } else if self.is_primary_5ghz() {
            Ok(BASE_FREQ_5GHZ)
        } else {
            bail!("cannot get band start freq for channel {}", self)
        }
    }

    // Note get_center_chan_idx() is to assist channel validity test.
    // Return of Ok() does not imply the channel under test is valid.
    fn get_center_chan_idx(&self) -> Result<u8, failure::Error> {
        if !self.is_primary_valid() {
            bail!(
                "cannot get center channel index for an invalid primary channel {}",
                self
            );
        }

        let p = self.primary;
        match self.cbw {
            Cbw::Cbw20 => Ok(p),
            Cbw::Cbw40 => Ok(p + 2),
            Cbw::Cbw40Below => Ok(p - 2),
            Cbw::Cbw80 | Cbw::Cbw80P80 { .. } => match p {
                36..=48 => Ok(42),
                52..=64 => Ok(58),
                100..=112 => Ok(106),
                116..=128 => Ok(122),
                132..=144 => Ok(138),
                148..=161_ => Ok(155),
                _ => bail!(
                    "cannot get center channel index for invalid channel {}",
                    self
                ),
            },
            Cbw::Cbw160 => {
                // See IEEE Std 802.11-2016 Table 9-252 and 9-253.
                // Note CBW160 has only one frequency segment, regardless of
                // encodings on CCFS0 and CCFS1 in VHT Operation Information IE.
                match p {
                    36..=64 => Ok(50),
                    100..=128 => Ok(114),
                    _ => bail!(
                        "cannot get center channel index for invalid channel {}",
                        self
                    ),
                }
            }
        }
    }

    /// Returns the center frequency of the first consecutive frequency segment of the channel
    /// in MHz if the channel is valid, Err(String) otherwise.
    pub fn get_center_freq(&self) -> Result<MHz, failure::Error> {
        // IEEE Std 802.11-2016, 21.3.14
        let start_freq = self.get_band_start_freq()?;
        let center_chan_idx = self.get_center_chan_idx()?;
        let spacing: MHz = 5;
        Ok(start_freq + spacing * center_chan_idx as u16)
    }

    /// Returns true if the primary channel index, channel bandwidth, and the secondary consecutive
    /// frequency segment (Cbw80P80 only) are all consistent and meet regulatory requirements of
    /// the USA. TODO(WLAN-870): Other countries.
    pub fn is_valid(&self) -> bool {
        if self.is_primary_2ghz() {
            self.is_valid_2ghz()
        } else if self.is_primary_5ghz() {
            self.is_valid_5ghz()
        } else {
            false
        }
    }

    fn is_valid_2ghz(&self) -> bool {
        if !self.is_primary_2ghz() {
            return false;
        }
        let p = self.primary;
        match self.cbw {
            Cbw::Cbw20 => true,
            Cbw::Cbw40 => p <= 7,
            Cbw::Cbw40Below => p >= 5,
            _ => false,
        }
    }

    fn is_valid_5ghz(&self) -> bool {
        if !self.is_primary_5ghz() {
            return false;
        }
        let p = self.primary;
        match self.cbw {
            Cbw::Cbw20 => true,
            Cbw::Cbw40 => p != 165 && (p % 8) == (if p <= 144 { 4 } else { 5 }),
            Cbw::Cbw40Below => p != 165 && (p % 8) == (if p <= 144 { 0 } else { 1 }),
            Cbw::Cbw80 => p != 165,
            Cbw::Cbw160 => p < 132,
            Cbw::Cbw80P80 { secondary80 } => {
                if p == 165 {
                    return false;
                }
                let valid_secondary80: [u8; 6] = [42, 58, 106, 122, 138, 155];
                if !valid_secondary80.contains(&secondary80) {
                    return false;
                }
                let ccfs0 = match self.get_center_chan_idx() {
                    Ok(v) => v,
                    Err(_) => return false,
                };
                let ccfs1 = secondary80;
                let gap = (ccfs0 as i16 - ccfs1 as i16).abs();
                gap > 16
            }
        }
    }

    pub fn is_2ghz(&self) -> bool {
        self.is_valid_2ghz()
    }

    pub fn is_5ghz(&self) -> bool {
        self.is_valid_5ghz()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fmt_display() {
        let mut c = Channel::new(100, Cbw::Cbw40);
        assert_eq!(format!("{}", c), "100+");
        c.cbw = Cbw::Cbw160;
        assert_eq!(format!("{}", c), "100W");
        c.cbw = Cbw::Cbw80P80 { secondary80: 200 };
        assert_eq!(format!("{}", c), "100+200P");
    }

    #[test]
    fn test_is_primary_valid() {
        // Note Cbw is ignored in this test.
        assert!(Channel::new(1, Cbw::Cbw160).is_primary_valid());
        assert!(!Channel::new(12, Cbw::Cbw160).is_primary_valid());
        assert!(Channel::new(36, Cbw::Cbw160).is_primary_valid());
        assert!(!Channel::new(37, Cbw::Cbw160).is_primary_valid());
        assert!(Channel::new(165, Cbw::Cbw160).is_primary_valid());
        assert!(!Channel::new(166, Cbw::Cbw160).is_primary_valid());

        assert!(Channel::new(1, Cbw::Cbw160).is_primary_2ghz());
        assert!(!Channel::new(1, Cbw::Cbw160).is_primary_5ghz());
        assert!(!Channel::new(36, Cbw::Cbw160).is_primary_2ghz());
        assert!(Channel::new(36, Cbw::Cbw160).is_primary_5ghz());
    }

    #[test]
    fn test_band_start_freq() {
        assert_eq!(
            BASE_FREQ_2GHZ,
            Channel::new(1, Cbw::Cbw20).get_band_start_freq().unwrap()
        );
        assert_eq!(
            BASE_FREQ_5GHZ,
            Channel::new(100, Cbw::Cbw20).get_band_start_freq().unwrap()
        );
        assert!(Channel::new(15, Cbw::Cbw20).get_band_start_freq().is_err());
        assert!(Channel::new(200, Cbw::Cbw20).get_band_start_freq().is_err());
    }

    #[test]
    fn test_get_center_chan_idx() {
        assert!(Channel::new(1, Cbw::Cbw80).get_center_chan_idx().is_err());
        assert_eq!(
            9,
            Channel::new(11, Cbw::Cbw40Below)
                .get_center_chan_idx()
                .unwrap()
        );
        assert_eq!(
            8,
            Channel::new(6, Cbw::Cbw40).get_center_chan_idx().unwrap()
        );
        assert_eq!(
            36,
            Channel::new(36, Cbw::Cbw20).get_center_chan_idx().unwrap()
        );
        assert_eq!(
            38,
            Channel::new(36, Cbw::Cbw40).get_center_chan_idx().unwrap()
        );
        assert_eq!(
            42,
            Channel::new(36, Cbw::Cbw80).get_center_chan_idx().unwrap()
        );
        assert_eq!(
            50,
            Channel::new(36, Cbw::Cbw160).get_center_chan_idx().unwrap()
        );
        assert_eq!(
            42,
            Channel::new(36, Cbw::Cbw80P80 { secondary80: 155 })
                .get_center_chan_idx()
                .unwrap()
        );
    }

    #[test]
    fn test_get_center_freq() {
        assert_eq!(
            2412 as MHz,
            Channel::new(1, Cbw::Cbw20).get_center_freq().unwrap()
        );
        assert_eq!(
            2437 as MHz,
            Channel::new(6, Cbw::Cbw20).get_center_freq().unwrap()
        );
        assert_eq!(
            2447 as MHz,
            Channel::new(6, Cbw::Cbw40).get_center_freq().unwrap()
        );
        assert_eq!(
            2427 as MHz,
            Channel::new(6, Cbw::Cbw40Below).get_center_freq().unwrap()
        );
        assert_eq!(
            5180 as MHz,
            Channel::new(36, Cbw::Cbw20).get_center_freq().unwrap()
        );
        assert_eq!(
            5190 as MHz,
            Channel::new(36, Cbw::Cbw40).get_center_freq().unwrap()
        );
        assert_eq!(
            5210 as MHz,
            Channel::new(36, Cbw::Cbw80).get_center_freq().unwrap()
        );
        assert_eq!(
            5250 as MHz,
            Channel::new(36, Cbw::Cbw160).get_center_freq().unwrap()
        );
        assert_eq!(
            5210 as MHz,
            Channel::new(36, Cbw::Cbw80P80 { secondary80: 155 })
                .get_center_freq()
                .unwrap()
        );
    }

    #[test]
    fn test_valid_combo() {
        assert!(Channel::new(1, Cbw::Cbw20).is_valid());
        assert!(Channel::new(1, Cbw::Cbw40).is_valid());
        assert!(Channel::new(5, Cbw::Cbw40Below).is_valid());
        assert!(Channel::new(6, Cbw::Cbw20).is_valid());
        assert!(Channel::new(6, Cbw::Cbw40).is_valid());
        assert!(Channel::new(6, Cbw::Cbw40Below).is_valid());
        assert!(Channel::new(7, Cbw::Cbw40).is_valid());
        assert!(Channel::new(11, Cbw::Cbw20).is_valid());
        assert!(Channel::new(11, Cbw::Cbw40Below).is_valid());

        assert!(Channel::new(36, Cbw::Cbw20).is_valid());
        assert!(Channel::new(36, Cbw::Cbw40).is_valid());
        assert!(Channel::new(36, Cbw::Cbw160).is_valid());
        assert!(Channel::new(40, Cbw::Cbw20).is_valid());
        assert!(Channel::new(40, Cbw::Cbw40Below).is_valid());
        assert!(Channel::new(40, Cbw::Cbw160).is_valid());
        assert!(Channel::new(36, Cbw::Cbw80P80 { secondary80: 155 }).is_valid());
        assert!(Channel::new(40, Cbw::Cbw80P80 { secondary80: 155 }).is_valid());
        assert!(Channel::new(161, Cbw::Cbw80P80 { secondary80: 42 }).is_valid());
    }

    #[test]
    fn test_invalid_combo() {
        assert!(!Channel::new(1, Cbw::Cbw40Below).is_valid());
        assert!(!Channel::new(4, Cbw::Cbw40Below).is_valid());
        assert!(!Channel::new(8, Cbw::Cbw40).is_valid());
        assert!(!Channel::new(11, Cbw::Cbw40).is_valid());
        assert!(!Channel::new(6, Cbw::Cbw80).is_valid());
        assert!(!Channel::new(6, Cbw::Cbw160).is_valid());
        assert!(!Channel::new(6, Cbw::Cbw80P80 { secondary80: 155 }).is_valid());

        assert!(!Channel::new(36, Cbw::Cbw40Below).is_valid());
        assert!(!Channel::new(36, Cbw::Cbw80P80 { secondary80: 58 }).is_valid());
        assert!(!Channel::new(40, Cbw::Cbw40).is_valid());
        assert!(!Channel::new(40, Cbw::Cbw80P80 { secondary80: 42 }).is_valid());

        assert!(!Channel::new(165, Cbw::Cbw80).is_valid());
        assert!(!Channel::new(165, Cbw::Cbw80P80 { secondary80: 42 }).is_valid());
    }

    #[test]
    fn test_is_2ghz_or_5ghz() {
        assert!(Channel::new(1, Cbw::Cbw20).is_2ghz());
        assert!(!Channel::new(1, Cbw::Cbw20).is_5ghz());
        assert!(Channel::new(36, Cbw::Cbw20).is_5ghz());
        assert!(!Channel::new(36, Cbw::Cbw20).is_2ghz());
    }
}
