// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::error::PowerManagerError;
use crate::log_if_err;
use crate::message::{Message, MessageReturn};
use crate::node::Node;
use crate::thermal_limiter;
use crate::types::{Celsius, Nanoseconds, Seconds, ThermalLoad, Watts};
use crate::utils::{CobaltIntHistogram, CobaltIntHistogramConfig};
use anyhow::{format_err, Error};
use async_trait::async_trait;
use fidl_fuchsia_cobalt::HistogramBucket;
use fuchsia_async as fasync;
use fuchsia_cobalt::{CobaltConnector, CobaltSender, ConnectionType};
use fuchsia_inspect::{self as inspect, HistogramProperty, LinearHistogramParams, Property};
use fuchsia_zircon as zx;
use futures::prelude::*;
use log::*;
use power_manager_metrics::power_manager_metrics as power_metrics_registry;
use power_metrics_registry::ThermalLimitResultMetricDimensionResult as thermal_limit_result;
use serde_derive::Deserialize;
use serde_json as json;
use std::cell::{Cell, RefCell, RefMut};
use std::collections::{HashMap, VecDeque};
use std::rc::Rc;

/// Node: ThermalPolicy
///
/// Summary: Implements the closed loop thermal control policy for the system
///
/// Handles Messages: N/A
///
/// Sends Messages:
///     - ReadTemperature
///     - SetMaxPowerConsumption
///     - SystemShutdown
///     - UpdateThermalLoad
///     - FileCrashReport
///
/// FIDL dependencies: N/A

pub struct ThermalPolicyBuilder<'a> {
    config: ThermalConfig,
    inspect_root: Option<&'a inspect::Node>,
    thermal_metrics: Option<CobaltMetrics>,
}

impl<'a> ThermalPolicyBuilder<'a> {
    pub fn new(config: ThermalConfig) -> Self {
        Self { config, inspect_root: None, thermal_metrics: None }
    }

    pub fn new_from_json(json_data: json::Value, nodes: &HashMap<String, Rc<dyn Node>>) -> Self {
        #[derive(Deserialize)]
        struct ControllerConfig {
            sample_interval: f64,
            filter_time_constant: f64,
            target_temperature: f64,
            e_integral_min: f64,
            e_integral_max: f64,
            sustainable_power: f64,
            proportional_gain: f64,
            integral_gain: f64,
        };

        #[derive(Deserialize)]
        struct NodeConfig {
            thermal_shutdown_temperature: f64,
            controller_params: ControllerConfig,
            throttle_end_delay: f64,
        };

        #[derive(Deserialize)]
        struct Dependencies {
            crash_report_handler_node: String,
            cpu_control_nodes: Vec<String>,
            system_power_handler_node: String,
            temperature_handler_node: String,
            thermal_limiter_node: String,
        }

        #[derive(Deserialize)]
        struct JsonData {
            config: NodeConfig,
            dependencies: Dependencies,
        };

        let data: JsonData = json::from_value(json_data).unwrap();
        let thermal_config = ThermalConfig {
            temperature_node: nodes[&data.dependencies.temperature_handler_node].clone(),
            cpu_control_nodes: data
                .dependencies
                .cpu_control_nodes
                .iter()
                .map(|node| nodes[node].clone())
                .collect(),
            sys_pwr_handler: nodes[&data.dependencies.system_power_handler_node].clone(),
            thermal_limiter_node: nodes[&data.dependencies.thermal_limiter_node].clone(),
            crash_report_handler: nodes[&data.dependencies.crash_report_handler_node].clone(),
            policy_params: ThermalPolicyParams {
                controller_params: ThermalControllerParams {
                    sample_interval: Seconds(data.config.controller_params.sample_interval),
                    filter_time_constant: Seconds(
                        data.config.controller_params.filter_time_constant,
                    ),
                    target_temperature: Celsius(data.config.controller_params.target_temperature),
                    e_integral_min: data.config.controller_params.e_integral_min,
                    e_integral_max: data.config.controller_params.e_integral_max,
                    sustainable_power: Watts(data.config.controller_params.sustainable_power),
                    proportional_gain: data.config.controller_params.proportional_gain,
                    integral_gain: data.config.controller_params.integral_gain,
                },
                thermal_shutdown_temperature: Celsius(data.config.thermal_shutdown_temperature),
                throttle_end_delay: Seconds(data.config.throttle_end_delay),
            },
        };
        Self::new(thermal_config)
    }

    #[cfg(test)]
    fn with_inspect_root(mut self, root: &'a inspect::Node) -> Self {
        self.inspect_root = Some(root);
        self
    }

    #[cfg(test)]
    fn with_thermal_metrics(mut self, thermal_metrics: CobaltMetrics) -> Self {
        self.thermal_metrics = Some(thermal_metrics);
        self
    }

    pub fn build(self) -> Result<Rc<ThermalPolicy>, Error> {
        // Create default values
        let inspect_root = self.inspect_root.unwrap_or(inspect::component::inspector().root());
        let thermal_metrics = self.thermal_metrics.unwrap_or(CobaltMetrics::new());

        let node = Rc::new(ThermalPolicy {
            config: self.config,
            state: ThermalState {
                prev_timestamp: Cell::new(Nanoseconds(0)),
                max_time_delta: Cell::new(Seconds(0.0)),
                prev_temperature: Cell::new(Celsius(0.0)),
                error_integral: Cell::new(0.0),
                state_initialized: Cell::new(false),
                thermal_load: Cell::new(ThermalLoad(0)),
                throttle_end_deadline: Cell::new(None),
            },
            inspect: InspectData::new(inspect_root, "ThermalPolicy".to_string()),
            thermal_metrics: RefCell::new(thermal_metrics),
        });

        node.inspect.set_thermal_config(&node.config);
        node.clone().start_periodic_thermal_loop();
        Ok(node)
    }
}

pub struct ThermalPolicy {
    config: ThermalConfig,
    state: ThermalState,

    /// A struct for managing Component Inspection data.
    inspect: InspectData,

    /// Metrics collection for thermals.
    thermal_metrics: RefCell<CobaltMetrics>,
}

/// A struct to store all configurable aspects of the ThermalPolicy node
pub struct ThermalConfig {
    /// The node to provide temperature readings for the thermal control loop. It is expected that
    /// this node responds to the ReadTemperature message.
    pub temperature_node: Rc<dyn Node>,

    /// The nodes used to impose limits on CPU power state. There will be one node for each CPU
    /// power domain (e.g., big.LITTLE). It is expected that these nodes respond to the
    /// SetMaxPowerConsumption message.
    pub cpu_control_nodes: Vec<Rc<dyn Node>>,

    /// The node to handle system power state changes (e.g., shutdown). It is expected that this
    /// node responds to the SystemShutdown message.
    pub sys_pwr_handler: Rc<dyn Node>,

    /// The node which will impose thermal limits on external clients according to the thermal
    /// load of the system. It is expected that this node responds to the UpdateThermalLoad
    /// message.
    pub thermal_limiter_node: Rc<dyn Node>,

    /// The node used for filing a crash report. It is expected that this node responds to the
    /// FileCrashReport message.
    pub crash_report_handler: Rc<dyn Node>,

    /// All parameter values relating to the thermal policy itself
    pub policy_params: ThermalPolicyParams,
}

/// A struct to store all configurable aspects of the thermal policy itself
pub struct ThermalPolicyParams {
    /// The thermal control loop parameters
    pub controller_params: ThermalControllerParams,

    /// If temperature reaches or exceeds this value, the policy will command a system shutdown
    pub thermal_shutdown_temperature: Celsius,

    /// Time to wait after throttling ends to officially declare a throttle event complete.
    pub throttle_end_delay: Seconds,
}

/// A struct to store the tunable thermal control loop parameters
#[derive(Clone, Debug)]
pub struct ThermalControllerParams {
    /// The interval at which to run the thermal control loop
    pub sample_interval: Seconds,

    /// Time constant for the low-pass filter used for smoothing the temperature input signal
    pub filter_time_constant: Seconds,

    /// Target temperature for the PI control calculation
    pub target_temperature: Celsius,

    /// Minimum integral error [degC * s] for the PI control calculation
    pub e_integral_min: f64,

    /// Maximum integral error [degC * s] for the PI control calculation
    pub e_integral_max: f64,

