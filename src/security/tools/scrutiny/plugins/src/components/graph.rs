// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::components::{
        controllers::component_controllers::*,
        controllers::package_controllers::*,
        controllers::route_controllers::*,
        controllers::zbi_controllers::*,
        http::{HttpGetter, PackageGetter},
        package_reader::*,
        types::*,
        util,
    },
    anyhow::{anyhow, Result},
    lazy_static::lazy_static,
    log::{debug, error, info, warn},
    regex::Regex,
    scrutiny::{
        collectors, controllers,
        engine::hook::PluginHooks,
        engine::plugin::{Plugin, PluginDescriptor},
        model::collector::DataCollector,
        model::controller::DataController,
        model::model::{Component, DataModel, Manifest, Package, Route, Zbi, ZbiType},
        plugin,
    },
    scrutiny_utils::{bootfs::*, zbi::*},
    std::collections::HashMap,
    std::env,
    std::str,
    std::sync::Arc,
};

// Constants/Statics
lazy_static! {
    static ref SERVICE_CONFIG_RE: Regex = Regex::new(r"data/sysmgr/.+\.config").unwrap();
}
pub const CONFIG_DATA_PKG_URL: &str = "fuchsia-pkg://fuchsia.com/config-data";

plugin!(
    ComponentGraphPlugin,
    PluginHooks::new(
        collectors! {
            "PackageDataCollector" => PackageDataCollector::new().unwrap(),
        },
        controllers! {
            "/components" => ComponentsGraphController::default(),
            "/component/id" => ComponentIdGraphController::default(),
            "/component/from_uri" => ComponentFromUriGraphController::default(),
            "/component/uses" => ComponentUsesGraphController::default(),
            "/component/used" => ComponentUsedGraphController::default(),
            "/component/raw_manifest" => RawManifestGraphController::default(),
            "/component/manifest/sandbox" => ComponentSandboxGraphController::default(),
            "/packages" => PackagesGraphController::default(),
            "/routes" => RoutesGraphController::default(),
            "/bootfs" => BootfsPathsController::default(),
        }
    ),
    vec![]
);

pub struct PackageDataCollector {
    package_reader: Box<dyn PackageReader>,
}

impl PackageDataCollector {
    fn new() -> Result<Self> {
        let fuchsia_dir = env::var("FUCHSIA_DIR")?;
        if fuchsia_dir.len() == 0 {
            return Err(anyhow!("Unable to retrieve $FUCHSIA_DIR, has it been set?"));
        }

        Ok(Self {
            package_reader: Box::new(PackageServerReader::new(
                fuchsia_dir,
                Box::new(HttpGetter::new(String::from("127.0.0.1:8083"))),
            )),
        })
    }

    fn get_packages(&self) -> Result<Vec<PackageDefinition>> {
        // Retrieve the json packages definition from the package server.
        let targets = self.package_reader.read_targets()?;

        let mut pkgs: Vec<PackageDefinition> = Vec::new();
        for (name, target) in targets.signed.targets.iter() {
            let pkg_def =
                self.package_reader.read_package_definition(&name, &target.custom.merkle)?;
            pkgs.push(pkg_def);
        }

        Ok(pkgs)
    }

    fn get_builtins(&self) -> Result<(Vec<PackageDefinition>, ServiceMapping)> {
        let builtins = self.package_reader.read_builtins()?;

        let mut packages = Vec::new();
        for package in builtins.packages {
            let mut pkg_def = PackageDefinition {
                url: util::to_package_url(&package.url)?,
                typ: PackageType::BUILTIN,
                merkle: String::from("0"),
                contents: HashMap::new(),
                cms: HashMap::new(),
            };

            pkg_def.cms.insert(String::from(""), CmxDefinition::from(package.manifest));
            packages.push(pkg_def);
        }

        Ok((packages, builtins.services))
    }

