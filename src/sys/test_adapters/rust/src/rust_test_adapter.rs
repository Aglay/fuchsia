// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_test as ftest,
    fsyslog::{fx_log_err, fx_log_info},
    ftest::{Invocation, TestListenerProxy},
    fuchsia_async as fasync, fuchsia_syslog as fsyslog, fuchsia_zircon as zx,
    futures::io::AsyncWriteExt,
    futures::prelude::*,
    lazy_static::lazy_static,
    regex::Regex,
    serde_derive::Deserialize,
    std::{ffi::CString, str::from_utf8},
};

#[cfg(rust_panic = "unwind")]
use {serde_json as json, std::ffi::CStr};

lazy_static! {
    pub static ref DASH_Z: CString = CString::new("-Z").unwrap();
    pub static ref USTABLE_OPTIONS: CString = CString::new("unstable-options").unwrap();
    pub static ref LIST: CString = CString::new("--list").unwrap();
    pub static ref JSON_FORMAT: CString = CString::new("--format=json").unwrap();
    pub static ref NO_CAPTURE: CString = CString::new("--nocapture").unwrap();
}

/// Holds information about the test to be run
#[derive(Debug, PartialEq)]
pub struct TestInfo {
    pub test_path: String,
    pub test_args: Vec<String>,
}

/// Marks whether a test or suite just started (ResultEvent::Started), passed (ResultEvent::Ok), or
/// failed (ResultEvent::Failed).
#[derive(Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
enum ResultEvent {
    /// A test or a suite of tests has started
    Started,
    /// A test or a suite of tests has failed
    Failed,
    /// A test or a suite of tests has passed
    Ok,
    /// A test was marked as `#[ignore]` and didn't run`
    Ignored,
    /// A test was not run due to the filter that was passed in
    Filtered,
}

/// Marks a message from the rust test as relating to a test suite or an individual test.
#[derive(Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
enum ResultType {
    /// The message pertains to a test suite
    Suite,
    /// This message is about an individual test
    Test,
}

/// This struct holds the deserialized results from running a test. We are not storing all of the
/// values that we get back since we only need to know whether it's a Test or a Suite and whether
/// it passed or not.
///
/// Sample json output:
/// { "type": "suite", "event": "started", "test_count": 1 }
/// { "type": "test", "event": "started", "name": "tests::purposefully_failing_test" }
/// { "type": "test", "name": "tests::purposefully_failing_test", "event": "failed", "stdout": "Rust Test Output" }
/// { "type": "suite", "event": "failed", "passed": 0, "failed": 1, "allowed_fail": 0, "ignored": 0, "measured": 0, "filtered_out": 2 }
///
/// { "type": "suite", "event": "started", "test_count": 1 }
/// { "type": "test", "event": "started", "name": "tests::test_full_path" }
/// { "type": "test", "name": "tests::test_full_path", "event": "ok" }
/// { "type": "suite", "event": "ok", "passed": 1, "failed": 0, "allowed_fail": 0, "ignored": 0, "measured": 0, "filtered_out": 2 }
///
#[derive(Debug, Deserialize)]
struct JsonResult {
    #[serde(rename(deserialize = "type"))]
    test_type: ResultType,
    event: ResultEvent,
    #[serde(rename(deserialize = "stdout"))]
    output: Option<String>,
}

#[derive(Debug)]
pub struct RustTestAdapter {
    test_info: TestInfo,
    c_test_path: CString,
    c_test_file_name: CString,
}

impl RustTestAdapter {
    /// Creates a new RustTestAdapter if the `test_path` is valid
    pub fn new(test_info: TestInfo) -> Result<RustTestAdapter, Error> {
        let test_file_name = test_adapter_lib::extract_test_filename(&test_info.test_path)?;
        Ok(RustTestAdapter {
            c_test_path: CString::new(&test_info.test_path[..])?,
            c_test_file_name: CString::new(&test_file_name[..])?,
            test_info: test_info,
        })
    }

    /// Implements the fuchsia.test.Suite protocol to enumerate and run tests
    pub async fn run_test_suite(&self, mut stream: ftest::SuiteRequestStream) -> Result<(), Error> {
        while let Some(event) =
            stream.try_next().await.context("Failed to get next test suite event")?
        {
            match event {
                ftest::SuiteRequest::GetTests { responder } => {
                    fx_log_info!("gathering tests");
                    let test_cases =
                        self.enumerate_tests().await.context("Failed to enumerate test cases")?;
                    responder
                        .send(
                            &mut test_cases
                                .into_iter()
                                .map(|name| ftest::Case { name: Some(name) }),
                        )
                        .context("Failed to send test cases to fuchsia.test.Suite")?;
                }
                ftest::SuiteRequest::Run { tests, listener, .. } => {
                    let proxy =
                        listener.into_proxy().context("Can't convert listener channel to proxy")?;

                    self.run_tests(tests, proxy).await.context("Failed to run tests")?;
                }
            }
        }
        Ok(())
    }

