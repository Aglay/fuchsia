// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// This module tests calls to the get_metadata API.
use {
    super::*,
    fidl::endpoints::{create_proxy, create_request_stream},
    fidl_fuchsia_io::{DirectoryMarker, DirectoryRequest, FileEvent::OnOpen_},
    fidl_fuchsia_pkg::{GetMetadataError, RepositoryUrl},
    fuchsia_url::pkg_url::RepoUrl,
    fuchsia_zircon::Status,
    futures::channel::oneshot,
    matches::assert_matches,
    vfs::file::pcb::read_only_static,
};

fn repo_url() -> RepoUrl {
    "fuchsia-pkg://fuchsia.com".parse().unwrap()
}

async fn verify_get_metadata_with_read_success(env: &TestEnv, path: &str, file_contents: &str) {
    let (file_proxy, server_end) = create_proxy().unwrap();

    let res = env
        .local_mirror_proxy()
        .get_metadata(&mut RepositoryUrl { url: repo_url().to_string() }, path, server_end)
        .await;

    assert_eq!(res.unwrap(), Ok(()));
    assert_matches!(
        file_proxy.take_event_stream().next().await,
        Some(Ok(OnOpen_{s, info: Some(_)})) if Status::ok(s) == Ok(())
    );
    assert_eq!(io_util::read_file(&file_proxy).await.unwrap(), file_contents.to_owned());
}

#[fasync::run_singlethreaded(test)]
async fn get_metadata_success() {
    let env = TestEnv::builder()
        .usb_dir(spawn_vfs(pseudo_directory! {
            "0" => pseudo_directory! {
                "fuchsia_pkg" => pseudo_directory! {
                    "repository_metadata" => pseudo_directory! {
                        repo_url().host() => pseudo_directory! {
                            "1.root.json" => read_only_static("beep"),
                            "2.root.json" => read_only_static("boop"),
                        },
                    },
                },
            },
        }))
        .build();

    verify_get_metadata_with_read_success(&env, "1.root.json", "beep").await;
    verify_get_metadata_with_read_success(&env, "2.root.json", "boop").await;
}

#[fasync::run_singlethreaded(test)]
async fn get_metadata_success_multiple_path_segments() {
    let env = TestEnv::builder()
        .usb_dir(spawn_vfs(pseudo_directory! {
            "0" => pseudo_directory! {
                "fuchsia_pkg" => pseudo_directory! {
                    "repository_metadata" => pseudo_directory! {
                        repo_url().host() => pseudo_directory! {
                            "foo" => pseudo_directory! {
                                "bar" => pseudo_directory! {
                                    "1.root.json" => read_only_static("beep"),
                                },
                            },
                            "baz" => pseudo_directory! {
                                "2.root.json" => read_only_static("boop"),
                            }
                        },
                    },
                },
            },
        }))
        .build();

    verify_get_metadata_with_read_success(&env, "foo/bar/1.root.json", "beep").await;
    verify_get_metadata_with_read_success(&env, "baz/2.root.json", "boop").await;
}

async fn verify_get_metadata_with_on_open_failure_status(
    env: &TestEnv,
    path: &str,
    status: Status,
) {
    let (file_proxy, server_end) = create_proxy().unwrap();

    let res = env
        .local_mirror_proxy()
        .get_metadata(&mut RepositoryUrl { url: repo_url().to_string() }, path, server_end)
        .await;

    assert_eq!(res.unwrap(), Ok(()));
    assert_matches!(
        file_proxy.take_event_stream().next().await,
        Some(Ok(OnOpen_{s, info: None})) if  Status::from_raw(s) == status
    );
    assert_matches!(io_util::read_file(&file_proxy).await, Err(_));
}

#[fasync::run_singlethreaded(test)]
async fn get_metadata_missing_repo_url_directory() {
    let env = TestEnv::builder().build();

    verify_get_metadata_with_on_open_failure_status(&env, "1.root.json", Status::NOT_FOUND).await;
}

#[fasync::run_singlethreaded(test)]
async fn get_metadata_missing_metadata_file() {
    let env = TestEnv::builder()
        .usb_dir(spawn_vfs(pseudo_directory! {
            "0" => pseudo_directory! {
                "fuchsia_pkg" => pseudo_directory! {
                    "repository_metadata" => pseudo_directory! {
                        repo_url().host() => pseudo_directory! {
                            "2.root.json" => read_only_static("boop"),
                        },
                    },
                },
            },
        }))
        .build();

    verify_get_metadata_with_on_open_failure_status(&env, "1.root.json", Status::NOT_FOUND).await;
}

#[fasync::run_singlethreaded(test)]
async fn get_metadata_error_opening_metadata() {
    let (unblocker_sender, unblocker_recv) = oneshot::channel();
    let (client_end, mut stream) = create_request_stream::<DirectoryMarker>().unwrap();
    fasync::Task::spawn(async move {
        if let Some(req) = stream.next().await {
            match req.unwrap() {
                DirectoryRequest::Open { object, .. } => {
                    drop(object);
                    let () = unblocker_sender.send(()).unwrap();
                }
                other => panic!("unhandled request type: {:?}", other),
            }
        }
    })
    .detach();
    let env = TestEnv::builder().usb_dir(client_end).build();
    let (file_proxy, server_end) = create_proxy().unwrap();

    // This prevents flakes by enforcing the channel to the metadata
    // dir is closed before the local mirror client calls get metadata.
    let () = unblocker_recv.await.unwrap();

    let res = env
        .local_mirror_proxy()
        .get_metadata(&mut RepositoryUrl { url: repo_url().to_string() }, "1.root.json", server_end)
        .await;

    assert_eq!(res.unwrap(), Err(GetMetadataError::ErrorOpeningMetadata));
    assert_matches!(file_proxy.take_event_stream().next().await, None);
    assert_matches!(io_util::read_file(&file_proxy).await, Err(_));
}