    // Combine service name->url mappings from builtins and those defined in config-data.
    // This consumes the builtins ServiceMapping as part of building the combined map.
    fn merge_services(
        &self,
        served: &Vec<PackageDefinition>,
        builtins: ServiceMapping,
    ) -> Result<ServiceMapping> {
        let mut combined = ServiceMapping::new();

        // Find and add all services as defined in config-data
        for pkg_def in served {
            if pkg_def.url == CONFIG_DATA_PKG_URL {
                for (name, merkle) in &pkg_def.contents {
                    if SERVICE_CONFIG_RE.is_match(&name) {
                        let service_pkg =
                            self.package_reader.read_service_package_definition(merkle)?;

                        if let Some(services) = service_pkg.services {
                            for (service_name, service_url_or_array) in services {
                                if combined.contains_key(&service_name) {
                                    debug!(
                                        "Service mapping collision on {} between {} and {}",
                                        service_name, combined[&service_name], service_url_or_array
                                    );
                                }

                                // service_url_or_array could be a String array with the service_url as the first index, and command line args following
                                let service_url: String;
                                if service_url_or_array.is_array() {
                                    let service_array = service_url_or_array.as_array().unwrap();
                                    if service_array[0].is_string() {
                                        service_url =
                                            service_array[0].as_str().unwrap().to_string();
                                    } else {
                                        error!(
                                            "Expected a string service url, instead got: {}:{}",
                                            service_name, service_array[0]
                                        );
                                        continue;
                                    }
                                } else if service_url_or_array.is_string() {
                                    service_url =
                                        service_url_or_array.as_str().unwrap().to_string();
                                } else {
                                    error!(
                                        "Unexpected service mapping found: {}:{}",
                                        service_name, service_url_or_array
                                    );
                                    continue;
                                }

                                combined.insert(service_name, service_url);
                            }
                        } else {
                            debug!("Expected service with merkle {} to exist. Optimistically continuing.", merkle);
                        }
                    }
                }
                // Stop looking for other config-data packages once we've found at least one.
                break;
            }
        }
        // Add all builtin services
        for (service_name, service_url) in builtins {
            if combined.contains_key(&service_name) {
                debug!(
                    "Service mapping collision on {} between {} and {}",
                    service_name, combined[&service_name], service_url
                );
            }

            combined.insert(service_name, service_url);
        }

        Ok(combined)
    }

    /// Extracts the ZBI from the update package and parses it into the ZBI
    /// model.
    fn extract_zbi(package: &PackageDefinition) -> Result<Zbi> {
        info!("Extracting the ZBI from {}", package.url);
        let getter = HttpGetter::new("127.0.0.1:8083".to_string());
        for (path, merkle) in package.contents.iter() {
            if path == "zbi" {
                let zbi_data = getter.read_raw(&format!("/blobs/{}", merkle))?;
                let mut reader = ZbiReader::new(zbi_data);
                let sections = reader.parse()?;
                let mut bootfs = HashMap::new();
                info!("Extracted {} sections from the ZBI", sections.len());
                for section in sections.iter() {
                    info!("Extracted sections {:?}", section.section_type);
                    if section.section_type == ZbiType::StorageBootfs {
                        let mut bootfs_reader = BootfsReader::new(section.buffer.clone());
                        let bootfs_result = bootfs_reader.parse();
                        if let Err(err) = bootfs_result {
                            warn!("Bootfs parse failed {}", err);
                        } else {
                            bootfs = bootfs_result.unwrap();
                            info!("Bootfs found {} files", bootfs.len());
                        }
                    }
                }
                return Ok(Zbi { sections, bootfs });
            }
        }
        return Err(anyhow!("Unable to find a zbi file in the package."));
    }

