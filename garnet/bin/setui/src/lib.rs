// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This declaration is required to support the `select!`.
#![recursion_limit = "256"]

use {
    crate::accessibility::accessibility_controller::AccessibilityController,
    crate::accessibility::types::AccessibilityInfo,
    crate::account::account_controller::AccountController,
    crate::agent::authority_impl::AuthorityImpl,
    crate::agent::base::{Authority, BlueprintHandle as AgentBlueprintHandle, Lifespan},
    crate::audio::audio_controller::AudioController,
    crate::audio::policy::audio_policy_handler::AudioPolicyHandler,
    crate::audio::policy::State,
    crate::audio::types::AudioInfo,
    crate::base::SettingType,
    crate::config::base::{AgentType, ControllerFlag},
    crate::device::device_controller::DeviceController,
    crate::display::display_controller::{DisplayController, ExternalBrightnessControl},
    crate::display::light_sensor_controller::LightSensorController,
    crate::display::types::DisplayInfo,
    crate::do_not_disturb::do_not_disturb_controller::DoNotDisturbController,
    crate::do_not_disturb::types::DoNotDisturbInfo,
    crate::factory_reset::factory_reset_controller::FactoryResetController,
    crate::factory_reset::types::FactoryResetInfo,
    crate::handler::base::GenerateHandler,
    crate::handler::device_storage::DeviceStorageFactory,
    crate::handler::setting_handler::persist::Handler as DataHandler,
    crate::handler::setting_handler::Handler,
    crate::handler::setting_handler_factory_impl::SettingHandlerFactoryImpl,
    crate::handler::setting_proxy::SettingProxy,
    crate::input::input_controller::InputController,
    crate::input::types::InputInfoSources,
    crate::inspect::inspect_broker::InspectBroker,
    crate::inspect::policy_inspect_broker::PolicyInspectBroker,
    crate::internal::core::message as core_message,
    crate::intl::intl_controller::IntlController,
    crate::intl::types::IntlInfo,
    crate::light::light_controller::LightController,
    crate::light::types::LightInfo,
    crate::monitor::base as monitor_base,
    crate::night_mode::night_mode_controller::NightModeController,
    crate::night_mode::types::NightModeInfo,
    crate::policy::base::PolicyType,
    crate::policy::policy_handler,
    crate::policy::policy_handler_factory_impl::PolicyHandlerFactoryImpl,
    crate::policy::policy_proxy::PolicyProxy,
    crate::power::power_controller::PowerController,
    crate::privacy::privacy_controller::PrivacyController,
    crate::privacy::types::PrivacyInfo,
    crate::service_context::GenerateService,
    crate::service_context::ServiceContext,
    crate::service_context::ServiceContextHandle,
    crate::setup::setup_controller::SetupController,
    crate::setup::types::SetupInfo,
    crate::switchboard::switchboard::SwitchboardBuilder,
    anyhow::{format_err, Error},
    fidl_fuchsia_settings::{
        AccessibilityRequestStream, AudioRequestStream, DeviceRequestStream, DisplayRequestStream,
        DoNotDisturbRequestStream, FactoryResetRequestStream, InputRequestStream,
        IntlRequestStream, LightRequestStream, NightModeRequestStream, PrivacyRequestStream,
        SetupRequestStream,
    },
    fidl_fuchsia_settings_policy::VolumePolicyControllerRequestStream,
    fuchsia_async as fasync,
    fuchsia_component::server::{NestedEnvironment, ServiceFs, ServiceFsDir, ServiceObj},
    fuchsia_inspect::component,
    fuchsia_syslog::fx_log_err,
    fuchsia_zircon::DurationNum,
    futures::lock::Mutex,
    futures::StreamExt,
    serde::Deserialize,
    std::collections::{HashMap, HashSet},
    std::sync::atomic::AtomicU64,
    std::sync::Arc,
};

mod accessibility;
mod account;
mod audio;
mod clock;
mod device;
mod display;
mod do_not_disturb;
mod factory_reset;
mod fidl_processor;
mod hanging_get_handler;
mod input;
mod inspect;
mod internal;
mod intl;
mod light;
mod night_mode;
mod policy;
mod power;
mod privacy;
mod service;
mod setup;
pub mod task;

pub use display::LightSensorConfig;
pub use input::input_device_configuration::InputConfiguration;
pub use light::light_hardware_configuration::LightHardwareConfiguration;