    /// The available power when there is no temperature error
    pub sustainable_power: Watts,

    /// The proportional gain [W / degC] for the PI control calculation
    pub proportional_gain: f64,

    /// The integral gain [W / (degC * s)] for the PI control calculation
    pub integral_gain: f64,
}

/// State information that is used for calculations across controller iterations
struct ThermalState {
    /// The time of the previous controller iteration
    prev_timestamp: Cell<Nanoseconds>,

    /// The largest observed time between controller iterations (may be used to detect hangs)
    max_time_delta: Cell<Seconds>,

    /// The temperature reading from the previous controller iteration
    prev_temperature: Cell<Celsius>,

    /// The integral error [degC * s] that is accumulated across controller iterations
    error_integral: Cell<f64>,

    /// A flag to know if the rest of ThermalState has not been initialized yet
    state_initialized: Cell<bool>,

    /// A cached value in the range [0 - MAX_THERMAL_LOAD] which is defined as
    /// ((temperature - range_start) / (range_end - range_start) * MAX_THERMAL_LOAD).
    thermal_load: Cell<ThermalLoad>,

    /// After we exit throttling, if `throttle_end_delay` is nonzero then this value will
    /// indicate the time that we may officially consider a throttle event complete.
    throttle_end_deadline: Cell<Option<Nanoseconds>>,
}

impl ThermalPolicy {
    /// Starts a periodic timer that fires at the interval specified by
    /// ThermalControllerParams.sample_interval. At each timer, `iterate_thermal_control` is called
    /// and any resulting errors are logged.
    fn start_periodic_thermal_loop(self: Rc<Self>) {
        let mut periodic_timer = fasync::Interval::new(zx::Duration::from_nanos(
            self.config.policy_params.controller_params.sample_interval.into_nanos(),
        ));

        fasync::spawn_local(async move {
            while let Some(()) = periodic_timer.next().await {
                fuchsia_trace::instant!(
                    "power_manager",
                    "ThermalPolicy::periodic_timer_fired",
                    fuchsia_trace::Scope::Thread
                );
                let result = self.iterate_thermal_control().await;
                log_if_err!(result, "Error while running thermal control iteration");
                fuchsia_trace::instant!(
                    "power_manager",
                    "ThermalPolicy::iterate_thermal_control_result",
                    fuchsia_trace::Scope::Thread,
                    "result" => format!("{:?}", result).as_str()
                );
            }
        });
    }

    /// This is the main body of the closed loop thermal control logic. The function is called
    /// periodically by the timer started in `start_periodic_thermal_loop`. For each iteration, the
    /// following steps will be taken:
    ///     1. Read the current temperature from the temperature driver specified in ThermalConfig
    ///     2. Filter the raw temperature value using a low-pass filter
    ///     3. Use the new filtered temperature to calculate the proportional error and integral
    ///        error of temperature relative to the configured target temperature
    ///     4. Use the proportional error and integral error to derive thermal load and available
    ///        power values
    ///     5. Update the relevant nodes with the new thermal load and available power information
    pub async fn iterate_thermal_control(&self) -> Result<(), Error> {
        fuchsia_trace::duration!("power_manager", "ThermalPolicy::iterate_thermal_control");

        let raw_temperature = self.get_temperature().await?;
        let timestamp = Nanoseconds(fasync::Time::now().into_nanos());

        // We should have run the iteration at least once before proceeding
        if !self.state.state_initialized.get() {
            self.state.prev_temperature.set(raw_temperature);
            self.state.prev_timestamp.set(timestamp);
            self.state.state_initialized.set(true);
            self.inspect.state_initialized.set(1);
            return Ok(());
        }

        let time_delta = self.get_time_delta(timestamp);
        let filtered_temperature = self.get_filtered_temperature(raw_temperature, time_delta);
        let (error_proportional, error_integral) =
            self.get_temperature_error(filtered_temperature, time_delta);
        let thermal_load = Self::calculate_thermal_load(
            error_integral,
            self.config.policy_params.controller_params.e_integral_min,
            self.config.policy_params.controller_params.e_integral_max,
        );

        self.log_thermal_iteration_metrics(
            timestamp,
            time_delta,
            raw_temperature,
            filtered_temperature,
            error_integral,
            thermal_load,
        );

        // If the new temperature is above the critical threshold then shut down the system
        let result = self.check_critical_temperature(timestamp, filtered_temperature).await;
        log_if_err!(result, "Error checking critical temperature");
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::check_critical_temperature_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        // Update the ThermalLimiter node with the latest thermal load
        let result = self.update_thermal_load(timestamp, thermal_load).await;
        log_if_err!(result, "Error updating thermal load");
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::update_thermal_load_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        // Update the power allocation according to the new temperature error readings
        let result = self.update_power_allocation(error_proportional, error_integral).await;
        log_if_err!(result, "Error running thermal feedback controller");
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::update_power_allocation_result",
            fuchsia_trace::Scope::Thread,
            "result" => format!("{:?}", result).as_str()
        );

