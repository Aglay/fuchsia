// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::core::collection::{Components, Routes},
    crate::core::controller::utils::DefaultComponentRequest,
    anyhow::{Error, Result},
    scrutiny::{
        model::controller::{DataController, HintDataType},
        model::model::DataModel,
    },
    scrutiny_utils::usage::UsageBuilder,
    serde::{Deserialize, Serialize},
    serde_json::{self, value::Value},
    std::io::{self, ErrorKind},
    std::sync::Arc,
};

#[derive(Default)]
pub struct RoutesGraphController {}

impl DataController for RoutesGraphController {
    fn query(&self, model: Arc<DataModel>, _: Value) -> Result<Value> {
        let routes = model.get::<Routes>()?.entries.clone();
        Ok(serde_json::to_value(routes)?)
    }

    fn description(&self) -> String {
        "Returns every route between components".to_string()
    }

    fn usage(&self) -> String {
        UsageBuilder::new()
            .name("routes - Dumps every route between components.")
            .summary("routes")
            .description(
                "Dumps every route used between components. \
            This dumps all the routes in the Data Model and is intended \
            as a way to export this data into a file for further analysis as \
            raw data.",
            )
            .build()
    }
}

#[derive(Default)]
pub struct ComponentUsesGraphController {}

#[derive(Deserialize, Serialize)]
struct ComponentUsesResponse {
    uses: Vec<i32>,
}

impl DataController for ComponentUsesGraphController {
    fn query(&self, model: Arc<DataModel>, value: Value) -> Result<Value> {
        let req: DefaultComponentRequest = serde_json::from_value(value)?;
        let component_id = req.component_id(model.clone())?;

        let routes = model.get::<Routes>()?.entries.clone();
        let mut resp = ComponentUsesResponse { uses: Vec::new() };
        for route in routes.iter() {
            if component_id == route.src_id as i64 {
                resp.uses.push(route.dst_id);
            }
        }

        Ok(serde_json::to_value(resp)?)
    }

    fn description(&self) -> String {
        "Returns all the services a given component uses.".to_string()
    }

    fn usage(&self) -> String {
        UsageBuilder::new()
            .name("component.uses - Shows all the services a component uses.")
            .summary("component.uses --url [COMPONENT_URL] --component_id [COMPONENT_ID]")
            .description("Lists every component that this component uses. Provide either the url or the component_id.")
            .arg("--component_id", "The component id for the component")
            .arg("--url", "The url for the component")
            .build()
    }

    fn hints(&self) -> Vec<(String, HintDataType)> {
        vec![
            ("--url".to_string(), HintDataType::NoType),
            ("--component_id".to_string(), HintDataType::NoType),
        ]
    }
}

#[derive(Default)]
pub struct ComponentUsedGraphController {}

#[derive(Deserialize, Serialize)]
struct ComponentUsedResponse {
    used_by: Vec<i32>,
}

impl DataController for ComponentUsedGraphController {
    fn query(&self, model: Arc<DataModel>, value: Value) -> Result<Value> {
        let req: DefaultComponentRequest = serde_json::from_value(value)?;
        let component_id = req.component_id(model.clone())?;

        {
            let mut found = false;
            let components = &model.get::<Components>()?.entries;
            for component in components.iter() {
                if component_id == component.id as i64 {
                    found = true;
                }
            }

            if !found {
                return Err(Error::new(io::Error::new(
                    ErrorKind::Other,
                    format!("Could not find a component with component_id {}.", component_id),
                )));
            }
        }

        let routes = &model.get::<Routes>()?.entries;
        let mut resp = ComponentUsedResponse { used_by: Vec::new() };
        for route in routes.iter() {
            if component_id == route.dst_id as i64 {
                resp.used_by.push(route.src_id);
            }
        }

        Ok(serde_json::to_value(resp)?)
    }

    fn description(&self) -> String {
        "Returns all the components a given component is used by.".to_string()
    }

    fn usage(&self) -> String {
        UsageBuilder::new()
            .name("component.used - Shows all the services a component is used by.")
            .summary("component.used --url [COMPONENT_URL] --component_id [COMPONENT_ID]")
            .description("Lists every component that this component is used by. Provide the url or the component_id")
            .arg("--component_id", "The component id for the component")
            .arg("--url", "The url for the component")
            .build()
    }

