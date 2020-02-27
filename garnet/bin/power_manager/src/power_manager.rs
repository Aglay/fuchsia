// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::node::Node;
use crate::types::{Celsius, Farads, Hertz, Seconds, Watts};
use anyhow::{format_err, Error};
use fuchsia_component::server::{ServiceFs, ServiceObjLocal};
use fuchsia_inspect::component;
use std::rc::Rc;

// nodes
use crate::{
    cpu_control_handler, cpu_stats_handler, dev_control_handler, system_power_handler,
    temperature_handler, thermal_limiter, thermal_policy,
};

pub struct PowerManager {
    board: String,
    nodes: Vec<Rc<dyn Node>>,
}

impl PowerManager {
    pub fn new(board: String) -> Result<Self, Error> {
        let pm = PowerManager { board, nodes: Vec::new() };
        Ok(pm)
    }

    pub async fn init<'a, 'b>(
        &mut self,
        service_fs: &'a mut ServiceFs<ServiceObjLocal<'b, ()>>,
    ) -> Result<(), Error> {
        // Required call to serve the inspect tree
        let inspector = component::inspector();
        inspector.serve(service_fs)?;

        match self.board.as_ref() {
            "astro" => self.init_astro(service_fs).await,
            _ => Err(format_err!("Invalid target: {}", self.board)),
        }?;

        Ok(())
    }

    async fn init_astro<'a, 'b>(
        &mut self,
        service_fs: &'a mut ServiceFs<ServiceObjLocal<'b, ()>>,
    ) -> Result<(), Error> {
        let cpu_temperature = temperature_handler::TemperatureHandlerBuilder::new_with_driver_path(
            "/dev/class/thermal/000".to_string(),
        )
        .build()?;
        self.nodes.push(cpu_temperature.clone());

        let cpu_stats_node = cpu_stats_handler::CpuStatsHandlerBuilder::new().build().await?;
        self.nodes.push(cpu_stats_node.clone());

        let cpu_path = "/dev/class/cpu-ctrl/000";
        let cpu_dev_handler_node =
            dev_control_handler::DeviceControlHandlerBuilder::new_with_driver_path(
                cpu_path.to_string(),
            )
            .build()?;
        self.nodes.push(cpu_dev_handler_node.clone());

        let cpu_control_node = cpu_control_handler::CpuControlHandlerBuilder::new_with_driver_path(
            cpu_path.to_string(),
            // This value is for Astro. It is determined by measuring power consumption under max
            // load for each P-state and then choosing a capacitance and power offset such that
            // the model
            //     power = offset + capacitance * voltage^2 * frequency * load
            // best fits the data.
            Farads(120.0e-12),
            cpu_stats_node,
            cpu_dev_handler_node,
        )
        .with_min_cpu_clock_speed(Hertz(1.0e9))
        .build()
        .await?;
        self.nodes.push(cpu_control_node.clone());

        let sys_pwr_handler =
            system_power_handler::SystemPowerStateHandlerBuilder::new().build()?;
        self.nodes.push(sys_pwr_handler.clone());

        let thermal_limiter_node =
            thermal_limiter::ThermalLimiterBuilder::new().with_service_fs(service_fs).build()?;
        self.nodes.push(thermal_limiter_node.clone());

        let thermal_config = thermal_policy::ThermalConfig {
            temperature_node: cpu_temperature,
            cpu_control_node,
            sys_pwr_handler,
            thermal_limiter_node,

            policy_params: thermal_policy::ThermalPolicyParams {
                // TODO(fxb/45951): Capture design critera for parameters in a simulator-based
                // unit test where possible.
                controller_params: thermal_policy::ThermalControllerParams {
                    sample_interval: Seconds(1.0),
                    filter_time_constant: Seconds(5.0),
                    target_temperature: Celsius(80.0),
                    e_integral_min: -20.0,
                    e_integral_max: 0.0,
                    // TOOD(fxb/45942): This is currently set to the maximum power required by the
                    // Astro CPU via
                    //     max_power = capacitance * V^2 * f * num_cores,
                    // where V and f correspond to the highest clock speed. Once CpuControlHandler
                    // has an explicit initialization phase, it should derive this value instead
                    // and supply it to the thermal policy.
                    sustainable_power: Watts(0.876),
                    proportional_gain: 0.0,
                    integral_gain: 0.08,
                },
                thermal_limiting_range: [Celsius(77.0), Celsius(84.0)],
                thermal_shutdown_temperature: Celsius(95.0),
            },
        };
        let thermal_policy = thermal_policy::ThermalPolicyBuilder::new(thermal_config).build()?;
        self.nodes.push(thermal_policy);

        Ok(())
    }

    #[cfg(test)]
    fn list_nodes(&self) -> Vec<&'static str> {
        self.nodes.iter().map(|node| node.name()).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_create_power_manager() {
        let board_name = "astro";
        let power_manager = PowerManager::new(board_name.to_string()).unwrap();
        assert_eq!(power_manager.board, board_name);
        assert_eq!(power_manager.list_nodes().len(), 0);
    }
}