        Ok(())
    }

    /// Queries the current temperature from the temperature handler node
    async fn get_temperature(&self) -> Result<Celsius, Error> {
        fuchsia_trace::duration!("power_manager", "ThermalPolicy::get_temperature");
        match self.send_message(&self.config.temperature_node, &Message::ReadTemperature).await {
            Ok(MessageReturn::ReadTemperature(t)) => Ok(t),
            Ok(r) => Err(format_err!("ReadTemperature had unexpected return value: {:?}", r)),
            Err(e) => Err(format_err!("ReadTemperature failed: {:?}", e)),
        }
    }

    /// Gets the time delta from the previous call to this function using the provided timestamp.
    /// Logs the largest delta into Inspect.
    fn get_time_delta(&self, timestamp: Nanoseconds) -> Seconds {
        let time_delta = Seconds::from_nanos(timestamp.0 - self.state.prev_timestamp.get().0);
        if time_delta.0 > self.state.max_time_delta.get().0 {
            self.state.max_time_delta.set(time_delta);
            self.inspect.max_time_delta.set(time_delta.0);
        }
        self.state.prev_timestamp.set(timestamp);
        time_delta
    }

    /// Calculates a filtered temperature using the provided input temperature and time delta, and
    /// using the previous temperature from the recorded state. Updates the previous temperature
    /// state with the new input temperature.
    fn get_filtered_temperature(&self, raw_temperature: Celsius, time_delta: Seconds) -> Celsius {
        let filtered_temperature = Celsius(low_pass_filter(
            raw_temperature.0,
            self.state.prev_temperature.get().0,
            time_delta.0,
            self.config.policy_params.controller_params.filter_time_constant.0,
        ));
        self.state.prev_temperature.set(filtered_temperature);
        filtered_temperature
    }

    /// Calculates proportional error and integral error of temperature using the provided input
    /// temperature and time delta. Stores the new integral error and logs it to Inspect.
    fn get_temperature_error(&self, temperature: Celsius, time_delta: Seconds) -> (f64, f64) {
        let controller_params = &self.config.policy_params.controller_params;
        let error_proportional = controller_params.target_temperature.0 - temperature.0;
        let error_integral = num_traits::clamp(
            self.state.error_integral.get() + error_proportional * time_delta.0,
            controller_params.e_integral_min,
            controller_params.e_integral_max,
        );
        self.state.error_integral.set(error_integral);
        self.inspect.error_integral.set(error_integral);
        (error_proportional, error_integral)
    }

    /// Logs various state data that is updated on each iteration of the thermal policy.
    fn log_thermal_iteration_metrics(
        &self,
        timestamp: Nanoseconds,
        time_delta: Seconds,
        raw_temperature: Celsius,
        filtered_temperature: Celsius,
        temperature_error_integral: f64,
        thermal_load: ThermalLoad,
    ) {
        self.thermal_metrics.borrow_mut().log_raw_temperature(raw_temperature);
        self.inspect.timestamp.set(timestamp.0);
        self.inspect.time_delta.set(time_delta.0);
        self.inspect.temperature_raw.set(raw_temperature.0);
        self.inspect.temperature_filtered.set(filtered_temperature.0);
        fuchsia_trace::instant!(
            "power_manager",
            "ThermalPolicy::thermal_control_iteration_data",
            fuchsia_trace::Scope::Thread,
            "timestamp" => timestamp.0
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy raw_temperature",
            0,
            "raw_temperature" => raw_temperature.0
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy filtered_temperature",
            0,
            "filtered_temperature" => filtered_temperature.0
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy error_integral", 0,
            "error_integral" => temperature_error_integral
        );
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy thermal_load",
            0,
            "thermal_load" => thermal_load.0
        );
    }

    /// Compares the supplied temperature with the thermal config thermal shutdown temperature. If
    /// we've reached or exceeded the shutdown temperature, message the system power handler node
    /// to initiate a system shutdown.
    async fn check_critical_temperature(
        &self,
        timestamp: Nanoseconds,
        temperature: Celsius,
    ) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::check_critical_temperature",
            "temperature" => temperature.0
        );

        // Temperature has exceeded the thermal shutdown temperature
        if temperature.0 >= self.config.policy_params.thermal_shutdown_temperature.0 {
            fuchsia_trace::instant!(
                "power_manager",
                "ThermalPolicy::thermal_shutdown_reached",
                fuchsia_trace::Scope::Thread,
                "temperature" => temperature.0,
                "shutdown_temperature" => self.config.policy_params.thermal_shutdown_temperature.0
            );

            self.thermal_metrics.borrow_mut().log_throttle_end_shutdown(timestamp);
            self.inspect.throttle_history().mark_throttling_inactive(timestamp);

            // TODO(pshickel): We shouldn't ever get an error here. But we should probably have
            // some type of fallback or secondary mechanism of halting the system if it somehow
            // does happen. This could have physical safety implications.
            self.send_message(
                &self.config.sys_pwr_handler,
                &Message::SystemShutdown(
                    format!(
                        "Exceeded thermal limit ({}C > {}C)",
                        temperature.0, self.config.policy_params.thermal_shutdown_temperature.0
                    )
                    .to_string(),
                ),
            )
            .await
            .map_err(|e| format_err!("Failed to shut down the system: {}", e))?;
        }

        Ok(())
    }

    /// Process a new thermal load value. If there is a change from the cached thermal_load, then
    /// the new value is sent out to the ThermalLimiter node.
    async fn update_thermal_load(
        &self,
        timestamp: Nanoseconds,
        thermal_load: ThermalLoad,
    ) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::update_thermal_load",
            "timestamp" => timestamp.0,
            "thermal_load" => thermal_load.0
        );

        let mut return_val = Ok(());

        if thermal_load != self.state.thermal_load.get() {
            fuchsia_trace::instant!(
                "power_manager",
                "ThermalPolicy::thermal_load_changed",
                fuchsia_trace::Scope::Thread,
                "old_load" => self.state.thermal_load.get().0,
                "new_load" => thermal_load.0
            );

            if self.state.thermal_load.get().0 == 0 {
                // We've just entered thermal limiting
                info!("Begin thermal mitigation");
                self.thermal_metrics.borrow_mut().log_throttle_start(timestamp);
                self.inspect.throttle_history().mark_throttling_active(timestamp);
                self.state.throttle_end_deadline.set(None);
            } else if thermal_load.0 == 0 {
                // We've just exited thermal limiting. Set the deadline time that we may consider
                // the throttle event officially complete.
                info!("End thermal mitigation");
                let throttle_end_deadline = timestamp
                    + Nanoseconds(self.config.policy_params.throttle_end_delay.into_nanos());
                self.state.throttle_end_deadline.set(Some(throttle_end_deadline));
            }

            self.state.thermal_load.set(thermal_load);

            // Record any errors here, but don't return early from the function in case we have a
            // pending throttle_end_deadline to deal with
            return_val = match self
                .send_message(
                    &self.config.thermal_limiter_node,
                    &Message::UpdateThermalLoad(thermal_load),
                )
                .await
            {
                Ok(_) => Ok(()),
                Err(e) => Err(Error::from(e)),
            };
        }

        // If a throttle end deadline time was set and the deadline has been passed, then declare
        // the throttle event complete and clear the deadline
        if self.state.throttle_end_deadline.get().is_some()
            && timestamp >= self.state.throttle_end_deadline.get().unwrap()
        {
            self.state.throttle_end_deadline.set(None);
            self.thermal_metrics.borrow_mut().log_throttle_end_mitigated(timestamp);
            self.inspect.throttle_history().mark_throttling_inactive(timestamp);
            self.file_thermal_crash_report().await;
        }

        self.inspect.throttle_history().record_thermal_load(thermal_load);

        return_val
    }

    /// Calculates the thermal load which is a value in the range [0 - MAX_THERMAL_LOAD] defined as
    /// ((error_integral - range_start) / (range_end - range_start) * MAX_THERMAL_LOAD), where the
    /// range is defined by the maximum and minimum integral error according to the controller
    /// parameters.
    fn calculate_thermal_load(
        error_integral: f64,
        error_integral_min: f64,
        error_integral_max: f64,
    ) -> ThermalLoad {
        debug_assert!(
            error_integral >= error_integral_min,
            format!(
                "error_integral ({}) less than error_integral_min ({})",
                error_integral, error_integral_min
            )
        );
        debug_assert!(
            error_integral <= error_integral_max,
            format!(
                "error_integral ({}) greater than error_integral_max ({})",
                error_integral, error_integral_max
            )
        );

        if error_integral < error_integral_min {
            error!(
                "error_integral {} less than error_integral_min {}",
                error_integral, error_integral_min
            );
            thermal_limiter::MAX_THERMAL_LOAD
        } else if error_integral > error_integral_max {
            error!(
                "error_integral {} greater than error_integral_max {}",
                error_integral, error_integral_max
            );
            ThermalLoad(0)
        } else {
            ThermalLoad(
                ((error_integral - error_integral_max) / (error_integral_min - error_integral_max)
                    * thermal_limiter::MAX_THERMAL_LOAD.0 as f64) as u32,
            )
        }
    }

    /// Updates the power allocation according to the provided proportional error and integral error
    /// of temperature.
    async fn update_power_allocation(
        &self,
        error_proportional: f64,
        error_integral: f64,
    ) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::update_power_allocation",
            "error_proportional" => error_proportional,
            "error_integral" => error_integral
        );
        let available_power = self.calculate_available_power(error_proportional, error_integral);
        self.inspect.throttle_history().record_available_power(available_power);
        fuchsia_trace::counter!(
            "power_manager",
            "ThermalPolicy available_power",
            0,
            "available_power" => available_power.0
        );

        self.distribute_power(available_power).await
    }

    /// A PI control algorithm that uses temperature as the measured process variable, and
    /// available power as the control variable.
    fn calculate_available_power(&self, error_proportional: f64, error_integral: f64) -> Watts {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::calculate_available_power",
            "error_proportional" => error_proportional,
            "error_integral" => error_integral
        );

        let controller_params = &self.config.policy_params.controller_params;
        let p_term = error_proportional * controller_params.proportional_gain;
        let i_term = error_integral * controller_params.integral_gain;
        let power_available =
            f64::max(0.0, controller_params.sustainable_power.0 + p_term + i_term);

        Watts(power_available)
    }

    /// This function is responsible for distributing the available power (as determined by the
    /// prior PI calculation) to the various power actors that are included in this closed loop
    /// system. Initially, CPU is the only power actor. In later versions of the thermal policy,
    /// there may be more power actors with associated "weights" for distributing power amongst
    /// them.
    async fn distribute_power(&self, mut total_available_power: Watts) -> Result<(), Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "ThermalPolicy::distribute_power",
            "total_available_power" => total_available_power.0
        );

        // The power distribution currently works by allocating the total available power to the
        // first CPU control node in `cpu_control_nodes`. The node replies to the
        // SetMaxPowerConsumption message with the amount of power it was able to utilize. This
        // utilized amount is subtracted from the total available power, then the remaining power is
        // allocated to the remaining CPU control nodes in the same way.

        // TODO(fxb/48205): We may want to revisit this distribution algorithm to avoid potentially
        // starving some CPU control nodes. We'll want to have some discussions and learn more about
        // intended big.LITTLE scheduling and operation to better inform our decisions here. We may
        // find that we'll need to first query the nodes to learn how much power they intend to use
        // before making allocation decisions.
        for (i, node) in self.config.cpu_control_nodes.iter().enumerate() {
            if let MessageReturn::SetMaxPowerConsumption(power_used) = self
                .send_message(&node, &Message::SetMaxPowerConsumption(total_available_power))
                .await?
            {
                self.inspect
                    .throttle_history
                    .borrow_mut()
                    .record_cpu_power_consumption(i, power_used);
                total_available_power = total_available_power - power_used;
            }
        }

        Ok(())
    }

    /// File a crash report with the signature "fuchsia-thermal-throttle".
    async fn file_thermal_crash_report(&self) {
        log_if_err!(
            self.send_message(
                &self.config.crash_report_handler,
                &Message::FileCrashReport("fuchsia-thermal-throttle".to_string()),
            )
            .await,
            "Failed to file crash report"
        );
    }
}

