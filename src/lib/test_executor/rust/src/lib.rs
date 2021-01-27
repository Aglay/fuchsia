// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This crate runs and collects results from a test which implements fuchsia.test.Suite protocol.

use {
    anyhow::{format_err, Context as _, Error},
    fidl::handle::AsHandleRef,
    fidl_fuchsia_test::{
        CaseListenerRequest::Finished,
        CaseListenerRequestStream, Invocation,
        RunListenerRequest::{OnFinished, OnTestCaseStarted},
        SuiteProxy,
    },
    fidl_fuchsia_test_manager::{HarnessProxy, LaunchOptions, SuiteControllerProxy},
    fuchsia_zircon_status as zx_status,
    futures::{
        channel::mpsc,
        future::join_all,
        future::BoxFuture,
        io::{self, AsyncRead},
        prelude::*,
        ready,
        task::{Context, Poll},
    },
    glob,
    linked_hash_map::LinkedHashMap,
    log::*,
    std::{cell::RefCell, collections::HashMap, marker::Unpin, pin::Pin},
};

/// Options that apply when executing a test suite.
///
/// For the FIDL equivalent, see [`fidl_fuchsia_test::RunOptions`].
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct TestRunOptions {
    /// How to handle tests that were marked disabled/ignored by the developer.
    pub disabled_tests: DisabledTestHandling,

    /// Number of test cases to run in parallel.
    pub parallel: Option<u16>,

    /// Arguments passed to tests.
    pub arguments: Vec<String>,
}

/// How to handle tests that were marked disabled/ignored by the developer.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum DisabledTestHandling {
    /// Skip tests that were marked disabled/ignored by the developer.
    Exclude,
    /// Explicitly include tests that were marked disabled/ignored by the developer.
    Include,
}

impl Default for DisabledTestHandling {
    fn default() -> Self {
        DisabledTestHandling::Exclude
    }
}

impl From<TestRunOptions> for fidl_fuchsia_test::RunOptions {
    fn from(test_run_options: TestRunOptions) -> Self {
        // Note: This will *not* break if new members are added to the FIDL table.
        fidl_fuchsia_test::RunOptions {
            parallel: test_run_options.parallel,
            arguments: Some(test_run_options.arguments),
            include_disabled_tests: Some(matches!(
                test_run_options.disabled_tests,
                DisabledTestHandling::Include
            )),
            ..fidl_fuchsia_test::RunOptions::EMPTY
        }
    }
}

/// Defines the result of a test case run.
#[derive(PartialEq, Debug, Eq, Hash, Ord, PartialOrd, Copy, Clone)]
pub enum TestResult {
    /// Test case passed.
    Passed,
    /// Test case failed.
    Failed,
    /// Test case skipped.
    Skipped,
    /// Test case did not communicate the result.
    Error,
}

/// Event to send to caller of `run_test_component`
#[derive(PartialEq, Debug, Eq, Hash, Ord, PartialOrd, Clone)]
pub enum TestEvent {
    /// A new test case is started.
    TestCaseStarted { test_case_name: String },

    /// Test case finished.
    TestCaseFinished { test_case_name: String, result: TestResult },

    /// Test case produced a stdout message.
    StdoutMessage { test_case_name: String, msg: String },

    /// Test finishes successfully.
    Finish,
}

impl TestEvent {
    pub fn test_case_started(s: &str) -> TestEvent {
        TestEvent::TestCaseStarted { test_case_name: s.to_string() }
    }

    pub fn stdout_message(name: &str, message: &str) -> TestEvent {
        TestEvent::StdoutMessage { test_case_name: name.to_string(), msg: message.to_string() }
    }

    pub fn test_case_finished(name: &str, result: TestResult) -> TestEvent {
        TestEvent::TestCaseFinished { test_case_name: name.to_string(), result: result }
    }

    pub fn test_finished() -> TestEvent {
        TestEvent::Finish
    }

