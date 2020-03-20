// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{
        act::{Actions, ActionsSchema},
        metrics::{Metric, Metrics},
        validate::{validate, TestsSchema},
        Options,
    },
    anyhow::{bail, format_err, Error},
    serde::Deserialize,
    serde_json as json,
    std::{collections::HashMap, fs, path::Path, str::FromStr},
};

pub mod parse;

/// Schema for JSON triage configuration. This structure is parsed directly from the configuration
/// files using serde_json.
#[derive(Deserialize, Default, Debug)]
pub struct ConfigFileSchema {
    /// Map of named Selectors. Each Selector selects a value from Diagnostic data.
    #[serde(rename = "select")]
    pub file_selectors: HashMap<String, String>,
    /// Map of named Evals. Each Eval calculates a value.
    #[serde(rename = "eval")]
    pub file_evals: HashMap<String, String>,
    /// Map of named Actions. Each Action uses a boolean value to trigger a warning.
    #[serde(rename = "act")]
    pub file_actions: ActionsSchema,
    /// Map of named Tests. Each test applies sample data to lists of actions that should or
    /// should not trigger.
    #[serde(rename = "test")]
    pub file_tests: TestsSchema,
}

impl ConfigFileSchema {
    pub fn parse(s: String) -> Result<ConfigFileSchema, Error> {
        match json5::from_str::<ConfigFileSchema>(&s) {
            Ok(config) => Ok(config),
            Err(e) => return Err(format_err!("Error {}", e)),
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum OutputFormat {
    Text,
    CSV,
}

impl FromStr for OutputFormat {
    type Err = anyhow::Error;
    fn from_str(output_format: &str) -> Result<Self, Self::Err> {
        match output_format {
            "csv" => Ok(OutputFormat::CSV),
            "text" => Ok(OutputFormat::Text),
            incorrect => {
                Err(format_err!("Invalid output type '{}' - must be 'csv' or 'text'", incorrect))
            }
        }
    }
}

/// Permanent storage of program execution context. This lives as long as the program, so we can
/// store references to it in various other structs.
pub struct StateHolder {
    pub metrics: Metrics,
    pub actions: Actions,
    pub inspect_contexts: Vec<InspectContext>,
    pub output_format: OutputFormat,
}

/// Top-level schema of the inspect.json file found in bugreport.zip.
pub struct InspectData {
    entries: Vec<json::Value>,
}

impl InspectData {
    pub fn new(entries: Vec<json::Value>) -> InspectData {
        InspectData { entries }
    }

    /// Parses a JSON string formatted as an array of [json::Value]'s.
    pub fn from(data: String) -> Result<InspectData, Error> {
        let raw_json = match data.parse::<json::Value>() {
            Ok(data) => data,
            Err(_) => return Err(format_err!("Couldn't parse Inspect file '{}' as JSON", data)),
        };
        match raw_json {
            json::Value::Array(entries) => Ok(InspectData { entries }),
            _ => return Err(format_err!("Array expected in inspect.json format")),
        }
    }

    pub fn as_json(&self) -> &Vec<json::Value> {
        &self.entries
    }
}

/// The path of the inspect file and the data contained within it.
pub struct InspectContext {
    pub source: String,
    pub data: InspectData,
}

impl InspectContext {
    pub fn initialize_from_file(filename: String) -> Result<InspectContext, Error> {
        let text = match fs::read_to_string(&filename) {
            Ok(data) => data,
            Err(_) => {
                return Err(format_err!("Couldn't read Inspect file '{}' to string", filename))
            }
        };

        let inspect_data = match InspectData::from(text) {
            Ok(data) => data,
            Err(e) => return Err(e),
        };

        Ok(InspectContext { source: filename, data: inspect_data })
    }
}

/// Parses the inspect.json file and all the config files.
pub fn initialize(options: Options) -> Result<StateHolder, Error> {
    let Options { directories, output_format, inspect, config_files, .. } = options;

    let mut inspect_contexts = Vec::new();
    inspect_contexts.push(InspectContext::initialize_from_file(inspect.unwrap())?);

    for directory in directories {
        let inspect_file = Path::new(&directory).join("inspect.json").into_os_string();
        match InspectContext::initialize_from_file(inspect_file.to_string_lossy().to_string()) {
            Ok(c) => inspect_contexts.push(c),
            Err(e) => println!("{}", e),
        };
    }

    if config_files.len() == 0 {
        bail!("Need at least one config file; use --config");
    }
    let mut actions = HashMap::new();
    let mut metrics = HashMap::new();
    let mut tests = HashMap::new();
    for file_name in config_files {
        let namespace = base_name(&file_name)?;
        let file_data = match fs::read_to_string(file_name.clone()) {
            Ok(data) => data,
            Err(_) => {
                bail!("Couldn't read config file '{}' to string", file_name);
            }
        };
        let file_config = match ConfigFileSchema::parse(file_data) {
            Ok(c) => c,
            Err(e) => bail!("Parsing file '{}': {}", file_name, e),
        };
        let ConfigFileSchema { file_actions, file_selectors, file_evals, file_tests } = file_config;
        let mut file_metrics = HashMap::new();
        for (key, value) in file_selectors.into_iter() {
            file_metrics.insert(key, Metric::Selector(value));
        }
        for (key, value) in file_evals.into_iter() {
            if file_metrics.contains_key(&key) {
                bail!("Duplicate metric name {} in file {}", key, namespace);
            }
            file_metrics.insert(key, Metric::Eval(value));
        }
        metrics.insert(namespace.clone(), file_metrics);
        actions.insert(namespace.clone(), file_actions);
        tests.insert(namespace, file_tests);
    }
    validate(&metrics, &actions, &tests)?;

    Ok(StateHolder { metrics, actions, inspect_contexts, output_format })
}

fn base_name(path: &String) -> Result<String, Error> {
    let file_path = Path::new(path);
    if let Some(s) = file_path.file_stem() {
        if let Some(s) = s.to_str() {
            return Ok(s.to_owned());
        }
    }
    bail!("Bad path {} - can't find file_stem", path)
}

#[cfg(test)]
mod test {
    use {super::*, anyhow::Error};

    // initialize() will be tested in the integration test: "fx triage --test"
    // TODO(cphoenix) - set up dirs under test/ and test initialize() here.

    #[test]
    fn base_name_works() -> Result<(), Error> {
        assert_eq!(base_name(&"foo/bar/baz.ext".to_string())?, "baz".to_string());
        Ok(())
    }

    #[test]
    fn inspect_data_from_works() -> Result<(), Error> {
        assert!(InspectData::from("foo".to_string()).is_err(), "'foo' isn't valid JSON");
        assert!(InspectData::from(r#"{"a":5}"#.to_string()).is_err(), "Needed an array");
        assert!(InspectData::from("[]".to_string()).is_ok(), "A JSON array should have worked");
        Ok(())
    }

    #[test]
    fn output_format_from_string() -> Result<(), Error> {
        assert_eq!(OutputFormat::from_str("csv")?, OutputFormat::CSV);
        assert_eq!(OutputFormat::from_str("text")?, OutputFormat::Text);
        assert!(OutputFormat::from_str("").is_err(), "Should have returned 'Err' on ''");
        assert!(OutputFormat::from_str("CSV").is_err(), "Should have returned 'Err' on 'CSV'");
        assert!(OutputFormat::from_str("Text").is_err(), "Should have returned 'Err' on 'Text'");
        assert!(
            OutputFormat::from_str("GARBAGE").is_err(),
            "Should have returned 'Err' on 'GARBAGE'"
        );
        Ok(())
    }
}