pub mod agent;
pub mod base;
pub mod config;
pub mod fidl_common;
pub mod handler;
pub mod message;
pub mod monitor;
pub mod service_context;
pub mod switchboard;

const DEFAULT_SETTING_PROXY_MAX_ATTEMPTS: u64 = 3;
const DEFAULT_SETTING_PROXY_RESPONSE_TIMEOUT_MS: i64 = 10_000;

/// A common trigger for exiting.
pub type ExitSender = futures::channel::mpsc::UnboundedSender<()>;

/// Runtime defines where the environment will exist. Service is meant for
/// production environments and will hydrate components to be discoverable as
/// an environment service. Nested creates a service only usable in the scope
/// of a test.
#[derive(PartialEq)]
enum Runtime {
    Service,
    Nested(&'static str),
}

#[derive(Debug, Default, Clone, Deserialize)]
pub struct AgentConfiguration {
    pub agent_types: HashSet<AgentType>,
}

#[derive(PartialEq, Debug, Clone, Deserialize)]
pub struct EnabledServicesConfiguration {
    pub services: HashSet<SettingType>,
}

impl EnabledServicesConfiguration {
    pub fn with_services(services: HashSet<SettingType>) -> Self {
        Self { services }
    }
}

#[derive(PartialEq, Debug, Clone, Deserialize)]
pub struct EnabledPoliciesConfiguration {
    pub policies: HashSet<PolicyType>,
}

impl EnabledPoliciesConfiguration {
    pub fn with_policies(policies: HashSet<PolicyType>) -> Self {
        Self { policies }
    }
}

#[derive(Default, Debug, Clone, Deserialize)]
pub struct ServiceFlags {
    pub controller_flags: HashSet<ControllerFlag>,
}

#[derive(PartialEq, Debug, Default, Clone)]
pub struct ServiceConfiguration {
    pub agent_types: HashSet<AgentType>,
    pub services: HashSet<SettingType>,
    pub policies: HashSet<PolicyType>,
    pub controller_flags: HashSet<ControllerFlag>,
}

impl ServiceConfiguration {
    pub fn from(
        agent_types: AgentConfiguration,
        services: EnabledServicesConfiguration,
        policies: EnabledPoliciesConfiguration,
        flags: ServiceFlags,
    ) -> Self {
        Self {
            agent_types: agent_types.agent_types,
            services: services.services,
            policies: policies.policies,
            controller_flags: flags.controller_flags,
        }
    }

    fn set_services(&mut self, services: HashSet<SettingType>) {
        self.services = services;
    }

    fn set_policies(&mut self, policies: HashSet<PolicyType>) {
        self.policies = policies;
    }

    fn set_controller_flags(&mut self, controller_flags: HashSet<ControllerFlag>) {
        self.controller_flags = controller_flags;
    }
}

/// Environment is handed back when an environment is spawned from the
/// EnvironmentBuilder. A nested environment (if available) is returned,
/// along with a receiver to be notified when initialization/setup is
/// complete.
pub struct Environment {
    pub nested_environment: Option<NestedEnvironment>,
}

impl Environment {
    pub fn new(nested_environment: Option<NestedEnvironment>) -> Environment {
        Environment { nested_environment }
    }
}

/// The EnvironmentBuilder aggregates the parameters surrounding an environment
/// and ultimately spawns an environment based on them.
pub struct EnvironmentBuilder<T: DeviceStorageFactory + Send + Sync + 'static> {
    configuration: Option<ServiceConfiguration>,
    agent_blueprints: Vec<AgentBlueprintHandle>,
    agent_mapping_func: Option<Box<dyn Fn(AgentType) -> AgentBlueprintHandle>>,
    event_subscriber_blueprints: Vec<internal::event::subscriber::BlueprintHandle>,
    storage_factory: Arc<T>,
    generate_service: Option<GenerateService>,
    handlers: HashMap<SettingType, GenerateHandler<T>>,
    resource_monitors: Vec<monitor_base::monitor::Generate>,
}

macro_rules! register_handler {
    (
        $components:ident,
        $storage_factory:ident,
        $handler_factory:ident,
        $setting_type:expr,
        $controller:ty,
        $spawn_method:expr
    ) => {
        if $components.contains(&$setting_type) {
            $storage_factory
                .initialize::<$controller>()
                .await
                .expect("should be initializing still");
        }
        $handler_factory.register($setting_type, Box::new($spawn_method));
    };
}

