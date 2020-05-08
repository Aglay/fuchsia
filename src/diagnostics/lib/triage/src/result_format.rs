// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::act::ActionResults;

pub struct ActionResultFormatter<'a> {
    action_results: Vec<&'a ActionResults>,
}

impl<'a> ActionResultFormatter<'a> {
    pub fn new(action_results: Vec<&ActionResults>) -> ActionResultFormatter<'_> {
        ActionResultFormatter { action_results }
    }

    pub fn to_warnings(&self) -> String {
        if self.action_results.iter().all(|results| results.get_warnings().is_empty()) {
            return String::from("No actions were triggered. All targets OK.");
        }

        let mut output = String::new();

        let warning_output = self
            .action_results
            .iter()
            .filter(|results| !results.get_warnings().is_empty())
            .map(|results| {
                let header =
                    Self::make_underline(&format!("Warnings for target {}", results.source));
                format!("{}{}", header, results.get_warnings().join("\n"))
            })
            .collect::<Vec<String>>()
            .join("\n\n");
        output.push_str(&format!("{}\n", &warning_output));

        let non_warning_output = self
            .action_results
            .iter()
            .filter(|results| results.get_warnings().is_empty())
            .map(|results| {
                let header = Self::make_underline(&format!(
                    "No actions were triggered for target {}",
                    results.source
                ));
                format!("{}", header)
            })
            .collect::<Vec<String>>()
            .join("\n");
        output.push_str(&non_warning_output);

        output
    }

    fn make_underline(content: &str) -> String {
        let mut output = String::new();
        output.push_str(&format!("{}\n", content));
        output.push_str(&format!("{}\n", "-".repeat(content.len())));
        output
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn action_result_formatter_to_warnings_when_no_actions_triggered() {
        let action_results_1 = ActionResults::new("inspect1");
        let action_results_2 = ActionResults::new("inspect2");
        let formatter = ActionResultFormatter::new(vec![&action_results_1, &action_results_2]);

        assert_eq!(
            String::from("No actions were triggered. All targets OK."),
            formatter.to_warnings()
        );
    }

    #[test]
    fn action_result_formatter_to_warnings_when_actions_triggered() {
        let warnings = String::from(
            "Warnings for target inspect1\n\
        ----------------------------\n\
        w1\n\
        w2\n\n\
        Warnings for target inspect2\n\
        ----------------------------\n\
        w3\n\
        w4\n\
        No actions were triggered for target inspect3\n\
        ---------------------------------------------\n",
        );

        let mut action_results_1 = ActionResults::new("inspect1");
        action_results_1.add_warning(String::from("w1"));
        action_results_1.add_warning(String::from("w2"));

        let mut action_results_2 = ActionResults::new("inspect2");
        action_results_2.add_warning(String::from("w3"));
        action_results_2.add_warning(String::from("w4"));

        let action_results_3 = ActionResults::new("inspect3");

        let formatter = ActionResultFormatter::new(vec![
            &action_results_1,
            &action_results_2,
            &action_results_3,
        ]);

        assert_eq!(warnings, formatter.to_warnings());
    }
}