    /// Returns the name of the test case to which the event belongs, if applicable.
    pub fn test_case_name(&self) -> Option<&String> {
        match self {
            TestEvent::TestCaseStarted { test_case_name } => Some(test_case_name),
            TestEvent::TestCaseFinished { test_case_name, result: _ } => Some(test_case_name),
            TestEvent::StdoutMessage { test_case_name, msg: _ } => Some(test_case_name),
            TestEvent::Finish => None,
            // NOTE: If new global event types (not tied to a specific test case) are added,
            // `GroupByTestCase` must also be updated so as to preserve correct event ordering.
            // Otherwise, all such events will end up with a key of `None` in the map, and might
            // therefore be spuriously bunched together.
        }
    }

    /// Same as `test_case_name`, but returns an owned `Option<String>`.
    pub fn owned_test_case_name(&self) -> Option<String> {
        self.test_case_name().map(String::from)
    }
}

/// Trait allowing iterators over `TestEvent` to be partitioned by test case name.
///
/// Note that the current implementation assumes that the only `TestEvent` type that occurs
/// _outside of test cases_ is `TestEvent::Finish`. If new global `TestEvent` types are added,
/// the implementation will have to be changed.
pub trait GroupByTestCase: Iterator<Item = TestEvent> + Sized {
    /// Groups the `TestEvent`s by test case name into a map that preserves insertion order.
    /// The overall order of test cases (by first event) and the orders of events within each test
    /// case are preserved, but events from different test cases are effectively de-interleaved.
    ///
    /// Example:
    /// ```rust
    /// use test_executor::{TestEvent, GroupByTestCase as _};
    /// use linked_hash_map::LinkedHashMap;
    ///
    /// let events: Vec<TestEvent> = get_events();
    /// let grouped: LinkedHashMap<Option<String>, TestEvent> =
    ///     events.into_iter().group_by_test_case();
    /// ```
    fn group_by_test_case_ordered(self) -> LinkedHashMap<Option<String>, Vec<TestEvent>> {
        let mut map = LinkedHashMap::new();
        for test_event in self {
            map.entry(test_event.owned_test_case_name()).or_insert(Vec::new()).push(test_event);
        }
        map
    }

    /// De-interleaves the `TestEvents` by test case. The overall order of test cases (by first
    /// event) and the orders of events within each test case are preserved.
    fn deinterleave(self) -> Box<dyn Iterator<Item = TestEvent>> {
        Box::new(
            self.group_by_test_case_ordered()
                .into_iter()
                .flat_map(|(_, events)| events.into_iter()),
        )
    }

    /// Groups the `TestEvent`s by test case name into an unordered map. The orders of events within
    /// each test case are preserved, but the test cases themselves are not in a defined order.
    fn group_by_test_case_unordered(self) -> HashMap<Option<String>, Vec<TestEvent>> {
        let mut map = HashMap::new();
        for test_event in self {
            map.entry(test_event.owned_test_case_name()).or_insert(Vec::new()).push(test_event);
        }
        map
    }
}

impl<T> GroupByTestCase for T where T: Iterator<Item = TestEvent> + Sized {}

#[must_use = "futures/streams"]
pub struct StdoutStream {
    socket: fidl::AsyncSocket,
}
impl Unpin for StdoutStream {}

thread_local! {
    pub static BUFFER:
        RefCell<[u8; 2048]> = RefCell::new([0; 2048]);
}

impl StdoutStream {
    /// Creates a new `StdoutStream` for given `socket`.
    pub fn new(socket: fidl::Socket) -> Result<StdoutStream, anyhow::Error> {
        let stream = StdoutStream {
            socket: fidl::AsyncSocket::from_socket(socket).context("Invalid zircon socket")?,
        };
        Ok(stream)
    }
}

fn process_stdout_bytes(bytes: &[u8]) -> String {
    // TODO(anmittal): Change this to consider break in logs and handle it.
    let log = std::str::from_utf8(bytes).unwrap();
    log.to_string()
}

impl Stream for StdoutStream {
    type Item = io::Result<String>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        BUFFER.with(|b| {
            let mut b = b.borrow_mut();
            let len = ready!(Pin::new(&mut self.socket).poll_read(cx, &mut *b)?);
            if len == 0 {
                return Poll::Ready(None);
            }
            Poll::Ready(Some(process_stdout_bytes(&b[0..len])).map(Ok))
        })
    }
}