/// This macro conditionally adds a FIDL service handler based on the presence
/// of `SettingType`s in the available components. The caller specifies the
/// mod containing a generated fidl_io mod to handle the incoming request
/// streams, the target FIDL interface, and a list of `SettingType`s whose
/// presence will cause this handler to be included.
macro_rules! register_fidl_handler {
    ($components:ident, $service_dir:ident, $service_messenger_factory:ident,
            $interface:ident, $handler_mod:ident$(, $target:ident)+) => {
        if false $(|| $components.contains(&SettingType::$target))+
        {
            let service_messenger_factory = $service_messenger_factory.clone();
            $service_dir.add_fidl_service(
                    move |stream: $interface| {
                        crate::$handler_mod::fidl_io::spawn(service_messenger_factory.clone(),
                        stream);
                    });
        }
    }
}

impl<T: DeviceStorageFactory + Send + Sync + 'static> EnvironmentBuilder<T> {
    pub fn new(storage_factory: Arc<T>) -> EnvironmentBuilder<T> {
        EnvironmentBuilder {
            configuration: None,
            agent_blueprints: vec![],
            agent_mapping_func: None,
            event_subscriber_blueprints: vec![],
            storage_factory,
            generate_service: None,
            handlers: HashMap::new(),
            resource_monitors: vec![],
        }
    }

    pub fn handler(
        mut self,
        setting_type: SettingType,
        generate_handler: GenerateHandler<T>,
    ) -> EnvironmentBuilder<T> {
        self.handlers.insert(setting_type, generate_handler);
        self
    }

    /// A service generator to be used as an overlay on the ServiceContext.
    pub fn service(mut self, generate_service: GenerateService) -> EnvironmentBuilder<T> {
        self.generate_service = Some(generate_service);
        self
    }

    /// A preset configuration to load preset parameters as a base.
    pub fn configuration(mut self, configuration: ServiceConfiguration) -> EnvironmentBuilder<T> {
        self.configuration = Some(configuration);
        self
    }

    /// Setting types to participate.
    pub fn settings(mut self, settings: &[SettingType]) -> EnvironmentBuilder<T> {
        if self.configuration.is_none() {
            self.configuration = Some(ServiceConfiguration::default());
        }

        self.configuration.as_mut().map(|c| c.set_services(settings.iter().copied().collect()));
        self
    }

    /// Sets policies types that are enabled.
    pub fn policies(mut self, policies: &[PolicyType]) -> EnvironmentBuilder<T> {
        if self.configuration.is_none() {
            self.configuration = Some(ServiceConfiguration::default());
        }

        self.configuration.as_mut().map(|c| c.set_policies(policies.iter().copied().collect()));
        self
    }

    /// Setting types to participate with customized controllers.
    pub fn flags(mut self, controller_flags: &[ControllerFlag]) -> EnvironmentBuilder<T> {
        if self.configuration.is_none() {
            self.configuration = Some(ServiceConfiguration::default());
        }

        self.configuration
            .as_mut()
            .map(|c| c.set_controller_flags(controller_flags.iter().map(|f| *f).collect()));
        self
    }

    pub fn agent_mapping<F>(mut self, agent_mapping_func: F) -> EnvironmentBuilder<T>
    where
        F: Fn(AgentType) -> AgentBlueprintHandle + 'static,
    {
        self.agent_mapping_func = Some(Box::new(agent_mapping_func));
        self
    }

    pub fn agents(mut self, blueprints: &[AgentBlueprintHandle]) -> EnvironmentBuilder<T> {
        self.agent_blueprints.append(&mut blueprints.to_vec());
        self
    }

    pub fn resource_monitors(
        mut self,
        monitors: &[monitor_base::monitor::Generate],
    ) -> EnvironmentBuilder<T> {
        self.resource_monitors.append(&mut monitors.to_vec());
        self
    }

    /// Event subscribers to participate
    pub fn event_subscribers(
        mut self,
        subscribers: &[internal::event::subscriber::BlueprintHandle],
    ) -> EnvironmentBuilder<T> {
        self.event_subscriber_blueprints.append(&mut subscribers.to_vec());
        self
    }

    async fn prepare_env(
        self,
        runtime: Runtime,
    ) -> Result<ServiceFs<ServiceObj<'static, ()>>, Error> {
        let mut fs = ServiceFs::new();

        let service_dir;
        if runtime == Runtime::Service {
            // Initialize inspect.
            component::inspector().serve(&mut fs).ok();

            service_dir = fs.dir("svc");
        } else {
            service_dir = fs.root_dir();
        }

        // Define top level MessageHub for service communication.
        let messenger_factory = service::message::create_hub();

        let (agent_types, settings, policies, flags) = match self.configuration {
            Some(configuration) => (
                configuration.agent_types,
                configuration.services,
                configuration.policies,
                configuration.controller_flags,
            ),
            _ => (HashSet::new(), HashSet::new(), HashSet::new(), HashSet::new()),
        };

        let event_messenger_factory = internal::event::message::create_hub();
        let service_context =
            ServiceContext::create(self.generate_service, Some(event_messenger_factory.clone()));

        let context_id_counter = Arc::new(AtomicU64::new(1));

        let mut handler_factory = SettingHandlerFactoryImpl::new(
            settings.clone(),
            service_context.clone(),
            self.storage_factory.clone(),
            context_id_counter.clone(),
        );

        // Create the policy handler factory and register policy handlers.
        let mut policy_handler_factory = PolicyHandlerFactoryImpl::new(
            policies.clone(),
            settings.clone(),
            self.storage_factory.clone(),
            context_id_counter,
        );
        // If policy registration becomes configurable, then this initialization needs to be made
        // configurable with the registration.
        PolicyType::Audio
            .initialize_storage(&self.storage_factory)
            .await
            .expect("was not able to initialize storage for audio policy");
        policy_handler_factory.register(
            PolicyType::Audio,
            Box::new(policy_handler::create_handler::<State, AudioPolicyHandler, _>),
        );

        EnvironmentBuilder::get_configuration_handlers(
            &settings,
            Arc::clone(&self.storage_factory),
            &flags,
            &mut handler_factory,
        )
        .await;

        // Override the configuration handlers with any custom handlers specified
        // in the environment.
        for (setting_type, handler) in self.handlers {
            handler_factory.register(setting_type, handler);
        }

        for agent_type in &agent_types {
            agent_type
                .initialize_storage(&self.storage_factory)
                .await
                .expect("unable to initialize storage for agent");
        }

        let agent_blueprints = self
            .agent_mapping_func
            .map(|agent_mapping_func| {
                agent_types.into_iter().map(|agent_type| (agent_mapping_func)(agent_type)).collect()
            })
            .unwrap_or(self.agent_blueprints);

        create_environment(
            service_dir,
            messenger_factory,
            settings,
            policies,
            agent_blueprints,
            self.resource_monitors,
            self.event_subscriber_blueprints,
            service_context,
            event_messenger_factory,
            Arc::new(Mutex::new(handler_factory)),
            Arc::new(Mutex::new(policy_handler_factory)),
        )
        .await
        .map_err(|err| format_err!("could not create environment: {:?}", err))?;

        Ok(fs)
    }

    pub fn spawn(self, mut executor: fasync::Executor) -> Result<(), Error> {
        match executor.run_singlethreaded(self.prepare_env(Runtime::Service)) {
            Ok(mut fs) => {
                fs.take_and_serve_directory_handle().expect("could not service directory handle");
                let () = executor.run_singlethreaded(fs.collect());

                Ok(())
            }
            Err(error) => Err(error),
        }
    }

    pub async fn spawn_nested(self, env_name: &'static str) -> Result<Environment, Error> {
        match self.prepare_env(Runtime::Nested(env_name)).await {
            Ok(mut fs) => {
                let nested_environment = Some(fs.create_salted_nested_environment(&env_name)?);
                fasync::Task::spawn(fs.collect()).detach();

                Ok(Environment::new(nested_environment))
            }
            Err(error) => Err(error),
        }
    }

    /// Spawns a nested environment and returns the associated
    /// NestedEnvironment. Note that this is a helper function that provides a
    /// shortcut for calling EnvironmentBuilder::name() and
    /// EnvironmentBuilder::spawn().
    pub async fn spawn_and_get_nested_environment(
        self,
        env_name: &'static str,
    ) -> Result<NestedEnvironment, Error> {
        let environment = self.spawn_nested(env_name).await?;

        if let Some(env) = environment.nested_environment {
            return Ok(env);
        }

        return Err(format_err!("nested environment not created"));
    }

    async fn get_configuration_handlers(
        components: &HashSet<SettingType>,
        storage_factory: Arc<T>,
        controller_flags: &HashSet<ControllerFlag>,
        factory_handle: &mut SettingHandlerFactoryImpl<T>,
    ) {
        // Power
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Power,
            PowerController,
            Handler::<PowerController>::spawn
        );
        // Accessibility
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Accessibility,
            AccessibilityController,
            DataHandler::<AccessibilityInfo, AccessibilityController>::spawn
        );
        // Account
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Account,
            AccountController,
            Handler::<AccountController>::spawn
        );
        // Audio
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Audio,
            AudioController,
            DataHandler::<AudioInfo, AudioController>::spawn
        );
        // Device
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Device,
            DeviceController,
            Handler::<DeviceController>::spawn
        );
        // Display
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Display,
            DisplayController,
            if controller_flags.contains(&ControllerFlag::ExternalBrightnessControl) {
                DataHandler::<DisplayInfo, DisplayController<ExternalBrightnessControl>>::spawn
            } else {
                DataHandler::<DisplayInfo, DisplayController>::spawn
            }
        );
        // Light
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Light,
            LightController,
            DataHandler::<LightInfo, LightController>::spawn
        );
        // Light sensor
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::LightSensor,
            LightSensorController,
            Handler::<LightSensorController>::spawn
        );
        // Input
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Input,
            InputController,
            DataHandler::<InputInfoSources, InputController>::spawn
        );
        // Intl
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Intl,
            IntlController,
            DataHandler::<IntlInfo, IntlController>::spawn
        );
        // Do not disturb
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::DoNotDisturb,
            DoNotDisturbController,
            DataHandler::<DoNotDisturbInfo, DoNotDisturbController>::spawn
        );
        // Factory Reset
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::FactoryReset,
            FactoryResetController,
            DataHandler::<FactoryResetInfo, FactoryResetController>::spawn
        );
        // Night mode
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::NightMode,
            NightModeController,
            DataHandler::<NightModeInfo, NightModeController>::spawn
        );
        // Privacy
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Privacy,
            PrivacyController,
            DataHandler::<PrivacyInfo, PrivacyController>::spawn
        );
        // Setup
        register_handler!(
            components,
            storage_factory,
            factory_handle,
            SettingType::Setup,
            SetupController,
            DataHandler::<SetupInfo, SetupController>::spawn
        );
    }
}

