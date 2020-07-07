// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::endpoints::{create_endpoints, create_proxy, Proxy, ServiceMarker},
    fidl_fuchsia_io as io, fidl_fuchsia_io_test as io_test, fuchsia_async as fasync,
    fuchsia_zircon::Status,
    futures::StreamExt,
    io_conformance::{
        io1_harness_receiver::Io1HarnessReceiver,
        io1_request_logger_factory::Io1RequestLoggerFactory,
    },
    test_utils_lib::test_utils::OpaqueTest,
};

// Creates a OpaqueTest component from the |url|, listens for the child component to connect
// to a HarnessReceiver injector, and returns the connected HarnessProxy.
async fn setup_harness_connection(
    url: String,
) -> Result<(io_test::Io1HarnessProxy, OpaqueTest), Error> {
    let test = OpaqueTest::default(&url)
        .await
        .context(format!("Cannot create OpaqueTest with url {}", &url))?;
    let event_source =
        test.connect_to_event_source().await.context("Cannot connect to event source.")?;

    // Install HarnessReceiver injector for the child component to connect and to send
    // the harness to run the test through.
    let (capability, mut rx) = Io1HarnessReceiver::new();
    let injector = event_source
        .install_injector(capability, None)
        .await
        .context("Cannot install injector.")?;

    event_source.start_component_tree().await?;

    // Wait for the injector to receive the TestHarness connection from the child component
    // before continuing.
    let harness = rx.next().await.unwrap();
    let harness = harness.into_proxy()?;

    injector.abort();

    Ok((harness, test))
}

/// Helper function to open the desired node in the root folder. Only use this
/// if testing something other than the open call directly.
async fn open_node<T: ServiceMarker>(
    dir: &io::DirectoryProxy,
    flags: u32,
    mode: u32,
    path: &str,
) -> T::Proxy {
    let flags = flags | io::OPEN_FLAG_DESCRIBE;
    let (node_proxy, node_server) = create_proxy::<io::NodeMarker>().expect("Cannot create proxy.");
    dir.open(flags, mode, path, node_server).expect("Cannot open node");

    // Listening to make sure open call succeeded.
    {
        let mut events = node_proxy.take_event_stream();
        let io::NodeEvent::OnOpen_ { s, info: _ } =
            events.next().await.expect("OnOpen event not received").expect("no fidl error");
        assert_eq!(Status::from_raw(s), Status::OK);
    }
    T::Proxy::from_channel(node_proxy.into_channel().expect("Cannot convert node proxy to channel"))
}

/// Constant representing the aggregate of all io.fidl rights.
const ALL_RIGHTS: u32 = io::OPEN_RIGHT_ADMIN
    | io::OPEN_RIGHT_EXECUTABLE
    | io::OPEN_RIGHT_READABLE
    | io::OPEN_RIGHT_WRITABLE;

/// Returns a list of flag combinations to test. Returns a vector of the aggregate of
/// every constant flag and every combination of variable flags.
/// Ex. build_flag_combinations([100], [010, 001]) would return [100, 110, 101, 111]
/// for flags expressed as binary. We exclude the no rights case as that is an
/// invalid argument in most cases. Ex. build_flag_combinations([], [010, 001])
/// would return [010, 001, 011] without the 000 case.
/// All flags passed in must be single bit values.
fn build_flag_combinations(constant_flags: &[u32], variable_flags: &[u32]) -> Vec<u32> {
    // Initial check to make sure all flags are single bit.
    for flag in constant_flags {
        assert_eq!(flag & (flag - 1), 0);
    }
    for flag in variable_flags {
        assert_eq!(flag & (flag - 1), 0);
    }
    let mut base_flag = 0;
    for flag in constant_flags {
        base_flag |= flag;
    }
    let mut vec = vec![base_flag];
    for flag in variable_flags {
        let length = vec.len();
        for i in 0..length {
            vec.push(vec[i] | flag);
        }
    }
    vec.retain(|element| *element != 0);

    vec
}

