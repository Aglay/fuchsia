// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        elf_runner::{ElfRunner, ProcessLauncherConnector},
        framework::RealmCapabilityHost,
        fuchsia_base_pkg_resolver,
        fuchsia_boot_resolver::{self, FuchsiaBootResolver},
        fuchsia_pkg_resolver,
        model::{
            error::ModelError, hub::Hub, testing::breakpoints::BreakpointSystem,
            BuiltinEnvironment, ComponentManagerConfig, HooksRegistration, Model, ModelParams,
            OutgoingBinder, ResolverRegistry,
        },
        process_launcher::ProcessLauncher,
        system_controller::SystemController,
        vmex::VmexService,
        work_scheduler::WorkScheduler,
    },
    failure::{format_err, Error, ResultExt},
    fidl::endpoints::ServiceMarker,
    fidl_fuchsia_sys::{LoaderMarker, LoaderProxy},
    fuchsia_component::client,
    futures::lock::Mutex,
    log::info,
    std::{
        path::PathBuf,
        sync::{Arc, Weak},
    },
};

/// Command line arguments that control component_manager's behavior. Use [Arguments::from_args()]
/// or [Arguments::new()] to create an instance.
// structopt would be nice to use here but the binary size impact from clap - which it depends on -
// is too large.
#[derive(Debug, Default, Clone, Eq, PartialEq)]
pub struct Arguments {
    /// If true, component_manager will serve an instance of fuchsia.process.Launcher and use this
    /// launcher for the built-in ELF component runner. The root component can additionally
    /// use and/or offer this service using '/builtin/fuchsia.process.Launcher' from realm.
    // This argument exists because the built-in process launcher *only* works when
    // component_manager runs under a job that has ZX_POL_NEW_PROCESS set to allow, like the root
    // job. Otherwise, the component_manager process cannot directly create process through
    // zx_process_create. When we run component_manager elsewhere, like in test environments, it
    // has to use the fuchsia.process.Launcher service provided through its namespace instead.
    pub use_builtin_process_launcher: bool,

    /// If true, component_manager will serve an instance of fuchsia.security.resource.Vmex to the
    /// root realm.
    pub use_builtin_vmex: bool,

    /// If true, component manager will be in debug mode. In this mode, the breakpoints FIDL
    /// service will be exposed via the ServiceFs directory (usually the out dir). ComponentManager
    /// will not start until it is resumed by the breakpoints FIDL API.
    ///
    /// This is done so that an external component (say an integration test) can register
    /// breakpoints before ComponentManager has begun.
    pub debug: bool,

    /// URL of the root component to launch.
    pub root_component_url: String,
}

impl Arguments {
    /// Parse `Arguments` from the given String Iterator.
    ///
    /// This parser is relatively simple since component_manager is not a user-facing binary that
    /// requires or would benefit from more flexible UX. Recognized arguments are extracted from
    /// the given Iterator and used to create the returned struct. Unrecognized flags starting with
    /// "--" result in an error being returned. A single non-flag argument is expected for the root
    /// component URL.
    pub fn new<I>(iter: I) -> Result<Self, Error>
    where
        I: IntoIterator<Item = String>,
    {
        let mut iter = iter.into_iter();
        let mut args = Self::default();
        while let Some(arg) = iter.next() {
            if arg == "--use-builtin-process-launcher" {
                args.use_builtin_process_launcher = true;
            } else if arg == "--use-builtin-vmex" {
                args.use_builtin_vmex = true;
            } else if arg == "--debug" {
                args.debug = true;
            } else if arg.starts_with("--") {
                return Err(format_err!("Unrecognized flag: {}", arg));
            } else {
                if !args.root_component_url.is_empty() {
                    return Err(format_err!("Multiple non-flag arguments given"));
                }
                args.root_component_url = arg;
            }
        }

        if args.root_component_url.is_empty() {
            return Err(format_err!("No root component URL found"));
        }
        Ok(args)
    }

    /// Parse `Arguments` from [std::env::args()].
    ///
    /// See [Arguments::new()] for more details.
    pub fn from_args() -> Result<Self, Error> {
        // Ignore first argument with executable name, then delegate to generic iterator impl.
        Self::new(std::env::args().skip(1))
    }

    /// Returns a usage message for the supported arguments.
    pub fn usage() -> String {
        format!(
            "Usage: {} [options] <root-component-url>\n\
             Options:\n\
             --use-builtin-process-launcher   Provide and use a built-in implementation of\n\
             fuchsia.process.Launcher\n\
             --use-builtin-vmex Provide and use a built-in implementation of\n\
             fuchsia.security.resource.Vmex",
            std::env::args().next().unwrap_or("component_manager".to_string())
        )
    }
}