fn low_pass_filter(y: f64, y_prev: f64, time_delta: f64, time_constant: f64) -> f64 {
    y_prev + (time_delta / time_constant) * (y - y_prev)
}

#[async_trait(?Send)]
impl Node for ThermalPolicy {
    fn name(&self) -> &'static str {
        "ThermalPolicy"
    }

    async fn handle_message(&self, msg: &Message) -> Result<MessageReturn, PowerManagerError> {
        match msg {
            _ => Err(PowerManagerError::Unsupported),
        }
    }
}

struct InspectData {
    // Nodes
    root_node: inspect::Node,

    // Properties
    timestamp: inspect::IntProperty,
    time_delta: inspect::DoubleProperty,
    temperature_raw: inspect::DoubleProperty,
    temperature_filtered: inspect::DoubleProperty,
    error_integral: inspect::DoubleProperty,
    state_initialized: inspect::UintProperty,
    max_time_delta: inspect::DoubleProperty,
    throttle_history: RefCell<InspectThrottleHistory>,
}

impl InspectData {
    /// Rolling number of throttle events to store in `throttle_history`.
    const NUM_THROTTLE_EVENTS: usize = 10;

    fn new(parent: &inspect::Node, name: String) -> Self {
        // Create a local root node and properties
        let root_node = parent.create_child(name);
        let state_node = root_node.create_child("state");
        let stats_node = root_node.create_child("stats");
        let timestamp = state_node.create_int("timestamp (ns)", 0);
        let time_delta = state_node.create_double("time_delta (s)", 0.0);
        let temperature_raw = state_node.create_double("temperature_raw (C)", 0.0);
        let temperature_filtered = state_node.create_double("temperature_filtered (C)", 0.0);
        let error_integral = state_node.create_double("error_integral", 0.0);
        let state_initialized = state_node.create_uint("state_initialized", 0);
        let max_time_delta = stats_node.create_double("max_time_delta (s)", 0.0);
        let throttle_history = RefCell::new(InspectThrottleHistory::new(
            root_node.create_child("throttle_history"),
            Self::NUM_THROTTLE_EVENTS,
        ));

        // Pass ownership of the new nodes to the root node, otherwise they'll be dropped
        root_node.record(state_node);
        root_node.record(stats_node);

        InspectData {
            root_node,
            timestamp,
            time_delta,
            max_time_delta,
            temperature_raw,
            temperature_filtered,
            error_integral,
            state_initialized,
            throttle_history,
        }
    }

    fn set_thermal_config(&self, config: &ThermalConfig) {
        let policy_params_node = self.root_node.create_child("policy_params");
        let ctrl_params_node = policy_params_node.create_child("controller_params");
        let params = &config.policy_params.controller_params;
        ctrl_params_node.record_double("sample_interval (s)", params.sample_interval.0);
        ctrl_params_node.record_double("filter_time_constant (s)", params.filter_time_constant.0);
        ctrl_params_node.record_double("target_temperature (C)", params.target_temperature.0);
        ctrl_params_node.record_double("e_integral_min", params.e_integral_min);
        ctrl_params_node.record_double("e_integral_max", params.e_integral_max);
        ctrl_params_node.record_double("sustainable_power (W)", params.sustainable_power.0);
        ctrl_params_node.record_double("proportional_gain", params.proportional_gain);
        ctrl_params_node.record_double("integral_gain", params.integral_gain);
        policy_params_node.record(ctrl_params_node);

        self.root_node.record(policy_params_node);
    }

    /// A convenient wrapper to mutably borrow `throttle_history`.
    fn throttle_history(&self) -> RefMut<'_, InspectThrottleHistory> {
        self.throttle_history.borrow_mut()
    }
}

/// Captures and retains data from previous throttling events in a rolling buffer.
struct InspectThrottleHistory {
    /// The Inspect node that will be used as the parent for throttle event child nodes.
    root_node: inspect::Node,

    /// A running count of the number of throttle events ever captured in `throttle_history_list`.
    /// The count is always increasing, even when older throttle events are removed from the list.
    entry_count: usize,

    /// The maximum number of throttling events to keep in `throttle_history_list`.
    capacity: usize,

    /// State to track if throttling is currently active (used to ignore readings when throttling
    /// isn't active).
    throttling_active: bool,

    /// List to store the throttle entries.
    throttle_history_list: VecDeque<InspectThrottleHistoryEntry>,
}

impl InspectThrottleHistory {
    fn new(root_node: inspect::Node, capacity: usize) -> Self {
        Self {
            entry_count: 0,
            capacity,
            throttling_active: false,
            throttle_history_list: VecDeque::with_capacity(capacity),
            root_node,
        }
    }

    /// Mark the start of throttling.
    fn mark_throttling_active(&mut self, timestamp: Nanoseconds) {
        // Must have ended previous throttling
        assert_eq!(self.throttling_active, false);

        // Begin a new throttling entry
        self.new_entry();

        self.throttling_active = true;
        self.throttle_history_list.back().unwrap().throttle_start_time.set(timestamp.0);
    }

    /// Mark the end of throttling.
    fn mark_throttling_inactive(&mut self, timestamp: Nanoseconds) {
        if self.throttling_active {
            self.throttle_history_list.back().unwrap().throttle_end_time.set(timestamp.0);
            self.throttling_active = false
        }
    }

    /// Begin a new throttling entry. Removes the oldest entry once we've reached
    /// InspectData::NUM_THROTTLE_EVENTS number of entries.
    fn new_entry(&mut self) {
        if self.throttle_history_list.len() >= self.capacity {
            self.throttle_history_list.pop_front();
        }

        let node = self.root_node.create_child(&self.entry_count.to_string());
        let entry = InspectThrottleHistoryEntry::new(node);
        self.throttle_history_list.push_back(entry);
        self.entry_count += 1;
    }

    /// Record the current thermal load. No-op unless throttling has been set active.
    fn record_thermal_load(&self, thermal_load: ThermalLoad) {
        if self.throttling_active {
            self.throttle_history_list
                .back()
                .unwrap()
                .thermal_load_hist
                .insert(thermal_load.0.into());
        }
    }

    /// Record the current available power. No-op unless throttling has been set active.
    fn record_available_power(&self, available_power: Watts) {
        if self.throttling_active {
            self.throttle_history_list
                .back()
                .unwrap()
                .available_power_hist
                .insert(available_power.0);
        }
    }

    /// Record the current CPU power consumption for a given CPU index. No-op unless throttling has
    /// been set active.
    fn record_cpu_power_consumption(&mut self, cpu_index: usize, power_used: Watts) {
        if self.throttling_active {
            self.throttle_history_list
                .back_mut()
                .unwrap()
                .get_cpu_power_usage_property(cpu_index)
                .insert(power_used.0);
        }
    }
}

/// Stores data for a single throttle event.
struct InspectThrottleHistoryEntry {
    _node: inspect::Node,
    throttle_start_time: inspect::IntProperty,
    throttle_end_time: inspect::IntProperty,
    thermal_load_hist: inspect::UintLinearHistogramProperty,
    available_power_hist: inspect::DoubleLinearHistogramProperty,
    cpu_power_usage_node: inspect::Node,
    cpu_power_usage: Vec<inspect::DoubleLinearHistogramProperty>,
}

impl InspectThrottleHistoryEntry {
    /// Creates a new InspectThrottleHistoryEntry which creates new properties under `node`.
    fn new(node: inspect::Node) -> Self {
        Self {
            throttle_start_time: node.create_int("throttle_start_time", 0),
            throttle_end_time: node.create_int("throttle_end_time", 0),
            thermal_load_hist: node.create_uint_linear_histogram(
                "thermal_load_hist",
                LinearHistogramParams { floor: 0, step_size: 1, buckets: 100 },
            ),
            available_power_hist: node.create_double_linear_histogram(
                "available_power_hist",
                LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            ),
            cpu_power_usage_node: node.create_child("cpu_power_usage"),
            cpu_power_usage: Vec::new(),
            _node: node,
        }
    }