// Example test to start up a v2 component harness to test when opening a path that goes through a
// remote mount point, the server forwards the request to the remote correctly.
#[fasync::run_singlethreaded(test)]
async fn open_remote_directory_test() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let (remote_dir_client, remote_dir_server) =
        create_endpoints::<io::DirectoryMarker>().expect("Cannot create endpoints.");

    let remote_name = "remote_directory";

    // Request an extra directory connection from the harness to use as the remote,
    // and interpose the requests from the server under test to this remote.
    let (logger, mut rx) = Io1RequestLoggerFactory::new();
    let remote_dir_server =
        logger.get_logged_directory(remote_name.to_string(), remote_dir_server).await;
    harness
        .get_empty_directory(io::OPEN_RIGHT_READABLE | io::OPEN_RIGHT_WRITABLE, remote_dir_server)
        .expect("Cannot get empty remote directory.");

    let (test_dir_proxy, test_dir_server) =
        create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
    harness
        .get_directory_with_remote_directory(
            remote_dir_client,
            remote_name,
            io::OPEN_RIGHT_READABLE | io::OPEN_RIGHT_WRITABLE,
            test_dir_server,
        )
        .expect("Cannot get test harness directory.");

    let (_remote_dir_proxy, remote_dir_server) =
        create_proxy::<io::NodeMarker>().expect("Cannot create proxy.");
    test_dir_proxy
        .open(io::OPEN_RIGHT_READABLE, io::MODE_TYPE_DIRECTORY, remote_name, remote_dir_server)
        .expect("Cannot open remote directory.");

    // Wait on an open call to the interposed remote directory.
    let open_request_string = rx.next().await.expect("Local tx/rx channel was closed");

    // TODO(fxb/45613):: Bare-metal testing against returned request string. We need
    // to find a more ergonomic return format.
    assert_eq!(open_request_string, "remote_directory flags:1, mode:16384, path:.");
}

#[fasync::run_singlethreaded(test)]
async fn file_read_with_sufficient_rights() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let filename = "testing.txt";

    let constant_flags = [io::OPEN_RIGHT_READABLE];
    let variable_flags = [io::OPEN_RIGHT_WRITABLE, io::OPEN_RIGHT_EXECUTABLE, io::OPEN_RIGHT_ADMIN];

    let directory_flags = ALL_RIGHTS;
    let file_flags_set = build_flag_combinations(&constant_flags, &variable_flags);
    for file_flags in file_flags_set {
        let (test_dir_proxy, test_dir_server) =
            create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
        harness
            .get_directory_with_empty_file(filename, directory_flags, test_dir_server)
            .expect("Cannot get remote directory with file.");

        let file =
            open_node::<io::FileMarker>(&test_dir_proxy, file_flags, io::MODE_TYPE_FILE, filename)
                .await;
        let (status, _data) = file.read(0).await.expect("Read failed.");
        assert_eq!(Status::from_raw(status), Status::OK);
    }
}

#[fasync::run_singlethreaded(test)]
async fn file_read_with_insufficient_rights() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let filename = "testing.txt";

    let constant_flags = [];
    let variable_flags = [io::OPEN_RIGHT_WRITABLE, io::OPEN_RIGHT_EXECUTABLE, io::OPEN_RIGHT_ADMIN];

    let directory_flags = ALL_RIGHTS;
    let file_flags_set = build_flag_combinations(&constant_flags, &variable_flags);
    for file_flags in file_flags_set {
        let (test_dir_proxy, test_dir_server) =
            create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
        harness
            .get_directory_with_empty_file(filename, directory_flags, test_dir_server)
            .expect("Cannot get remote directory with file.");

        let file =
            open_node::<io::FileMarker>(&test_dir_proxy, file_flags, io::MODE_TYPE_FILE, filename)
                .await;
        let (status, _data) = file.read(0).await.expect("Read failed.");
        assert_ne!(Status::from_raw(status), Status::OK);
    }
}

#[fasync::run_singlethreaded(test)]
async fn file_read_at_with_sufficient_rights() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let filename = "testing.txt";

    let constant_flags = [io::OPEN_RIGHT_READABLE];
    let variable_flags = [io::OPEN_RIGHT_WRITABLE, io::OPEN_RIGHT_EXECUTABLE, io::OPEN_RIGHT_ADMIN];

    let directory_flags = ALL_RIGHTS;
    let file_flags_set = build_flag_combinations(&constant_flags, &variable_flags);
    for file_flags in file_flags_set {
        let (test_dir_proxy, test_dir_server) =
            create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
        harness
            .get_directory_with_empty_file(filename, directory_flags, test_dir_server)
            .expect("Cannot get remote directory with file.");

        let file =
            open_node::<io::FileMarker>(&test_dir_proxy, file_flags, io::MODE_TYPE_FILE, filename)
                .await;
        let (status, _data) = file.read_at(0, 0).await.expect("Read at failed.");
        assert_eq!(Status::from_raw(status), Status::OK);
    }
}

