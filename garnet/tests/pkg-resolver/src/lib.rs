// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use {
    failure::Error,
    fdio,
    fidl::endpoints::{ClientEnd, DiscoverableService},
    fidl_fuchsia_amber::ControlMarker as AmberMarker,
    fidl_fuchsia_inspect::TreeMarker,
    fidl_fuchsia_io::{DirectoryMarker, DirectoryProxy, CLONE_FLAG_SAME_RIGHTS},
    fidl_fuchsia_pkg::{
        ExperimentToggle as Experiment, PackageCacheMarker, PackageResolverAdminMarker,
        PackageResolverAdminProxy, PackageResolverMarker, PackageResolverProxy,
        RepositoryManagerMarker, RepositoryManagerProxy, UpdatePolicy,
    },
    fidl_fuchsia_pkg_rewrite::{
        EngineMarker as RewriteEngineMarker, EngineProxy as RewriteEngineProxy,
    },
    fidl_fuchsia_sys::LauncherProxy,
    fuchsia_async as fasync,
    fuchsia_component::{
        client::{App, AppBuilder},
        server::{NestedEnvironment, ServiceFs},
    },
    fuchsia_inspect::reader::NodeHierarchy,
    fuchsia_pkg_testing::{pkgfs::TestPkgFs, serve::ServedRepository, Package, PackageBuilder},
    fuchsia_zircon::{self as zx, Status},
    futures::{compat::Stream01CompatExt, prelude::*},
    hyper::Body,
    serde_derive::Serialize,
    std::{fs::File, io::BufWriter},
    tempfile::TempDir,
};

mod dynamic_repositories_disabled;
mod dynamic_rewrite_disabled;
mod inspect;
mod mock_filesystem;
mod resolve_propagates_pkgfs_failure;
mod resolve_recovers_from_http_errors;
mod resolve_succeeds;

trait PkgFs {
    fn root_dir_client_end(&self) -> Result<ClientEnd<DirectoryMarker>, Error>;
}

impl PkgFs for TestPkgFs {
    fn root_dir_client_end(&self) -> Result<ClientEnd<DirectoryMarker>, Error> {
        TestPkgFs::root_dir_client_end(self)
    }
}

struct Mounts {
    pkg_resolver_data: DirOrProxy,
    pkg_resolver_config_data: DirOrProxy,
}

enum DirOrProxy {
    Dir(TempDir),
    Proxy(DirectoryProxy),
}

trait AppBuilderExt {
    fn add_dir_or_proxy_to_namespace(
        self,
        path: impl Into<String>,
        dir_or_proxy: &DirOrProxy,
    ) -> Self;
}

impl AppBuilderExt for AppBuilder {
    fn add_dir_or_proxy_to_namespace(
        self,
        path: impl Into<String>,
        dir_or_proxy: &DirOrProxy,
    ) -> Self {
        match dir_or_proxy {
            DirOrProxy::Dir(d) => {
                self.add_dir_to_namespace(path.into(), File::open(d.path()).unwrap()).unwrap()
            }
            DirOrProxy::Proxy(p) => {
                self.add_handle_to_namespace(path.into(), clone_directory_proxy(p))
            }
        }
    }
}

fn clone_directory_proxy(proxy: &DirectoryProxy) -> zx::Handle {
    let (client, server) = fidl::endpoints::create_endpoints().unwrap();
    proxy.clone(CLONE_FLAG_SAME_RIGHTS, server).unwrap();
    client.into()
}

#[derive(Serialize)]
struct Config {
    disable_dynamic_configuration: bool,
}

impl Mounts {
    fn new() -> Self {
        Self {
            pkg_resolver_data: DirOrProxy::Dir(tempfile::tempdir().expect("/tmp to exist")),
            pkg_resolver_config_data: DirOrProxy::Dir(tempfile::tempdir().expect("/tmp to exist")),
        }
    }
    fn add_config(&self, config: &Config) {
        if let DirOrProxy::Dir(ref d) = self.pkg_resolver_config_data {
            let f = File::create(d.path().join("config.json")).unwrap();
            serde_json::to_writer(BufWriter::new(f), &config).unwrap();
        } else {
            panic!("not supported");
        }
    }
}

struct Apps {
    _amber: App,
    _pkg_cache: App,
    _pkg_resolver: App,
}

struct Proxies {
    resolver_admin: PackageResolverAdminProxy,
    resolver: PackageResolverProxy,
    repo_manager: RepositoryManagerProxy,
    rewrite_engine: RewriteEngineProxy,
}