/// Returns a ResolverRegistry configured with the component resolvers available to the current
/// process.
pub fn available_resolvers(
    model_for_resolver: Arc<Mutex<Option<Weak<Model>>>>,
) -> Result<ResolverRegistry, Error> {
    let mut resolver_registry = ResolverRegistry::new();

    // Either the fuchsia-boot or fuchsia-pkg resolver may be unavailable in certain contexts.
    let boot_resolver = FuchsiaBootResolver::new().context("Failed to create boot resolver")?;
    match boot_resolver {
        None => info!("No /boot directory in namespace, fuchsia-boot resolver unavailable"),
        Some(r) => {
            resolver_registry.register(fuchsia_boot_resolver::SCHEME.to_string(), Box::new(r));
        }
    };

    if let Some(loader) = connect_sys_loader()? {
        resolver_registry.register(
            fuchsia_pkg_resolver::SCHEME.to_string(),
            Box::new(fuchsia_pkg_resolver::FuchsiaPkgResolver::new(loader)),
        );
    } else {
        resolver_registry.register(
            fuchsia_base_pkg_resolver::SCHEME.to_string(),
            Box::new(fuchsia_base_pkg_resolver::FuchsiaPkgResolver::new(model_for_resolver)),
        );
    }

    Ok(resolver_registry)
}

/// Checks if the appmgr loader service is available through our namespace and connects to it if
/// so. If not availble, returns Ok(None).
fn connect_sys_loader() -> Result<Option<LoaderProxy>, Error> {
    let service_path = PathBuf::from(format!("/svc/{}", LoaderMarker::NAME));
    if !service_path.exists() {
        return Ok(None);
    }

    let loader = client::connect_to_service::<LoaderMarker>()
        .context("error connecting to system loader")?;
    return Ok(Some(loader));
}

/// Serves services built into component_manager and provides methods for connecting to those
/// services.
pub struct BuiltinRootCapabilities {
    pub work_scheduler: Arc<WorkScheduler>,
    process_launcher: Option<Arc<ProcessLauncher>>,
    vmex_service: Option<Arc<VmexService>>,
    system_controller: Arc<SystemController>,
}

// TODO(fsamuel): Merge BuiltinRootCapabilities and BuiltinEnvironment.
impl BuiltinRootCapabilities {
    /// Creates a new BuiltinRootCapabilities. The available built-in capabilities depends on the
    /// configuration provided in Arguments:
    ///
    /// * If [Arguments::use_builtin_process_launcher] is true, a fuchsia.process.Launcher service
    ///   is available.
    /// * If [Arguments::use_builtin_vmex] is true, a fuchsia.security.resource.Vmex service is
    ///   available.
    pub async fn new(args: &Arguments, model: &Arc<Model>) -> Self {
        let mut process_launcher = None;
        if args.use_builtin_process_launcher {
            process_launcher = Some(Arc::new(ProcessLauncher::new()));
        }
        let mut vmex_service = None;
        if args.use_builtin_vmex {
            vmex_service = Some(Arc::new(VmexService::new()));
        }
        Self {
            work_scheduler: WorkScheduler::new(Arc::downgrade(model) as Weak<dyn OutgoingBinder>)
                .await,
            process_launcher,
            vmex_service,
            system_controller: Arc::new(SystemController::new()),
        }
    }

    pub fn hooks(&self) -> Vec<HooksRegistration> {
        let mut all_hooks: Vec<HooksRegistration> = vec![];
        all_hooks.append(&mut WorkScheduler::hooks(&self.work_scheduler));
        if let Some(process_launcher) = &self.process_launcher {
            all_hooks.append(&mut process_launcher.hooks());
        }
        if let Some(vmex_service) = &self.vmex_service {
            all_hooks.append(&mut vmex_service.hooks());
        }
        all_hooks.append(&mut self.system_controller.hooks());
        all_hooks
    }
}

/// Creates and sets up a model with standard parameters. This is easier than setting up the
/// model manually with `Model::new()`.
pub async fn model_setup(args: &Arguments) -> Result<Arc<Model>, Error> {
    // The model needs the resolver registry to be created, but the resolver registry needs the
    // model to be created so that the fuchsia_pkg resolver can route the pkgfs handle. Thus we
    // create an empty Arc<Mutex<<Option<Model>>>, give it to the resolvers, create the model with
    // the resolvers, and then lock this mutex and set it to a clone of the model we just created.
    let model_for_resolver = Arc::new(Mutex::new(None));

    let launcher_connector = ProcessLauncherConnector::new(&args);
    let runner = ElfRunner::new(launcher_connector);
    let resolver_registry = available_resolvers(model_for_resolver.clone())?;
    let params = ModelParams {
        root_component_url: args.root_component_url.clone(),
        root_resolver_registry: resolver_registry,
        elf_runner: Arc::new(runner),
    };
    let model = Arc::new(Model::new(params));
    let notifier_hooks = {
        let notifier = model.notifier.lock().await;
        let notifier = notifier.as_ref();
        notifier.expect("Notifier must exist. Model is not created!").hooks()
    };
    model.root_realm.hooks.install(notifier_hooks).await;

    *model_for_resolver.lock().await = Some(Arc::downgrade(&model));
    Ok(model)
}

