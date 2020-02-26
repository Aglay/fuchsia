// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_test::{Invocation, Result_ as TestResult, Status, TestListenerProxy},
    fuchsia_async as fasync,
    fuchsia_syslog::{fx_log_err, fx_log_info},
    fuchsia_zircon as zx,
    futures::prelude::*,
    serde_derive::{Deserialize, Serialize},
    std::{ffi::CString, fmt, fs, fs::File, io::BufReader, str::from_utf8},
    // TODO(anmittal): don't add this dependency in runner.
    uuid::Uuid,
};

/// Provides info about individual test cases.
/// Example: For test FOO.Bar, this contains info about Bar.
/// Please refer to documentation of `ListTestResult` for details.
#[derive(Serialize, Deserialize, Debug)]
struct IndividualTestInfo {
    pub name: String,
    pub file: String,
    pub line: u64,
}

/// Provides info about individual test suites.
/// Example: For test FOO.Bar, this contains info about FOO.
/// Please refer to documentation of `ListTestResult` for details.
#[derive(Serialize, Deserialize, Debug)]
struct TestSuiteResult {
    pub tests: usize,
    pub name: String,
    pub testsuite: Vec<IndividualTestInfo>,
}

/// Sample json will look like
/// ```
/// {
/// "tests": 6,
/// "name": "AllTests",
/// "testsuites": [
///    {
///      "name": "SampleTest1",
///      "tests": 2,
///      "testsuite": [
///        {
///          "name": "Test1",
///          "file": "../../src/sys/test_adapters/gtest/test_data/sample_tests.cc",
///          "line": 7
///        },
///        {
///          "name": "Test2",
///          "file": "../../src/sys/test_adapters/gtest/test_data/sample_tests.cc",
///          "line": 9
///        }
///      ]
///    },
///  ]
///}
///```
#[derive(Serialize, Deserialize, Debug)]
struct ListTestResult {
    pub tests: usize,
    pub name: String,
    pub testsuites: Vec<TestSuiteResult>,
}

/// Provides info about test case failures if any.
#[derive(Serialize, Deserialize, Debug)]
struct Failures {
    pub failure: String,
}

/// Provides info about individual test cases.
/// Example: For test FOO.Bar, this contains info about Bar.
/// Please refer to documentation of `TestOutput` for details.
#[derive(Serialize, Deserialize, Debug)]
struct IndividualTestOutput {
    pub name: String,
    pub status: String,
    pub time: String,
    pub failures: Option<Vec<Failures>>,
}

/// Provides info about individual test suites.
/// Refer to https://github.com/google/googletest/blob/2002f267f05be6f41a3d458954414ba2bfa3ff1d/googletest/docs/advanced.md#generating-a-json-report
/// for output structure.
#[derive(Serialize, Deserialize, Debug)]
struct TestSuiteOutput {
    pub name: String,
    pub tests: usize,
    pub failures: usize,
    pub disabled: usize,
    pub time: String,
    pub testsuite: Vec<IndividualTestOutput>,
}

/// Provides info test and the its run result.
/// Example: For test FOO.Bar, this contains info about FOO.
/// Please refer to documentation of `TestOutput` for details.
#[derive(Serialize, Deserialize, Debug)]
struct TestOutput {
    pub testsuites: Vec<TestSuiteOutput>,
}

/// This structure will try to delete underlying file when it goes out of scope.
/// This doesn't return error or doesn't panic if remove file operation fails.
/// We base this object on filename because this adapter is not the one to create it,
/// it is created by test process.
struct FileScope {
    file_name: String,
}

impl FileScope {
    fn new(name: String) -> Self {
        Self { file_name: name }
    }

    fn file_name(&self) -> &str {
        &self.file_name
    }
}

impl Drop for FileScope {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.file_name);
    }
}

impl fmt::Display for FileScope {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.file_name)
    }
}

#[derive(Debug)]
pub struct GTestAdapter {
    c_test_path: CString,
    c_test_file_name: CString,
    test_file_name: String,
}

impl GTestAdapter {
    /// Creates a new GTest adapter if `test_path` is valid
    pub fn new(test_path: String) -> Result<GTestAdapter, Error> {
        let test_file_name = test_adapter_lib::extract_test_filename(&test_path)?;

        Ok(GTestAdapter {
            c_test_path: CString::new(&test_path[..])?,
            c_test_file_name: CString::new(&test_file_name[..])?,
            test_file_name: test_file_name,
        })
    }

