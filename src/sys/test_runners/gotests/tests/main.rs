// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    futures::{channel::mpsc, prelude::*},
    pretty_assertions::assert_eq,
    regex::Regex,
    test_executor::GroupByTestCase,
    test_executor::{DisabledTestHandling, TestEvent, TestResult},
};

async fn run_test(
    test_url: &str,
    disabled_tests: DisabledTestHandling,
    parallel: Option<u16>,
    test_args: Vec<String>,
) -> Result<Vec<TestEvent>, Error> {
    let time_taken = Regex::new(r" \(.*?\)$").unwrap();
    let harness = test_runners_test_lib::connect_to_test_manager().await?;
    let suite_instance = test_executor::SuiteInstance::new(&harness, test_url).await?;

    let (sender, recv) = mpsc::channel(1);

    let run_options =
        test_executor::TestRunOptions { disabled_tests, parallel, arguments: test_args };

    let (events, ()) = futures::future::try_join(
        recv.collect::<Vec<_>>().map(Ok),
        suite_instance.run_and_collect_results(sender, None, run_options),
    )
    .await
    .context("running test")?;

    let mut test_events = test_runners_test_lib::process_events(events, false);

    for event in test_events.iter_mut() {
        match event {
            TestEvent::StdoutMessage { test_case_name, msg } => {
                let log = time_taken.replace(&msg, "");
                *event = TestEvent::stdout_message(test_case_name, &log);
            }
            _ => {}
        }
    }

    Ok(test_events)
}

#[fuchsia_async::run_singlethreaded(test)]
async fn launch_and_test_echo_test() {
    let test_url = "fuchsia-pkg://fuchsia.com/go-test-runner-example#meta/echo-test-realm.cm";
    let events = run_test(test_url, DisabledTestHandling::Exclude, Some(10), vec![]).await.unwrap();

    let expected_events = vec![
        TestEvent::test_case_started("TestEcho"),
        TestEvent::test_case_finished("TestEcho", TestResult::Passed),
        TestEvent::test_finished(),
    ];
    assert_eq!(expected_events, events);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn launch_and_test_file_with_no_test() {
    let test_url = "fuchsia-pkg://fuchsia.com/go-test-runner-example#meta/empty_go_test.cm";
    let events = run_test(test_url, DisabledTestHandling::Exclude, Some(10), vec![]).await.unwrap();

    let expected_events = vec![TestEvent::test_finished()];
    assert_eq!(expected_events, events);
}

async fn launch_and_run_sample_test_helper(parallel: Option<u16>) {
    let test_url = "fuchsia-pkg://fuchsia.com/go-test-runner-example#meta/sample_go_test.cm";
    let mut events = run_test(
        test_url,
        DisabledTestHandling::Exclude,
        parallel,
        vec!["-my_custom_flag_2".to_owned()],
    )
    .await
    .unwrap();

    let mut expected_events = vec![
        TestEvent::test_case_started("TestFailing"),
        TestEvent::stdout_message("TestFailing", "    sample_go_test.go:25: This will fail"),
        TestEvent::test_case_finished("TestFailing", TestResult::Failed),
        TestEvent::test_case_started("TestPassing"),
        TestEvent::test_case_started("TestPrefix"),
        TestEvent::stdout_message("TestPrefix", "Testing that given two tests where one test is prefix of another can execute independently."),
        TestEvent::test_case_finished("TestPrefix", TestResult::Passed),
        TestEvent::stdout_message("TestPassing", "This test will pass"),
        TestEvent::stdout_message("TestPassing", "It will also print this line"),
        TestEvent::stdout_message("TestPassing", "And this line"),
        TestEvent::test_case_finished("TestPassing", TestResult::Passed),
        TestEvent::test_case_started("TestCrashing"),
        TestEvent::stdout_message("TestCrashing", "Test exited abnormally"),
        TestEvent::test_case_finished("TestCrashing", TestResult::Failed),
        TestEvent::test_case_started("TestSkipped"),
        TestEvent::stdout_message("TestSkipped", "    sample_go_test.go:33: Skipping this test"),
        TestEvent::test_case_finished("TestSkipped", TestResult::Skipped),
        TestEvent::test_case_started("TestSubtests"),
        TestEvent::stdout_message("TestSubtests", "=== RUN   TestSubtests/Subtest1"),
        TestEvent::stdout_message("TestSubtests", "=== RUN   TestSubtests/Subtest2"),
        TestEvent::stdout_message("TestSubtests", "=== RUN   TestSubtests/Subtest3"),
        TestEvent::stdout_message("TestSubtests", "    --- PASS: TestSubtests/Subtest1"),
        TestEvent::stdout_message("TestSubtests", "    --- PASS: TestSubtests/Subtest2"),
        TestEvent::stdout_message("TestSubtests", "    --- PASS: TestSubtests/Subtest3"),
        TestEvent::test_case_finished("TestSubtests", TestResult::Passed),
        TestEvent::test_case_started("TestPrefixExtra"),
        TestEvent::stdout_message("TestPrefixExtra", "Testing that given two tests where one test is prefix of another can execute independently."),
        TestEvent::test_case_finished("TestPrefixExtra", TestResult::Passed),
        TestEvent::test_case_started("TestPrintMultiline"),
        TestEvent::stdout_message("TestPrintMultiline", "This test will print the msg in multi-line."),
        TestEvent::test_case_finished("TestPrintMultiline", TestResult::Passed),
        TestEvent::test_case_started("TestCustomArg"),
        TestEvent::test_case_finished("TestCustomArg", TestResult::Passed),
        TestEvent::test_case_started("TestCustomArg2"),
        TestEvent::test_case_finished("TestCustomArg2", TestResult::Passed),
        TestEvent::test_finished(),
    ];
    assert_eq!(events.last(), Some(&TestEvent::test_finished()));

    // check logs order
    let passed_test_logs: Vec<&TestEvent> = events
        .iter()
        .filter(|x| match x {
            TestEvent::StdoutMessage { test_case_name, msg: _ } => test_case_name == "TestPassing",
            _ => false,
        })
        .collect();
    assert_eq!(
        passed_test_logs,
        vec![
            &TestEvent::stdout_message("TestPassing", "This test will pass"),
            &TestEvent::stdout_message("TestPassing", "It will also print this line"),
            &TestEvent::stdout_message("TestPassing", "And this line")
        ]
    );

    expected_events.sort();
    events.sort();
    assert_eq!(events, expected_events);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn launch_and_run_sample_test() {
    launch_and_run_sample_test_helper(Some(10)).await;
}

#[fuchsia_async::run_singlethreaded(test)]
async fn launch_and_run_sample_test_no_concurrent() {
    launch_and_run_sample_test_helper(None).await;
}

// This test will hang if test cases are not executed in parallel.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_parallel_execution() {
    let test_url = "fuchsia-pkg://fuchsia.com/go-test-runner-example#meta/concurrency-test.cm";
    let events = run_test(test_url, DisabledTestHandling::Exclude, Some(5), vec![])
        .await
        .unwrap()
        .into_iter()
        .group_by_test_case_unordered();

    let mut expected_events = vec![];
    for i in 1..=5 {
        let s = format!("Test{}", i);
        expected_events.extend(vec![
            TestEvent::test_case_started(&s),
            TestEvent::test_case_finished(&s, TestResult::Passed),
        ])
    }
    expected_events.push(TestEvent::test_finished());
    let expected_events = expected_events.into_iter().group_by_test_case_unordered();
    assert_eq!(events, expected_events);
}