    fn hints(&self) -> Vec<(String, HintDataType)> {
        vec![
            ("--url".to_string(), HintDataType::NoType),
            ("--component_id".to_string(), HintDataType::NoType),
        ]
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::core::collection::{Component, Route},
        serde_json::json,
        tempfile::tempdir,
    };

    fn empty_value() -> Value {
        serde_json::from_str("{}").unwrap()
    }

    fn make_component(id: i32, url: &str, version: i32, inferred: bool) -> Component {
        Component { id: id, url: url.to_string(), version: version, inferred: inferred }
    }

    fn make_route(id: i32, src: i32, dst: i32) -> Route {
        Route { id, src_id: src, dst_id: dst, service_name: "service".to_string(), protocol_id: 0 }
    }

    #[test]
    fn routes_controller_returns_all_routes() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, true);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 1, 2);
        let route2 = make_route(2, 2, 3);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());
        model.set(routes).unwrap();

        let controller = RoutesGraphController::default();
        let val = controller.query(model, empty_value()).unwrap();
        let response: Vec<Route> = serde_json::from_value(val).unwrap();

        assert_eq!(2, response.len());
        assert_eq!(route1, response[0]);
        assert_eq!(route2, response[1]);
    }

    #[test]
    fn uses_controller_known_id_returns_all_dependency_ids() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, false);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 2, 1);
        let route2 = make_route(2, 2, 3);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());
        model.set(routes).unwrap();

        let controller = ComponentUsesGraphController::default();
        let json_body = json!({
            "url": "fake_url_2",
        });
        let val = controller.query(model, json_body).unwrap();
        let response: ComponentUsesResponse = serde_json::from_value(val).unwrap();

        assert_eq!(2, response.uses.len());
        assert_eq!(1, response.uses[0]);
        assert_eq!(3, response.uses[1]);
    }

    #[test]
    fn uses_controller_unknown_id_returns_err() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, false);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 2, 1);
        let route2 = make_route(2, 2, 3);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());
        model.set(routes).unwrap();

        let controller = ComponentUsesGraphController::default();
        let json_body = json!({
            "component_id": "4"
        });
        assert!(controller.query(model, json_body).is_err());
    }

    #[test]
    fn uses_controller_known_id_no_dependencies_returns_empty() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, false);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 2, 1);
        let route2 = make_route(2, 2, 3);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());
        model.set(routes).unwrap();

        let controller = ComponentUsesGraphController::default();
        let json_body = json!({
            "component_id": 1
        });
        let val = controller.query(model, json_body).unwrap();
        let response: ComponentUsesResponse = serde_json::from_value(val).unwrap();

        assert!(response.uses.is_empty());
    }

    #[test]
    fn used_controller_known_id_returns_all_dependency_ids() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, false);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 1, 2);
        let route2 = make_route(2, 3, 2);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());
        model.set(routes).unwrap();

        let controller = ComponentUsedGraphController::default();
        let json_body = json!({
            "url": "fake_url_2",
        });
        let val = controller.query(model, json_body).unwrap();
        let response: ComponentUsedResponse = serde_json::from_value(val).unwrap();

        assert_eq!(2, response.used_by.len());
        assert_eq!(1, response.used_by[0]);
        assert_eq!(3, response.used_by[1]);
    }

    #[test]
    fn used_controller_unknown_id_returns_err() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, false);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 2, 1);
        let route2 = make_route(2, 2, 3);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());

        let controller = ComponentUsedGraphController::default();
        let json_body = json!({
            "component_id": 4
        });
        assert!(controller.query(model, json_body).is_err());
    }

    #[test]
    fn used_controller_known_id_no_dependencies_returns_empty() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, false);
        let comp3 = make_component(3, "fake_url_3", 0, false);
        let mut components = Components::default();
        components.entries.push(comp1.clone());
        components.entries.push(comp2.clone());
        components.entries.push(comp3.clone());
        model.set(components).unwrap();

        let route1 = make_route(1, 2, 1);
        let route2 = make_route(2, 2, 3);
        let mut routes = Routes::default();
        routes.entries.push(route1.clone());
        routes.entries.push(route2.clone());
        model.set(routes).unwrap();

        let controller = ComponentUsedGraphController::default();
        let json_body = json!({
            "component_id": "2"
        });
        let val = controller.query(model, json_body).unwrap();
        let response: ComponentUsedResponse = serde_json::from_value(val).unwrap();

        assert!(response.used_by.is_empty());
    }
}
