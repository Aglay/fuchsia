// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{
        act::Actions,
        config::ParseResult,
        metrics::{fetch::InspectFetcher, MetricState, MetricValue},
    },
    anyhow::{format_err, Error},
    serde::Deserialize,
    serde_json as json,
    std::{collections::HashMap, convert::TryFrom},
};

#[derive(Clone, Deserialize, Debug)]
pub struct Trial {
    yes: Vec<String>,
    no: Vec<String>,
    inspect: Vec<json::Value>,
}

// Outer String is namespace, inner String is trial name
pub type Trials = HashMap<String, TrialsSchema>;
pub type TrialsSchema = HashMap<String, Trial>;

pub fn validate(parse_result: &ParseResult) -> Result<(), Error> {
    let ParseResult { metrics, actions, tests } = parse_result;
    let mut failed = false;
    for (namespace, trial_map) in tests {
        for (trial_name, trial) in trial_map {
            let inspect = InspectFetcher::try_from(trial.inspect.clone())?;
            let state = MetricState { metrics, inspect: &inspect };
            for action_name in trial.yes.clone().into_iter() {
                failed = check_failure(namespace, trial_name, &action_name, actions, &state, true)
                    || failed;
            }
            for action_name in trial.no.clone().into_iter() {
                failed = check_failure(namespace, trial_name, &action_name, actions, &state, false)
                    || failed;
            }
        }
    }
    if failed {
        return Err(format_err!("Config validation test failed"));
    } else {
        Ok(())
    }
}

// Returns true iff the trial did NOT get the expected result.
fn check_failure(
    namespace: &String,
    trial_name: &String,
    action_name: &String,
    actions: &Actions,
    metric_state: &MetricState<'_>,
    expected: bool,
) -> bool {
    match actions.get(namespace) {
        None => {
            println!("Namespace {} not found in trial {}", action_name, trial_name);
            return true;
        }
        Some(action_map) => match action_map.get(action_name) {
            None => {
                println!("Action {} not found in trial {}", action_name, trial_name);
                return true;
            }
            Some(action) => match metric_state.metric_value(namespace, &action.trigger) {
                MetricValue::Bool(actual) if actual == expected => return false,
                other => {
                    println!(
                        "Test {} failed: trigger '{}' of action {} returned {:?}, expected {}",
                        trial_name, action.trigger, action_name, other, expected
                    );
                    return true;
                }
            },
        },
    }
}

#[cfg(test)]
mod test {
    use {super::*, crate::act::Action, crate::metrics::Metric, anyhow::Error};

    macro_rules! build_map {($($tuple:expr),*) => ({
        let mut map = HashMap::new();
        $(
            let (key, value) = $tuple;
            map.insert(key.to_string(), value);
         )*
            map
    })}

    macro_rules! create_parse_result {
        (metrics: $metrics:expr, actions: $actions:expr, tests: $tests:expr) => {
            ParseResult {
                metrics: $metrics.clone(),
                actions: $actions.clone(),
                tests: $tests.clone(),
            }
        };
    }

    #[test]
    fn validate_works() -> Result<(), Error> {
        let metrics = build_map!((
            "foo",
            build_map!(
                ("true", Metric::Eval("1==1".to_string())),
                ("false", Metric::Eval("1==0".to_string()))
            )
        ));
        let actions = build_map!((
            "foo",
            build_map!(
                (
                    "fires",
                    Action {
                        trigger: Metric::Eval("true".to_string()),
                        print: "good".to_string(),
                        tag: None
                    }
                ),
                (
                    "inert",
                    Action {
                        trigger: Metric::Eval("false".to_string()),
                        print: "what?!?".to_string(),
                        tag: None
                    }
                )
            )
        ));
        let good_trial = Trial {
            yes: vec!["fires".to_string()],
            no: vec!["inert".to_string()],
            inspect: vec![],
        };
        assert!(validate(&create_parse_result!(
            metrics: metrics,
            actions: actions,
            tests: build_map!(("foo", build_map!(("good", good_trial))))
        ))
        .is_ok());
        // Make sure it objects if a trial that should fire doesn't.
        // Also, make sure it signals failure if there's both a good and a bad trial.
        let bad_trial = Trial {
            yes: vec!["fires".to_string(), "inert".to_string()],
            no: vec![],
            inspect: vec![],
        };
        let good_trial = Trial {
            yes: vec!["fires".to_string()],
            no: vec!["inert".to_string()],
            inspect: vec![],
        };
        assert!(validate(&create_parse_result!(
            metrics: metrics,
            actions: actions,
            tests: build_map!(("foo", build_map!(("good", good_trial), ("bad", bad_trial))))
        ))
        .is_err());
        // Make sure it objects if a trial fires when it shouldn't.
        let bad_trial = Trial {
            yes: vec![],
            no: vec!["fires".to_string(), "inert".to_string()],
            inspect: vec![],
        };
        assert!(validate(&create_parse_result!(
            metrics: metrics,
            actions: actions,
            tests: build_map!(("foo", build_map!(("bad", bad_trial))))
        ))
        .is_err());
        Ok(())
    }
}
