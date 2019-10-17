// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_hardware_power as hpower;
use fidl_fuchsia_power as fpower;
use fuchsia_async as fasync;
use fuchsia_syslog::fx_log_info;
use fuchsia_zircon as zx;
use parking_lot::Mutex;
use std::convert::{From, Into};
use std::sync::Arc;

#[derive(Debug, PartialEq)]
struct TimeRemainingWrapper(fpower::TimeRemaining);

impl Clone for TimeRemainingWrapper {
    fn clone(&self) -> TimeRemainingWrapper {
        match self {
            TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(i)) => {
                TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(i.clone()))
            }
            TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(i)) => {
                TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(i.clone()))
            }
            _ => TimeRemainingWrapper(fpower::TimeRemaining::Indeterminate(0)),
        }
    }
}

impl From<TimeRemainingWrapper> for fpower::TimeRemaining {
    fn from(time_remaining: TimeRemainingWrapper) -> Self {
        match time_remaining {
            TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(i)) => {
                fpower::TimeRemaining::FullCharge(i)
            }
            TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(i)) => {
                fpower::TimeRemaining::BatteryLife(i)
            }
            _ => fpower::TimeRemaining::Indeterminate(0),
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
pub struct BatteryInfoWrapper {
    status: fpower::BatteryStatus,
    charge_status: fpower::ChargeStatus,
    charge_source: fpower::ChargeSource,
    level_percent: f32,
    level_status: fpower::LevelStatus,
    health: fpower::HealthStatus,
    time_remaining: TimeRemainingWrapper,
    timestamp: i64,
}

// Default initization for BatteryInfo state prior to obtaining
// actual values from hardware FIDL protocol implementation (driver)
impl Default for BatteryInfoWrapper {
    fn default() -> BatteryInfoWrapper {
        BatteryInfoWrapper {
            status: fpower::BatteryStatus::NotAvailable,
            charge_status: fpower::ChargeStatus::Unknown,
            charge_source: fpower::ChargeSource::Unknown,
            level_percent: 0.0,
            level_status: fpower::LevelStatus::Ok,
            health: fpower::HealthStatus::Unknown,
            time_remaining: TimeRemainingWrapper(fpower::TimeRemaining::Indeterminate(0)),
            timestamp: get_current_time(),
        }
    }
}

impl From<BatteryInfoWrapper> for fpower::BatteryInfo {
    fn from(battery_info: BatteryInfoWrapper) -> Self {
        let res = fpower::BatteryInfo {
            status: Some(battery_info.status),
            charge_status: Some(battery_info.charge_status),
            charge_source: Some(battery_info.charge_source),
            level_percent: Some(battery_info.level_percent),
            level_status: Some(battery_info.level_status),
            health: Some(battery_info.health),
            time_remaining: Some(battery_info.time_remaining.into()),
            timestamp: Some(battery_info.timestamp),
        };

        return res;
    }
}

enum StatusUpdateResult {
    Notify,
    DoNotNotify,
}

// General holder struct used to pass around battery info state
// and the associated watchers
// TODO (DNO-686): refactoring will include fixing this, as it
// does not lend itself to scaling across all the facets of battery
// API currently under design.
pub struct BatteryManager {
    battery_info: BatteryInfoWrapper,
    watchers: Arc<Mutex<Vec<fpower::BatteryInfoWatcherProxy>>>,
}

#[inline]
fn get_current_time() -> i64 {
    let t = zx::Time::get(zx::ClockId::UTC);
    (t.into_nanos() / 1000) as i64
}

impl BatteryManager {
    pub fn new() -> BatteryManager {
        BatteryManager {
            battery_info: BatteryInfoWrapper::default(),
            watchers: Arc::new(Mutex::new(Vec::new())),
        }
    }

    // Adds watcher
    pub fn add_watcher(&mut self, watcher: fpower::BatteryInfoWatcherProxy) {
        self.watchers.lock().push(watcher)
    }

    // Updates the status
    pub fn update_status(
        &mut self,
        power_info: hpower::SourceInfo,
        battery_info: Option<hpower::BatteryInfo>,
    ) -> Result<(), failure::Error> {
        fx_log_info!(
            "update status with power info: {:#?} and battery info: {:#?}",
            &power_info,
            &battery_info
        );

        match self.update_battery_info(power_info, battery_info) {
            Ok(StatusUpdateResult::Notify) => {
                let watchers_clone = self.watchers.clone();
                let info_clone = self.get_battery_info_copy();

                BatteryManager::run_watchers(watchers_clone, info_clone);
            }
            Ok(StatusUpdateResult::DoNotNotify) => {}
            Err(e) => return Err(e),
        }

        Ok(())
    }

    pub fn run_watchers(
        watchers: Arc<Mutex<Vec<fpower::BatteryInfoWatcherProxy>>>,
        info: BatteryInfoWrapper,
    ) {
        fasync::spawn(async move {
            let watchers = watchers.lock().clone();

            for w in &watchers {
                let _ = w.on_change_battery_info(info.clone().into()).await;
            }
            // TODO (DNO-686): refactoring will include fixing this, which
            // is necessary to clean up the watcher list (retain...) in the
            // event that client connections are closed/dropped.
        })
    }