#[fasync::run_singlethreaded(test)]
async fn file_read_at_with_insufficient_rights() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let filename = "testing.txt";

    let constant_flags = [];
    let variable_flags = [io::OPEN_RIGHT_WRITABLE, io::OPEN_RIGHT_EXECUTABLE, io::OPEN_RIGHT_ADMIN];

    let directory_flags = ALL_RIGHTS;
    let file_flags_set = build_flag_combinations(&constant_flags, &variable_flags);
    for file_flags in file_flags_set {
        let (test_dir_proxy, test_dir_server) =
            create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
        harness
            .get_directory_with_empty_file(filename, directory_flags, test_dir_server)
            .expect("Cannot get remote directory with file.");

        let file =
            open_node::<io::FileMarker>(&test_dir_proxy, file_flags, io::MODE_TYPE_FILE, filename)
                .await;
        let (status, _data) = file.read_at(0, 0).await.expect("Read at failed.");
        assert_ne!(Status::from_raw(status), Status::OK);
    }
}

#[fasync::run_singlethreaded(test)]
async fn file_write_with_sufficient_rights() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let filename = "testing.txt";

    let constant_flags = [io::OPEN_RIGHT_WRITABLE];
    let variable_flags = [io::OPEN_RIGHT_READABLE, io::OPEN_RIGHT_EXECUTABLE, io::OPEN_RIGHT_ADMIN];

    let directory_flags = ALL_RIGHTS;
    let file_flags_set = build_flag_combinations(&constant_flags, &variable_flags);
    for file_flags in file_flags_set {
        let (test_dir_proxy, test_dir_server) =
            create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
        harness
            .get_directory_with_empty_file(filename, directory_flags, test_dir_server)
            .expect("Cannot get remote directory with file.");

        let file =
            open_node::<io::FileMarker>(&test_dir_proxy, file_flags, io::MODE_TYPE_FILE, filename)
                .await;
        let (status, _actual) = file.write("".as_bytes()).await.expect("Failed to write file");
        assert_eq!(Status::from_raw(status), Status::OK);
    }
}

#[fasync::run_singlethreaded(test)]
async fn file_write_with_insufficient_rights() {
    let (harness, _test) = setup_harness_connection(
        "fuchsia-pkg://fuchsia.com/io_fidl_conformance_tests#meta/io_conformance_harness_ulibfs.cm"
            .to_string(),
    )
    .await
    .expect("Could not setup harness connection.");

    let filename = "testing.txt";

    let constant_flags = [];
    let variable_flags = [io::OPEN_RIGHT_READABLE, io::OPEN_RIGHT_EXECUTABLE, io::OPEN_RIGHT_ADMIN];

    let directory_flags = ALL_RIGHTS;
    let file_flags_set = build_flag_combinations(&constant_flags, &variable_flags);
    for file_flags in file_flags_set {
        let (test_dir_proxy, test_dir_server) =
            create_proxy::<io::DirectoryMarker>().expect("Cannot create proxy.");
        harness
            .get_directory_with_empty_file(filename, directory_flags, test_dir_server)
            .expect("Cannot get remote directory with file.");

        let file =
            open_node::<io::FileMarker>(&test_dir_proxy, file_flags, io::MODE_TYPE_FILE, filename)
                .await;
        let (status, _actual) = file.write("".as_bytes()).await.expect("Failed to write file");
        assert_ne!(Status::from_raw(status), Status::OK);
    }
}

#[cfg(test)]
mod tests {
    use super::build_flag_combinations;

    #[test]
    fn test_build_flag_combinations() {
        let constant_flags = [0b100];
        let variable_flags = [0b010, 0b001];
        let generated_combinations = build_flag_combinations(&constant_flags, &variable_flags);
        let expected_result = [0b100, 0b110, 0b101, 0b111];
        assert_eq!(generated_combinations, expected_result);
    }

    #[test]
    fn test_build_flag_combinations_without_empty_rights() {
        let constant_flags = [];
        let variable_flags = [0b010, 0b001];
        let generated_combinations = build_flag_combinations(&constant_flags, &variable_flags);
        let expected_result = [0b010, 0b001, 0b011];
        assert_eq!(generated_combinations, expected_result);
    }
}
