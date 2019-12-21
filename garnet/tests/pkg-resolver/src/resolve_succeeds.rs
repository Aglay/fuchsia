// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// This module tests the property that pkg_resolver successfully
/// services fuchsia.pkg.PackageResolver.Resolve FIDL requests for
/// different types of packages and when blobfs and pkgfs are in
/// various intermediate states.
use {
    super::*,
    fidl_fuchsia_pkg_ext::MirrorConfigBuilder,
    fuchsia_inspect::assert_inspect_tree,
    fuchsia_merkle::{Hash, MerkleTree},
    fuchsia_pkg_testing::{
        serve::{handler, AtomicToggle, UriPathHandler},
        RepositoryBuilder,
    },
    futures::{
        channel::{mpsc, oneshot},
        future::{ready, BoxFuture},
        join,
    },
    hyper::{Body, Response},
    matches::assert_matches,
    parking_lot::Mutex,
    std::{
        collections::HashSet,
        fs::File,
        io::{self, Read},
        path::{Path, PathBuf},
        sync::Arc,
    },
};

impl TestEnv<PkgfsRamdisk> {
    fn add_slice_to_blobfs(&self, slice: &[u8]) {
        let merkle = MerkleTree::from_reader(slice).expect("merkle slice").root().to_string();
        let mut blob = self
            .pkgfs
            .blobfs()
            .root_dir()
            .expect("blobfs has root dir")
            .write_file(merkle, 0)
            .expect("create file in blobfs");
        blob.set_len(slice.len() as u64).expect("set_len");
        io::copy(&mut &slice[..], &mut blob).expect("copy from slice to blob");
    }

    fn add_file_with_merkle_to_blobfs(&self, mut file: File, merkle: &Hash) {
        let mut blob = self
            .pkgfs
            .blobfs()
            .root_dir()
            .expect("blobfs has root dir")
            .write_file(merkle.to_string(), 0)
            .expect("create file in blobfs");
        blob.set_len(file.metadata().expect("file has metadata").len()).expect("set_len");
        io::copy(&mut file, &mut blob).expect("copy file to blobfs");
    }

    fn add_file_to_pkgfs_at_path(&self, mut file: File, path: impl openat::AsPath) {
        let mut blob = self
            .pkgfs
            .root_dir()
            .expect("pkgfs root_dir")
            .new_file(path, 0)
            .expect("create file in pkgfs");
        blob.set_len(file.metadata().expect("file has metadata").len()).expect("set_len");
        io::copy(&mut file, &mut blob).expect("copy file to pkgfs");
    }

    fn partially_add_file_to_pkgfs_at_path(&self, mut file: File, path: impl openat::AsPath) {
        let full_len = file.metadata().expect("file has metadata").len();
        assert!(full_len > 1, "can't partially write 1 byte");
        let mut partial_bytes = vec![0; full_len as usize / 2];
        file.read_exact(partial_bytes.as_mut_slice()).expect("partial read of file");
        let mut blob = self
            .pkgfs
            .root_dir()
            .expect("pkgfs root_dir")
            .new_file(path, 0)
            .expect("create file in pkgfs");
        blob.set_len(full_len).expect("set_len");
        io::copy(&mut partial_bytes.as_slice(), &mut blob).expect("copy file to pkgfs");
    }

    fn partially_add_slice_to_pkgfs_at_path(&self, slice: &[u8], path: impl openat::AsPath) {
        assert!(slice.len() > 1, "can't partially write 1 byte");
        let partial_slice = &slice[0..slice.len() / 2];
        let mut blob = self
            .pkgfs
            .root_dir()
            .expect("pkgfs root_dir")
            .new_file(path, 0)
            .expect("create file in pkgfs");
        blob.set_len(slice.len() as u64).expect("set_len");
        io::copy(&mut &partial_slice[..], &mut blob).expect("copy file to pkgfs");
    }
}