    fn update_battery_info(
        &mut self,
        power_info: hpower::SourceInfo,
        battery_info: Option<hpower::BatteryInfo>,
    ) -> Result<(StatusUpdateResult), failure::Error> {
        let now = get_current_time();
        let old_battery_info = self.get_battery_info_copy();

        // process new battery info if it is available
        if let Some(bi) = battery_info {
            // general battery status
            self.battery_info.status = fpower::BatteryStatus::Ok;

            // charge status
            if power_info.state & hpower::POWER_STATE_CHARGING != 0 {
                self.battery_info.charge_status = fpower::ChargeStatus::Charging;
            } else if power_info.state & hpower::POWER_STATE_DISCHARGING != 0 {
                self.battery_info.charge_status = fpower::ChargeStatus::Discharging;
            } else {
                self.battery_info.charge_status = fpower::ChargeStatus::NotCharging;
            }

            if bi.remaining_capacity == bi.last_full_capacity {
                self.battery_info.charge_status = fpower::ChargeStatus::Full;
            }

            // charge source
            if self.battery_info.charge_status == fpower::ChargeStatus::Charging
                || self.battery_info.charge_status == fpower::ChargeStatus::Full
            {
                if power_info.type_ == hpower::PowerType::Ac {
                    self.battery_info.charge_source = fpower::ChargeSource::AcAdapter;
                } else {
                    //TODO: how to detect USB/Wireless
                    self.battery_info.charge_source = fpower::ChargeSource::Unknown;
                }
            } else {
                self.battery_info.charge_source = fpower::ChargeSource::None;
            }

            // level percent
            self.battery_info.level_percent =
                (bi.remaining_capacity.saturating_mul(100)) as f32 / bi.last_full_capacity as f32;

            // level_status
            if power_info.state & hpower::POWER_STATE_CRITICAL != 0 {
                self.battery_info.level_status = fpower::LevelStatus::Critical;
            } else if bi.remaining_capacity <= bi.capacity_low {
                self.battery_info.level_status = fpower::LevelStatus::Low;
            } else if bi.remaining_capacity <= bi.capacity_warning {
                self.battery_info.level_status = fpower::LevelStatus::Warning;
            } else {
                self.battery_info.level_status = fpower::LevelStatus::Ok;
            }

            // time remaining, provided by hardware as hours
            let nanos_in_one_hour = zx::Duration::from_hours(1);

            if bi.present_rate < 0 {
                // discharging
                let remaining_hours =
                    bi.remaining_capacity as f32 / (bi.present_rate.saturating_mul(-1)) as f32;
                self.battery_info.time_remaining =
                    TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(
                        (remaining_hours as i64).saturating_mul(nanos_in_one_hour.into_nanos()),
                    ));
            } else {
                // charging
                let remaining_hours = (bi.last_full_capacity as f32 - bi.remaining_capacity as f32)
                    / (bi.present_rate) as f32;
                self.battery_info.time_remaining =
                    TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(
                        (remaining_hours as i64).saturating_mul(nanos_in_one_hour.into_nanos()),
                    ));
            }

            // TODO: determine actual battery health
            self.battery_info.health = fpower::HealthStatus::Unknown;
        } else {
            // without battery info, we can still see if battery is online
            // via general power info.
            if power_info.type_ == hpower::PowerType::Battery {
                if power_info.state & hpower::POWER_STATE_ONLINE != 0 {
                    self.battery_info.status = fpower::BatteryStatus::Ok;
                } else {
                    self.battery_info.status = fpower::BatteryStatus::NotAvailable;
                }
            } else {
                self.battery_info.status = fpower::BatteryStatus::Unknown;
            }
            self.battery_info.charge_status = fpower::ChargeStatus::Unknown;
            self.battery_info.health = fpower::HealthStatus::Unknown;
            self.battery_info.time_remaining =
                TimeRemainingWrapper(fpower::TimeRemaining::Indeterminate(0));
        }

