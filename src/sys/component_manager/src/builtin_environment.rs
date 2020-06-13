// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        builtin::{
            arguments::Arguments as BootArguments,
            capability::BuiltinCapability,
            kernel_stats::KernelStats,
            log::{ReadOnlyLog, WriteOnlyLog},
            process_launcher::ProcessLauncher,
            root_job::{RootJob, ROOT_JOB_CAPABILITY_PATH, ROOT_JOB_FOR_INSPECT_CAPABILITY_PATH},
            root_resource::RootResource,
            runner::{BuiltinRunner, BuiltinRunnerFactory},
            system_controller::SystemController,
            vmex::VmexService,
        },
        capability_ready_notifier::CapabilityReadyNotifier,
        config::RuntimeConfig,
        framework::RealmCapabilityHost,
        fuchsia_base_pkg_resolver, fuchsia_boot_resolver, fuchsia_pkg_resolver,
        model::{
            binding::Binder,
            environment::{Environment, RunnerRegistration, RunnerRegistry},
            error::ModelError,
            event_logger::EventLogger,
            events::{
                event::SyncMode, registry::EventRegistry, running_provider::RunningProvider,
                source_factory::EventSourceFactory, stream_provider::EventStreamProvider,
            },
            hooks::EventType,
            hub::Hub,
            model::{Model, ModelParams},
            resolver::{Resolver, ResolverRegistry},
        },
        root_realm_stop_notifier::RootRealmStopNotifier,
        startup::Arguments,
        work_scheduler::WorkScheduler,
    },
    anyhow::{format_err, Context as _, Error},
    cm_rust::CapabilityName,
    fidl::endpoints::{create_endpoints, create_proxy, ServerEnd, ServiceMarker},
    fidl_fuchsia_io::{
        DirectoryMarker, DirectoryProxy, MODE_TYPE_DIRECTORY, OPEN_RIGHT_READABLE,
        OPEN_RIGHT_WRITABLE,
    },
    fidl_fuchsia_sys::{LoaderMarker, LoaderProxy},
    fuchsia_async as fasync,
    fuchsia_component::{client, server::*},
    fuchsia_runtime::{take_startup_handle, HandleType},
    fuchsia_zircon::{self as zx, HandleBased},
    futures::{channel::oneshot, prelude::*},
    log::info,
    std::{
        path::PathBuf,
        sync::{Arc, Weak},
    },
};

// TODO(viktard): Merge Arguments, RuntimeConfig and root_component_url from ModelParams
#[derive(Default)]
pub struct BuiltinEnvironmentBuilder {
    args: Option<Arguments>,
    config: Option<RuntimeConfig>,
    runners: Vec<(CapabilityName, Arc<dyn BuiltinRunnerFactory>)>,
    resolvers: ResolverRegistry,
    // This is used to initialize fuchsia_base_pkg_resolver's Model reference. Resolvers must
    // be created to construct the Model.
    model_for_resolver: Option<oneshot::Sender<Weak<Model>>>,
}

impl BuiltinEnvironmentBuilder {
    pub fn new() -> Self {
        BuiltinEnvironmentBuilder::default()
    }

    pub fn set_args(mut self, args: Arguments) -> Self {
        self.args = Some(args);
        self
    }

    pub fn set_config(mut self, config: RuntimeConfig) -> Self {
        self.config = Some(config);
        self
    }

    pub fn add_runner(
        mut self,
        name: CapabilityName,
        runner: Arc<dyn BuiltinRunnerFactory>,
    ) -> Self {
        // We don't wrap these in a BuiltinRunner immediately because that requires the
        // RuntimeConfig, which may be provided after this or may fall back to the default.
        self.runners.push((name, runner));
        self
    }

    pub fn add_resolver(
        mut self,
        scheme: String,
        resolver: Box<dyn Resolver + Send + Sync + 'static>,
    ) -> Self {
        self.resolvers.register(scheme, resolver);
        self
    }