#[fasync::run_singlethreaded(test)]
async fn package_resolution() {
    let env = TestEnvBuilder::new().build();

    let s = "package_resolution";
    let pkg = PackageBuilder::new(s)
        .add_resource_at(format!("bin/{}", s), &test_package_bin(s)[..])
        .add_resource_at(format!("meta/{}.cmx", s), &test_package_cmx(s)[..])
        .add_resource_at("data/duplicate_a", "same contents".as_bytes())
        .add_resource_at("data/duplicate_b", "same contents".as_bytes())
        .build()
        .await
        .unwrap();
    let repo = RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
        .add_package(&pkg)
        .build()
        .await
        .unwrap();
    let served_repository = repo.serve(env.launcher()).await.unwrap();

    let repo_url = "fuchsia-pkg://test".parse().unwrap();
    let repo_config = served_repository.make_repo_config(repo_url);

    env.proxies.repo_manager.add(repo_config.into()).await.unwrap();

    let package = env
        .resolve_package(format!("fuchsia-pkg://test/{}", s).as_str())
        .await
        .expect("package to resolve without error");

    // Verify the served package directory contains the exact expected contents.
    pkg.verify_contents(&package).await.unwrap();

    // All blobs in the repository should now be present in blobfs.
    assert_eq!(env.pkgfs.blobfs().list_blobs().unwrap(), repo.list_blobs().unwrap());

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn separate_blobs_url() {
    let env = TestEnvBuilder::new().build();
    let pkg_name = "separate_blobs_url";
    let pkg = make_pkg_with_extra_blobs(pkg_name, 3).await;
    let repo = RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
        .add_package(&pkg)
        .build()
        .await
        .unwrap();
    let served_repository = repo.serve(env.launcher()).await.unwrap();

    // Rename the blobs directory so the blobs can't be found in the usual place.
    // Both amber and the package resolver currently require Content-Length headers when
    // downloading content blobs. "pm serve" will gzip compress paths that aren't prefixed with
    // "/blobs", which removes the Content-Length header. To ensure "pm serve" does not compress
    // the blobs stored at this alternate path, its name must start with "blobs".
    let repo_root = repo.path();
    std::fs::rename(repo_root.join("blobs"), repo_root.join("blobsbolb")).unwrap();

    // Configure the repo manager with different TUF and blobs URLs.
    let repo_url = "fuchsia-pkg://test".parse().unwrap();
    let mut repo_config = served_repository.make_repo_config(repo_url);
    let mirror = &repo_config.mirrors()[0];
    let mirror = MirrorConfigBuilder::new(mirror.mirror_url())
        .subscribe(mirror.subscribe())
        .blob_mirror_url(format!("{}/blobsbolb", mirror.mirror_url()))
        .build();
    repo_config.insert_mirror(mirror).unwrap();
    env.proxies.repo_manager.add(repo_config.into()).await.unwrap();

    // Verify package installation from the split repo succeeds.
    let package = env
        .resolve_package(format!("fuchsia-pkg://test/{}", pkg_name).as_str())
        .await
        .expect("package to resolve without error");
    pkg.verify_contents(&package).await.unwrap();
    std::fs::rename(repo_root.join("blobsbolb"), repo_root.join("blobs")).unwrap();
    assert_eq!(env.pkgfs.blobfs().list_blobs().unwrap(), repo.list_blobs().unwrap());

    env.stop().await;
}

async fn verify_resolve_with_altered_env(
    pkg: Package,
    alter_env: impl FnOnce(&TestEnv, &Package),
) -> () {
    let env = TestEnvBuilder::new().build();

    let repo = RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
        .add_package(&pkg)
        .build()
        .await
        .unwrap();
    let served_repository = repo.serve(env.launcher()).await.unwrap();

    let repo_url = "fuchsia-pkg://test".parse().unwrap();
    let repo_config = served_repository.make_repo_config(repo_url);

    env.proxies.repo_manager.add(repo_config.into()).await.unwrap();

    alter_env(&env, &pkg);

    let pkg_url = format!("fuchsia-pkg://test/{}", pkg.name());
    let package_dir = env.resolve_package(&pkg_url).await.unwrap();

    pkg.verify_contents(&package_dir).await.unwrap();
    assert_eq!(env.pkgfs.blobfs().list_blobs().unwrap(), repo.list_blobs().unwrap());

    env.stop().await;
}

fn verify_resolve(pkg: Package) -> impl Future<Output = ()> {
    verify_resolve_with_altered_env(pkg, |_, _| {})
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_only() {
    verify_resolve(PackageBuilder::new("uniblob").build().await.unwrap()).await
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_and_empty_blob() {
    verify_resolve(
        PackageBuilder::new("emptyblob")
            .add_resource_at("data/empty", "".as_bytes())
            .build()
            .await
            .unwrap(),
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn large_blobs() {
    let s = "large_blobs";
    verify_resolve(
        PackageBuilder::new(s)
            .add_resource_at("bin/numbers", &test_package_bin(s)[..])
            .add_resource_at("data/ones", io::repeat(1).take(1 * 1024 * 1024))
            .add_resource_at("data/twos", io::repeat(2).take(2 * 1024 * 1024))
            .add_resource_at("data/threes", io::repeat(3).take(3 * 1024 * 1024))
            .build()
            .await
            .unwrap(),
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn pinned_merkle_resolution() {
    let env = TestEnvBuilder::new().build();

    // Since our test harness doesn't yet include a way to update a package, we generate two
    // separate packages to test resolution with a pinned merkle root.
    // We can do this with two packages because the TUF metadata doesn't currently contain
    // package names, only the latest known merkle root for a given name.
    // So, generate two packages, and then resolve one package with the merkle of the other
    // to test resolution with a pinned merkle.
    let pkg1 = PackageBuilder::new("pinned-merkle-foo")
        .add_resource_at("data/foo", "foo".as_bytes())
        .build()
        .await
        .unwrap();
    let pkg2 = PackageBuilder::new("pinned-merkle-bar")
        .add_resource_at("data/bar", "bar".as_bytes())
        .build()
        .await
        .unwrap();

    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg1)
            .add_package(&pkg2)
            .build()
            .await
            .unwrap(),
    );

    let served_repository = repo.build_server().start().unwrap();
    env.register_repo(&served_repository).await;

    let pkg1_url_with_pkg2_merkle =
        format!("fuchsia-pkg://test/pinned-merkle-foo?hash={}", pkg2.meta_far_merkle_root());

    let package_dir = env.resolve_package(&pkg1_url_with_pkg2_merkle).await.unwrap();
    pkg2.verify_contents(&package_dir).await.unwrap();

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn variant_resolution() {
    let env = TestEnvBuilder::new().build();
    let pkg = PackageBuilder::new("variant-foo")
        .add_resource_at("data/foo", "foo".as_bytes())
        .build()
        .await
        .unwrap();

    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );

    let served_repository = repo.build_server().start().unwrap();
    env.register_repo(&served_repository).await;

    let pkg_url = &"fuchsia-pkg://test/variant-foo/0";

    let package_dir = env.resolve_package(pkg_url).await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn error_codes() {
    let env = TestEnvBuilder::new().build();
    let pkg = PackageBuilder::new("error-foo")
        .add_resource_at("data/foo", "foo".as_bytes())
        .build()
        .await
        .unwrap();

    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );

    let served_repository = repo.build_server().start().unwrap();
    env.register_repo(&served_repository).await;

    // Invalid URL
    assert_matches!(
        env.resolve_package("fuchsia-pkg://test/bad-url!").await,
        Err(Status::INVALID_ARGS)
    );

    // Nonexistant repo
    assert_matches!(
        env.resolve_package("fuchsia-pkg://a/nonexistent").await,
        Err(Status::NOT_FOUND)
    );

    // Nonexistant package
    assert_matches!(
        env.resolve_package("fuchsia-pkg://test/nonexistent").await,
        Err(Status::INTERNAL)
    );

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn many_blobs() {
    verify_resolve(make_pkg_with_extra_blobs("many_blobs", 200).await).await
}

#[fasync::run_singlethreaded(test)]
async fn identity() {
    verify_resolve(Package::identity().await.unwrap()).await
}

#[fasync::run_singlethreaded(test)]
async fn identity_hyper() {
    let env = TestEnvBuilder::new().build();

    let pkg = Package::identity().await.unwrap();
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );
    let served_repository = repo.build_server().start().unwrap();
    env.register_repo(&served_repository).await;

    let pkg_url = format!("fuchsia-pkg://test/{}", pkg.name());
    let package_dir = env.resolve_package(&pkg_url).await.unwrap();

    pkg.verify_contents(&package_dir).await.unwrap();

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn retries() {
    let env = TestEnvBuilder::new().build();

    let pkg = PackageBuilder::new("try-hard")
        .add_resource_at("data/foo", "bar".as_bytes())
        .build()
        .await
        .unwrap();
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );
    let served_repository = repo
        .build_server()
        .uri_path_override_handler(handler::ForPathPrefix::new(
            "/blobs",
            handler::OncePerPath::new(handler::StaticResponseCode::server_error()),
        ))
        .start()
        .unwrap();
    env.register_repo(&served_repository).await;

    let package_dir = env.resolve_package("fuchsia-pkg://test/try-hard").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();

    let hierarchy = env.pkg_resolver_inspect_hierarchy().await;
    let repo_blob_url = format!("{}/blobs", served_repository.local_url());
    let repo_blob_url = &repo_blob_url;
    assert_inspect_tree!(
        hierarchy,
        root: contains {
            repository_manager: contains {
                stats: {
                    mirrors: {
                        var repo_blob_url: {
                            network_blips: 2u64,
                            network_rate_limits: 0u64,
                        },
                    },
                },
            },
        }
    );

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn handles_429_responses() {
    let env = TestEnvBuilder::new().build();

    let pkg1 = PackageBuilder::new("rate-limit-far")
        .add_resource_at("data/foo", "foo".as_bytes())
        .build()
        .await
        .unwrap();
    let pkg2 = PackageBuilder::new("rate-limit-content")
        .add_resource_at("data/bar", "bar".as_bytes())
        .build()
        .await
        .unwrap();
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg1)
            .add_package(&pkg2)
            .build()
            .await
            .unwrap(),
    );
    let served_repository = repo
        .build_server()
        .uri_path_override_handler(handler::ForPath::new(
            format!("/blobs/{}", pkg1.meta_far_merkle_root()),
            handler::ForRequestCount::new(2, handler::StaticResponseCode::too_many_requests()),
        ))
        .uri_path_override_handler(handler::ForPath::new(
            format!("/blobs/{}", pkg2.meta_contents().unwrap().contents()["data/bar"]),
            handler::ForRequestCount::new(2, handler::StaticResponseCode::too_many_requests()),
        ))
        .start()
        .unwrap();
    env.register_repo(&served_repository).await;

    // Simultaneously resolve both packages to minimize the time spent waiting for timeouts in
    // these tests.
    let proxy1 = env.connect_to_resolver();
    let proxy2 = env.connect_to_resolver();
    let pkg1_fut = resolve_package(&proxy1, "fuchsia-pkg://test/rate-limit-far");
    let pkg2_fut = resolve_package(&proxy2, "fuchsia-pkg://test/rate-limit-content");

    // The packages should resolve successfully.
    let (pkg1_dir, pkg2_dir) = join!(pkg1_fut, pkg2_fut);
    pkg1.verify_contents(&pkg1_dir.unwrap()).await.unwrap();
    pkg2.verify_contents(&pkg2_dir.unwrap()).await.unwrap();

    // And the inspect data for the package resolver should indicate that it handled 429 responses.
    let hierarchy = env.pkg_resolver_inspect_hierarchy().await;

    let repo_blob_url = format!("{}/blobs", served_repository.local_url());
    let repo_blob_url = &repo_blob_url;
    assert_inspect_tree!(
        hierarchy,
        root: contains {
            repository_manager: contains {
                stats: {
                    mirrors: {
                        var repo_blob_url: {
                            network_blips: 0u64,
                            network_rate_limits: 4u64,
                        },
                    },
                },
            },
        }
    );

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn use_cached_package() {
    let env = TestEnvBuilder::new().build();

    let pkg = PackageBuilder::new("resolve-twice")
        .add_resource_at("data/foo", "bar".as_bytes())
        .build()
        .await
        .unwrap();
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );
    let fail_requests = AtomicToggle::new(true);
    let served_repository = repo
        .build_server()
        .uri_path_override_handler(handler::Toggleable::new(
            &fail_requests,
            handler::StaticResponseCode::server_error(),
        ))
        .start()
        .unwrap();

    // the package can't be resolved before the repository is configured.
    assert_matches!(
        env.resolve_package("fuchsia-pkg://test/resolve-twice").await,
        Err(Status::NOT_FOUND)
    );

    env.register_repo(&served_repository).await;

    // the package can't be resolved before the repository can be updated without error.
    assert_matches!(
        env.resolve_package("fuchsia-pkg://test/resolve-twice").await,
        Err(Status::INTERNAL)
    );

    // package resolves as expected.
    fail_requests.unset();
    let package_dir = env.resolve_package("fuchsia-pkg://test/resolve-twice").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();

    // if no mirrors are accessible, the cached package is returned.
    fail_requests.set();
    let package_dir = env.resolve_package("fuchsia-pkg://test/resolve-twice").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();

    served_repository.stop().await;
    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_installed_blobs_not_installed() {
    verify_resolve_with_altered_env(
        make_pkg_with_extra_blobs("meta_far_installed_blobs_not_installed", 3).await,
        |env, pkg| {
            env.add_file_to_pkgfs_at_path(
                pkg.meta_far().unwrap(),
                format!("install/pkg/{}", pkg.meta_far_merkle_root().to_string()),
            )
        },
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_partially_installed() {
    verify_resolve_with_altered_env(
        make_pkg_with_extra_blobs("meta_far_partially_installed", 3).await,
        |env, pkg| {
            env.partially_add_file_to_pkgfs_at_path(
                pkg.meta_far().unwrap(),
                format!("install/pkg/{}", pkg.meta_far_merkle_root().to_string()),
            )
        },
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_already_in_blobfs() {
    verify_resolve_with_altered_env(
        make_pkg_with_extra_blobs("meta_far_already_in_blobfs", 3).await,
        |env, pkg| {
            env.add_file_with_merkle_to_blobfs(pkg.meta_far().unwrap(), pkg.meta_far_merkle_root())
        },
    )
    .await
}

#[fasync::run_singlethreaded(test)]
async fn all_blobs_already_in_blobfs() {
    let s = "all_blobs_already_in_blobfs";
    verify_resolve_with_altered_env(make_pkg_with_extra_blobs(s, 3).await, |env, pkg| {
        env.add_file_with_merkle_to_blobfs(pkg.meta_far().unwrap(), pkg.meta_far_merkle_root());
        env.add_slice_to_blobfs(&test_package_bin(s)[..]);
        for i in 0..3 {
            env.add_slice_to_blobfs(extra_blob_contents(s, i).as_slice());
        }
    })
    .await
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_installed_one_blob_in_blobfs() {
    let s = "meta_far_installed_one_blob_in_blobfs";
    verify_resolve_with_altered_env(make_pkg_with_extra_blobs(s, 3).await, |env, pkg| {
        env.add_file_to_pkgfs_at_path(
            pkg.meta_far().unwrap(),
            format!("install/pkg/{}", pkg.meta_far_merkle_root().to_string()),
        );
        env.add_slice_to_blobfs(&test_package_bin(s)[..]);
    })
    .await
}

#[fasync::run_singlethreaded(test)]
async fn meta_far_installed_one_blob_partially_installed() {
    let s = "meta_far_installed_one_blob_partially_installed";
    verify_resolve_with_altered_env(make_pkg_with_extra_blobs(s, 3).await, |env, pkg| {
        env.add_file_to_pkgfs_at_path(
            pkg.meta_far().unwrap(),
            format!("install/pkg/{}", pkg.meta_far_merkle_root().to_string()),
        );
        env.partially_add_slice_to_pkgfs_at_path(
            &test_package_bin(s)[..],
            format!(
                "install/blob/{}",
                MerkleTree::from_reader(&test_package_bin(s)[..])
                    .expect("merkle slice")
                    .root()
                    .to_string()
            ),
        );
    })
    .await
}

struct OneShotSenderWrapper {
    sender: Mutex<Option<oneshot::Sender<Box<dyn FnOnce() + Send>>>>,
}

impl OneShotSenderWrapper {
    fn send(&self, unblocking_closure: Box<dyn FnOnce() + Send>) {
        self.sender
            .lock()
            .take()
            .expect("a single request for this path")
            .send(unblocking_closure)
            .map_err(|_err| ())
            .expect("receiver is present");
    }
}

/// Blocks sending a response body (but not headers) for a single path once until unblocked by a test.
struct BlockResponseBodyOnceUriPathHandler {
    unblocking_closure_sender: OneShotSenderWrapper,
    path_to_block: String,
}

impl BlockResponseBodyOnceUriPathHandler {
    pub fn new_path_handler_and_channels(
        path_to_block: String,
    ) -> (Self, oneshot::Receiver<Box<dyn FnOnce() + Send>>) {
        let (unblocking_closure_sender, unblocking_closure_receiver) = oneshot::channel();
        let blocking_uri_path_handler = BlockResponseBodyOnceUriPathHandler {
            unblocking_closure_sender: OneShotSenderWrapper {
                sender: Mutex::new(Some(unblocking_closure_sender)),
            },
            path_to_block,
        };
        (blocking_uri_path_handler, unblocking_closure_receiver)
    }
}

impl UriPathHandler for BlockResponseBodyOnceUriPathHandler {
    fn handle(
        &self,
        uri_path: &Path,
        mut response: Response<Body>,
    ) -> BoxFuture<'_, Response<Body>> {
        // Only block requests for the duplicate blob
        let duplicate_blob_uri = Path::new(&self.path_to_block);
        if uri_path != duplicate_blob_uri {
            return ready(response).boxed();
        }

        async move {
            // By only sending the content length header, this will guarantee the
            // duplicate blob remains open and truncated until the content is sent
            let (mut sender, new_body) = Body::channel();
            let old_body = std::mem::replace(response.body_mut(), new_body);
            let contents = body_to_bytes(old_body).await;

            // Send a closure to the test that will unblock when executed
            self.unblocking_closure_sender
                .send(Box::new(move || sender.send_data(contents.into()).expect("sending body")));
            response
        }
        .boxed()
    }
}

#[fasync::run_singlethreaded(test)]
async fn test_concurrent_blob_writes() {
    // Create our test packages and find out the merkle of the duplicate blob
    let duplicate_blob_path = "blob/duplicate";
    let duplicate_blob_contents = &b"I am the duplicate"[..];
    let unique_blob_path = "blob/unique";
    let pkg1 = PackageBuilder::new("package1")
        .add_resource_at(duplicate_blob_path, duplicate_blob_contents)
        .build()
        .await
        .unwrap();
    let pkg2 = PackageBuilder::new("package2")
        .add_resource_at(duplicate_blob_path, duplicate_blob_contents)
        .add_resource_at(unique_blob_path, &b"I am unique"[..])
        .build()
        .await
        .unwrap();
    let duplicate_blob_merkle = pkg1.meta_contents().expect("extracted contents").contents()
        [duplicate_blob_path]
        .to_string();
    let unique_blob_merkle =
        pkg2.meta_contents().expect("extracted contents").contents()[unique_blob_path].to_string();

    // Create the path handler and the channel to communicate with it
    let (blocking_uri_path_handler, unblocking_closure_receiver) =
        BlockResponseBodyOnceUriPathHandler::new_path_handler_and_channels(format!(
            "/blobs/{}",
            duplicate_blob_merkle
        ));

    // Construct the repo
    let env = TestEnvBuilder::new().build();
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg1)
            .add_package(&pkg2)
            .build()
            .await
            .unwrap(),
    );
    let served_repository =
        repo.build_server().uri_path_override_handler(blocking_uri_path_handler).start().unwrap();
    env.register_repo(&served_repository).await;

    // Construct the resolver proxies (clients)
    let resolver_proxy_1 =
        env.env.connect_to_service::<PackageResolverMarker>().expect("connect to package resolver");
    let resolver_proxy_2 =
        env.env.connect_to_service::<PackageResolverMarker>().expect("connect to package resolver");

    // Create a GET request to the hyper server for the duplicate blob
    let package1_resolution_fut =
        resolve_package(&resolver_proxy_1, &"fuchsia-pkg://test/package1");

    // Wait for GET request to be received by hyper server
    let send_shared_blob_body =
        unblocking_closure_receiver.await.expect("received unblocking future from hyper server");

    // Wait for duplicate blob to be truncated -- we know it is truncated when we get a
    // permission denied error when trying to update the blob in blobfs.
    let blobfs_dir = env.pkgfs.blobfs().root_dir().expect("blobfs has root dir");
    while blobfs_dir.update_file(Path::new(&duplicate_blob_merkle), 0).is_ok() {
        fasync::Timer::new(fasync::Time::after(fuchsia_zircon::Duration::from_millis(10))).await;
    }

    // At this point, we are confident that the duplicate blob is truncated. So, if we enqueue
    // another package resolve for a package that contains the duplicate blob, pkgfs should expose
    // that blob as a need, and the package resolver should block resolving the package on that
    // blob fetch finishing.
    let package2_resolution_fut =
        resolve_package(&resolver_proxy_2, &"fuchsia-pkg://test/package2");

    // Wait for the unique blob to exist in blobfs.
    while blobfs_dir.update_file(Path::new(&unique_blob_merkle), 0).is_ok() {
        fasync::Timer::new(fasync::Time::after(fuchsia_zircon::Duration::from_millis(10))).await;
    }

    // At this point, both package resolves should be blocked on the shared blob download. Unblock
    // the server and verify both packages resolve to valid directories.
    send_shared_blob_body();
    let ((), ()) = futures::join!(
        async move {
            let package1_dir = package1_resolution_fut.await.unwrap();
            pkg1.verify_contents(&package1_dir).await.unwrap();
        },
        async move {
            let package2_dir = package2_resolution_fut.await.unwrap();
            pkg2.verify_contents(&package2_dir).await.unwrap();
        },
    );

    env.stop().await;
}

/// A response that is waiting to be sent.
struct BlockedResponse {
    path: PathBuf,
    unblocker: oneshot::Sender<()>,
}

impl BlockedResponse {
    fn path(&self) -> &Path {
        &self.path
    }

    fn unblock(self) {
        self.unblocker.send(()).expect("request to still be pending")
    }
}

/// Blocks sending response headers and bodies for a set of paths until unblocked by a test.
struct BlockResponseUriPathHandler {
    paths_to_block: HashSet<PathBuf>,
    blocked_responses: mpsc::UnboundedSender<BlockedResponse>,
}

impl BlockResponseUriPathHandler {
    fn new(paths_to_block: HashSet<PathBuf>) -> (Self, mpsc::UnboundedReceiver<BlockedResponse>) {
        let (sender, receiver) = mpsc::unbounded();

        (Self { paths_to_block, blocked_responses: sender }, receiver)
    }
}

impl UriPathHandler for BlockResponseUriPathHandler {
    fn handle(&self, path: &Path, response: Response<Body>) -> BoxFuture<'_, Response<Body>> {
        // Only block paths that were requested to be blocked
        if !self.paths_to_block.contains(path) {
            return async move { response }.boxed();
        }

        // Return a future that notifies the test that the request was blocked and wait for it to
        // unblock the response
        let path = path.to_owned();
        let mut blocked_responses = self.blocked_responses.clone();
        async move {
            let (unblocker, waiter) = oneshot::channel();
            blocked_responses
                .send(BlockedResponse { path, unblocker })
                .await
                .expect("receiver to still exist");
            waiter.await.expect("request to be unblocked");
            response
        }
        .boxed()
    }
}

#[fasync::run_singlethreaded(test)]
async fn dedup_concurrent_content_blob_fetches() {
    let env = TestEnvBuilder::new().build();

    // Make a few test packages with no more than 6 blobs.  There is no guarantee what order the
    // package resolver will fetch blobs in other than it will fetch one of the meta FARs first and
    // it will fetch a meta FAR before fetching any unique content blobs for that package.
    //
    // Note that this test depends on the fact that the global queue has a concurrency limit of 5.
    // A concurrency limit less than 4 would cause this test to hang as it needs to be able to wait
    // for a unique blob request to come in for each package, and ordering of blob requests is not
    // guaranteed.
    let pkg1 = PackageBuilder::new("package1")
        .add_resource_at("data/unique1", "package1unique1".as_bytes())
        .add_resource_at("data/shared1", "shared1".as_bytes())
        .add_resource_at("data/shared2", "shared2".as_bytes())
        .build()
        .await
        .unwrap();
    let pkg2 = PackageBuilder::new("package2")
        .add_resource_at("data/unique1", "package2unique1".as_bytes())
        .add_resource_at("data/shared1", "shared1".as_bytes())
        .add_resource_at("data/shared2", "shared2".as_bytes())
        .build()
        .await
        .unwrap();

    // Create the request handler to block all content blobs until we are ready to unblock them.
    let content_blob_paths = {
        let pkg1_meta_contents = pkg1.meta_contents().expect("meta/contents to parse");
        let pkg2_meta_contents = pkg2.meta_contents().expect("meta/contents to parse");

        pkg1_meta_contents
            .contents()
            .values()
            .chain(pkg2_meta_contents.contents().values())
            .map(|blob| format!("/blobs/{}", blob).into())
            .collect::<HashSet<_>>()
    };
    let (request_handler, mut incoming_requests) =
        BlockResponseUriPathHandler::new(content_blob_paths.iter().cloned().collect());

    // Serve and register the repo with our request handler that blocks headers for content blobs.
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg1)
            .add_package(&pkg2)
            .build()
            .await
            .expect("repo to build"),
    );
    let served_repository = repo
        .build_server()
        .uri_path_override_handler(request_handler)
        .start()
        .expect("repo to serve");

    env.register_repo(&served_repository).await;

    // Start resolving both packages using distinct proxies, which should block waiting for the
    // meta FAR responses.
    let pkg1_fut = {
        let proxy = env
            .env
            .connect_to_service::<PackageResolverMarker>()
            .expect("connect to package resolver");
        resolve_package(&proxy, "fuchsia-pkg://test/package1")
    };
    let pkg2_fut = {
        let proxy = env
            .env
            .connect_to_service::<PackageResolverMarker>()
            .expect("connect to package resolver");
        resolve_package(&proxy, "fuchsia-pkg://test/package2")
    };

    // Wait for all content blob requests to come in so that this test can be sure that pkgfs has
    // imported both packages and that the package resolver has not truncated any content blobs
    // yet (which would trigger 39488, the pkgfs blob presence bug).
    let mut expected_requests = content_blob_paths.clone();
    let mut blocked_requests = vec![];
    while !expected_requests.is_empty() {
        let req = incoming_requests.next().await.expect("more incoming requests");
        // Panic if the blob request wasn't expected or has already happened and was not de-duped
        // as expected.
        assert!(expected_requests.remove(req.path()));
        blocked_requests.push(req);
    }

    // Unblock all content blobs, and verify both packages resolve without error.
    for req in blocked_requests {
        req.unblock();
    }

    let pkg1_dir = pkg1_fut.await.expect("package 1 to resolve");
    let pkg2_dir = pkg2_fut.await.expect("package 2 to resolve");

    pkg1.verify_contents(&pkg1_dir).await.unwrap();
    pkg2.verify_contents(&pkg2_dir).await.unwrap();

    env.stop().await;
}

#[fasync::run_singlethreaded(test)]
async fn rust_tuf_experiment_identity() {
    let env = TestEnvBuilder::new().include_amber(false).build();
    let pkg = Package::identity().await.unwrap();
    let repo = RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
        .add_package(&pkg)
        .build()
        .await
        .unwrap();
    let served_repository = repo.serve(env.launcher()).await.unwrap();
    let repo_url = "fuchsia-pkg://test".parse().unwrap();
    let repo_config = served_repository.make_repo_config(repo_url);
    env.proxies.repo_manager.add(repo_config.into()).await.unwrap();

    // Verify we can resolve a package using rust tuf
    env.set_experiment_state(Experiment::RustTuf, true).await;
    let pkg_url = format!("fuchsia-pkg://test/{}", pkg.name());
    let package = env.resolve_package(&pkg_url).await.expect("package to resolve without error");
    pkg.verify_contents(&package).await.unwrap();
    assert_eq!(env.pkgfs.blobfs().list_blobs().unwrap(), repo.list_blobs().unwrap());
    env.stop().await;
}