/// Brings up the settings service environment.
///
/// This method generates the necessary infrastructure to support the settings
/// service (switchboard, proxies, etc.) and brings up the components necessary
/// to support the components specified in the components HashSet.
async fn create_environment<'a, T: DeviceStorageFactory + Send + Sync + 'static>(
    mut service_dir: ServiceFsDir<'_, ServiceObj<'a, ()>>,
    messenger_factory: service::message::Factory,
    components: HashSet<SettingType>,
    policies: HashSet<PolicyType>,
    agent_blueprints: Vec<AgentBlueprintHandle>,
    resource_monitor_generators: Vec<monitor_base::monitor::Generate>,
    event_subscriber_blueprints: Vec<internal::event::subscriber::BlueprintHandle>,
    service_context_handle: ServiceContextHandle,
    event_messenger_factory: internal::event::message::Factory,
    handler_factory: Arc<Mutex<SettingHandlerFactoryImpl<T>>>,
    policy_handler_factory: Arc<Mutex<PolicyHandlerFactoryImpl<T>>>,
) -> Result<(), Error> {
    let core_messenger_factory = internal::core::message::create_hub();
    let switchboard_messenger_factory = internal::switchboard::message::create_hub();
    let policy_messenger_factory = internal::policy::message::create_hub();

    for blueprint in event_subscriber_blueprints {
        blueprint.create(event_messenger_factory.clone()).await;
    }

    let monitor_actor = if resource_monitor_generators.is_empty() {
        None
    } else {
        Some(monitor::environment::Builder::new().add_monitors(resource_monitor_generators).build())
    };

    // Attach inspect broker, which watches messages between proxies and setting handlers to
    // record settings values to inspect.
    let settings_inspect_node = component::inspector().root().create_child("setting_values");
    InspectBroker::create(messenger_factory.clone(), settings_inspect_node)
        .await
        .expect("could not create inspect");

    let mut proxies = HashMap::new();

    // TODO(fxbug.dev/58893): make max attempts a configurable option.
    // TODO(fxbug.dev/59174): make setting proxy response timeout and retry configurable.
    for setting_type in &components {
        proxies.insert(
            *setting_type,
            SettingProxy::create(
                *setting_type,
                handler_factory.clone(),
                messenger_factory.clone(),
                core_messenger_factory.clone(),
                event_messenger_factory.clone(),
                DEFAULT_SETTING_PROXY_MAX_ATTEMPTS,
                Some(DEFAULT_SETTING_PROXY_RESPONSE_TIMEOUT_MS.millis()),
                true,
            )
            .await?
            .0,
        );
    }

    // Mapping from core message hub signature to setting type.
    let mut policy_core_signatures: HashMap<core_message::Signature, SettingType> = HashMap::new();

    for policy_type in policies {
        let setting_type = policy_type.setting_type();
        if components.contains(&setting_type) {
            let core_signature = PolicyProxy::create(
                policy_type,
                policy_handler_factory.clone(),
                messenger_factory.clone(),
                core_messenger_factory.clone(),
                policy_messenger_factory.clone(),
                proxies
                    .get(&setting_type)
                    .expect(format!("{:?} proxy not found", policy_type).as_str())
                    .clone(),
            )
            .await?;
            policy_core_signatures.insert(core_signature, setting_type);
        }
    }

    // Attach the policy inspect broker, which watches messages to the policy layer and records
    // policy state to inspect.
    let policy_inspect_node = component::inspector().root().create_child("policy_values");
    PolicyInspectBroker::create(policy_messenger_factory.clone(), policy_inspect_node)
        .await
        .expect("could not create inspect");

    // Creates switchboard, handed to interface implementations to send messages
    // to handlers.
    SwitchboardBuilder::create()
        .core_messenger_factory(core_messenger_factory)
        .switchboard_messenger_factory(switchboard_messenger_factory.clone())
        .add_setting_proxies(proxies.clone())
        .add_policy_proxies(policy_core_signatures)
        .build()
        .await
        .expect("could not create switchboard");

    let mut agent_authority = AuthorityImpl::create(
        messenger_factory.clone(),
        internal::agent::message::create_hub(),
        switchboard_messenger_factory.clone(),
        event_messenger_factory.clone(),
        components.clone(),
        monitor_actor,
    )
    .await?;

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        LightRequestStream,
        light,
        Light
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        AccessibilityRequestStream,
        accessibility,
        Accessibility
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        AudioRequestStream,
        audio,
        Audio
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        DeviceRequestStream,
        device,
        Device
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        DisplayRequestStream,
        display,
        Display,
        LightSensor
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        DoNotDisturbRequestStream,
        do_not_disturb,
        DoNotDisturb
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        FactoryResetRequestStream,
        factory_reset,
        FactoryReset
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        IntlRequestStream,
        intl,
        Intl
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        NightModeRequestStream,
        night_mode,
        NightMode
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        PrivacyRequestStream,
        privacy,
        Privacy
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        InputRequestStream,
        input,
        Input
    );

    register_fidl_handler!(
        components,
        service_dir,
        messenger_factory,
        SetupRequestStream,
        setup,
        Setup
    );

    // TODO(fxbug.dev/60925): allow configuration of policy API
    service_dir.add_fidl_service(move |stream: VolumePolicyControllerRequestStream| {
        crate::audio::policy::volume_policy_fidl_handler::fidl_io::spawn(
            messenger_factory.clone(),
            stream,
        );
    });

    for blueprint in agent_blueprints {
        if agent_authority.register(blueprint).await.is_err() {
            fx_log_err!("failed to register agent via blueprint");
        }
    }

    // Execute initialization agents sequentially
    if agent_authority
        .execute_lifespan(Lifespan::Initialization, service_context_handle.clone(), true)
        .await
        .is_err()
    {
        return Err(format_err!("Agent initialization failed"));
    }

    // Execute service agents concurrently
    agent_authority
        .execute_lifespan(Lifespan::Service, service_context_handle.clone(), false)
        .await
        .ok();

    return Ok(());
}

#[cfg(test)]
mod tests;