    /// Runs each of the tests passed in by name
    async fn run_tests(
        &self,
        invocations: Vec<Invocation>,
        proxy: TestListenerProxy,
    ) -> Result<(), Error> {
        fx_log_info!("running tests");
        for invocation in invocations {
            let name = invocation
                .name
                .as_ref()
                .ok_or(format_err!("Name should be present in Invocation"))?
                .to_string();

            let (log_end, test_logger) =
                zx::Socket::create(zx::SocketOpts::empty()).context("Failed to create socket")?;

            let mut test_logger = fasync::Socket::from_socket(test_logger)?;

            let (case_listener_proxy, listener) =
                fidl::endpoints::create_proxy::<fidl_fuchsia_test::TestCaseListenerMarker>()
                    .context("cannot create proxy")?;

            proxy
                .on_test_case_started(invocation, log_end, listener)
                .context("on_test_case_started failed")?;

            match self.run_test(&name, &mut test_logger).await {
                Ok(result) => {
                    case_listener_proxy.finished(result).context("on_test_case_finished failed")?
                }
                Err(error) => {
                    fx_log_err!("failed to run test. {}", error);
                    case_listener_proxy
                        .finished(ftest::Result_ { status: Some(ftest::Status::Failed) })
                        .context("on_test_case_finished failed")?;
                }
            }
        }

        Ok(())
    }

    /// Lauches a process that lists the tests without actually running any of them. It then parses
    /// the output of that process into a vector of strings.
    ///
    /// Example output:
    ///
    /// tests::purposefully_failing_test: test
    /// tests::test_full_path: test
    /// tests::test_minimal_path: test
    ///
    /// 3 tests, 0 benchmarks
    ///
    async fn enumerate_tests(&self) -> Result<Vec<String>, Error> {
        let (process, logger) = test_adapter_lib::launch_process(
            &self.c_test_path,
            &self.c_test_file_name,
            &[&self.c_test_path, &DASH_Z, &USTABLE_OPTIONS, &LIST],
            None,
        )?;

        fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
            .await
            .context("Error waiting for test process to exit")?;

        let process_info = process.info().context("Error getting info from process")?;

        if process_info.return_code != 0 {
            let logs = logger.try_concat().await.context("Failed to concatenate log output")?;
            let output = from_utf8(&logs).context("Failed to convert logs from utf-8")?;
            // TODO(anmittal): Add a error logger to API before porting this to runner so that we
            // can display test stdout logs.
            fx_log_err!("Failed getting list of tests:\n{}", output);
            return Err(format_err!("Can't get list of tests.\noutput: {}", output));
        }

        let output =
            logger.try_concat().await.context("Failed to concatenate the enumerated tests")?;
        let test_list =
            from_utf8(&output).context("Failed to convert logs from utf-8")?.to_string();

        let mut test_names = vec![];
        let regex = Regex::new(r"(.*): test").unwrap();

        for test in test_list.split("\n") {
            if let Some(capture) = regex.captures(test) {
                if let Some(name) = capture.get(1) {
                    test_names.push(String::from(name.as_str()));
                }
            }
        }

        Ok(test_names)
    }