struct TestEnv<P = TestPkgFs> {
    pkgfs: P,
    env: NestedEnvironment,
    apps: Apps,
    proxies: Proxies,
    _mounts: Mounts,
    nested_environment_label: String,
}

impl TestEnv<TestPkgFs> {
    fn new() -> Self {
        Self::new_with_pkg_fs(TestPkgFs::start().expect("pkgfs to start"))
    }

    fn new_with_mounts(mounts: Mounts) -> Self {
        Self::new_with_pkg_fs_and_mounts(TestPkgFs::start().expect("pkgfs to start"), mounts)
    }

    async fn stop(self) {
        // Tear down the environment in reverse order, ending with the storage.
        drop(self.proxies);
        drop(self.apps);
        drop(self.env);
        self.pkgfs.stop().await.expect("pkgfs to stop gracefully");
    }
}

impl<P: PkgFs> TestEnv<P> {
    fn new_with_pkg_fs(pkgfs: P) -> Self {
        Self::new_with_pkg_fs_and_mounts(pkgfs, Mounts::new())
    }

    fn new_with_pkg_fs_and_mounts(pkgfs: P, mounts: Mounts) -> Self {
        let mut amber =
            AppBuilder::new("fuchsia-pkg://fuchsia.com/pkg-resolver-tests#meta/amber.cmx")
                .add_handle_to_namespace(
                    "/pkgfs".to_owned(),
                    pkgfs.root_dir_client_end().expect("pkgfs dir to open").into(),
                );

        let mut pkg_cache = AppBuilder::new(
            "fuchsia-pkg://fuchsia.com/pkg-resolver-tests#meta/pkg_cache.cmx".to_owned(),
        )
        .add_handle_to_namespace(
            "/pkgfs".to_owned(),
            pkgfs.root_dir_client_end().expect("pkgfs dir to open").into(),
        );

        let mut pkg_resolver = AppBuilder::new(
            "fuchsia-pkg://fuchsia.com/pkg-resolver-tests#meta/pkg_resolver.cmx".to_owned(),
        )
        .add_handle_to_namespace(
            "/pkgfs".to_owned(),
            pkgfs.root_dir_client_end().expect("pkgfs dir to open").into(),
        )
        .add_dir_or_proxy_to_namespace("/data", &mounts.pkg_resolver_data)
        .add_dir_or_proxy_to_namespace("/config/data", &mounts.pkg_resolver_config_data);

        let mut fs = ServiceFs::new();
        fs.add_proxy_service::<fidl_fuchsia_net::NameLookupMarker, _>()
            .add_proxy_service::<fidl_fuchsia_posix_socket::ProviderMarker, _>()
            .add_proxy_service_to::<AmberMarker, _>(amber.directory_request().unwrap().clone())
            .add_proxy_service_to::<PackageCacheMarker, _>(
                pkg_cache.directory_request().unwrap().clone(),
            )
            .add_proxy_service_to::<RepositoryManagerMarker, _>(
                pkg_resolver.directory_request().unwrap().clone(),
            )
            .add_proxy_service_to::<PackageResolverAdminMarker, _>(
                pkg_resolver.directory_request().unwrap().clone(),
            )
            .add_proxy_service_to::<PackageResolverMarker, _>(
                pkg_resolver.directory_request().unwrap().clone(),
            );

        let mut salt = [0; 4];
        zx::cprng_draw(&mut salt[..]).expect("zx_cprng_draw does not fail");
        let environment_label = format!("pkg-resolver-env_{}", hex::encode(&salt));
        let env = fs
            .create_nested_environment(&environment_label)
            .expect("nested environment to create successfully");
        fasync::spawn(fs.collect());

        let amber = amber.spawn(env.launcher()).expect("amber to launch");
        let pkg_cache = pkg_cache.spawn(env.launcher()).expect("package cache to launch");
        let pkg_resolver = pkg_resolver.spawn(env.launcher()).expect("package resolver to launch");

        let resolver_proxy =
            env.connect_to_service::<PackageResolverMarker>().expect("connect to package resolver");
        let resolver_admin_proxy = env
            .connect_to_service::<PackageResolverAdminMarker>()
            .expect("connect to package resolver admin");
        let repo_manager_proxy = env
            .connect_to_service::<RepositoryManagerMarker>()
            .expect("connect to repository manager");
        let rewrite_engine_proxy = pkg_resolver
            .connect_to_service::<RewriteEngineMarker>()
            .expect("connect to rewrite engine");

        Self {
            env,
            pkgfs,
            apps: Apps { _amber: amber, _pkg_cache: pkg_cache, _pkg_resolver: pkg_resolver },
            proxies: Proxies {
                resolver: resolver_proxy,
                resolver_admin: resolver_admin_proxy,
                repo_manager: repo_manager_proxy,
                rewrite_engine: rewrite_engine_proxy,
            },
            _mounts: mounts,
            nested_environment_label: environment_label,
        }
    }