pub async fn builtin_environment_setup(
    args: &Arguments,
    model: &Arc<Model>,
    config: ComponentManagerConfig,
) -> Result<BuiltinEnvironment, ModelError> {
    let builtin_capabilities = Arc::new(BuiltinRootCapabilities::new(args, model).await);
    let realm_capability_host = RealmCapabilityHost::new(model.clone(), config);
    model.root_realm.hooks.install(realm_capability_host.hooks()).await;
    model.root_realm.hooks.install(builtin_capabilities.hooks()).await;
    let hub = Hub::new(args.root_component_url.clone())?;
    let breakpoint_system = {
        if args.debug {
            Some(BreakpointSystem::new())
        } else {
            None
        }
    };
    Ok(BuiltinEnvironment::new(builtin_capabilities, realm_capability_host, hub, breakpoint_system))
}

#[cfg(test)]
mod tests {
    use {super::*, fuchsia_async as fasync};

    #[fasync::run_singlethreaded(test)]
    async fn parse_arguments() -> Result<(), Error> {
        let dummy_url = || "fuchsia-pkg://fuchsia.com/pkg#meta/component.cm".to_string();
        let dummy_url2 = || "fuchsia-pkg://fuchsia.com/pkg#meta/component2.cm".to_string();
        let unknown_flag = || "--unknown".to_string();
        let use_builtin_launcher = || "--use-builtin-process-launcher".to_string();
        let use_builtin_vmex = || "--use-builtin-vmex".to_string();
        let debug = || "--debug".to_string();

        // Zero or multiple positional arguments is an error; must be exactly one URL.
        assert!(Arguments::new(vec![]).is_err());
        assert!(Arguments::new(vec![use_builtin_launcher()]).is_err());
        assert!(Arguments::new(vec![use_builtin_vmex()]).is_err());
        assert!(Arguments::new(vec![dummy_url(), dummy_url2()]).is_err());
        assert!(Arguments::new(vec![dummy_url(), use_builtin_launcher(), dummy_url2()]).is_err());

        // An unknown option is an error.
        assert!(Arguments::new(vec![unknown_flag()]).is_err());
        assert!(Arguments::new(vec![unknown_flag(), dummy_url()]).is_err());
        assert!(Arguments::new(vec![dummy_url(), unknown_flag()]).is_err());

        // Single positional argument with no options is parsed correctly
        assert_eq!(
            Arguments::new(vec![dummy_url()]).expect("Unexpected error with just URL"),
            Arguments { root_component_url: dummy_url(), ..Default::default() }
        );
        assert_eq!(
            Arguments::new(vec![dummy_url2()]).expect("Unexpected error with just URL"),
            Arguments { root_component_url: dummy_url2(), ..Default::default() }
        );

        // Options are parsed correctly and do not depend on order.
        assert_eq!(
            Arguments::new(vec![use_builtin_launcher(), dummy_url()])
                .expect("Unexpected error with option"),
            Arguments {
                use_builtin_process_launcher: true,
                root_component_url: dummy_url(),
                ..Default::default()
            }
        );
        assert_eq!(
            Arguments::new(vec![dummy_url(), use_builtin_launcher()])
                .expect("Unexpected error with option"),
            Arguments {
                use_builtin_process_launcher: true,
                root_component_url: dummy_url(),
                ..Default::default()
            }
        );
        assert_eq!(
            Arguments::new(vec![dummy_url(), use_builtin_launcher(), use_builtin_vmex()])
                .expect("Unexpected error with option"),
            Arguments {
                use_builtin_process_launcher: true,
                use_builtin_vmex: true,
                root_component_url: dummy_url(),
                debug: false,
            }
        );
        assert_eq!(
            Arguments::new(vec![dummy_url(), use_builtin_launcher(), use_builtin_vmex(), debug()])
                .expect("Unexpected error with option"),
            Arguments {
                use_builtin_process_launcher: true,
                use_builtin_vmex: true,
                root_component_url: dummy_url(),
                debug: true,
            }
        );

        Ok(())
    }
}