    #[cfg(rust_panic = "unwind")]
    async fn run_test<W: AsyncWriteExt + std::marker::Unpin>(
        &self,
        name: &str,
        test_logger: &mut W,
    ) -> Result<ftest::Result_, Error> {
        let c_test_name = CString::new(name).unwrap();
        let mut args: Vec<&CStr> = vec![&self.c_test_path, &c_test_name];

        let mut c_args: Vec<CString> = vec![];
        c_args.extend(self.test_info.test_args.iter().map(|arg| CString::new(&arg[..]).unwrap()));
        args.extend(c_args.iter().map(|arg| arg.as_ref()));
        args.extend_from_slice(&[&DASH_Z, &USTABLE_OPTIONS, &JSON_FORMAT, &NO_CAPTURE]);

        let (process, logger) = test_adapter_lib::launch_process(
            &self.c_test_path,
            &self.c_test_file_name,
            &args[..],
            None,
        )
        .context("Failed to launch test process")?;

        fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
            .await
            .context("Error waiting for test process to exit")?;

        // TODO(casbor): The process always ends with a return code of 0 for passing tests. In all
        // other cases it returns 101. It doesn't matter if it's an error running the test or if the
        // test just fails. For now we're not going to rely on the return code and see if we can
        // parse the json. If we can then it's a passing or failing test. If we can't, then it's an
        // error and can be reported as such. Tracking bug: https://github.com/rust-lang/rust/issues/67210

        let results = logger.try_concat().await.context("Failed to concatenate the test output")?;
        let json = from_utf8(&results).context("Failed to convert logs from utf-8")?.to_string();
        let json = json.trim();

        for line in json.split("\n") {
            let json_result: Result<JsonResult, serde_json::error::Error> = json::from_str(&line);
            match json_result {
                Ok(result) => {
                    if let ResultType::Test = result.test_type {
                        match result.event {
                            ResultEvent::Failed => {
                                if let Some(output) = result.output {
                                    test_logger
                                        .write(format!("test failed:\n{}", output).as_bytes())
                                        .await
                                        .context("cannot write logs")?;
                                }
                                return Ok(ftest::Result_ { status: Some(ftest::Status::Failed) });
                            }
                            // TODO(b/43756): There isn't an "Ignored" or "Filtered" status yet, so
                            // for now just return that the passed.
                            ResultEvent::Ok | ResultEvent::Ignored | ResultEvent::Filtered => {
                                return Ok(ftest::Result_ { status: Some(ftest::Status::Passed) })
                            }
                            _ => (),
                        }
                    }
                }
                // This isn't json so just pass it on to the console.
                Err(_) => fx_log_info!("{}", line),
            }
        }

        Err(format_err!("Received unknown repsonse from Rust test runner: {}", json))
    }

    /// Launches a process that actually runs the test and parses the resulting json output.
    #[cfg(rust_panic = "abort")]
    async fn run_test<W: AsyncWriteExt + std::marker::Unpin>(
        &self,
        name: &str,
        test_logger: &mut W,
    ) -> Result<ftest::Result_, Error> {
        // Exit codes used by Rust's libtest runner.
        const TR_OK: i64 = 50;
        const TR_FAILED: i64 = 51;

        let args = [self.c_test_path.as_c_str()];

        let test_invoke = CString::new(format!("__RUST_TEST_INVOKE={}", name))?;
        let environ = [test_invoke.as_c_str()];

        let (process, logger) = test_adapter_lib::launch_process(
            &self.c_test_path,
            &self.c_test_file_name,
            &args,
            Some(&environ),
        )
        .context("Failed to launch test process")?;

        let output = logger.try_concat().await.context("Failed to gather test output")?;
        let output = from_utf8(&output).context("Could not convert logs from UTF-8")?;

        fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
            .await
            .context("Error waiting for test process to exit")?;
        let process_info = process.info().context("Error getting info from process")?;

        match process_info.return_code {
            TR_OK => Ok(ftest::Result_ { status: Some(ftest::Status::Passed) }),
            TR_FAILED => {
                test_logger
                    .write(format!("test failed:\n{}", output).as_bytes())
                    .await
                    .context("cannot write logs")?;
                Ok(ftest::Result_ { status: Some(ftest::Status::Failed) })
            }
            other => Err(format_err!(
                "Received unexpected exit code {} from test process\noutput: {}",
                other,
                output
            )),
        }
    }