    /// Runs tests defined by `tests_names` and uses `test_listener` to send test events.
    // TODO(anmittal): Support disabled tests.
    // TODO(anmittal): Don't run tests which are not present in test file or handle them.
    // TODO(anmittal): Support test stdout, or devise a mechanism to replace it.
    pub async fn run_tests(
        &self,
        invocations: Vec<Invocation>,
        test_listener: TestListenerProxy,
    ) -> Result<(), Error> {
        for invocation in invocations {
            let test = invocation
                .name
                .as_ref()
                .ok_or(format_err!("Name should be present in Invocation"))?
                .to_string();

            fx_log_info!("Running test {}", test);
            let test_result_file =
                FileScope::new(format!("/tmp/test_result_{}.json", Uuid::new_v4().to_simple()));

            let (process, logger) = test_adapter_lib::launch_process(
                &self.c_test_path,
                &self.c_test_file_name,
                &[
                    &self.c_test_file_name,
                    &CString::new(format!("--gtest_filter={}", test))?,
                    &CString::new(format!("--gtest_output=json:{}", test_result_file))?,
                ],
                None,
            )?;

            let (test_logger, log_client) =
                zx::Socket::create(zx::SocketOpts::DATAGRAM).context("cannot create socket")?;

            let (case_listener_proxy, listener) =
                fidl::endpoints::create_proxy::<fidl_fuchsia_test::TestCaseListenerMarker>()
                    .context("cannot create proxy")?;

            test_listener
                .on_test_case_started(invocation, log_client, listener)
                .context("Cannot send start event")?;

            let mut test_logger = fasync::Socket::from_socket(test_logger)?;

            // collect stdout in background before waiting for process termination.
            let (logger_handle, logger_fut) = logger.try_concat().remote_handle();
            fasync::spawn_local(async move {
                logger_handle.await;
            });

            fx_log_info!("waiting for test to finish: {}", test);

            fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
                .await
                .context("Error waiting for test process to exit")?;

            fx_log_info!("collecting logs for {}", test);
            let logs = logger_fut.await?;
            let output = from_utf8(&logs)?;

            fx_log_info!("open output file for {}", test);
            // Open the file in read-only mode with buffer.
            // TODO(anmittal): Convert this to async ops.
            let output_file = if let Ok(f) = File::open(test_result_file.file_name()) {
                f
            } else {
                // TODO(anmittal): Introduce Status::InternalError.
                test_logger
                    .write(format!("test did not complete, test output:\n{}", output).as_bytes())
                    .await
                    .context("cannot write logs")?;

                case_listener_proxy
                    .finished(TestResult { status: Some(Status::Failed) })
                    .context("Cannot send finish event")?;
                continue; // run next test
            };

            let reader = BufReader::new(output_file);

            fx_log_info!("parse output file for {}", test);
            let test_list: TestOutput =
                serde_json::from_reader(reader).context("Can't get test result")?;

            fx_log_info!("parsed output file for {}", test);
            if test_list.testsuites.len() != 1 || test_list.testsuites[0].testsuite.len() != 1 {
                // TODO(anmittal): Introduce Status::InternalError.
                test_logger
                    .write(format!("unexpected output:\n{}", output).as_bytes())
                    .await
                    .context("cannot write logs")?;

                case_listener_proxy
                    .finished(TestResult { status: Some(Status::Failed) })
                    .context("Cannot send finish event")?;
                continue; // run next test
            }

            // as we only run one test per iteration result would be always at 0 index in the arrays.
            match &test_list.testsuites[0].testsuite[0].failures {
                Some(failures) => {
                    for f in failures {
                        test_logger
                            .write(format!("failure: {}\n", f.failure).as_bytes())
                            .await
                            .context("cannot write logs")?;
                    }
                    case_listener_proxy
                        .finished(TestResult { status: Some(Status::Failed) })
                        .context("Cannot send finish event")?;
                }
                None => {
                    case_listener_proxy
                        .finished(TestResult { status: Some(Status::Passed) })
                        .context("Cannot send finish event")?;
                }
            }
            fx_log_info!("test finish {}", test);
        }
        Ok(())
    }