struct TestCaseProcessor {
    f: Option<BoxFuture<'static, Result<(), anyhow::Error>>>,
}

impl TestCaseProcessor {
    /// This will start processing of stdout logs and events in the background. The owner of this
    /// object should call `wait_for_finish` method  to make sure all the background task completed.
    pub fn new(
        test_case_name: String,
        listener: CaseListenerRequestStream,
        stdout_socket: fidl::Socket,
        sender: mpsc::Sender<TestEvent>,
    ) -> Self {
        let stdout_fut =
            Self::collect_and_send_stdout(test_case_name.clone(), stdout_socket, sender.clone());
        let f = Self::process_run_event(test_case_name, listener, stdout_fut, sender);

        let (remote, remote_handle) = f.remote_handle();
        fuchsia_async::Task::spawn(remote).detach();

        TestCaseProcessor { f: Some(remote_handle.boxed()) }
    }

    async fn process_run_event(
        name: String,
        mut listener: CaseListenerRequestStream,
        mut stdout_fut: Option<BoxFuture<'static, Result<(), anyhow::Error>>>,
        mut sender: mpsc::Sender<TestEvent>,
    ) -> Result<(), anyhow::Error> {
        while let Some(result) = listener.try_next().await.context("waiting for listener")? {
            match result {
                Finished { result, control_handle: _ } => {
                    // get all test stdout logs before sending finish event.
                    if let Some(ref mut stdout_fut) = stdout_fut.take().as_mut() {
                        stdout_fut.await?;
                    }

                    let result = match result.status {
                        Some(status) => match status {
                            fidl_fuchsia_test::Status::Passed => TestResult::Passed,
                            fidl_fuchsia_test::Status::Failed => TestResult::Failed,
                            fidl_fuchsia_test::Status::Skipped => TestResult::Skipped,
                        },
                        // This will happen when test protocol is not properly implemented
                        // by the test and it forgets to set the result.
                        None => TestResult::Error,
                    };
                    sender.send(TestEvent::test_case_finished(&name, result)).await?;
                    return Ok(());
                }
            }
        }
        if let Some(ref mut stdout_fut) = stdout_fut.take().as_mut() {
            stdout_fut.await?;
        }
        Ok(())
    }

    /// Internal method that put a listener on `stdout_socket`, process and send test stdout logs
    /// asynchronously in the background.
    fn collect_and_send_stdout(
        name: String,
        stdout_socket: fidl::Socket,
        mut sender: mpsc::Sender<TestEvent>,
    ) -> Option<BoxFuture<'static, Result<(), anyhow::Error>>> {
        if stdout_socket.as_handle_ref().is_invalid() {
            return None;
        }

        let mut stream = match StdoutStream::new(stdout_socket) {
            Err(e) => {
                error!("Stdout Logger: Failed to create fuchsia async socket: {:?}", e);
                return None;
            }
            Ok(stream) => stream,
        };

        let f = async move {
            while let Some(log) = stream.try_next().await.context("reading stdout log msg")? {
                sender
                    .send(TestEvent::stdout_message(&name, &log))
                    .await
                    .context("sending stdout log msg")?
            }
            Ok(())
        };

        let (remote, remote_handle) = f.remote_handle();
        fuchsia_async::Task::spawn(remote).detach();
        Some(remote_handle.boxed())
    }

    /// This will wait for all the stdout logs and events to be collected
    pub async fn wait_for_finish(&mut self) -> Result<(), anyhow::Error> {
        if let Some(ref mut f) = self.f.take().as_mut() {
            return Ok(f.await?);
        }
        Ok(())
    }
}

/// Encapsulates running suite instance.
pub struct SuiteInstance {
    suite: SuiteProxy,
    // For safekeeping so that running component remains alive.
    controller: SuiteControllerProxy,
}

impl SuiteInstance {
    /// Launches the test and returns an encapsulated object.
    pub async fn new(harness: &HarnessProxy, test_url: &str) -> Result<Self, anyhow::Error> {
        if !test_url.ends_with(".cm") {
            return Err(anyhow::anyhow!(
                "Tried to run a component as a v2 test that doesn't have a .cm extension"
            ));
        }

        let (suite, controller) = Self::launch_test_suite(harness, test_url).await?;
        Ok(Self { suite, controller })
    }