    fn _check_process_return_code(process: &zx::Process) -> Result<(), Error> {
        let process_info = process.info().context("Error getting info from process")?;
        if process_info.return_code != 0 && process_info.return_code != 101 {
            Err(format_err!("test process returned an error: {}", process_info.return_code))
        } else {
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl_fuchsia_test::{
            TestCaseListenerRequest::Finished, TestListenerMarker,
            TestListenerRequest::OnTestCaseStarted, TestListenerRequestStream,
        },
        std::cmp::PartialEq,
    };

    fn names_to_invocation(names: Vec<&str>) -> Vec<Invocation> {
        names.iter().map(|s| Invocation { name: Some(s.to_string()), tag: None }).collect()
    }

    #[derive(PartialEq, Debug)]
    enum ListenerEvent {
        StartTest(String),
        FinishTest(String, ftest::Result_),
    }

    async fn collect_results(
        mut listener: TestListenerRequestStream,
    ) -> Result<Vec<ListenerEvent>, Error> {
        let mut events = vec![];
        while let Some(result_event) = listener.try_next().await? {
            match result_event {
                OnTestCaseStarted { invocation, primary_log: _, listener, .. } => {
                    let name = invocation.name.unwrap();
                    events.push(ListenerEvent::StartTest(name.clone()));
                    let mut listener = listener.into_stream()?;
                    while let Some(result) = listener.try_next().await? {
                        match result {
                            Finished { result, .. } => {
                                events.push(ListenerEvent::FinishTest(name, result));
                                break;
                            }
                        }
                    }
                }
            }
        }
        Ok(events)
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn no_tests() {
        let test_info =
            TestInfo { test_path: String::from("/pkg/bin/no_rust_tests"), test_args: vec![] };

        let adapter = RustTestAdapter::new(test_info).expect("Cannot create adapter");

        let expected_tests: Vec<String> = vec![];
        let actual_tests = adapter.enumerate_tests().await.expect("Can't enumerate tests");

        assert_eq!(expected_tests, actual_tests);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn enumerate_simple_tests() {
        let test_info =
            TestInfo { test_path: String::from("/pkg/bin/simple_rust_tests"), test_args: vec![] };

        let adapter = RustTestAdapter::new(test_info).expect("Cannot create adapter");

        let mut expected_tests = vec![
            String::from("tests::simple_test_one"),
            String::from("tests::simple_test_two"),
            String::from("tests::simple_test_three"),
            String::from("tests::simple_test_four"),
        ];

        expected_tests.sort();

        let actual_tests = adapter.enumerate_tests().await.expect("Can't enumerate tests");

        assert_eq!(expected_tests, actual_tests);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_passing_test() {
        let test_info =
            TestInfo { test_path: String::from("/pkg/bin/test_results"), test_args: vec![] };

        let adapter = RustTestAdapter::new(test_info).expect("Cannot create adapter");
        let test_name = String::from("tests::a_passing_test");
        let mut logs = vec![];
        let result = adapter.run_test(&test_name, &mut logs).await.expect("Failed to run test");

        assert_eq!(ftest::Result_ { status: Some(ftest::Status::Passed) }, result);
        assert_eq!(logs, vec![]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_failing_test() {
        let test_info =
            TestInfo { test_path: String::from("/pkg/bin/test_results"), test_args: vec![] };

        let adapter = RustTestAdapter::new(test_info).expect("Cannot create adapter");
        let test_name = String::from("tests::b_failing_test");
        let mut logs = vec![];
        let result = adapter.run_test(&test_name, &mut logs).await.expect("Failed to run test");

        assert_eq!(ftest::Result_ { status: Some(ftest::Status::Failed) }, result);
        let logs = from_utf8(&logs).unwrap();
        assert!(logs.contains("I'm supposed panic!()"));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_entire_test_suite() {
        let test_info =
            TestInfo { test_path: String::from("/pkg/bin/test_results"), test_args: vec![] };

        let adapter = RustTestAdapter::new(test_info).expect("Cannot create adapter");

        let tests = names_to_invocation(vec![
            "tests::a_passing_test",
            "tests::b_failing_test",
            "tests::c_passing_test",
        ]);

        let (test_listener_client, test_listener) =
            fidl::endpoints::create_request_stream::<TestListenerMarker>()
                .expect("failed to create test_listener");
        let proxy = test_listener_client.into_proxy().expect("can't convert listener into proxy");

        let run_future = adapter.run_tests(tests, proxy);
        let result_future = collect_results(test_listener);

        let (run_option, result_option) = future::join(run_future, result_future).await;

        run_option.expect("Failed running tests");

        let expected_results = vec![
            ListenerEvent::StartTest(String::from("tests::a_passing_test")),
            ListenerEvent::FinishTest(
                String::from("tests::a_passing_test"),
                ftest::Result_ { status: Some(ftest::Status::Passed) },
            ),
            ListenerEvent::StartTest(String::from("tests::b_failing_test")),
            ListenerEvent::FinishTest(
                String::from("tests::b_failing_test"),
                ftest::Result_ { status: Some(ftest::Status::Failed) },
            ),
            ListenerEvent::StartTest(String::from("tests::c_passing_test")),
            ListenerEvent::FinishTest(
                String::from("tests::c_passing_test"),
                ftest::Result_ { status: Some(ftest::Status::Passed) },
            ),
        ];

        let actual_results = result_option.expect("Failed to collect results");

        assert_eq!(expected_results, actual_results);
    }

    #[test]
    fn invalid_file() {
        let test_info =
            TestInfo { test_path: String::from("/pkg/bin/invalid_test_file"), test_args: vec![] };
        RustTestAdapter::new(test_info).expect_err("This should fail");
    }
}