    /// Gets the property to record CPU power usage for the given CPU index. These properties are
    /// created dynamically because CPU domain count is not a fixed number.
    fn get_cpu_power_usage_property(
        &mut self,
        index: usize,
    ) -> &inspect::DoubleLinearHistogramProperty {
        if self.cpu_power_usage.get(index).is_none() {
            self.cpu_power_usage.push(self.cpu_power_usage_node.create_double_linear_histogram(
                index.to_string(),
                LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            ))
        }
        &self.cpu_power_usage[index]
    }
}

/// Stores and dispatches Cobalt metric data.
struct CobaltMetrics {
    /// Sends Cobalt events to the Cobalt FIDL service.
    cobalt_sender: CobaltSender,

    /// Timestamp of the start of a throttling duration.
    throttle_start_time: Option<Nanoseconds>,

    /// Histogram of raw temperature readings.
    temperature_histogram: CobaltIntHistogram,
}

impl CobaltMetrics {
    /// Number of temperature readings before dispatching a Cobalt event.
    const NUM_TEMPERATURE_READINGS: u32 = 100;

    fn new() -> Self {
        let (cobalt_sender, sender_future) = CobaltConnector::default()
            .serve(ConnectionType::project_id(power_metrics_registry::PROJECT_ID));

        // Spawn the future that handles sending data to Cobalt
        fasync::spawn_local(sender_future);

        Self::new_with_cobalt_sender(cobalt_sender)
    }

    /// Creates a new CobaltMetrics without connecting to the Cobalt FIDL service. Returns a channel
    /// to receive the Cobalt events that would have otherwise been delivered to the Cobalt service.
    fn new_with_cobalt_sender(cobalt_sender: CobaltSender) -> Self {
        Self {
            cobalt_sender,
            throttle_start_time: None,
            temperature_histogram: CobaltIntHistogram::new(CobaltIntHistogramConfig {
                floor: power_metrics_registry::RAW_TEMPERATURE_INT_BUCKETS_FLOOR,
                num_buckets: power_metrics_registry::RAW_TEMPERATURE_INT_BUCKETS_NUM_BUCKETS,
                step_size: power_metrics_registry::RAW_TEMPERATURE_INT_BUCKETS_STEP_SIZE,
            }),
        }
    }

    /// Log the start of a thermal throttling duration. Must call `throttle_end` before calling this
    /// function a second time.
    fn log_throttle_start(&mut self, timestamp: Nanoseconds) {
        assert!(
            self.throttle_start_time.is_none(),
            "throttle_start called before ending previous throttle"
        );

        self.throttle_start_time = Some(timestamp);
    }

    /// Log the end of a thermal throttling duration due to successful mitigation.
    fn log_throttle_end_mitigated(&mut self, timestamp: Nanoseconds) {
        self.log_throttle_end_with_result(thermal_limit_result::Mitigated, timestamp)
    }

    /// Log the end of a thermal throttling duration due to a shutdown.
    fn log_throttle_end_shutdown(&mut self, timestamp: Nanoseconds) {
        self.log_throttle_end_with_result(thermal_limit_result::Shutdown, timestamp)
    }

    /// Log the end of a thermal throttling duration with a specified reason.
    fn log_throttle_end_with_result(
        &mut self,
        result: thermal_limit_result,
        timestamp: Nanoseconds,
    ) {
        if self.throttle_start_time.is_some() {
            let elapsed_time = timestamp - self.throttle_start_time.unwrap();
            self.throttle_start_time = None;
            self.dispatch_limiting_elapsed_time_metric(elapsed_time);
        }

        self.dispatch_limit_result_metric(result);
    }

    /// Log a raw temperature reading.
    fn log_raw_temperature(&mut self, temperature: Celsius) {
        self.temperature_histogram.add_data(temperature.0 as i64);
        if self.temperature_histogram.count() == Self::NUM_TEMPERATURE_READINGS {
            self.dispatch_temperature_metric(self.temperature_histogram.get_data());
            self.temperature_histogram.clear();
        }
    }

    /// Dispatch a Cobalt event for the thermal_limiting_elapsed_time metric.
    fn dispatch_limiting_elapsed_time_metric(&mut self, elapsed_time: Nanoseconds) {
        self.cobalt_sender.log_elapsed_time(
            power_metrics_registry::THERMAL_LIMITING_ELAPSED_TIME_METRIC_ID,
            (),
            elapsed_time.0,
        );
    }

    /// Dispatch a Cobalt event for the thermal_limit_result metric.
    fn dispatch_limit_result_metric(&mut self, result: thermal_limit_result) {
        self.cobalt_sender
            .log_event(power_metrics_registry::THERMAL_LIMIT_RESULT_METRIC_ID, vec![result as u32]);
    }

    /// Dispatch a Cobalt event for the raw_temperature metric.
    fn dispatch_temperature_metric(&mut self, histogram: Vec<HistogramBucket>) {
        self.cobalt_sender.log_int_histogram(
            power_metrics_registry::RAW_TEMPERATURE_METRIC_ID,
            (),
            histogram,
        );
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::test::mock_node::{create_dummy_node, create_mock_node, MessageMatcher};
    use crate::{msg_eq, msg_ok_return};
    use fidl_fuchsia_cobalt::{CobaltEvent, Event, EventPayload};
    use inspect::assert_inspect_tree;

    pub fn get_sample_interval(thermal_policy: &ThermalPolicy) -> Seconds {
        thermal_policy.config.policy_params.controller_params.sample_interval
    }

    fn default_policy_params() -> ThermalPolicyParams {
        ThermalPolicyParams {
            controller_params: ThermalControllerParams {
                sample_interval: Seconds(1.0),
                filter_time_constant: Seconds(10.0),
                target_temperature: Celsius(85.0),
                e_integral_min: -20.0,
                e_integral_max: 0.0,
                sustainable_power: Watts(1.1),
                proportional_gain: 0.0,
                integral_gain: 0.2,
            },
            thermal_shutdown_temperature: Celsius(95.0),
            throttle_end_delay: Seconds(0.0),
        }
    }

    /// Tests the low_pass_filter function for correctness.
    #[test]
    fn test_low_pass_filter() {
        let y_0 = 0.0;
        let y_1 = 10.0;
        let time_delta = 1.0;
        let time_constant = 10.0;
        assert_eq!(low_pass_filter(y_1, y_0, time_delta, time_constant), 1.0);
    }

    /// Tests the calculate_thermal_load function for correctness.
    #[test]
    fn test_calculate_thermal_load() {
        // These tests using invalid error integral values will panic on debug builds and the
        // `test_calculate_thermal_load_error_integral_*` tests will verify that. For now (non-debug
        // build), just test that the invalid values are clamped to a valid ThermalLoad value.
        if cfg!(not(debug_assertions)) {
            // An invalid error_integral greater than e_integral_max should clamp at ThermalLoad(0)
            assert_eq!(ThermalPolicy::calculate_thermal_load(5.0, -20.0, 0.0), ThermalLoad(0));

            // An invalid error_integral less than e_integral_min should clamp at ThermalLoad(100)
            assert_eq!(ThermalPolicy::calculate_thermal_load(-25.0, -20.0, 0.0), ThermalLoad(100));
        }

        // Test some error_integral values ranging from e_integral_max to e_integral_min
        assert_eq!(ThermalPolicy::calculate_thermal_load(0.0, -20.0, 0.0), ThermalLoad(0));
        assert_eq!(ThermalPolicy::calculate_thermal_load(-10.0, -20.0, 0.0), ThermalLoad(50));
        assert_eq!(ThermalPolicy::calculate_thermal_load(-20.0, -20.0, 0.0), ThermalLoad(100));
    }

    /// Tests that an invalid low error integral value will cause a panic on debug builds.
    #[test]
    #[should_panic = "error_integral (-25) less than error_integral_min (-20)"]
    #[cfg(debug_assertions)]
    fn test_calculate_thermal_load_error_integral_low_panic() {
        assert_eq!(ThermalPolicy::calculate_thermal_load(-25.0, -20.0, 0.0), ThermalLoad(100));
    }

    /// Tests that an invalid high error integral value will cause a panic on debug builds.
    #[test]
    #[should_panic = "error_integral (5) greater than error_integral_max (0)"]
    #[cfg(debug_assertions)]
    fn test_calculate_thermal_load_error_integral_high_panic() {
        assert_eq!(ThermalPolicy::calculate_thermal_load(5.0, -20.0, 0.0), ThermalLoad(0));
    }

    /// Tests that the `get_time_delta` function correctly calculates time delta while updating the
    /// `max_time_delta` state variable.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_time_delta() {
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let node = ThermalPolicyBuilder::new(thermal_config).build().unwrap();

        assert_eq!(node.get_time_delta(Nanoseconds(Seconds(1.5).into_nanos())), Seconds(1.5));
        assert_eq!(node.get_time_delta(Nanoseconds(Seconds(2.0).into_nanos())), Seconds(0.5));
        assert_eq!(node.state.max_time_delta.get(), Seconds(1.5));
    }