    /// Function to build the component graph model out of the packages and services retrieved
    /// by this collector.
    fn build_model(
        served_pkgs: Vec<PackageDefinition>,
        _builtin_pkgs: Vec<PackageDefinition>,
        mut service_map: ServiceMapping,
    ) -> Result<(HashMap<String, Component>, Vec<Package>, Vec<Manifest>, Vec<Route>, Option<Zbi>)>
    {
        let mut components: HashMap<String, Component> = HashMap::new();
        let mut packages: Vec<Package> = Vec::new();
        let mut manifests: Vec<Manifest> = Vec::new();
        let mut routes: Vec<Route> = Vec::new();
        let mut zbi: Option<Zbi> = None;

        // Iterate through all served packages, for each cmx they define, create a node.
        let mut idx = 0;
        for pkg in served_pkgs.iter() {
            let package = Package {
                url: pkg.url.clone(),
                merkle: pkg.merkle.clone(),
                contents: pkg.contents.clone(),
            };
            packages.push(package);

            // If the package is the update package attempt to extract the ZBI.
            if pkg.url == "fuchsia-pkg://fuchsia.com/update" {
                let zbi_result = PackageDataCollector::extract_zbi(&pkg);
                if let Err(err) = zbi_result {
                    warn!("{}", err);
                } else {
                    zbi = Some(zbi_result.unwrap());
                }
            }

            for (path, cm) in &pkg.cms {
                if path.starts_with("meta/") && path.ends_with(".cmx") {
                    idx += 1;
                    let url = format!("{}#{}", pkg.url, path);
                    components.insert(
                        url.clone(),
                        Component { id: idx, url: url.clone(), version: 1, inferred: false },
                    );

                    let mani = {
                        if let Some(sandbox) = &cm.sandbox {
                            Manifest {
                                component_id: idx,
                                manifest: serde_json::to_string(&sandbox)?,
                                uses: {
                                    match sandbox.services.as_ref() {
                                        Some(svcs) => svcs.clone(),
                                        None => Vec::new(),
                                    }
                                },
                            }
                        } else {
                            Manifest {
                                component_id: idx,
                                manifest: String::from(""),
                                uses: Vec::new(),
                            }
                        }
                    };
                    manifests.push(mani);
                }
            }
        }

        // Iterate through all services mappings, for each one, find the associated node or create a new
        // inferred node and mark it as a provider of that service.
        for (service_name, pkg_url) in service_map.iter() {
            if !components.contains_key(pkg_url) {
                // We don't already know about the component that *should* provide this service.
                // Create an inferred node.
                debug!("Expected component {} exist to provide service {} but it does not exist. Creating inferred node.", pkg_url, service_name);
                idx += 1;
                components.insert(
                    pkg_url.clone(),
                    Component {
                        id: idx,
                        url: pkg_url.clone(),
                        version: 1,
                        inferred: true,
                    },
                );
            }
        }

        // Iterate through all nodes created thus far, creating edges between them based on the services they use.
        // If a service provider node is not able to be found, create a new inferred service provider node.
        // Since manifests more naturally hold the list of services that the component requires, we iterate through
        // those instead. Can be changed relatively effortlessly if the model make sense otherwise.
        let mut route_idx = 0;
        for mani in &manifests {
            for service_name in &mani.uses {
                let target_id = {
                    if service_map.contains_key(service_name) {
                        // FIXME: Options do not impl Try so we cannot ? but there must be some better way to get at a value...
                        components.get(service_map.get(service_name).unwrap()).unwrap().id
                    } else {
                        // Even the service map didn't know about this service. We should create an inferred component
                        // that provides this service.
                        debug!("Expected a service provider for service {} but it does not exist. Creating inferred node.", service_name);
                        idx += 1;
                        let url = format!("fuchsia-pkg://inferred#meta/{}.cmx", service_name);
                        components.insert(
                            url.clone(),
                            Component {
                                id: idx,
                                url: url.clone(),
                                version: 1,
                                inferred: true,
                            },
                        );
                        // Add the inferred node to the service map to be found by future consumers of the service
                        service_map.insert(String::from(service_name), url);
                        idx
                    }
                };
                route_idx += 1;
                routes.push(Route {
                    id: route_idx,
                    src_id: mani.component_id,
                    dst_id: target_id,
                    service_name: service_name.to_string(),
                    protocol_id: 0, // FIXME:
                });
            }
        }

        info!(
            "Components: {}, Manifests {}, Routes {}.",
            components.len(),
            manifests.len(),
            routes.len()
        );

        Ok((components, packages, manifests, routes, zbi))
    }
}

impl DataCollector for PackageDataCollector {
    /// Collects and builds a DAG of component nodes (with manifests) and routes that
    /// connect the nodes.
    fn collect(&self, model: Arc<DataModel>) -> Result<()> {
        let served_packages = self.get_packages()?;

        let (builtin_packages, builtin_services) = self.get_builtins()?;

        let services = self.merge_services(&served_packages, builtin_services)?;

        info!(
            "Done collecting. Found {} services, {} served packages, and {} builtin packages.",
            services.keys().len(),
            served_packages.len(),
            builtin_packages.len()
        );

        let (components, mut packages, mut manifests, mut routes, zbi) =
            PackageDataCollector::build_model(served_packages, builtin_packages, services)?;
        let mut model_comps = model.components().write().unwrap();
        model_comps.clear();
        for (_, val) in components.into_iter() {
            model_comps.push(val);
        }

        let mut model_packages = model.packages().write().unwrap();
        model_packages.clear();
        model_packages.append(&mut packages);

        let mut model_manifests = model.manifests().write().unwrap();
        model_manifests.clear();
        model_manifests.append(&mut manifests);

        let mut model_routes = model.routes().write().unwrap();
        model_routes.clear();
        model_routes.append(&mut routes);

        let mut model_zbi = model.zbi().write().unwrap();
        *model_zbi = zbi;

        Ok(())
    }
}

