// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    scrutiny::{model::controller::DataController, model::model::DataModel},
    serde_json::{self, value::Value},
    std::sync::Arc,
};

#[derive(Default)]
pub struct BootfsPathsController {}

impl DataController for BootfsPathsController {
    fn query(&self, model: Arc<DataModel>, _: Value) -> Result<Value> {
        if let Some(zbi) = &*model.zbi().read().unwrap() {
            let paths = zbi.bootfs.keys().cloned().collect::<Vec<String>>();
            Ok(serde_json::to_value(paths)?)
        } else {
            let empty: Vec<String> = vec![];
            Ok(serde_json::to_value(empty)?)
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, scrutiny::model::model::*, serde_json::json, std::collections::HashMap,
        tempfile::tempdir,
    };

    #[test]
    fn bootfs_returns_files() {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let model = Arc::new(DataModel::connect(uri).unwrap());
        let mut zbi = Zbi { sections: vec![], bootfs: HashMap::new() };
        zbi.bootfs.insert("foo".to_string(), vec![]);
        *model.zbi().write().unwrap() = Some(zbi);
        let controller = BootfsPathsController::default();
        let bootfs: Vec<String> =
            serde_json::from_value(controller.query(model, json!("")).unwrap()).unwrap();
        assert_eq!(bootfs.len(), 1);
        assert_eq!(bootfs[0], "foo".to_string());
    }
}
