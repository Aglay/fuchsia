// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

use futures::prelude::*;
use regex::{Regex, RegexSet};
use serde::Serialize;
use std::{borrow::Cow, sync::Arc};

mod serialize;
pub use serialize::{Redacted, RedactedItem};

pub const UNREDACTED_CANARY_MESSAGE: &str = "Log redaction canary: \
    Email: alice@website.tld, \
    IPv4: 8.8.8.8, \
    IPv6: 2001:503:eEa3:0:0:0:0:30, \
    UUID: ddd0fA34-1016-11eb-adc1-0242ac120002";

pub const REDACTED_CANARY_MESSAGE: &str = "Log redaction canary: \
    Email: <REDACTED>, IPv4: <REDACTED>, IPv6: <REDACTED>, UUID: <REDACTED>";

pub fn emit_canary() {
    tracing::info!("{}", UNREDACTED_CANARY_MESSAGE);
}

/// A `Redactor` is responsible for removing text patterns that seem like user data in logs.
pub struct Redactor {
    /// Used to determine which regexes match, each pattern has the same index as in `replacements`.
    to_redact: RegexSet,

    /// Used to replace substrings of matching text, each pattern has the same index as in
    /// `to_redact`.
    replacements: Vec<Regex>,
}

const REPLACEMENT: &str = "<REDACTED>";
const KNOWN_BAD_PATTERNS: &[&str] = &[
    // Email stub alice@website.tld
    r"[a-zA-Z0-9]*@[a-zA-Z0-9]*\.[a-zA-Z]*",
    // IPv4 Address
    r"((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\.){3,3}(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])",
    // IPv6
    r"(?:[a-fA-F0-9]{1,4}:){7}[a-fA-F0-9]{1,4}",
    // uuid
    r"[0-9a-fA-F]{8}\b-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-\b[0-9a-fA-F]{12}",
    // mac address
    r"([0-9a-fA-F]{1,2}([\.:-])){5}[0-9a-fA-F]{1,2}",
];

impl Redactor {
    pub fn noop() -> Self {
        Self::new(&[]).unwrap()
    }

    pub fn with_static_patterns() -> Self {
        Self::new(KNOWN_BAD_PATTERNS).unwrap()
    }

    fn new(patterns: &[&str]) -> Result<Self, regex::Error> {
        let replacements = patterns.iter().map(|p| Regex::new(p)).collect::<Result<Vec<_>, _>>()?;
        let to_redact = RegexSet::new(patterns)?;
        Ok(Self { to_redact, replacements })
    }

    /// Replace any instances of this redactor's patterns with the value of [`REPLACEMENT`].
    pub fn redact_text<'t>(&self, text: &'t str) -> Cow<'t, str> {
        let mut redacted = Cow::Borrowed(text);
        for idx in self.to_redact.matches(text) {
            redacted =
                Cow::Owned(self.replacements[idx].replace_all(&redacted, REPLACEMENT).to_string());
        }
        redacted
    }

    /// Returns a wrapper around `item` which implements [`serde::Serialize`], redacting from
    /// any strings in `item`, recursively.
    pub fn redact<'m, 'r, M>(&'r self, item: &'m M) -> Redacted<'m, 'r, M>
    where
        M: ?Sized + Serialize,
    {
        Redacted { inner: item, redactor: self }
    }

    pub fn redact_stream<M: Serialize + 'static>(
        self: &Arc<Self>,
        stream: impl Stream<Item = Arc<M>>,
    ) -> impl Stream<Item = RedactedItem<M>> {
        let redactor = self.clone();
        stream.map(move |inner| RedactedItem { inner, redactor: redactor.clone() })
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::logs::message::{Message, Severity, TEST_IDENTITY};
    use diagnostics_data::{LogsField, LogsHierarchy, LogsProperty};
    use futures::stream::iter as iter2stream;
    use std::sync::Arc;

    fn test_message(contents: &str) -> Message {
        Message::new(
            0u64, // time
            Severity::Info,
            0, // size
            0, // dropped_logs
            &*TEST_IDENTITY,
            LogsHierarchy::new(
                "root",
                vec![LogsProperty::String(LogsField::Msg, contents.to_string())],
                vec![],
            ),
        )
    }

    macro_rules! test_redaction {
        ($($test_name:ident: $input:expr => $output:expr,)+) => {
        paste::paste!{$(
            #[test]
            fn [<redact_ $test_name>] () {
                let noop = Redactor::noop();
                let real = Redactor::with_static_patterns();
                assert_eq!(noop.redact_text($input), $input, "no-op redaction must match input exactly");
                assert_eq!(real.redact_text($input), $output);
            }

            #[test]
            fn [<redact_json_ $test_name>] () {
                let input = test_message($input);
                let output = test_message($output);
                let noop = Redactor::noop();
                let real = Redactor::with_static_patterns();

                let input_json = serde_json::to_string_pretty(&input).unwrap();
                let expected_json = serde_json::to_string_pretty(&output).unwrap();
                let noop_json = serde_json::to_string_pretty(&noop.redact(&input)).unwrap();
                let real_json = serde_json::to_string_pretty(&real.redact(&input)).unwrap();

                assert_eq!(noop_json, input_json, "no-op redaction must match input exactly");
                assert_eq!(real_json, expected_json);
            }
        )+}

            #[fuchsia_async::run_singlethreaded(test)]
            async fn redact_all_in_stream() {
                let inputs = vec![$( Arc::new(test_message($input)), )+];
                let outputs = vec![$( Arc::new(test_message($output)), )+];

                let noop = Arc::new(Redactor::noop());
                let real = Arc::new(Redactor::with_static_patterns());

                let input_stream = iter2stream(inputs.clone());
                let noop_stream = noop.redact_stream(iter2stream(inputs.clone()));
                let real_stream = real.redact_stream(iter2stream(inputs.clone()));
                let output_stream = iter2stream(outputs);
                let mut all_streams =
                    input_stream.zip(noop_stream).zip(real_stream).zip(output_stream);

                while let Some((((input, noop), real), output)) = all_streams.next().await {
                    let input_json = serde_json::to_string_pretty(&*input).unwrap();
                    let expected_json = serde_json::to_string_pretty(&*output).unwrap();
                    let noop_json = serde_json::to_string_pretty(&noop).unwrap();
                    let real_json = serde_json::to_string_pretty(&real).unwrap();

                    assert_eq!(noop_json, input_json, "no-op redaction must match input exactly");
                    assert_eq!(real_json, expected_json);
                }
            }
        };
    }

    test_redaction! {
        email: "Email: alice@website.tld" => "Email: <REDACTED>",
        ipv4: "IPv4: 8.8.8.8" => "IPv4: <REDACTED>",
        ipv6: "IPv6: 2001:503:eEa3:0:0:0:0:30" => "IPv6: <REDACTED>",
        uuid: "UUID: ddd0fA34-1016-11eb-adc1-0242ac120002" => "UUID: <REDACTED>",
        mac_address: "MAC address: 00:0a:95:9F:68:16" => "MAC address: <REDACTED>",
        combined: "Combined: Email alice@website.tld, IPv4 8.8.8.8" =>
                "Combined: Email <REDACTED>, IPv4 <REDACTED>",
        canary: UNREDACTED_CANARY_MESSAGE => REDACTED_CANARY_MESSAGE,
    }
}