        match old_battery_info == self.battery_info {
            true => Ok(StatusUpdateResult::DoNotNotify),
            false => {
                self.battery_info.timestamp = now;
                Ok(StatusUpdateResult::Notify)
            }
        }
    }

    pub fn get_battery_info_copy(&self) -> BatteryInfoWrapper {
        return BatteryInfoWrapper { ..self.battery_info.clone() };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    macro_rules! cmp_fields {
        ($got:ident, $want:ident, [$($field:ident,)*], $test_no:expr) => { $(
            assert_eq!($got.$field, $want.$field, "test no: {}", $test_no);
        )* }
    }

    fn check_status(
        got: &BatteryInfoWrapper,
        want: &BatteryInfoWrapper,
        updated: bool,
        test_no: u32,
    ) {
        if updated {
            cmp_fields!(
                want,
                got,
                [
                    status,
                    charge_status,
                    charge_source,
                    level_percent,
                    level_status,
                    health,
                    time_remaining,
                ],
                test_no
            );
        } else {
            assert_eq!(want, got, "test: {}", test_no);
        }
    }

    fn get_default_battery_info() -> hpower::BatteryInfo {
        let battery_info = hpower::BatteryInfo {
            unit: hpower::BatteryUnit::Ma,
            design_capacity: 5000,
            last_full_capacity: 5000,
            design_voltage: 7000,
            capacity_warning: 700,
            capacity_low: 500,
            capacity_granularity_low_warning: 1,
            capacity_granularity_warning_full: 1,
            present_rate: -500,
            remaining_capacity: 3000,
            present_voltage: 7000,
        };
        return battery_info;
    }

    #[test]
    fn update_battery_info() {
        let nanos_in_one_hour = zx::Duration::from_hours(1);

        let mut battery_manager = BatteryManager::new();
        let mut power_info = hpower::SourceInfo { type_: hpower::PowerType::Ac, state: 1 };
        // state: ac powered, with no battery info to update
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Unknown;
        want.charge_source = fpower::ChargeSource::Unknown;
        let _ = battery_manager.update_battery_info(power_info.clone(), None);
        check_status(&battery_manager.battery_info, &want, true, 1);

        // state: unchanged
        let want = battery_manager.get_battery_info_copy();
        let _ = battery_manager.update_battery_info(power_info.clone(), None);
        check_status(&battery_manager.battery_info, &want, false, 2);

        // state: battery powered, discharging
        power_info.type_ = hpower::PowerType::Battery;
        power_info.state = 0x3; // ONLINE | DISCHARGING
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Discharging;
        want.charge_source = fpower::ChargeSource::None;
        want.level_status = fpower::LevelStatus::Ok;
        want.level_percent = (3000.0 * 100.0) / 5000.0;
        want.time_remaining = TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(
            6 * nanos_in_one_hour.into_nanos(),
        ));
        let mut battery_info = get_default_battery_info();
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 3);

        // state: battery powered, discharging/warning
        power_info.state = 0x3; // ONLINE | DISCHARGING
        battery_info.remaining_capacity = 700;
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Discharging;
        want.charge_source = fpower::ChargeSource::None;
        want.level_status = fpower::LevelStatus::Warning;
        want.level_percent = (700.0 * 100.0) / 5000.0;
        want.time_remaining = TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(
            nanos_in_one_hour.into_nanos(),
        ));
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 4);

        // state: battery powered, discharging/low
        power_info.state = 0x3; // ONLINE | DISCHARGING
        battery_info.remaining_capacity = 500;
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Discharging;
        want.charge_source = fpower::ChargeSource::None;
        want.level_status = fpower::LevelStatus::Low;
        want.level_percent = (500.0 * 100.0) / 5000.0;
        want.time_remaining = TimeRemainingWrapper(fpower::TimeRemaining::BatteryLife(
            nanos_in_one_hour.into_nanos(),
        ));
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 5);

        // state: battery powered, discharging/critical
        power_info.state = 0xB; // ONLINE | DISCHARGING | CRITICAL
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Discharging;
        want.charge_source = fpower::ChargeSource::None;
        want.level_status = fpower::LevelStatus::Critical;
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 6);

        // state: ac powered, charging
        power_info.type_ = hpower::PowerType::Ac;
        power_info.state = 0x5; // ONLINE | CHARGING
        battery_info.present_rate = 1000;
        battery_info.remaining_capacity = 3000;
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Charging;
        want.charge_source = fpower::ChargeSource::AcAdapter;
        want.level_status = fpower::LevelStatus::Ok;
        want.level_percent = (3000.0 * 100.0) / 5000.0;
        want.time_remaining = TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(
            2 * nanos_in_one_hour.into_nanos(),
        ));
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 7);

        // state: ac powered, charging/critical
        power_info.state = 0xD; // ONLINE | CHARGING | CRITICAL
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Charging;
        want.charge_source = fpower::ChargeSource::AcAdapter;
        want.level_status = fpower::LevelStatus::Critical;
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 8);

        // state: ac powered, charging/full
        power_info.state = 0x5; // ONLINE | CHARGING
        battery_info.remaining_capacity = 5000;
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Full;
        want.charge_source = fpower::ChargeSource::AcAdapter;
        want.level_status = fpower::LevelStatus::Ok;
        want.level_percent = 100.0;
        want.time_remaining = TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(0));
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 9);

        // state: ac powered/charging with extreme values (check overflow)
        power_info.state = 0x5; // ONLINE | CHARGING
        battery_info.last_full_capacity = u32::max_value();
        battery_info.remaining_capacity = u32::min_value();
        battery_info.present_rate = 1;
        let mut want = battery_manager.get_battery_info_copy();
        want.status = fpower::BatteryStatus::Ok;
        want.charge_status = fpower::ChargeStatus::Charging;
        want.charge_source = fpower::ChargeSource::AcAdapter;
        want.level_status = fpower::LevelStatus::Low;
        want.level_percent = 0.0;
        want.time_remaining =
            TimeRemainingWrapper(fpower::TimeRemaining::FullCharge(i64::max_value()));
        let _ = battery_manager.update_battery_info(power_info.clone(), Some(battery_info.clone()));
        check_status(&battery_manager.battery_info, &want, true, 10);
    }
}