    /// Adds standard resolvers whose dependencies are available in the process's namespace. This
    /// includes:
    ///   - A fuchsia-boot resolver if /boot is available.
    ///   - One of two different fuchsia-pkg resolver implementations, either:
    ///       - If /svc/fuchsia.sys.Loader is present, then an implementation that proxies to that
    ///         protocol (which is the v1 resolver equivalent). This is used for tests or other
    ///         scenarios where component_manager runs as a v1 component.
    ///       - Otherwise, an implementation that resolves packages from a /pkgfs directory
    ///         capability if one is exposed from the root component. (See fuchsia_base_pkg_resolver
    ///         for more details.)
    ///
    /// TODO(fxb/46491): fuchsia_base_pkg_resolver should be replaced with a resolver provided by
    /// the topology.
    pub fn add_available_resolvers_from_namespace(mut self) -> Result<Self, Error> {
        // Either the fuchsia-boot or fuchsia-pkg resolver may be unavailable in certain contexts.
        let boot_resolver = fuchsia_boot_resolver::FuchsiaBootResolver::new()
            .context("Failed to create boot resolver")?;
        match boot_resolver {
            None => info!("No /boot directory in namespace, fuchsia-boot resolver unavailable"),
            Some(r) => {
                self.resolvers.register(fuchsia_boot_resolver::SCHEME.to_string(), Box::new(r));
            }
        };

        if let Some(loader) = Self::connect_sys_loader()? {
            self.resolvers.register(
                fuchsia_pkg_resolver::SCHEME.to_string(),
                Box::new(fuchsia_pkg_resolver::FuchsiaPkgResolver::new(loader)),
            );
        } else {
            // There's a circular dependency here. The model needs the resolver register to be
            // created, but fuchsia_base_pkg_resolver needs a reference to model so it can bind to
            // the pkgfs directory. We use a futures::oneshot::channel to send the Model to the
            // resolver once it has been created.
            let (sender, receiver) = oneshot::channel();
            self.resolvers.register(
                fuchsia_base_pkg_resolver::SCHEME.to_string(),
                Box::new(fuchsia_base_pkg_resolver::FuchsiaPkgResolver::new(receiver)),
            );
            self.model_for_resolver = Some(sender);
        }
        Ok(self)
    }

    pub async fn build(self) -> Result<BuiltinEnvironment, Error> {
        let args = self.args.unwrap_or_default();
        let runner_map = self
            .runners
            .iter()
            .map(|(name, _)| {
                (
                    name.clone(),
                    RunnerRegistration {
                        source_name: name.clone(),
                        source: cm_rust::RegistrationSource::Self_,
                    },
                )
            })
            .collect();
        let params = ModelParams {
            root_component_url: args.root_component_url.clone(),
            root_environment: Environment::new_root(
                RunnerRegistry::new(runner_map),
                self.resolvers,
            ),
        };
        let model = Arc::new(Model::new(params));

        // If we previously created a resolver that requires the Model (in
        // add_available_resolvers_from_namespace), send the just-created model to it.
        if let Some(sender) = self.model_for_resolver {
            // This only fails if the receiver has been dropped already which shouldn't happen.
            sender
                .send(Arc::downgrade(&model))
                .map_err(|_| format_err!("sending model to resolver failed"))?;
        }

        // Wrap BuiltinRunnerFactory in BuiltinRunner now that we have the definite RuntimeConfig.
        let config = Arc::new(self.config.unwrap_or_default());
        let builtin_runners = self
            .runners
            .into_iter()
            .map(|(name, runner)| {
                Arc::new(BuiltinRunner::new(name, runner, Arc::downgrade(&config)))
            })
            .collect();

        Ok(BuiltinEnvironment::new(model, args, config, builtin_runners).await?)
    }

    /// Checks if the appmgr loader service is available through our namespace and connects to it if
    /// so. If not available, returns Ok(None).
    fn connect_sys_loader() -> Result<Option<LoaderProxy>, Error> {
        let service_path = PathBuf::from(format!("/svc/{}", LoaderMarker::NAME));
        if !service_path.exists() {
            return Ok(None);
        }

        let loader = client::connect_to_service::<LoaderMarker>()
            .context("error connecting to system loader")?;
        return Ok(Some(loader));
    }
}

/// The built-in environment consists of the set of the root services and framework services. Use
/// BuiltinEnvironmentBuilder to construct one.
///
/// The available built-in capabilities depends on the configuration provided in Arguments:
/// * If [Arguments::use_builtin_process_launcher] is true, a fuchsia.process.Launcher service
///   is available.
pub struct BuiltinEnvironment {
    pub model: Arc<Model>,

    // Framework capabilities.
    pub boot_args: Arc<BootArguments>,
    pub kernel_stats: Option<Arc<KernelStats>>,
    pub process_launcher: Option<Arc<ProcessLauncher>>,
    pub root_job: Arc<RootJob>,
    pub root_job_for_inspect: Arc<RootJob>,
    pub read_only_log: Option<Arc<ReadOnlyLog>>,
    pub write_only_log: Option<Arc<WriteOnlyLog>>,
    pub root_resource: Option<Arc<RootResource>>,
    pub system_controller: Arc<SystemController>,
    pub vmex_service: Option<Arc<VmexService>>,

    pub work_scheduler: Arc<WorkScheduler>,
    pub realm_capability_host: Arc<RealmCapabilityHost>,
    pub hub: Arc<Hub>,
    pub builtin_runners: Vec<Arc<BuiltinRunner>>,
    pub event_registry: Arc<EventRegistry>,
    pub event_source_factory: Arc<EventSourceFactory>,
    pub stop_notifier: Arc<RootRealmStopNotifier>,
    pub capability_ready_notifier: Arc<CapabilityReadyNotifier>,
    pub event_stream_provider: Arc<EventStreamProvider>,
    pub event_logger: Option<Arc<EventLogger>>,
    is_debug: bool,
}