    async fn launch_test_suite(
        harness: &HarnessProxy,
        test_url: &str,
    ) -> Result<(SuiteProxy, SuiteControllerProxy), anyhow::Error> {
        let (suite_proxy, suite_server_end) = fidl::endpoints::create_proxy().unwrap();
        let (controller_proxy, controller_server_end) = fidl::endpoints::create_proxy().unwrap();

        debug!("Launching test component `{}`", test_url);
        harness
            .launch_suite(&test_url, LaunchOptions::EMPTY, suite_server_end, controller_server_end)
            .await
            .context("launch_test call failed")?
            .map_err(|e: fidl_fuchsia_test_manager::LaunchError| {
                anyhow::anyhow!("error launching test: {:?}", e)
            })?;

        Ok((suite_proxy, controller_proxy))
    }

    /// Enumerates test and return invocations.
    pub async fn enumerate_tests(
        &self,
        test_filter: &Option<&str>,
    ) -> Result<Vec<Invocation>, anyhow::Error> {
        debug!("enumerating tests");
        let (case_iterator, server_end) = fidl::endpoints::create_proxy()?;
        self.suite.get_tests(server_end).map_err(suite_error).context("getting test cases")?;
        let mut invocations = vec![];
        let pattern = test_filter
            .map(|filter| {
                glob::Pattern::new(filter)
                    .map_err(|e| anyhow::anyhow!("Bad test filter pattern: {}", e))
            })
            .transpose()?;
        loop {
            let cases = case_iterator
                .get_next()
                .await
                .map_err(suite_error)
                .context("getting test cases")?;
            if cases.is_empty() {
                break;
            }
            for case in cases {
                let case_name =
                    case.name.ok_or(format_err!("invocation should contain a name."))?;
                if pattern.as_ref().map_or(true, |p| p.matches(&case_name)) {
                    invocations.push(Invocation {
                        name: Some(case_name),
                        tag: None,
                        ..Invocation::EMPTY
                    });
                }
            }
        }

        debug!("invocations: {:#?}", invocations);

        Ok(invocations)
    }

    /// Enumerate tests and then run all the test cases in the suite.
    pub async fn run_and_collect_results(
        &self,
        sender: mpsc::Sender<TestEvent>,
        test_filter: Option<&str>,
        run_options: TestRunOptions,
    ) -> Result<(), anyhow::Error> {
        let invocations = self.enumerate_tests(&test_filter).await?;
        self.run_and_collect_results_for_invocations(sender, invocations, run_options).await
    }

    /// Runs the test component using `suite` and collects test stdout logs and results.
    pub async fn run_and_collect_results_for_invocations(
        &self,
        mut sender: mpsc::Sender<TestEvent>,
        invocations: Vec<Invocation>,
        run_options: TestRunOptions,
    ) -> Result<(), anyhow::Error> {
        debug!("running tests");
        let mut successful_completion = true; // will remain true, if there are no tests to run.
        let mut invocations_iter = invocations.into_iter();
        let run_options: fidl_fuchsia_test::RunOptions = run_options.into();
        loop {
            const INVOCATIONS_CHUNK: usize = 50;
            let chunk = invocations_iter.by_ref().take(INVOCATIONS_CHUNK).collect::<Vec<_>>();
            if chunk.is_empty() {
                break;
            }
            successful_completion &= self
                .run_invocations(chunk, run_options.clone(), &mut sender)
                .await
                .context("running test cases")?;
        }
        if successful_completion {
            sender.send(TestEvent::test_finished()).await.context("sending TestFinished event")?;
        }
        Ok(())
    }