    fn launcher(&self) -> &LauncherProxy {
        self.env.launcher()
    }

    async fn set_experiment_state(&self, experiment: Experiment, state: bool) {
        self.proxies
            .resolver_admin
            .set_experiment_state(experiment, state)
            .await
            .expect("experiment state to toggle");
    }

    pub async fn register_repo(&self, repo: &ServedRepository) {
        let repo_url = "fuchsia-pkg://test".parse().unwrap();
        let repo_config = repo.make_repo_config(repo_url);

        self.proxies.repo_manager.add(repo_config.into()).await.unwrap();
    }

    fn connect_to_resolver(&self) -> PackageResolverProxy {
        self.env.connect_to_service::<PackageResolverMarker>().expect("connect to package resolver")
    }

    fn resolve_package(&self, url: &str) -> impl Future<Output = Result<DirectoryProxy, Status>> {
        resolve_package(&self.proxies.resolver, url)
    }

    async fn pkg_resolver_inspect_hierarchy(&self) -> NodeHierarchy {
        let pattern = format!(
            "/hub/r/{}/*/c/pkg_resolver.cmx/*/out/diagnostics/{}",
            glob::Pattern::escape(&self.nested_environment_label),
            TreeMarker::SERVICE_NAME,
        );
        let paths = glob::glob_with(
            &pattern,
            glob::MatchOptions {
                case_sensitive: true,
                require_literal_separator: true,
                require_literal_leading_dot: false,
            },
        )
        .expect("glob pattern successfully compiles");
        let mut paths = paths.collect::<Result<Vec<_>, _>>().unwrap();
        assert_eq!(paths.len(), 1, "{:?}", paths);
        let path = paths.pop().unwrap();

        let (tree, server_end) =
            fidl::endpoints::create_proxy::<TreeMarker>().expect("failed to create Tree proxy");
        fdio::service_connect(&path.to_string_lossy().to_string(), server_end.into_channel())
            .expect("failed to connect to Tree service");
        NodeHierarchy::try_from_tree(&tree).await.expect("failed to get inspect hierarchy")
    }
}

const EMPTY_REPO_PATH: &str = "/pkg/empty-repo";
const ROLLDICE_BIN: &'static [u8] = b"#!/boot/bin/sh\necho 4\n";
const ROLLDICE_CMX: &'static [u8] = br#"{"program":{"binary":"bin/rolldice"}}"#;

fn extra_blob_contents(i: u32) -> Vec<u8> {
    format!("contents of file {}", i).as_bytes().to_owned()
}

async fn make_rolldice_pkg_with_extra_blobs(n: u32) -> Package {
    let mut pkg = PackageBuilder::new("rolldice")
        .add_resource_at("bin/rolldice", ROLLDICE_BIN)
        .add_resource_at("meta/rolldice.cmx", ROLLDICE_CMX);
    for i in 0..n {
        pkg = pkg.add_resource_at(format!("data/file{}", i), extra_blob_contents(i).as_slice());
    }
    pkg.build().await.unwrap()
}

fn resolve_package(
    resolver: &PackageResolverProxy,
    url: &str,
) -> impl Future<Output = Result<DirectoryProxy, Status>> {
    let (package, package_server_end) = fidl::endpoints::create_proxy().unwrap();
    let selectors: Vec<&str> = vec![];
    let status_fut = resolver.resolve(
        url,
        &mut selectors.into_iter(),
        &mut UpdatePolicy { fetch_if_absent: true, allow_old_versions: false },
        package_server_end,
    );
    async move {
        let status = status_fut.await.expect("package resolve fidl call");
        Status::ok(status)?;
        Ok(package)
    }
}

async fn body_to_bytes(body: Body) -> Vec<u8> {
    body.compat().try_concat().await.expect("body stream to complete").to_vec()
}