impl BuiltinEnvironment {
    async fn new(
        model: Arc<Model>,
        args: Arguments,
        config: Arc<RuntimeConfig>,
        builtin_runners: Vec<Arc<BuiltinRunner>>,
    ) -> Result<BuiltinEnvironment, ModelError> {
        // Set up ProcessLauncher if available.
        let process_launcher = if args.use_builtin_process_launcher {
            let process_launcher = Arc::new(ProcessLauncher::new());
            model.root_realm.hooks.install(process_launcher.hooks()).await;
            Some(process_launcher)
        } else {
            None
        };

        // Set up RootJob service.
        let root_job = RootJob::new(&ROOT_JOB_CAPABILITY_PATH, zx::Rights::SAME_RIGHTS);
        model.root_realm.hooks.install(root_job.hooks()).await;

        // Set up RootJobForInspect service.
        let root_job_for_inspect = RootJob::new(
            &ROOT_JOB_FOR_INSPECT_CAPABILITY_PATH,
            zx::Rights::INSPECT
                | zx::Rights::ENUMERATE
                | zx::Rights::DUPLICATE
                | zx::Rights::TRANSFER
                | zx::Rights::GET_PROPERTY,
        );
        model.root_realm.hooks.install(root_job_for_inspect.hooks()).await;

        let root_resource_handle =
            take_startup_handle(HandleType::Resource.into()).map(zx::Resource::from);

        // Set up BootArguments service.
        let boot_args = BootArguments::new();
        model.root_realm.hooks.install(boot_args.hooks()).await;

        // Set up KernelStats service.
        let kernel_stats = root_resource_handle.as_ref().map(|handle| {
            KernelStats::new(
                handle
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .expect("Failed to duplicate root resource handle"),
            )
        });
        if let Some(kernel_stats) = kernel_stats.as_ref() {
            model.root_realm.hooks.install(kernel_stats.hooks()).await;
        }

        // Set up ReadOnlyLog service.
        let read_only_log = root_resource_handle.as_ref().map(|handle| {
            ReadOnlyLog::new(
                handle
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .expect("Failed to duplicate root resource handle"),
            )
        });
        if let Some(read_only_log) = read_only_log.as_ref() {
            model.root_realm.hooks.install(read_only_log.hooks()).await;
        }

        // Set up WriteOnlyLog service.
        let write_only_log = root_resource_handle.as_ref().map(|handle| {
            WriteOnlyLog::new(zx::DebugLog::create(handle, zx::DebugLogOpts::empty()).unwrap())
        });
        if let Some(write_only_log) = write_only_log.as_ref() {
            model.root_realm.hooks.install(write_only_log.hooks()).await;
        }

        // Set up the Vmex service.
        let vmex_service = root_resource_handle.as_ref().map(|handle| {
            VmexService::new(
                handle
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .expect("Failed to duplicate root resource handle"),
            )
        });
        if let Some(vmex_service) = vmex_service.as_ref() {
            model.root_realm.hooks.install(vmex_service.hooks()).await;
        }

        // Set up RootResource service.
        let root_resource = root_resource_handle.map(RootResource::new);
        if let Some(root_resource) = root_resource.as_ref() {
            model.root_realm.hooks.install(root_resource.hooks()).await;
        }

        // Set up System Controller service.
        let system_controller = Arc::new(SystemController::new(model.clone()));
        model.root_realm.hooks.install(system_controller.hooks()).await;

        // Set up work scheduler.
        let work_scheduler =
            WorkScheduler::new(Arc::new(Arc::downgrade(&model)) as Arc<dyn Binder>).await;
        model.root_realm.hooks.install(work_scheduler.hooks()).await;

        // Set up the realm service.
        let realm_capability_host = Arc::new(RealmCapabilityHost::new(model.clone(), config));
        model.root_realm.hooks.install(realm_capability_host.hooks()).await;

        // Set up the builtin runners.
        for runner in &builtin_runners {
            model.root_realm.hooks.install(runner.hooks()).await;
        }

        // Set up the root realm stop notifier.
        let stop_notifier = Arc::new(RootRealmStopNotifier::new());
        model.root_realm.hooks.install(stop_notifier.hooks()).await;

        let hub = Arc::new(Hub::new(&model, args.root_component_url.clone())?);
        model.root_realm.hooks.install(hub.hooks()).await;

        // Set up the capability ready notifier.
        let capability_ready_notifier =
            Arc::new(CapabilityReadyNotifier::new(Arc::downgrade(&model)));
        model.root_realm.hooks.install(capability_ready_notifier.hooks()).await;

        // Set up the event registry.
        let event_registry = {
            let mut event_registry = EventRegistry::new(Arc::downgrade(&model));
            event_registry.register_synthesis_provider(
                EventType::CapabilityReady,
                capability_ready_notifier.clone(),
            );
            event_registry
                .register_synthesis_provider(EventType::Running, Arc::new(RunningProvider::new()));
            Arc::new(event_registry)
        };
        model.root_realm.hooks.install(event_registry.hooks()).await;

        // Set up the event source factory.
        let event_source_factory = Arc::new(EventSourceFactory::new(
            Arc::downgrade(&model),
            Arc::downgrade(&event_registry),
        ));
        model.root_realm.hooks.install(event_source_factory.hooks()).await;

        let event_stream_provider =
            Arc::new(EventStreamProvider::new(Arc::downgrade(&event_registry)));
        model.root_realm.hooks.install(event_stream_provider.hooks()).await;

        let event_logger = if args.debug {
            let event_logger = Arc::new(EventLogger::new());
            model.root_realm.hooks.install(event_logger.hooks()).await;
            Some(event_logger)
        } else {
            None
        };

        Ok(BuiltinEnvironment {
            model,
            boot_args,
            process_launcher,
            root_job,
            root_job_for_inspect,
            kernel_stats,
            read_only_log,
            write_only_log,
            root_resource,
            system_controller,
            vmex_service,

            work_scheduler,
            realm_capability_host,
            hub,
            builtin_runners,
            event_registry,
            event_source_factory,
            stop_notifier,
            capability_ready_notifier,
            event_stream_provider,
            event_logger,
            is_debug: args.debug,
        })
    }

