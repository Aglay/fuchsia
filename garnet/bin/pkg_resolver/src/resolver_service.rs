// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::amber_connector::AmberConnect,
    crate::rewrite_manager::RewriteManager,
    failure::Error,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{self, DirectoryMarker},
    fidl_fuchsia_pkg::{
        PackageCacheProxy, PackageResolverRequest, PackageResolverRequestStream, UpdatePolicy,
    },
    fidl_fuchsia_pkg_ext::BlobId,
    fuchsia_async as fasync,
    fuchsia_syslog::{fx_log_err, fx_log_info, fx_log_warn},
    fuchsia_url::pkg_url::PkgUrl,
    fuchsia_zircon::{Channel, MessageBuf, Signals, Status},
    futures::prelude::*,
    parking_lot::RwLock,
    std::sync::Arc,
};

// The error amber returns if it could not find the merkle for this package.
const PACKAGE_NOT_FOUND: &str = "merkle not found for package";

pub async fn run_resolver_service<A>(
    rewrites: Arc<RwLock<RewriteManager>>,
    amber_connector: A,
    cache: PackageCacheProxy,
    mut stream: PackageResolverRequestStream,
) -> Result<(), Error>
where
    A: AmberConnect,
{
    while let Some(event) = await!(stream.try_next())? {
        let PackageResolverRequest::Resolve {
            package_url,
            selectors,
            update_policy,
            dir,
            responder,
        } = event;

        let status = await!(resolve(
            &rewrites,
            &amber_connector,
            &cache,
            package_url,
            selectors,
            update_policy,
            dir
        ));

        responder.send(Status::from(status).into_raw())?;
    }

    Ok(())
}

/// Resolve the package.
///
/// FIXME: at the moment, we are proxying to Amber to resolve a package name and variant to a
/// merkleroot. Because of this, we cant' implement the update policy, so we just ignore it.
async fn resolve<'a, A>(
    rewrites: &'a Arc<RwLock<RewriteManager>>,
    amber_connector: &'a A,
    cache: &'a PackageCacheProxy,
    pkg_url: String,
    selectors: Vec<String>,
    _update_policy: UpdatePolicy,
    dir_request: ServerEnd<DirectoryMarker>,
) -> Result<(), Status>
where
    A: AmberConnect,
{
    let url = PkgUrl::parse(&pkg_url).map_err(|err| {
        fx_log_err!("failed to parse package url {:?}: {}", pkg_url, err);
        Err(Status::INVALID_ARGS)
    })?;
    let was_fuchsia_host = url.host() == "fuchsia.com";
    let url = rewrites.read().rewrite(url);

    // FIXME: at the moment only the fuchsia.com host is supported.
    if !was_fuchsia_host && url.host() != "fuchsia.com" {
        fx_log_err!("package url's host is currently unsupported: {}", url);
        return Err(Status::INVALID_ARGS);
    }

    // While the fuchsia-pkg:// spec doesn't require a package name, we do.
    let name = url.name().ok_or_else(|| {
        fx_log_err!("package url is missing a package name: {}", url);
        Err(Status::INVALID_ARGS)
    })?;

    // While the fuchsia-pkg:// spec allows resource paths, the package resolver should not be
    // given one.
    if url.resource().is_some() {
        fx_log_err!("package url should not contain a resource name: {}", url);
        return Err(Status::INVALID_ARGS);
    }

    // FIXME: need to implement selectors.
    if !selectors.is_empty() {
        fx_log_warn!("resolve does not support selectors yet");
    }

    // FIXME: use the package cache to fetch the package instead of amber.

    // Ask amber to cache the package.
    let amber = amber_connector.connect()?;
    let chan = await!(amber.get_update_complete(&name, url.variant(), url.package_hash()))
        .map_err(|err| {
            fx_log_err!("error communicating with amber: {:?}", err);
            Status::INTERNAL
        })?;

    let merkle = await!(wait_for_update_to_complete(chan, &url)).map_err(|err| {
        fx_log_err!("error when waiting for amber to complete: {:?}", err);
        err
    })?;

    fx_log_info!(
        "resolved {} as {} with the selectors {:?} to {}",
        pkg_url,
        url,
        selectors,
        merkle
    );

    await!(cache.open(&mut merkle.into(), &mut selectors.iter().map(|s| s.as_str()), dir_request))
        .map_err(|err| {
            fx_log_err!("error opening {}: {:?}", merkle, err);
            Status::INTERNAL
        })?;

    Ok(())
}