// It doesn't seem incredibly useful to test the json parsing or the far file
// parsing, as those are mainly provided by dependencies.
// The retireval from package server is defined here, but feels difficult to
// test in a meaningful way.
// As a result, the bulk of the testing is focused around the graph/model
// building logic.
#[cfg(test)]
mod tests {
    use {super::*, crate::components::jsons::*, std::sync::RwLock, tempfile::tempdir};

    struct MockPackageReader {
        targets: RwLock<Vec<TargetsJson>>,
        package_defs: RwLock<Vec<PackageDefinition>>,
        service_package_defs: RwLock<Vec<ServicePackageDefinition>>,
        builtins: RwLock<Vec<BuiltinsJson>>,
    }

    impl MockPackageReader {
        fn new() -> Self {
            Self {
                targets: RwLock::new(Vec::new()),
                package_defs: RwLock::new(Vec::new()),
                service_package_defs: RwLock::new(Vec::new()),
                builtins: RwLock::new(Vec::new()),
            }
        }

        // Adds values to the FIFO queue for targets
        fn append_target(&self, target: TargetsJson) {
            self.targets.write().unwrap().push(target);
        }
        // Adds values to the FIFO queue for package_defs
        fn append_pkg_def(&self, package_def: PackageDefinition) {
            self.package_defs.write().unwrap().push(package_def);
        }
        // Adds values to the FIFO queue for service_package_defs
        fn append_service_pkg_def(&self, svc_pkg_def: ServicePackageDefinition) {
            self.service_package_defs.write().unwrap().push(svc_pkg_def);
        }
        // Adds values to the FIFO queue for builtins
        fn append_builtin(&self, builtin: BuiltinsJson) {
            self.builtins.write().unwrap().push(builtin);
        }
    }

    impl PackageReader for MockPackageReader {
        fn read_targets(&self) -> Result<TargetsJson> {
            let mut borrow = self.targets.write().unwrap();
            {
                if borrow.len() == 0 {
                    return Err(anyhow!("No more targets left to return. Maybe append more?"));
                }
                Ok(borrow.remove(0))
            }
        }

        fn read_package_definition(
            &self,
            _pkg_name: &str,
            _merkle: &str,
        ) -> Result<PackageDefinition> {
            let mut borrow = self.package_defs.write().unwrap();
            {
                if borrow.len() == 0 {
                    return Err(anyhow!("No more package_defs left to return. Maybe append more?"));
                }
                Ok(borrow.remove(0))
            }
        }

        fn read_service_package_definition(
            &self,
            _merkle: &str,
        ) -> Result<ServicePackageDefinition> {
            let mut borrow = self.service_package_defs.write().unwrap();
            {
                if borrow.len() == 0 {
                    return Err(anyhow!(
                        "No more service_package_defs left to return. Maybe append more?"
                    ));
                }
                Ok(borrow.remove(0))
            }
        }

        fn read_builtins(&self) -> Result<BuiltinsJson> {
            let mut borrow = self.builtins.write().unwrap();
            {
                if borrow.len() == 0 {
                    return Err(anyhow!("No more builtins left to return. Maybe append more?"));
                }
                Ok(borrow.remove(0))
            }
        }
    }

    fn create_test_sandbox(uses: Vec<String>) -> SandboxDefinition {
        SandboxDefinition {
            dev: None,
            services: Some(uses),
            system: None,
            pkgfs: None,
            features: None,
        }
    }

    fn create_test_cmx_map(
        entries: Vec<(String, SandboxDefinition)>,
    ) -> HashMap<String, CmxDefinition> {
        entries
            .into_iter()
            .map(|entry| (entry.0, CmxDefinition { sandbox: Some(entry.1) }))
            .collect()
    }