    /// Setup a ServiceFs that contains the Hub and (optionally) the `BlockingEventSource` service.
    async fn create_service_fs<'a>(&self) -> Result<ServiceFs<ServiceObj<'a, ()>>, ModelError> {
        // Create the ServiceFs
        let mut service_fs = ServiceFs::new();

        // Setup the hub
        let (hub_proxy, hub_server_end) = create_proxy::<DirectoryMarker>().unwrap();
        self.hub
            .open_root(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE, hub_server_end.into_channel())
            .await?;
        service_fs.add_remote("hub", hub_proxy);

        // If component manager is in debug mode, create an event source scoped at the
        // root and offer it via ServiceFs to the outside world.
        if self.is_debug {
            let event_source = self.event_source_factory.create_for_debug(SyncMode::Sync).await?;
            service_fs.dir("svc").add_fidl_service(move |stream| {
                let event_source = event_source.clone();
                event_source.serve(stream);
            });
        }

        Ok(service_fs)
    }

    /// Bind ServiceFs to a provided channel
    async fn bind_service_fs(&self, channel: zx::Channel) -> Result<(), ModelError> {
        let mut service_fs = self.create_service_fs().await?;

        // Bind to the channel
        service_fs
            .serve_connection(channel)
            .map_err(|err| ModelError::namespace_creation_failed(err))?;

        // Start up ServiceFs
        fasync::spawn(async move {
            service_fs.collect::<()>().await;
        });
        Ok(())
    }

    /// Bind ServiceFs to the outgoing directory of this component, if it exists.
    pub async fn bind_service_fs_to_out(&self) -> Result<(), ModelError> {
        if let Some(handle) = fuchsia_runtime::take_startup_handle(
            fuchsia_runtime::HandleType::DirectoryRequest.into(),
        ) {
            self.bind_service_fs(zx::Channel::from(handle)).await?;
        }
        Ok(())
    }

    /// Bind ServiceFs to a new channel and return the Hub directory.
    /// Used mainly by integration tests.
    pub async fn bind_service_fs_for_hub(&self) -> Result<DirectoryProxy, ModelError> {
        // Create a channel that ServiceFs will operate on
        let (service_fs_proxy, service_fs_server_end) = create_proxy::<DirectoryMarker>().unwrap();

        self.bind_service_fs(service_fs_server_end.into_channel()).await?;

        // Open the Hub from within ServiceFs
        let (hub_client_end, hub_server_end) = create_endpoints::<DirectoryMarker>().unwrap();
        service_fs_proxy
            .open(
                OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
                MODE_TYPE_DIRECTORY,
                "hub",
                ServerEnd::new(hub_server_end.into_channel()),
            )
            .map_err(|err| ModelError::namespace_creation_failed(err))?;
        let hub_proxy = hub_client_end.into_proxy().unwrap();

        Ok(hub_proxy)
    }

    pub async fn wait_for_root_realm_stop(&self) {
        self.stop_notifier.wait_for_root_realm_stop().await;
    }
}