// Checks for the error amber returns if it could resolve a merkle for this
// package, but it couldn't download the package.
//
// Format: "not found in \\d+ active sources"
fn is_unavailable_msg(msg: &str) -> bool {
    const UNAVAILABLE_PRE: &str = "not found in ";
    const UNAVAILABLE_POST: &str = "active sources";

    if !msg.starts_with(UNAVAILABLE_PRE) {
        return false;
    }
    let (_unavailable_pre, tail) = msg.split_at(UNAVAILABLE_PRE.len());
    let tail_chars = &mut tail.chars();
    let mut c = tail_chars.next();
    // require at least one digit
    if !c.map_or(false, |c| c.is_numeric()) {
        return false;
    }
    loop {
        c = tail_chars.next();
        if !c.map_or(false, |c| c.is_numeric()) {
            // check for space after digit
            if let Some(' ') = c {
                break;
            } else {
                return false;
            }
        }
    }
    // take remaining digits
    let tail = tail_chars.as_str();
    return tail.starts_with(UNAVAILABLE_POST);
}

async fn wait_for_update_to_complete(chan: Channel, url: &PkgUrl) -> Result<BlobId, Status> {
    let mut buf = MessageBuf::new();

    let sigs = await!(fasync::OnSignals::new(
        &chan,
        Signals::CHANNEL_PEER_CLOSED | Signals::CHANNEL_READABLE
    ))?;

    if sigs.contains(Signals::CHANNEL_READABLE) {
        chan.read(&mut buf)?;
        let buf = buf.split().0;

        if sigs.contains(Signals::USER_0) {
            let msg = String::from_utf8_lossy(&buf);

            if msg.starts_with(PACKAGE_NOT_FOUND) {
                fx_log_info!("package {} was not found: {}", url, msg);
                return Err(Status::NOT_FOUND);
            }

            if is_unavailable_msg(&msg) {
                fx_log_info!("package {} is currently unavailable: {}", url, msg);
                return Err(Status::UNAVAILABLE);
            }

            fx_log_err!("error installing package {}: {}", url, msg);

            return Err(Status::INTERNAL);
        }

        let merkle = match String::from_utf8(buf) {
            Ok(merkle) => merkle,
            Err(err) => {
                let merkle = String::from_utf8_lossy(err.as_bytes());
                fx_log_err!("{:?} is not a valid UTF-8 encoded merkleroot: {:?}", merkle, err);

                return Err(Status::INTERNAL);
            }
        };

        let merkle = match merkle.parse() {
            Ok(merkle) => merkle,
            Err(err) => {
                fx_log_err!("{:?} is not a valid merkleroot: {:?}", merkle, err);

                return Err(Status::INTERNAL);
            }
        };

        Ok(merkle)
    } else {
        fx_log_err!("response channel closed unexpectedly");
        Err(Status::INTERNAL)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::amber_connector::AmberConnect;
    use crate::rewrite_manager::{tests::make_rule_config, RewriteManagerBuilder};
    use crate::test_util::{
        MockAmberBuilder, MockAmberConnector, MockPackageCache, Package, PackageKind,
    };
    use fidl::endpoints::{self, ServerEnd};
    use fidl_fuchsia_io::DirectoryProxy;
    use fidl_fuchsia_pkg::{self, PackageCacheProxy, UpdatePolicy};
    use files_async;
    use fuchsia_async as fasync;
    use fuchsia_url_rewrite::Rule;
    use fuchsia_zircon::{Channel, Status};
    use std::fs;
    use std::path::Path;
    use std::str;
    use std::sync::Arc;
    use tempfile::TempDir;

    struct ResolveTest {
        rewrite_manager: Arc<RwLock<RewriteManager>>,
        amber_connector: MockAmberConnector,
        cache_proxy: PackageCacheProxy,
        pkgfs: Arc<TempDir>,
    }

    impl ResolveTest {
        fn check_dir(&self, dir_path: &Path, want_files: &Vec<String>) {
            let mut files: Vec<String> = fs::read_dir(&dir_path)
                .expect("could not read dir")
                .into_iter()
                .map(|entry| {
                    entry
                        .expect("get directory entry")
                        .file_name()
                        .to_str()
                        .expect("valid utf8")
                        .into()
                })
                .collect();
            files.sort_unstable();
            assert_eq!(&files, want_files);
        }

        async fn check_dir_async<'a>(
            &'a self,
            dir: &'a DirectoryProxy,
            want_files: &'a Vec<String>,
        ) {
            let entries = await!(files_async::readdir(dir)).expect("could not read dir");
            let mut files: Vec<_> = entries.into_iter().map(|f| f.name).collect();
            files.sort_unstable();
            assert_eq!(&files, want_files);
        }

        async fn check_amber_update<'a>(
            &'a self,
            name: &'a str,
            variant: Option<&'a str>,
            merkle: Option<&'a str>,
            expected_res: Result<String, Status>,
        ) {
            let amber = self.amber_connector.connect().unwrap();
            let chan = await!(amber.get_update_complete(name, variant, merkle))
                .expect("error communicating with amber");
            let expected_res = expected_res.map(|r| r.parse().expect("could not parse blob"));

            let path = match variant {
                None => format!("/{}", name),
                Some(variant) => format!("/{}/{}", name, variant),
            };

            let url =
                PkgUrl::new_package("fuchsia.com".to_string(), path, merkle.map(|s| s.to_string()))
                    .unwrap();

            let res = await!(wait_for_update_to_complete(chan, &url));
            assert_eq!(res, expected_res);
        }

        async fn run_resolve<'a>(
            &'a self,
            url: &'a str,
            expected_res: Result<Vec<String>, Status>,
        ) {
            let selectors = vec![];
            let update_policy = UpdatePolicy { fetch_if_absent: true, allow_old_versions: false };
            let (package_dir_c, package_dir_s) = Channel::create().unwrap();
            let res = await!(resolve(
                &self.rewrite_manager,
                &self.amber_connector,
                &self.cache_proxy,
                url.to_string(),
                selectors,
                update_policy,
                ServerEnd::new(package_dir_s),
            ));
            if res.is_ok() {
                let expected_files = expected_res.as_ref().unwrap();
                let dir_proxy =
                    DirectoryProxy::new(fasync::Channel::from_channel(package_dir_c).unwrap());
                await!(self.check_dir_async(&dir_proxy, expected_files));
            }
            assert_eq!(res, expected_res.map(|_s| ()), "unexpected result for {}", url);
        }
    }

    struct ResolveTestBuilder {
        pkgfs: Arc<TempDir>,
        amber: MockAmberBuilder,
        static_rewrite_rules: Vec<Rule>,
        dynamic_rewrite_rules: Vec<Rule>,
    }

    impl ResolveTestBuilder {
        fn new() -> Self {
            let pkgfs = Arc::new(TempDir::new().expect("failed to create tmp dir"));
            let amber = MockAmberBuilder::new(pkgfs.clone());
            ResolveTestBuilder {
                pkgfs: pkgfs.clone(),
                amber: amber,
                static_rewrite_rules: vec![],
                dynamic_rewrite_rules: vec![],
            }
        }

        fn packages<I: IntoIterator<Item = Package>>(mut self, packages: I) -> Self {
            self.amber = self.amber.packages(packages);
            self
        }

        fn static_rewrite_rules<I: IntoIterator<Item = Rule>>(mut self, rules: I) -> Self {
            self.static_rewrite_rules.extend(rules);
            self
        }

        fn build(self) -> ResolveTest {
            let amber = self.amber.build();
            let amber_connector = MockAmberConnector::new(amber);

            let cache = Arc::new(
                MockPackageCache::new(self.pkgfs.clone()).expect("failed to create cache"),
            );
            let cache_proxy: PackageCacheProxy =
                endpoints::spawn_local_stream_handler(move |req| {
                    let cache = cache.clone();
                    async move {
                        cache.open(req);
                    }
                })
                .expect("failed to spawn handler");

            let inspector = fuchsia_inspect::Inspector::new();
            let node = inspector.root().create_child("top-level-node");
            let dynamic_rule_config = make_rule_config(self.dynamic_rewrite_rules);
            let rewrite_manager = RewriteManagerBuilder::new(node, &dynamic_rule_config)
                .unwrap()
                .static_rules(self.static_rewrite_rules)
                .build();

            ResolveTest {
                rewrite_manager: Arc::new(RwLock::new(rewrite_manager)),
                pkgfs: self.pkgfs,
                amber_connector,
                cache_proxy,
            }
        }
    }

    fn gen_merkle(c: char) -> String {
        (0..64).map(|_| c).collect()
    }

    fn gen_merkle_file(c: char) -> String {
        format!("{}_file", gen_merkle(c))
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_mock_amber() {
        let test = ResolveTestBuilder::new()
            .packages(vec![
                Package::new("foo", "0", &gen_merkle('a'), PackageKind::Ok),
                Package::new("bar", "stable", &gen_merkle('b'), PackageKind::Ok),
                Package::new("baz", "stable", &gen_merkle('c'), PackageKind::Ok),
                Package::new("buz", "0", &gen_merkle('c'), PackageKind::Ok),
            ])
            .build();

        // Name
        await!(test.check_amber_update("foo", None, None, Ok(gen_merkle('a'))));

        // Name and variant
        await!(test.check_amber_update("bar", Some("stable"), None, Ok(gen_merkle('b'))));

        // Name, variant, and merkle
        let merkle = gen_merkle('c');
        await!(test.check_amber_update("baz", Some("stable"), Some(&merkle), Ok(gen_merkle('c'))));

        // Nonexistent package
        await!(test.check_amber_update("nonexistent", None, None, Err(Status::NOT_FOUND)));

        // no merkle('d') since we didn't ask to update "buz".
        test.check_dir(test.pkgfs.path(), &vec![gen_merkle('a'), gen_merkle('b'), gen_merkle('c')]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_resolve_package() {
        let test = ResolveTestBuilder::new()
            .packages(vec![
                Package::new("foo", "0", &gen_merkle('a'), PackageKind::Ok),
                Package::new("bar", "stable", &gen_merkle('b'), PackageKind::Ok),
            ])
            .build();

        // Package name
        await!(test.run_resolve("fuchsia-pkg://fuchsia.com/foo", Ok(vec![gen_merkle_file('a')]),));

        // Package name and variant
        await!(test
            .run_resolve("fuchsia-pkg://fuchsia.com/bar/stable", Ok(vec![gen_merkle_file('b')]),));

        // Package name, variant, and merkle
        let url = format!("fuchsia-pkg://fuchsia.com/bar/stable?hash={}", gen_merkle('b'));
        await!(test.run_resolve(&url, Ok(vec![gen_merkle_file('b')],)));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_resolve_package_error() {
        let test = ResolveTestBuilder::new()
            .packages(vec![
                Package::new("foo", "stable", &gen_merkle('a'), PackageKind::Ok),
                Package::new(
                    "unavailable",
                    "0",
                    &gen_merkle('a'),
                    PackageKind::Error("not found in 1 active sources. last error: ".to_string()),
                ),
            ])
            .build();

        // Missing package
        await!(test.run_resolve("fuchsia-pkg://fuchsia.com/foo/beta", Err(Status::NOT_FOUND)));

        // Unavailable package
        await!(
            test.run_resolve("fuchsia-pkg://fuchsia.com/unavailable/0", Err(Status::UNAVAILABLE))
        );

        // Bad package URL
        await!(test.run_resolve("fuchsia-pkg://fuchsia.com/foo!", Err(Status::INVALID_ARGS)));

        // No package name
        await!(test.run_resolve("fuchsia-pkg://fuchsia.com", Err(Status::INVALID_ARGS)));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_resolve_package_unknown_host() {
        let test = ResolveTestBuilder::new()
            .packages(vec![
                Package::new("foo", "0", &gen_merkle('a'), PackageKind::Ok),
                Package::new("bar", "stable", &gen_merkle('b'), PackageKind::Ok),
            ])
            .static_rewrite_rules(vec![Rule::new(
                "example.com".to_owned(),
                "fuchsia.com".to_owned(),
                "/foo/".to_owned(),
                "/foo/".to_owned(),
            )
            .unwrap()])
            .build();

        await!(test.run_resolve("fuchsia-pkg://example.com/foo/0", Ok(vec![gen_merkle_file('a')]),));
        await!(test.run_resolve("fuchsia-pkg://fuchsia.com/foo/0", Ok(vec![gen_merkle_file('a')]),));
        await!(test.run_resolve("fuchsia-pkg://example.com/bar/stable", Err(Status::INVALID_ARGS)));
        await!(test
            .run_resolve("fuchsia-pkg://fuchsia.com/bar/stable", Ok(vec![gen_merkle_file('b')]),));
    }

    #[test]
    fn test_is_unavailable_msg() {
        // Success:
        assert!(is_unavailable_msg("not found in 1 active sources"), "single digit");
        assert!(
            is_unavailable_msg("not found in 12345678901928 active sources"),
            "multiple digits"
        );

        // Failure:
        assert!(!is_unavailable_msg("not found in  active sources"), "no digits");
        assert!(!is_unavailable_msg("not found in 1"), "no suffix");
        assert!(!is_unavailable_msg("1 active sources"), "no prefix");
        assert!(!is_unavailable_msg(""), "empty");
    }
}