    /// Runs the test component using `suite` and collects stdout logs and results.
    async fn run_invocations(
        &self,
        invocations: Vec<Invocation>,
        run_options: fidl_fuchsia_test::RunOptions,
        sender: &mut mpsc::Sender<TestEvent>,
    ) -> Result<bool, anyhow::Error> {
        let (run_listener_client, mut run_listener) =
            fidl::endpoints::create_request_stream::<fidl_fuchsia_test::RunListenerMarker>()
                .context("creating request stream")?;
        self.suite.run(
            &mut invocations.into_iter().map(|i| i.into()),
            run_options,
            run_listener_client,
        )?;

        let mut test_case_processors = Vec::new();
        let mut successful_completion = false;

        while let Some(result_event) =
            run_listener.try_next().await.context("waiting for listener")?
        {
            match result_event {
                OnTestCaseStarted { invocation, primary_log, listener, control_handle: _ } => {
                    let name =
                        invocation.name.ok_or(anyhow::anyhow!("cannot find name in invocation"))?;
                    sender.send(TestEvent::test_case_started(&name)).await?;
                    let test_case_processor = TestCaseProcessor::new(
                        name,
                        listener.into_stream()?,
                        primary_log,
                        sender.clone(),
                    );
                    test_case_processors.push(test_case_processor);
                }
                OnFinished { .. } => {
                    successful_completion = true;
                    break;
                }
            }
        }

        // await for all invocations to complete for which test case never completed.
        join_all(test_case_processors.iter_mut().map(|i| i.wait_for_finish()))
            .await
            .into_iter()
            .collect::<Result<Vec<()>, Error>>()?;
        Ok(successful_completion)
    }

    /// Consume this instance and returns underlying proxies.
    pub fn into_proxies(self) -> (SuiteProxy, SuiteControllerProxy) {
        return (self.suite, self.controller);
    }
}

fn suite_error(err: fidl::Error) -> anyhow::Error {
    match err {
        // Could get `ClientWrite` or `ClientChannelClosed` error depending on whether the request
        // was sent before or after the channel was closed.
        fidl::Error::ClientWrite(zx_status::Status::PEER_CLOSED)
        | fidl::Error::ClientChannelClosed { .. } => anyhow::anyhow!(
            "The test protocol was closed. This may mean `fuchsia.test.Suite` was not \
            configured correctly. Refer to: \
            https://fuchsia.dev/fuchsia-src/development/components/v2/troubleshooting#troubleshoot-test"
        ),
        err => err.into(),
    }
}

/// The full test coverage of this library lives at //src/testing/sl4f/tests/test_framework
/// They test that this library is able to handle various kind of tests launches and able to collect
/// and pass back results.
/// TODO(anmittal): move some of those tests here as unit tests.
#[cfg(test)]
mod tests {
    use super::*;
    use fidl::HandleBased;
    use maplit::hashmap;
    use pretty_assertions::assert_eq;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn collect_test_stdout() {
        let (sock_server, sock_client) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).expect("Failed while creating socket");

        let name = "test_name";

        let (sender, mut recv) = mpsc::channel(1);

        let fut = TestCaseProcessor::collect_and_send_stdout(name.to_string(), sock_client, sender)
            .expect("future should not be None");

        sock_server.write(b"test message 1").expect("Can't write msg to socket");
        sock_server.write(b"test message 2").expect("Can't write msg to socket");
        sock_server.write(b"test message 3").expect("Can't write msg to socket");

        let mut msg = recv.next().await;

        assert_eq!(
            msg,
            Some(TestEvent::stdout_message(&name, "test message 1test message 2test message 3"))
        );

        // can receive messages multiple times
        sock_server.write(b"test message 4").expect("Can't write msg to socket");
        msg = recv.next().await;

        assert_eq!(msg, Some(TestEvent::stdout_message(&name, "test message 4")));

        // messages can be read after socket server is closed.
        sock_server.write(b"test message 5").expect("Can't write msg to socket");
        sock_server.into_handle(); // this will drop this handle and close it.
        fut.await.expect("log collection should not fail");

        msg = recv.next().await;

        assert_eq!(msg, Some(TestEvent::stdout_message(&name, "test message 5")));