    fn create_test_package_with_cms(
        url: String,
        cms: HashMap<String, CmxDefinition>,
    ) -> PackageDefinition {
        PackageDefinition {
            url: url,
            merkle: String::from("0"),
            typ: PackageType::PACKAGE,
            contents: HashMap::new(),
            cms: cms,
        }
    }

    fn create_test_package_with_contents(
        url: String,
        contents: HashMap<String, String>,
    ) -> PackageDefinition {
        PackageDefinition {
            url: url,
            merkle: String::from("0"),
            typ: PackageType::PACKAGE,
            contents: contents,
            cms: HashMap::new(),
        }
    }

    fn create_svc_pkg_def(entries: Vec<(String, String)>) -> ServicePackageDefinition {
        ServicePackageDefinition {
            services: Some(
                entries.into_iter().map(|entry| (entry.0, serde_json::json!(entry.1))).collect(),
            ),
        }
    }

    fn create_svc_pkg_def_with_array(
        entries: Vec<(String, Vec<String>)>,
    ) -> ServicePackageDefinition {
        ServicePackageDefinition {
            services: Some(
                entries.into_iter().map(|entry| (entry.0, serde_json::json!(entry.1))).collect(),
            ),
        }
    }

    fn count_defined_inferred(components: HashMap<String, Component>) -> (usize, usize) {
        let mut defined_count = 0;
        let mut inferred_count = 0;
        for (_, comp) in components {
            if comp.inferred {
                inferred_count += 1;
            } else {
                defined_count += 1;
            }
        }
        (defined_count, inferred_count)
    }

    // =-=-=-=-= merge_services() tests =-=-=-=-= //

    #[test]
    fn test_merge_services_ignores_services_defined_on_non_config_data_package() {
        // Create a single package that is NOT the config data package
        let mock_reader = MockPackageReader::new();
        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };

        let mut contents = HashMap::new();
        contents.insert(String::from("data/sysmgr/foo.config"), String::from("test_merkle"));
        let pkg = create_test_package_with_contents(
            String::from("fuchsia-pkg://fuchsia.com/not-config-data"),
            contents,
        );
        let served = vec![pkg];
        let builtins = HashMap::new();

