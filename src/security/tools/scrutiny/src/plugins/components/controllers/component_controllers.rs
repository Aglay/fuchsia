// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{model::controller::DataController, model::model::DataModel},
    anyhow::{Error, Result},
    serde::{Deserialize, Serialize},
    serde_json::{self, value::Value},
    std::io::{self, ErrorKind},
    std::sync::Arc,
};

#[derive(Default)]
pub struct ComponentsGraphController {}

impl DataController for ComponentsGraphController {
    fn query(&self, model: Arc<DataModel>, _: Value) -> Result<Value> {
        let components = model.components().read().unwrap();
        Ok(serde_json::to_value(components.clone())?)
    }
}

#[derive(Default)]
pub struct ComponentIdGraphController {}

#[derive(Deserialize, Serialize)]
struct ComponentIdRequest {
    component_id: i32,
}

impl DataController for ComponentIdGraphController {
    fn query(&self, model: Arc<DataModel>, value: Value) -> Result<Value> {
        let req: ComponentIdRequest = serde_json::from_value(value)?;
        let components = model.components().read().unwrap();
        for component in components.iter() {
            if component.id == req.component_id {
                return Ok(serde_json::to_value(component)?);
            }
        }

        return Err(Error::new(io::Error::new(
            ErrorKind::Other,
            format!("Could not find a component with component_id {}.", req.component_id),
        )));
    }
}

#[derive(Default)]
pub struct ComponentFromUriGraphController {}

#[derive(Deserialize, Serialize)]
struct ComponentFromUriRequest {
    uri: String,
}

#[derive(Deserialize, Serialize)]
struct ComponentFromUriResponse {
    component_id: i32,
}

impl DataController for ComponentFromUriGraphController {
    fn query(&self, model: Arc<DataModel>, value: Value) -> Result<Value> {
        let req: ComponentFromUriRequest = serde_json::from_value(value)?;
        let components = model.components().read().unwrap();
        for component in components.iter() {
            if component.url == req.uri {
                return Ok(serde_json::to_value(ComponentFromUriResponse {
                    component_id: component.id,
                })?);
            }
        }

        Err(Error::new(io::Error::new(
            ErrorKind::Other,
            format!("Could not find manifest matching component uri {}.", req.uri),
        )))
    }
}

#[derive(Default)]
pub struct RawManifestGraphController {}

#[derive(Deserialize, Serialize)]
struct RawManifestRequest {
    component_id: i32,
}

impl DataController for RawManifestGraphController {
    fn query(&self, model: Arc<DataModel>, value: Value) -> Result<Value> {
        let req: RawManifestRequest = serde_json::from_value(value)?;
        let manifests = model.manifests().read().unwrap();
        for manifest in manifests.iter() {
            if manifest.component_id == req.component_id {
                return Ok(serde_json::to_value(manifest.manifest.clone())?);
            }
        }

        // Make these return a 400 or 404?
        Err(Error::new(io::Error::new(
            ErrorKind::Other,
            format!("Could not find manifest matching component_id {}.", req.component_id),
        )))
    }
}

#[derive(Default)]
pub struct ComponentSandboxGraphController {}

impl DataController for ComponentSandboxGraphController {
    fn query(&self, _: Arc<DataModel>, _: Value) -> Result<Value> {
        // TODO: Add sandbox to DataModel and implement this.
        unimplemented!("Not yet implemented.");
    }
}

#[cfg(test)]
mod tests {

    use {super::*, crate::model::model::*, serde_json::json, tempfile::tempdir};

    fn empty_value() -> Value {
        serde_json::from_str("{}").unwrap()
    }

    fn make_component(id: i32, url: &str, version: i32, inferred: bool) -> Component {
        Component { id: id, url: url.to_string(), version: version, inferred: inferred }
    }

    fn make_manifest(id: i32, manifest: &str) -> Manifest {
        Manifest { component_id: id, manifest: manifest.to_string(), uses: vec![] }
    }

    #[test]
    fn components_controller_returns_all_components() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, true);
        {
            let mut components = model.components().write().unwrap();
            components.push(comp1.clone());
            components.push(comp2.clone());
        }

        let controller = ComponentsGraphController::default();
        let val = controller.query(model, empty_value()).unwrap();
        let response: Vec<Component> = serde_json::from_value(val).unwrap();

        assert_eq!(2, response.len());
        assert_eq!(comp1, response[0]);
        assert_eq!(comp2, response[1]);
    }

    #[test]
    fn component_id_controller_known_id_returns_component() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, true);
        {
            let mut components = model.components().write().unwrap();
            components.push(comp1.clone());
            components.push(comp2.clone());
        }

        let controller = ComponentIdGraphController::default();
        let json_body = json!({
            "component_id": 1
        });
        let val = controller.query(model, json_body).unwrap();
        let response: Component = serde_json::from_value(val).unwrap();

        assert_eq!(comp1, response);
    }

    #[test]
    fn component_id_controller_unknown_id_returns_err() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, true);
        {
            let mut components = model.components().write().unwrap();
            components.push(comp1.clone());
            components.push(comp2.clone());
        }

        let controller = ComponentIdGraphController::default();
        let json_body = json!({
            "component_id": 3
        });
        assert!(controller.query(model, json_body).is_err());
    }

    #[test]
    fn component_raw_manifest_controller_known_id_returns_manifest() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, true);
        {
            let mut components = model.components().write().unwrap();
            components.push(comp1.clone());
            components.push(comp2.clone());
        }

        let manifest1 = make_manifest(1, "fake_manifest");
        {
            let mut manifests = model.manifests().write().unwrap();
            manifests.push(manifest1.clone());
        }

        let controller = RawManifestGraphController::default();
        let json_body = json!({
            "component_id": 1
        });
        let val = controller.query(model, json_body).unwrap();
        assert_eq!("fake_manifest", serde_json::from_value::<String>(val).unwrap());
    }

    #[test]
    fn component_raw_manifest_controller_unknown_id_returns_err() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());

        let comp1 = make_component(1, "fake_url", 0, false);
        let comp2 = make_component(2, "fake_url_2", 0, true);
        {
            let mut components = model.components().write().unwrap();
            components.push(comp1.clone());
            components.push(comp2.clone());
        }

        let manifest1 = make_manifest(1, "fake_manifest");
        {
            let mut manifests = model.manifests().write().unwrap();
            manifests.push(manifest1.clone());
        }

        let controller = RawManifestGraphController::default();
        let json_body = json!({
            "component_id": 3
        });
        assert!(controller.query(model, json_body).is_err());
    }

    #[ignore]
    #[test]
    fn component_sandbox_controller_valid_id_invalid_sandbox_fields_returns_err() {
        assert!(true);
    }

    #[ignore]
    #[test]
    fn component_sandbox_controller_invalid_id_valid_sandbox_fields_returns_err() {
        assert!(true);
    }

    #[ignore]
    #[test]
    fn component_sandbox_controller_valid_id_valid_sandbox_fields_returns_only_requested_fields() {
        assert!(true);
    }
}