        // socket was closed, this should return None
        msg = recv.next().await;
        assert_eq!(msg, None);
    }

    #[test]
    fn group_by_test_case_ordered() {
        let events = vec![
            TestEvent::test_case_started("a::a"),
            TestEvent::test_case_started("b::b"),
            TestEvent::stdout_message("a::a", "log"),
            TestEvent::test_case_started("c::c"),
            TestEvent::stdout_message("b::b", "log"),
            TestEvent::test_case_finished("c::c", TestResult::Passed),
            TestEvent::test_case_finished("a::a", TestResult::Failed),
            TestEvent::test_case_finished("b::b", TestResult::Passed),
            TestEvent::test_finished(),
        ];

        let actual: Vec<(Option<String>, Vec<TestEvent>)> =
            events.into_iter().group_by_test_case_ordered().into_iter().collect();

        let expected = vec![
            (
                Some("a::a".to_string()),
                vec![
                    TestEvent::test_case_started("a::a"),
                    TestEvent::stdout_message("a::a", "log"),
                    TestEvent::test_case_finished("a::a", TestResult::Failed),
                ],
            ),
            (
                Some("b::b".to_string()),
                vec![
                    TestEvent::test_case_started("b::b"),
                    TestEvent::stdout_message("b::b", "log"),
                    TestEvent::test_case_finished("b::b", TestResult::Passed),
                ],
            ),
            (
                Some("c::c".to_string()),
                vec![
                    TestEvent::test_case_started("c::c"),
                    TestEvent::test_case_finished("c::c", TestResult::Passed),
                ],
            ),
            (None, vec![TestEvent::test_finished()]),
        ];

        assert_eq!(actual, expected);
    }

    #[test]
    fn deinterleave() {
        let events = vec![
            TestEvent::test_case_started("a::a"),
            TestEvent::test_case_started("b::b"),
            TestEvent::stdout_message("a::a", "log"),
            TestEvent::test_case_started("c::c"),
            TestEvent::stdout_message("b::b", "log"),
            TestEvent::test_case_finished("c::c", TestResult::Passed),
            TestEvent::test_case_finished("a::a", TestResult::Failed),
            TestEvent::test_case_finished("b::b", TestResult::Passed),
            TestEvent::test_finished(),
        ];

        let expected = vec![
            TestEvent::test_case_started("a::a"),
            TestEvent::stdout_message("a::a", "log"),
            TestEvent::test_case_finished("a::a", TestResult::Failed),
            TestEvent::test_case_started("b::b"),
            TestEvent::stdout_message("b::b", "log"),
            TestEvent::test_case_finished("b::b", TestResult::Passed),
            TestEvent::test_case_started("c::c"),
            TestEvent::test_case_finished("c::c", TestResult::Passed),
            TestEvent::test_finished(),
        ];

        let actual: Vec<TestEvent> = events.into_iter().deinterleave().collect();

        assert_eq!(actual, expected);
    }

    #[test]
    fn group_by_test_case_unordered() {
        let events = vec![
            TestEvent::test_case_started("a::a"),
            TestEvent::test_case_started("b::b"),
            TestEvent::stdout_message("a::a", "log"),
            TestEvent::test_case_started("c::c"),
            TestEvent::stdout_message("b::b", "log"),
            TestEvent::test_case_finished("c::c", TestResult::Passed),
            TestEvent::test_case_finished("a::a", TestResult::Failed),
            TestEvent::test_case_finished("b::b", TestResult::Passed),
            TestEvent::test_finished(),
        ];

        let expected = hashmap! {
            Some("a::a".to_string()) => vec![
                TestEvent::test_case_started("a::a"),
                TestEvent::stdout_message("a::a", "log"),
                TestEvent::test_case_finished("a::a", TestResult::Failed),
            ],
            Some("b::b".to_string()) => vec![
                TestEvent::test_case_started("b::b"),
                TestEvent::stdout_message("b::b", "log"),
                TestEvent::test_case_finished("b::b", TestResult::Passed),
            ],
            Some("c::c".to_string()) => vec![
                TestEvent::test_case_started("c::c"),
                TestEvent::test_case_finished("c::c", TestResult::Passed),
            ],
            None => vec![TestEvent::Finish],
        };

        let actual = events.into_iter().group_by_test_case_unordered();

        assert_eq!(actual, expected)
    }
}