        let result = collector.merge_services(&served, builtins).unwrap();
        assert_eq!(0, result.len());
    }

    #[test]
    fn test_merge_services_ignores_services_defined_by_non_config_meta_contents() {
        // Create a single package that IS the config data package but
        // does not contain valid data/sysmgr/*.config meta content.
        let mock_reader = MockPackageReader::new();
        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };

        let mut contents = HashMap::new();
        contents.insert(String::from("not/valid/config"), String::from("test_merkle"));
        let pkg = create_test_package_with_contents(String::from(CONFIG_DATA_PKG_URL), contents);
        let served = vec![pkg];
        let builtins = HashMap::new();

        let result = collector.merge_services(&served, builtins).unwrap();
        assert_eq!(0, result.len());
    }

    #[test]
    fn test_merge_services_takes_the_last_defined_duplicate_config_data_services() {
        let mock_reader = MockPackageReader::new();
        // We will need 2 service package definitions that map the same service to different components
        mock_reader.append_service_pkg_def(create_svc_pkg_def(vec![(
            String::from("fuchsia.test.foo.bar"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/served1.cmx"),
        )]));
        mock_reader.append_service_pkg_def(create_svc_pkg_def(vec![(
            String::from("fuchsia.test.foo.bar"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/served2.cmx"),
        )]));

        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };

        let mut contents = HashMap::new();
        contents.insert(String::from("data/sysmgr/foo.config"), String::from("test_merkle"));
        contents.insert(String::from("data/sysmgr/bar.config"), String::from("test_merkle_2"));
        let pkg = create_test_package_with_contents(String::from(CONFIG_DATA_PKG_URL), contents);
        let served = vec![pkg];
        let builtins = HashMap::new();

        let result = collector.merge_services(&served, builtins).unwrap();
        assert_eq!(1, result.len());
        assert!(result.contains_key("fuchsia.test.foo.bar"));
        assert_eq!(
            "fuchsia-pkg://fuchsia.com/foo#meta/served2.cmx",
            result.get("fuchsia.test.foo.bar").unwrap()
        );
    }

    #[test]
    fn test_merge_services_prioritizes_builtins_defined_duplicate_service() {
        let mock_reader = MockPackageReader::new();
        // We will need 1 service package definition
        mock_reader.append_service_pkg_def(create_svc_pkg_def(vec![(
            String::from("fuchsia.test.foo.bar"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/served.cmx"),
        )]));

        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };

        let mut contents = HashMap::new();
        contents.insert(String::from("data/sysmgr/foo.config"), String::from("test_merkle"));
        let pkg = create_test_package_with_contents(String::from(CONFIG_DATA_PKG_URL), contents);
        let served = vec![pkg];

        // We need 1 builtin service package definition that matches the service name
        // created above, but with a different component
        let mut builtins = HashMap::new();
        builtins.insert(
            String::from("fuchsia.test.foo.bar"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/builtin.cmx"),
        );

        let result = collector.merge_services(&served, builtins).unwrap();
        assert_eq!(1, result.len());
        assert!(result.contains_key("fuchsia.test.foo.bar"));
        assert_eq!(
            "fuchsia-pkg://fuchsia.com/foo#meta/builtin.cmx",
            result.get("fuchsia.test.foo.bar").unwrap()
        );
    }

    #[test]
    fn test_merge_services_merges_unique_service_names() {
        let mock_reader = MockPackageReader::new();
        // We will need 2 service package definitions that map different services
        mock_reader.append_service_pkg_def(create_svc_pkg_def(vec![(
            String::from("fuchsia.test.foo.service1"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/served1.cmx"),
        )]));
        mock_reader.append_service_pkg_def(create_svc_pkg_def(vec![(
            String::from("fuchsia.test.foo.service2"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/served2.cmx"),
        )]));

        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };

        let mut contents = HashMap::new();
        contents.insert(String::from("data/sysmgr/service1.config"), String::from("test_merkle"));
        contents.insert(String::from("data/sysmgr/service2.config"), String::from("test_merkle_2"));
        let pkg = create_test_package_with_contents(String::from(CONFIG_DATA_PKG_URL), contents);
        let served = vec![pkg];

        // Create 1 builtin service that is unique to those above
        let mut builtins = HashMap::new();
        builtins.insert(
            String::from("fuchsia.test.foo.service3"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/builtin.cmx"),
        );

        let result = collector.merge_services(&served, builtins).unwrap();
        assert_eq!(3, result.len());
    }

    #[test]
    fn test_merge_services_reads_first_value_when_given_an_array_for_service_url_mapping() {
        let mock_reader = MockPackageReader::new();
        // Create 2 service map definitions that map different services
        mock_reader.append_service_pkg_def(create_svc_pkg_def(vec![(
            String::from("fuchsia.test.foo.service1"),
            String::from("fuchsia-pkg://fuchsia.com/foo#meta/served1.cmx"),
        )]));
        mock_reader.append_service_pkg_def(create_svc_pkg_def_with_array(vec![(
            String::from("fuchsia.test.foo.service2"),
            vec![
                String::from("fuchsia-pkg://fuchsia.com/foo#meta/served2.cmx"),
                String::from("--foo"),
                String::from("--bar"),
            ],
        )]));

        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };

        let mut contents = HashMap::new();
        contents.insert(String::from("data/sysmgr/service1.config"), String::from("test_merkle"));
        contents.insert(String::from("data/sysmgr/service2.config"), String::from("test_merkle_2"));
        let pkg = create_test_package_with_contents(String::from(CONFIG_DATA_PKG_URL), contents);
        let served = vec![pkg];

        let builtins = HashMap::new();

        let result = collector.merge_services(&served, builtins).unwrap();
        assert_eq!(2, result.len());
        assert_eq!(
            "fuchsia-pkg://fuchsia.com/foo#meta/served2.cmx",
            result.get("fuchsia.test.foo.service2").unwrap()
        );
    }

    // =-=-=-=-= build_model() tests =-=-=-=-= //

    #[test]
    fn test_build_model_with_no_services_infers_service() {
        // Create a single test package with a single unknown service dependency
        let sb = create_test_sandbox(vec![String::from("fuchsia.test.foo.bar")]);
        let cms = create_test_cmx_map(vec![(String::from("meta/baz.cmx"), sb)]);
        let pkg = create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/foo"), cms);
        let served = vec![pkg];

        let builtins = Vec::new();
        let services = HashMap::new();

        let (components, packages, manifests, routes, zbi) =
            PackageDataCollector::build_model(served, builtins, services).unwrap();

        assert_eq!(2, components.len());
        assert_eq!(1, manifests.len());
        assert_eq!(1, routes.len());
        assert_eq!(1, packages.len());
        assert_eq!(None, zbi);
        assert_eq!((1, 1), count_defined_inferred(components)); // 1 real, 1 inferred
    }

    #[test]
    fn test_build_model_with_known_services_but_no_matching_component_infers_component() {
        // Create a single test package with a single known service dependency
        let sb = create_test_sandbox(vec![String::from("fuchsia.test.foo.bar")]);
        let cms = create_test_cmx_map(vec![(String::from("meta/baz.cmx"), sb)]);
        let pkg = create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/foo"), cms);
        let served = vec![pkg];

        let builtins = Vec::new();
        // We know about the desired service in the service mapping, but the component doesn't exist
        let mut services = HashMap::new();
        services.insert(
            String::from("fuchsia.test.foo.bar"),
            String::from("fuchsia-pkg://fuchsia.com/aries#meta/taurus.cmx"),
        );

        let (components, packages, manifests, routes, zbi) =
            PackageDataCollector::build_model(served, builtins, services).unwrap();

        assert_eq!(2, components.len());
        assert_eq!(1, manifests.len());
        assert_eq!(1, routes.len());
        assert_eq!(1, packages.len());
        assert_eq!(None, zbi);
        assert_eq!((1, 1), count_defined_inferred(components)); // 1 real, 1 inferred
    }

    #[test]
    fn test_build_model_with_invalid_cmx_creates_empty_graph() {
        // Create a single test package with an invalid cmx path
        let sb = create_test_sandbox(vec![String::from("fuchsia.test.foo.bar")]);
        let sb2 = create_test_sandbox(vec![String::from("fuchsia.test.foo.baz")]);
        let cms = create_test_cmx_map(vec![
            (String::from("foo/bar.cmx"), sb),
            (String::from("meta/baz"), sb2),
        ]);
        let pkg = create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/foo"), cms);
        let served = vec![pkg];

        let builtins = Vec::new();
        let services = HashMap::new();

        let (components, packages, manifests, routes, zbi) =
            PackageDataCollector::build_model(served, builtins, services).unwrap();

        assert_eq!(0, components.len());
        assert_eq!(0, manifests.len());
        assert_eq!(0, routes.len());
        assert_eq!(1, packages.len());
        assert_eq!(None, zbi);
        assert_eq!((0, 0), count_defined_inferred(components)); // 0 real, 0 inferred
    }

    #[test]
    fn test_build_model_with_duplicate_inferred_services_reuses_inferred_service() {
        // Create two test packages that depend on the same inferred service
        let sb = create_test_sandbox(vec![String::from("fuchsia.test.service")]);
        let cms = create_test_cmx_map(vec![(String::from("meta/bar.cmx"), sb)]);
        let pkg = create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/foo"), cms);

        let sb2 = create_test_sandbox(vec![String::from("fuchsia.test.service")]);
        let cms2 = create_test_cmx_map(vec![(String::from("meta/taurus.cmx"), sb2)]);
        let pkg2 =
            create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/aries"), cms2);
        let served = vec![pkg, pkg2];

        let builtins = Vec::new();
        let services = HashMap::new();

        let (components, packages, manifests, routes, zbi) =
            PackageDataCollector::build_model(served, builtins, services).unwrap();

        assert_eq!(3, components.len());
        assert_eq!(2, manifests.len());
        assert_eq!(2, routes.len());
        assert_eq!(2, packages.len());
        assert_eq!(None, zbi);
        assert_eq!((2, 1), count_defined_inferred(components)); // 2 real, 1 inferred
    }

    #[test]
    fn test_build_model_with_known_services_does_not_infer_service() {
        // Create two test packages, one that depends on a service provided by the other
        let sb = create_test_sandbox(vec![String::from("fuchsia.test.taurus")]);
        let cms = create_test_cmx_map(vec![(String::from("meta/bar.cmx"), sb)]);
        let pkg = create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/foo"), cms);

        let sb2 = create_test_sandbox(Vec::new());
        let cms2 = create_test_cmx_map(vec![(String::from("meta/taurus.cmx"), sb2)]);
        let pkg2 =
            create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/aries"), cms2);
        let served = vec![pkg, pkg2];

        let builtins = Vec::new();
        // Map the service the first package requires to the second package
        let mut services = HashMap::new();
        services.insert(
            String::from("fuchsia.test.taurus"),
            String::from("fuchsia-pkg://fuchsia.com/aries#meta/taurus.cmx"),
        );

        let (components, packages, manifests, routes, zbi) =
            PackageDataCollector::build_model(served, builtins, services).unwrap();

        assert_eq!(2, components.len());
        assert_eq!(2, manifests.len());
        assert_eq!(1, routes.len());
        assert_eq!(2, packages.len());
        assert_eq!(None, zbi);
        assert_eq!((2, 0), count_defined_inferred(components)); // 2 real, 0 inferred
    }

    // =-=-=-=-= collect() tests =-=-=-=-= //

    fn create_model() -> (String, Arc<DataModel>) {
        let store_dir = tempdir().unwrap();
        let uri = store_dir.into_path().into_os_string().into_string().unwrap();
        let uri_clone = uri.clone();
        (uri, Arc::new(DataModel::connect(uri_clone).unwrap()))
    }

    #[test]
    fn test_collect_clears_data_model_before_adding_new() {
        let mock_reader = MockPackageReader::new();
        let (_, model) = create_model();
        // Put some "previous" content into the model.
        {
            let mut comps = model.components().write().unwrap();
            comps.push(Component {
                id: 1,
                url: String::from("test.component"),
                version: 0,
                inferred: false,
            });
            comps.push(Component {
                id: 1,
                url: String::from("foo.bar"),
                version: 0,
                inferred: false,
            });

            let mut manis = model.manifests().write().unwrap();
            manis.push(scrutiny::model::model::Manifest {
                component_id: 1,
                manifest: String::from("test.component.manifest"),
                uses: vec![String::from("test.service")],
            });
            manis.push(scrutiny::model::model::Manifest {
                component_id: 2,
                manifest: String::from("foo.bar.manifest"),
                uses: Vec::new(),
            });

            let mut routes = model.routes().write().unwrap();
            routes.push(Route {
                id: 1,
                src_id: 1,
                dst_id: 2,
                service_name: String::from("test.service"),
                protocol_id: 0,
            })
        }

        mock_reader.append_builtin(BuiltinsJson { packages: Vec::new(), services: HashMap::new() });
        let mut targets = HashMap::new();
        targets.insert(
            String::from("123"),
            FarPackageDefinition { custom: Custom { merkle: String::from("123") } },
        );
        mock_reader.append_target(TargetsJson { signed: Signed { targets: targets } });
        let sb = create_test_sandbox(vec![String::from("fuchsia.test.service")]);
        let cms = create_test_cmx_map(vec![(String::from("meta/bar.cmx"), sb)]);
        let pkg = create_test_package_with_cms(String::from("fuchsia-pkg://fuchsia.com/foo"), cms);
        mock_reader.append_pkg_def(pkg);

        let collector = PackageDataCollector { package_reader: Box::new(mock_reader) };
        collector.collect(Arc::clone(&model)).unwrap();

        // Ensure the model reflects only the latest collection.
        let comps = model.components().read().unwrap();
        let manis = model.manifests().read().unwrap();
        let routes = model.routes().read().unwrap();
        // There are 2 components (1 inferred, 1 defined),
        // 1 manifest (for the defined package), and 1 route
        assert_eq!(comps.len(), 2);
        assert_eq!(manis.len(), 1);
        assert_eq!(routes.len(), 1);
    }

    #[test]
    fn test_malformed_zbi() {
        let mut contents = HashMap::new();
        contents.insert(String::from("zbi"), String::from("000"));
        let pkg = create_test_package_with_contents(
            String::from("fuchsia-pkg://fuchsia.com/update"),
            contents,
        );
        let served = vec![pkg];
        let builtins = vec![];
        let services = HashMap::new();

        let (_components, _packages, _manifests, _routes, zbi) =
            PackageDataCollector::build_model(served, builtins, services).unwrap();
        assert_eq!(None, zbi);
    }
}