    /// Launches test process and gets test list out. Returns list of tests names in the format
    /// defined by gtests, i.e FOO.Bar
    pub async fn enumerate_tests(&self) -> Result<Vec<String>, Error> {
        let test_list_file =
            FileScope::new(format!("/tmp/test_list_{}.json", Uuid::new_v4().to_simple()));

        let (process, logger) = test_adapter_lib::launch_process(
            &self.c_test_path,
            &self.c_test_file_name,
            &[
                &self.c_test_file_name,
                &CString::new("--gtest_list_tests")?,
                &CString::new(format!("--gtest_output=json:{}", test_list_file))?,
            ],
            None,
        )?;

        fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
            .await
            .context("Error waiting for test process to exit")?;

        let process_info = process.info().context("Error getting info from process")?;

        if process_info.return_code != 0 {
            let logs = logger.try_concat().await?;
            let output = from_utf8(&logs)?;
            // TODO(anmittal): Add a error logger to API before porting this to runner so that we
            // can display test stdout logs.
            fx_log_err!("Failed getting list of tests:\n{}", output);
            return Err(format_err!("Can't get list of tests. check logs"));
        }

        // Open the file in read-only mode with buffer.
        let open_file_result = File::open(test_list_file.file_name());
        if let Err(e) = open_file_result {
            let logs = logger.try_concat().await?;
            let output = from_utf8(&logs)?;
            fx_log_err!("Failed getting list of tests from\n{}", output);
            return Err(e.into());
        }

        let output_file = open_file_result?;

        let reader = BufReader::new(output_file);

        let test_list: ListTestResult =
            serde_json::from_reader(reader).context("Can't get test from gtest")?;

        let mut tests = Vec::<String>::with_capacity(test_list.tests);

        for suite in &test_list.testsuites {
            for test in &suite.testsuite {
                tests.push(format!("{}.{}", suite.name, test.name))
            }
        }

        return Ok(tests);
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
        std::path::Path,
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn empty_test_file() {
        let adapter =
            GTestAdapter::new("/pkg/bin/no_tests".to_owned()).expect("Cannot create adapter");
        let tests = adapter.enumerate_tests().await.expect("Can't enumerate tests");
        assert_eq!(tests.len(), 0, "got {:?}", tests);
    }

    fn names_to_invocation(names: Vec<&str>) -> Vec<Invocation> {
        names.iter().map(|s| Invocation { name: Some(s.to_string()), tag: None }).collect()
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn sample_test_file() {
        let adapter =
            GTestAdapter::new("/pkg/bin/sample_tests".to_owned()).expect("Cannot create adapter");
        let tests = adapter.enumerate_tests().await.expect("Can't enumerate tests");
        assert_eq!(
            tests,
            vec![
                "SampleTest1.SimpleFail".to_owned(),
                "SampleTest1.Crashing".to_owned(),
                "SampleTest2.SimplePass".to_owned(),
                "SampleFixture.Test1".to_owned(),
                "SampleFixture.Test2".to_owned(),
                "SampleDisabled.DISABLED_Test1".to_owned(),
                "Tests/SampleParameterizedTestFixture.Test/0".to_owned(),
                "Tests/SampleParameterizedTestFixture.Test/1".to_owned(),
                "Tests/SampleParameterizedTestFixture.Test/2".to_owned(),
                "Tests/SampleParameterizedTestFixture.Test/3".to_owned(),
            ]
        );
    }

    #[derive(PartialEq, Debug)]
    enum ListenerEvent {
        StartTest(String),
        FinishTest(String, TestResult),
    }

    async fn collect_listener_event(
        mut listener: TestListenerRequestStream,
    ) -> Result<Vec<ListenerEvent>, Error> {
        let mut ret = vec![];
        // collect loggers so that they do not die.
        let mut loggers = vec![];
        while let Some(result_event) = listener.try_next().await? {
            match result_event {
                OnTestCaseStarted { invocation, primary_log, listener, .. } => {
                    let name = invocation.name.unwrap();
                    ret.push(ListenerEvent::StartTest(name.clone()));
                    loggers.push(primary_log);

                    let mut listener = listener.into_stream()?;
                    while let Some(result) = listener.try_next().await? {
                        match result {
                            Finished { result, .. } => {
                                ret.push(ListenerEvent::FinishTest(name, result));
                                break;
                            }
                        }
                    }
                }
            }
        }
        Ok(ret)
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_multiple_tests() {
        fuchsia_syslog::init_with_tags(&["gtest_adapter"]).expect("cannot init logger");

        let (test_listener_client, test_listener) =
            fidl::endpoints::create_request_stream::<TestListenerMarker>()
                .expect("Failed to create test_listener");
        let adapter =
            GTestAdapter::new("/pkg/bin/sample_tests".to_owned()).expect("Cannot create adapter");

        let run_fut = adapter.run_tests(
            names_to_invocation(vec![
                "SampleTest1.SimpleFail",
                "SampleTest1.Crashing",
                "SampleTest2.SimplePass",
                "Tests/SampleParameterizedTestFixture.Test/2",
            ]),
            test_listener_client.into_proxy().expect("Can't convert listener into proxy"),
        );

        let result_fut = collect_listener_event(test_listener);

        let (result, events_result) = future::join(run_fut, result_fut).await;
        result.expect("Failed to run tests");

        let events = events_result.expect("Failed to collect events");

        let expected_events = vec![
            ListenerEvent::StartTest("SampleTest1.SimpleFail".to_owned()),
            ListenerEvent::FinishTest(
                "SampleTest1.SimpleFail".to_owned(),
                TestResult { status: Some(Status::Failed) },
            ),
            ListenerEvent::StartTest("SampleTest1.Crashing".to_owned()),
            ListenerEvent::FinishTest(
                "SampleTest1.Crashing".to_owned(),
                TestResult { status: Some(Status::Failed) },
            ),
            ListenerEvent::StartTest("SampleTest2.SimplePass".to_owned()),
            ListenerEvent::FinishTest(
                "SampleTest2.SimplePass".to_owned(),
                TestResult { status: Some(Status::Passed) },
            ),
            ListenerEvent::StartTest("Tests/SampleParameterizedTestFixture.Test/2".to_owned()),
            ListenerEvent::FinishTest(
                "Tests/SampleParameterizedTestFixture.Test/2".to_owned(),
                TestResult { status: Some(Status::Passed) },
            ),
        ];

        assert_eq!(expected_events, events);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_no_test() {
        let (test_listener_client, test_listener) =
            fidl::endpoints::create_request_stream::<TestListenerMarker>()
                .expect("Failed to create test_listener");
        let adapter =
            GTestAdapter::new("/pkg/bin/sample_tests".to_owned()).expect("Cannot create adapter");

        let run_fut = adapter.run_tests(
            vec![],
            test_listener_client.into_proxy().expect("Can't convert listener into proxy"),
        );

        let result_fut = collect_listener_event(test_listener);

        let (result, events_result) = future::join(run_fut, result_fut).await;
        result.expect("Failed to run tests");

        let events = events_result.expect("Failed to collect events");
        assert_eq!(events.len(), 0);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_one_test() {
        let (test_listener_client, test_listener) =
            fidl::endpoints::create_request_stream::<TestListenerMarker>()
                .expect("Failed to create test_listener");
        let adapter =
            GTestAdapter::new("/pkg/bin/sample_tests".to_owned()).expect("Cannot create adapter");

        let run_fut = adapter.run_tests(
            names_to_invocation(vec!["SampleTest2.SimplePass"]),
            test_listener_client.into_proxy().expect("Can't convert listener into proxy"),
        );

        let result_fut = collect_listener_event(test_listener);

        let (result, events_result) = future::join(run_fut, result_fut).await;
        result.expect("Failed to run tests");

        let events = events_result.expect("Failed to collect events");

        let expected_events = vec![
            ListenerEvent::StartTest("SampleTest2.SimplePass".to_owned()),
            ListenerEvent::FinishTest(
                "SampleTest2.SimplePass".to_owned(),
                TestResult { status: Some(Status::Passed) },
            ),
        ];

        assert_eq!(expected_events, events);
    }

    #[test]
    fn invalid_file() {
        GTestAdapter::new("/pkg/bin/invalid_test_file".to_owned()).expect_err("This should fail");
    }

    #[test]
    fn file_scope_works_when_no_file_exists() {
        let _file = FileScope::new("/tmp/file_scope_works_when_no_file_exists".to_owned());

        // should not crash when there is no file with this name.
    }

    #[test]
    fn file_scope_works() {
        let file_name = "/tmp/file_scope_works";

        {
            let _file = FileScope::new(file_name.to_string());

            File::create(file_name).expect("should not fail");

            assert!(Path::new(file_name).exists());
        }

        assert!(!Path::new(file_name).exists());
    }
}