    /// Tests that the `get_filtered_temperature` function correctly calculates filtered
    /// temperature.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_filtered_temperature() {
        let mut policy_params = default_policy_params();
        policy_params.controller_params.filter_time_constant = Seconds(10.0);
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };
        let node = ThermalPolicyBuilder::new(thermal_config).build().unwrap();

        assert_eq!(node.get_filtered_temperature(Celsius(40.0), Seconds(1.0)), Celsius(4.0));
        assert_eq!(node.get_filtered_temperature(Celsius(40.0), Seconds(1.0)), Celsius(7.6));
    }

    /// Tests that the `get_temperature_error` function correctly calculates proportional error and
    /// integral error.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_temperature_error() {
        let mut policy_params = default_policy_params();
        policy_params.controller_params.target_temperature = Celsius(80.0);
        policy_params.controller_params.e_integral_min = -20.0;
        policy_params.controller_params.e_integral_max = 0.0;

        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };
        let node = ThermalPolicyBuilder::new(thermal_config).build().unwrap();

        assert_eq!(node.get_temperature_error(Celsius(40.0), Seconds(1.0)), (40.0, 0.0));
        assert_eq!(node.get_temperature_error(Celsius(90.0), Seconds(1.0)), (-10.0, -10.0));
        assert_eq!(node.get_temperature_error(Celsius(90.0), Seconds(1.0)), (-10.0, -20.0));
    }

    /// Tests that the ThermalPolicy will correctly divide total available power amongst multiple
    /// CPU control nodes.
    #[fasync::run_singlethreaded(test)]
    async fn test_multiple_cpu_actors() {
        // Set up the two CpuControlHandler mock nodes. The message reply to SetMaxPowerConsumption
        // indicates how much power the mock node was able to utilize, and ultimately drives the
        // test logic.
        let cpu_node_1 = create_mock_node(
            "CpuCtrlNode1",
            vec![
                // On the first iteration, this node will consume all available power (1W)
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(1.0))),
                ),
                // On the second iteration, this node will consume half of the available power
                // (0.5W)
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.5))),
                ),
                // On the third iteration, this node will consume none of the available power
                // (0.0W)
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.0))),
                ),
            ],
        );
        let cpu_node_2 = create_mock_node(
            "CpuCtrlNode2",
            vec![
                // On the first iteration, the first node consumes all available power (1W), so
                // expect to receive a power allocation of 0W
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(0.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.0))),
                ),
                // On the second iteration, the first node consumes half of the available power
                // (1W), so expect to receive a power allocation of 0.5W
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(0.5))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(0.5))),
                ),
                // On the third iteration, the first node consumes none of the available power
                // (1W), so expect to receive a power allocation of 1W
                (
                    msg_eq!(SetMaxPowerConsumption(Watts(1.0))),
                    msg_ok_return!(SetMaxPowerConsumption(Watts(1.0))),
                ),
            ],
        );

        let thermal_config = ThermalConfig {
            temperature_node: create_mock_node("TemperatureNode", vec![]),
            cpu_control_nodes: vec![cpu_node_1, cpu_node_2],
            sys_pwr_handler: create_mock_node("SysPwrNode", vec![]),
            thermal_limiter_node: create_mock_node("ThermalLimiterNode", vec![]),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let node = ThermalPolicyBuilder::new(thermal_config).build().unwrap();

        // Distribute 1W of total power across the two CPU nodes. The real test logic happens inside
        // the mock node, where we verify that the expected power amounts are granted to both CPU
        // nodes via the SetMaxPowerConsumption message. Repeat for the number of messages that the
        // mock nodes expect to receive (three).
        node.distribute_power(Watts(1.0)).await.unwrap();
        node.distribute_power(Watts(1.0)).await.unwrap();
        node.distribute_power(Watts(1.0)).await.unwrap();
    }

    /// Tests for the presence and correctness of dynamically-added inspect data
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_data() {
        let policy_params = default_policy_params();
        let thermal_config = ThermalConfig {
            temperature_node: create_mock_node("TemperatureNode", vec![]),
            cpu_control_nodes: vec![create_mock_node("CpuCtrlNode", vec![])],
            sys_pwr_handler: create_mock_node("SysPwrNode", vec![]),
            thermal_limiter_node: create_mock_node("ThermalLimiterNode", vec![]),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let inspector = inspect::Inspector::new();
        let _node = ThermalPolicyBuilder::new(thermal_config)
            .with_inspect_root(inspector.root())
            .build()
            .unwrap();

        assert_inspect_tree!(
            inspector,
            root: {
                ThermalPolicy: {
                    state: contains {},
                    stats: contains {},
                    throttle_history: contains {},
                    policy_params: {
                        controller_params: {
                            "sample_interval (s)":
                                policy_params.controller_params.sample_interval.0,
                            "filter_time_constant (s)":
                                policy_params.controller_params.filter_time_constant.0,
                            "target_temperature (C)":
                                policy_params.controller_params.target_temperature.0,
                            "e_integral_min": policy_params.controller_params.e_integral_min,
                            "e_integral_max": policy_params.controller_params.e_integral_max,
                            "sustainable_power (W)":
                                policy_params.controller_params.sustainable_power.0,
                            "proportional_gain": policy_params.controller_params.proportional_gain,
                            "integral_gain": policy_params.controller_params.integral_gain,
                        }
                    }
                }
            }
        );
    }

    /// Tests that throttle data is collected and stored properly in Inspect.
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_throttle_history() {
        // Set relevant policy parameters so we have deterministic power and thermal load
        // calculations
        let mut policy_params = default_policy_params();
        policy_params.controller_params.sustainable_power = Watts(1.0);
        policy_params.controller_params.proportional_gain = 0.0;
        policy_params.controller_params.integral_gain = 0.0;

        // Calculate the values that we expect to be reported by the thermal policy based on the
        // parameters chosen above
        let thermal_load = ThermalLoad(50);
        let throttle_available_power = policy_params.controller_params.sustainable_power;
        let throttle_available_power_cpu1 = throttle_available_power;
        let throttle_cpu1_power_used = throttle_available_power_cpu1 - Watts(0.3); // arbitrary
        let throttle_available_power_cpu2 =
            throttle_available_power_cpu1 - throttle_cpu1_power_used;
        let throttle_cpu2_power_used = throttle_available_power_cpu2;
        let throttle_start_time = Nanoseconds(0);
        let throttle_end_time = Nanoseconds(1000);

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![
                create_mock_node(
                    "Cpu1Node",
                    vec![(
                        msg_eq!(SetMaxPowerConsumption(throttle_available_power_cpu1)),
                        msg_ok_return!(SetMaxPowerConsumption(throttle_cpu1_power_used)),
                    )],
                ),
                create_mock_node(
                    "Cpu2Node",
                    vec![(
                        msg_eq!(SetMaxPowerConsumption(throttle_available_power_cpu2)),
                        msg_ok_return!(SetMaxPowerConsumption(throttle_cpu2_power_used)),
                    )],
                ),
            ],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };
        let inspector = inspect::Inspector::new();
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_inspect_root(inspector.root())
            .build()
            .unwrap();

        // Causes Inspect to receive throttle_start_time and one reading into thermal_load_hist
        let _ = node.update_thermal_load(throttle_start_time, thermal_load).await;

        // Causes Inspect to receive one reading into available_power_hist and one reading into both
        // entries of cpu_power_usage
        let _ = node.update_power_allocation(0.0, 0.0).await;

        // Causes Inspect to receive throttle_end_time
        let _ = node.update_thermal_load(throttle_end_time, ThermalLoad(0)).await;

        // TODO(fxb/49483): The `assert_inspect_tree!` macro really needs better support for testing
        // histogram correctness. Since that doesn't exist today, the only option is to recreate the
        // histogram property in the format that assert_inspect_tree expects. This requires
        // knowledge of the underlying Inspect histogram implementation, which is bad but it's our
        // only option at this time.
        fn create_hist_vec<T>(params: LinearHistogramParams<T>, data: Vec<T>) -> Vec<T>
        where
            T: std::convert::From<u32>
                + std::ops::AddAssign
                + std::ops::Sub<Output = T>
                + std::ops::Div<Output = T>
                + std::cmp::PartialOrd
                + std::clone::Clone
                + core::marker::Copy,
        {
            // The inspect histogram adds four extra buckets:
            //  - [0]: floor
            //  - [1]: step_size
            //  - [2]: underflow bucket
            //  - [-1]: overflow bucket
            let mut hist_vec: Vec<T> = vec![T::from(0); params.buckets + 4];
            hist_vec[0] = params.floor;
            hist_vec[1] = params.step_size;

            // Populate the histogram using the supplied data
            for v in data.into_iter() {
                // Index selection copied from the Inspect implementation:
                // https://fuchsia.googlesource.com/fuchsia/+/ca376a675e18428aa31f2c2b78a4cfe2cf1c69a2/src/lib/inspect/rust/fuchsia-inspect/src/lib.rs#999
                let index = {
                    let mut current_floor = params.floor;
                    let mut _idx = 2;
                    while v >= current_floor && _idx < params.buckets - 1 {
                        current_floor += params.step_size;
                        _idx += 1;
                    }
                    _idx as usize
                };
                hist_vec[index] += T::from(1);
            }
            hist_vec
        }

        let expected_thermal_load_hist = create_hist_vec::<u64>(
            LinearHistogramParams { floor: 0, step_size: 1, buckets: 100 },
            vec![thermal_load.0.into()],
        );

        let expected_available_power_hist = create_hist_vec::<f64>(
            LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            vec![throttle_available_power.0],
        );

        let expected_cpu_1_power_usage_hist = create_hist_vec::<f64>(
            LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            vec![throttle_cpu1_power_used.0],
        );

        let expected_cpu_2_power_usage_hist = create_hist_vec::<f64>(
            LinearHistogramParams { floor: 0.0, step_size: 0.1, buckets: 100 },
            vec![throttle_cpu2_power_used.0],
        );

        assert_inspect_tree!(
            inspector,
            root: {
                ThermalPolicy: contains {
                    throttle_history: {
                        "0": {
                            throttle_start_time: throttle_start_time.0,
                            throttle_end_time: throttle_end_time.0,
                            thermal_load_hist: expected_thermal_load_hist,
                            available_power_hist: expected_available_power_hist,
                            cpu_power_usage: {
                                "0": expected_cpu_1_power_usage_hist,
                                "1": expected_cpu_2_power_usage_hist
                            }
                        },
                    }
                }
            }
        )
    }

    /// Verifies that InspectThrottleHistory correctly removes old entries without increasing the
    /// size of the underlying vector.
    #[test]
    fn test_inspect_throttle_history_length() {
        // Create a InspectThrottleHistory with capacity for only one throttling entry
        let mut throttle_history = InspectThrottleHistory::new(
            inspect::Inspector::new().root().create_child("test_node"),
            1,
        );

        // Add a throttling entry
        throttle_history.mark_throttling_active(Nanoseconds(0));
        throttle_history.mark_throttling_inactive(Nanoseconds(0));

        // Verify one entry and unchanged capacity
        assert_eq!(throttle_history.throttle_history_list.len(), 1);
        assert_eq!(throttle_history.throttle_history_list.capacity(), 1);
        assert_eq!(throttle_history.entry_count, 1);

        // Add one more throttling entry
        throttle_history.mark_throttling_active(Nanoseconds(0));
        throttle_history.mark_throttling_inactive(Nanoseconds(0));

        // Verify still one entry and unchanged capacity
        assert_eq!(throttle_history.throttle_history_list.len(), 1);
        assert_eq!(throttle_history.throttle_history_list.capacity(), 1);
        assert_eq!(throttle_history.entry_count, 2);
    }

    /// Tests that well-formed configuration JSON does not panic the `new_from_json` function.
    #[fasync::run_singlethreaded(test)]
    async fn test_new_from_json() {
        let json_data = json::json!({
            "type": "ThermalPolicy",
            "name": "thermal_policy",
            "config": {
                "thermal_shutdown_temperature": 95.0,
                "throttle_end_delay": 0.0,
                "controller_params": {
                    "sample_interval": 1.0,
                    "filter_time_constant": 5.0,
                    "target_temperature": 80.0,
                    "e_integral_min": -20.0,
                    "e_integral_max": 0.0,
                    "sustainable_power": 0.876,
                    "proportional_gain": 0.0,
                    "integral_gain": 0.08
                }
            },
            "dependencies": {
                "cpu_control_nodes": [
                    "cpu_control"
                ],
                "system_power_handler_node": "sys_power",
                "temperature_handler_node": "temperature",
                "thermal_limiter_node": "limiter",
                "crash_report_handler_node": "crash_report"
              },
        });

        let mut nodes: HashMap<String, Rc<dyn Node>> = HashMap::new();
        nodes.insert("temperature".to_string(), create_dummy_node());
        nodes.insert("cpu_control".to_string(), create_dummy_node());
        nodes.insert("sys_power".to_string(), create_dummy_node());
        nodes.insert("limiter".to_string(), create_dummy_node());
        nodes.insert("crash_report".to_string(), create_dummy_node());
        let _ = ThermalPolicyBuilder::new_from_json(json_data, &nodes);
    }

    /// Tests that the ThermalPolicy reports the thermal_limiting_elapsed_time and
    /// thermal_limit_result metrics after successful thermal mitigation.
    #[fasync::run_singlethreaded(test)]
    async fn test_cobalt_metrics_throttle_elapsed_time() {
        let throttle_duration = Nanoseconds(1e9 as i64); // 1s

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };

        let (sender, mut receiver) = futures::channel::mpsc::channel(10);
        let cobalt_metrics = CobaltMetrics::new_with_cobalt_sender(CobaltSender::new(sender));
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_thermal_metrics(cobalt_metrics)
            .build()
            .unwrap();

        // Cause the thermal policy to begin thermal limiting
        let _ = node.update_thermal_load(Nanoseconds(0), ThermalLoad(50)).await;

        // Cause the thermal policy to end thermal limiting
        let _ = node.update_thermal_load(throttle_duration, ThermalLoad(0)).await;

        // Verify the expected Cobalt event for the thermal_limiting_elapsed_time metric
        assert_eq!(
            receiver.try_next().unwrap().unwrap(),
            CobaltEvent {
                metric_id: power_metrics_registry::THERMAL_LIMITING_ELAPSED_TIME_METRIC_ID,
                event_codes: vec![],
                component: None,
                payload: EventPayload::ElapsedMicros(throttle_duration.0 as i64)
            }
        );

        // Verify the expected Cobalt event for the thermal_limit_result metric
        assert_eq!(
            receiver.try_next().unwrap().unwrap(),
            CobaltEvent {
                metric_id: power_metrics_registry::THERMAL_LIMIT_RESULT_METRIC_ID,
                event_codes: vec![thermal_limit_result::Mitigated as u32],
                component: None,
                payload: EventPayload::Event(Event),
            }
        );

        // Verify there were no more dispatched Cobalt events
        assert!(receiver.try_next().is_err());
    }

    /// Tests that the ThermalPolicy reports the thermal_limiting_elapsed_time and
    /// thermal_limit_result metrics in the case of a thermal shutdown.
    #[fasync::run_singlethreaded(test)]
    async fn test_cobalt_metrics_thermal_shutdown() {
        let policy_params = default_policy_params();
        let shutdown_temperature = policy_params.thermal_shutdown_temperature + Celsius(1.0);

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };
        let (sender, mut receiver) = futures::channel::mpsc::channel(10);
        let cobalt_metrics = CobaltMetrics::new_with_cobalt_sender(CobaltSender::new(sender));
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_thermal_metrics(cobalt_metrics)
            .build()
            .unwrap();

        // Cause the thermal policy to enter thermal shutdown
        let _ = node.check_critical_temperature(Nanoseconds(0), shutdown_temperature).await;

        // Verify the expected Cobalt event for the thermal_limit_result metric
        assert_eq!(
            receiver.try_next().unwrap().unwrap(),
            CobaltEvent {
                metric_id: power_metrics_registry::THERMAL_LIMIT_RESULT_METRIC_ID,
                event_codes: vec![thermal_limit_result::Shutdown as u32],
                component: None,
                payload: EventPayload::Event(Event),
            }
        );

        // Verify there were no more dispatched Cobalt events
        assert!(receiver.try_next().is_err());
    }

    /// Tests that the ThermalPolicy reports the raw_temperature metric with the correct data and
    /// after the expected number of temperature readings.
    #[fasync::run_singlethreaded(test)]
    async fn test_cobalt_metrics_raw_temperature() {
        // The temperature that the mock TemperatureNode will respond with, and that the test will
        // verify is received in the Cobalt histogram event
        let test_temperature = Celsius(50.0);

        // The number of temperature readings that the test will perform and are expected to be
        // reported in the Cobalt histogram event
        let num_temperature_readings = CobaltMetrics::NUM_TEMPERATURE_READINGS + 1;

        // Mock temperature node that responds to `num_temperature_readings` number of
        // ReadTemperature messages with a `test_temperature` reading
        let temperature_node = create_mock_node(
            "TemperatureNode",
            (0..num_temperature_readings)
                .map(|_| {
                    (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(test_temperature)))
                })
                .collect(),
        );

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node,
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params: default_policy_params(),
        };
        let (sender, mut receiver) = futures::channel::mpsc::channel(10);
        let cobalt_metrics = CobaltMetrics::new_with_cobalt_sender(CobaltSender::new(sender));
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_thermal_metrics(cobalt_metrics)
            .build()
            .unwrap();

        // Iterate the controller for `num_temperature_readings` iterations to trigger a Cobalt
        // metric event
        for _ in 0..num_temperature_readings {
            node.iterate_thermal_control().await.unwrap();
        }

        // Generate the expected raw_temperature Cobalt event
        let mut expected_histogram = CobaltIntHistogram::new(CobaltIntHistogramConfig {
            floor: power_metrics_registry::RAW_TEMPERATURE_INT_BUCKETS_FLOOR,
            num_buckets: power_metrics_registry::RAW_TEMPERATURE_INT_BUCKETS_NUM_BUCKETS,
            step_size: power_metrics_registry::RAW_TEMPERATURE_INT_BUCKETS_STEP_SIZE,
        });
        for _ in 0..num_temperature_readings - 1 {
            expected_histogram.add_data(test_temperature.0 as i64);
        }

        let expected_cobalt_event = CobaltEvent {
            metric_id: power_metrics_registry::RAW_TEMPERATURE_METRIC_ID,
            event_codes: vec![],
            component: None,
            payload: EventPayload::IntHistogram(expected_histogram.get_data()),
        };

        // Verify that the expected Cobalt event was received, and there were no extra events
        assert_eq!(receiver.try_next().unwrap().unwrap(), expected_cobalt_event);
        assert!(receiver.try_next().is_err());
    }

    /// Tests that we can call `throttle_start` a second time if we first call `throttle_end`.
    #[test]
    fn test_cobalt_metrics_throttle_restart() {
        let (sender, _) = futures::channel::mpsc::channel(10);
        let mut cobalt_metrics = CobaltMetrics::new_with_cobalt_sender(CobaltSender::new(sender));
        cobalt_metrics.log_throttle_start(Nanoseconds(0));
        cobalt_metrics.log_throttle_end_mitigated(Nanoseconds(1000));
        cobalt_metrics.log_throttle_start(Nanoseconds(2000));
    }

    /// Tests that calling CobaltMetrics `throttle_start` twice without first calling `throttle_end`
    /// causes a panic.
    #[test]
    #[should_panic(expected = "throttle_start called before ending previous throttle")]
    fn test_cobalt_metrics_double_start_panic() {
        let (sender, _) = futures::channel::mpsc::channel(10);
        let mut cobalt_metrics = CobaltMetrics::new_with_cobalt_sender(CobaltSender::new(sender));
        cobalt_metrics.log_throttle_start(Nanoseconds(0));
        cobalt_metrics.log_throttle_start(Nanoseconds(0));
    }

    /// Tests that Cobalt events are not dispatched until after a specified throttle end delay.
    #[fasync::run_singlethreaded(test)]
    async fn test_cobalt_metrics_throttle_deadline() {
        // Set the test parameters
        let mut policy_params = default_policy_params();
        let throttle_end_delay = Seconds(60.0);
        policy_params.throttle_end_delay = throttle_end_delay;
        let initial_throttle_duration = Nanoseconds(1e9 as i64); // 1s
        let total_throttle_duration =
            initial_throttle_duration + Nanoseconds(throttle_end_delay.into_nanos());

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_dummy_node(),
            policy_params,
        };

        let (sender, mut receiver) = futures::channel::mpsc::channel(10);
        let cobalt_metrics = CobaltMetrics::new_with_cobalt_sender(CobaltSender::new(sender));
        let node = ThermalPolicyBuilder::new(thermal_config)
            .with_thermal_metrics(cobalt_metrics)
            .build()
            .unwrap();

        // Cause the thermal policy to begin thermal limiting
        let _ = node.update_thermal_load(Nanoseconds(0), ThermalLoad(50)).await;

        // Cause the thermal policy to end thermal limiting, but the deadline timer should still be
        // active
        let _ = node.update_thermal_load(initial_throttle_duration, ThermalLoad(0)).await;

        // Verify there were no dispatched Cobalt events because the deadline timer is still running
        assert!(receiver.try_next().is_err());

        // Cause the deadline timer to expire
        let _ = node.update_thermal_load(total_throttle_duration, ThermalLoad(0)).await;

        // Verify the expected Cobalt event for the thermal_limiting_elapsed_time metric
        assert_eq!(
            receiver.try_next().unwrap().unwrap(),
            CobaltEvent {
                metric_id: power_metrics_registry::THERMAL_LIMITING_ELAPSED_TIME_METRIC_ID,
                event_codes: vec![],
                component: None,
                payload: EventPayload::ElapsedMicros(total_throttle_duration.0 as i64)
            }
        );

        // Verify the expected Cobalt event for the thermal_limit_result metric
        assert_eq!(
            receiver.try_next().unwrap().unwrap(),
            CobaltEvent {
                metric_id: power_metrics_registry::THERMAL_LIMIT_RESULT_METRIC_ID,
                event_codes: vec![thermal_limit_result::Mitigated as u32],
                component: None,
                payload: EventPayload::Event(Event),
            }
        );

        // Verify there were no more dispatched Cobalt events
        assert!(receiver.try_next().is_err());
    }

    /// Tests that when thermal throttling exits, the ThermalPolicy triggers a crash report on the
    /// CrashReportHandler node.
    #[fasync::run_singlethreaded(test)]
    async fn test_throttle_crash_report() {
        // Define the test parameters
        let mut policy_params = default_policy_params();
        policy_params.throttle_end_delay = Seconds(0.0);

        // Set up the ThermalPolicy node
        let thermal_config = ThermalConfig {
            temperature_node: create_dummy_node(),
            cpu_control_nodes: vec![create_dummy_node()],
            sys_pwr_handler: create_dummy_node(),
            thermal_limiter_node: create_dummy_node(),
            crash_report_handler: create_mock_node(
                "CrashReportMock",
                vec![(
                    msg_eq!(FileCrashReport("fuchsia-thermal-throttle".to_string())),
                    msg_ok_return!(FileCrashReport),
                )],
            ),
            policy_params,
        };
        let node = ThermalPolicyBuilder::new(thermal_config).build().unwrap();

        // Enter and then exit thermal throttling. The mock crash_report_handler node will assert if
        // it does not receive a FileCrashReport message.
        let _ = node.update_thermal_load(Nanoseconds(0), ThermalLoad(50)).await;
        let _ = node.update_thermal_load(Nanoseconds(0), ThermalLoad(0)).await;
    }
}
