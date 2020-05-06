// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::Result;
use anyhow::Context as _;
use fidl_fuchsia_net_stack as net_stack;
use fidl_fuchsia_net_stack_ext::FidlReturn;
use fidl_fuchsia_netemul_environment as netemul_environment;
use fidl_fuchsia_netemul_network as netemul_network;
use fidl_fuchsia_netemul_sandbox as netemul_sandbox;
use fuchsia_zircon as zx;
use std::convert::TryInto;

/// Helper definition to help type system identify `None` as `IntoIterator`
/// where `Item: Into<netemul_environment::LaunchService`.
const NO_SERVICES: Option<netemul_environment::LaunchService> = None;

/// The Netstack version. Used to specify which Netstack version to use in a
/// Netstack-served [`KnownServices`].
#[derive(Copy, Clone, Eq, PartialEq, Debug)]
pub enum NetstackVersion {
    Netstack2,
    Netstack3,
}

impl NetstackVersion {
    /// Gets the Fuchsia URL for this Netstack component.
    pub fn get_url(&self) -> &'static str {
        match self {
            NetstackVersion::Netstack2 => {
                "fuchsia-pkg://fuchsia.com/netstack#meta/netstack_debug.cmx"
            }
            NetstackVersion::Netstack3 => {
                fuchsia_component::fuchsia_single_component_package_url!("netstack3")
            }
        }
    }
}

/// Known services used in tests.
#[derive(Copy, Clone, Eq, PartialEq, Debug)]
pub enum KnownServices {
    Stack(NetstackVersion),
    Netstack(NetstackVersion),
    SocketProvider(NetstackVersion),
    Stash,
    MockCobalt,
    SecureStash,
    DhcpServer,
    LoopkupAdmin,
}

impl KnownServices {
    /// Gets the service name and its Fuchsia package URL.
    pub fn get_name_url(&self) -> (&'static str, &'static str) {
        match self {
            KnownServices::Stack(v) => (<net_stack::StackMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                        v.get_url()),
            KnownServices::MockCobalt => (<fidl_fuchsia_cobalt::LoggerFactoryMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                          fuchsia_component::fuchsia_single_component_package_url!("mock_cobalt")),
            KnownServices::Netstack(v) => (<fidl_fuchsia_netstack::NetstackMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                           v.get_url()),
            KnownServices::Stash => (
                <fidl_fuchsia_stash::StoreMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                fuchsia_component::fuchsia_single_component_package_url!("stash")),
            KnownServices::SocketProvider(v) => (<fidl_fuchsia_posix_socket::ProviderMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                              v.get_url()),
            KnownServices::SecureStash => (<fidl_fuchsia_stash::SecureStoreMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                           "fuchsia-pkg://fuchsia.com/stash#meta/stash_secure.cmx"),
            KnownServices::DhcpServer => (<fidl_fuchsia_net_dhcp::Server_Marker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                          fuchsia_component::fuchsia_single_component_package_url!("dhcpd-testing")),
            KnownServices::LoopkupAdmin => (<fidl_fuchsia_net_name::LookupAdminMarker as fidl::endpoints::DiscoverableService>::SERVICE_NAME,
                                            fuchsia_component::fuchsia_single_component_package_url!("dns-resolver"))
        }
    }

    /// Gets the service name.
    pub fn get_name(&self) -> &'static str {
        self.get_name_url().0
    }

    /// Gets the service URL.
    pub fn get_url(&self) -> &'static str {
        self.get_name_url().1
    }

    /// Transforms into a [`netemul_environment::LaunchService`] with no
    /// arguments.
    pub fn into_launch_service(self) -> netemul_environment::LaunchService {
        self.into_launch_service_with_arguments::<Option<String>>(None)
    }

    /// Transforms into a [`netemul_environment::LaunchService`] with no
    /// arguments with `args`.
    pub fn into_launch_service_with_arguments<I>(
        self,
        args: I,
    ) -> netemul_environment::LaunchService
    where
        I: IntoIterator,
        I::Item: Into<String>,
    {
        let (name, url) = self.get_name_url();
        netemul_environment::LaunchService {
            name: name.to_string(),
            url: url.to_string(),
            arguments: Some(args.into_iter().map(Into::into).collect()),
        }
    }
}

impl<'a> From<&'a KnownServices> for netemul_environment::LaunchService {
    fn from(s: &'a KnownServices) -> Self {
        s.into_launch_service()
    }
}

// TODO(gongt) Use an attribute macro to reduce the boilerplate of running the
// same test for both N2 and N3.
/// Abstraction for a Fuchsia component which offers network stack services.
pub trait Netstack: Copy + Clone {
    /// The Netstack version.
    const VERSION: NetstackVersion;
}

/// Uninstantiable type that represents Netstack2's implementation of a
/// network stack.
#[derive(Copy, Clone)]
pub enum Netstack2 {}

impl Netstack for Netstack2 {
    const VERSION: NetstackVersion = NetstackVersion::Netstack2;
}

/// Uninstantiable type that represents Netstack3's implementation of a
/// network stack.
#[derive(Copy, Clone)]
pub enum Netstack3 {}

impl Netstack for Netstack3 {
    const VERSION: NetstackVersion = NetstackVersion::Netstack3;
}

/// A test sandbox backed by a [`netemul_sandbox::SandboxProxy`].
///
/// `TestSandbox` provides various utility methods to set up network
/// environments for use in testing. The lifetime of the `TestSandbox` is tied
/// to the netemul sandbox itself, dropping it will cause all the created
/// environments, networks, and endpoints to be destroyed.
#[must_use]
pub struct TestSandbox {
    sandbox: netemul_sandbox::SandboxProxy,
}

impl TestSandbox {
    /// Creates a new empty sandbox.
    pub fn new() -> Result<TestSandbox> {
        let sandbox = fuchsia_component::client::connect_to_service::<
            fidl_fuchsia_netemul_sandbox::SandboxMarker,
        >()
        .context("failed to connect to sandbox service")?;
        Ok(TestSandbox { sandbox })
    }

    /// Creates an environment with `name` and `services`.
    ///
    /// To create an environment with Netstack services see
    /// [`TestSandbox::create_netstack_environment`].
    pub fn create_environment<I>(
        &self,
        name: impl Into<String>,
        services: I,
    ) -> Result<TestEnvironment<'_>>
    where
        I: IntoIterator,
        I::Item: Into<netemul_environment::LaunchService>,
    {
        let (environment, server) =
            fidl::endpoints::create_proxy::<netemul_environment::ManagedEnvironmentMarker>()?;
        let name = name.into();
        let () = self.sandbox.create_environment(
            server,
            netemul_environment::EnvironmentOptions {
                name: Some(name.clone()),
                services: Some(services.into_iter().map(Into::into).collect()),
                devices: None,
                inherit_parent_launch_services: None,
                logger_options: Some(netemul_environment::LoggerOptions {
                    enabled: Some(true),
                    klogs_enabled: None,
                    filter_options: None,
                    syslog_output: Some(true),
                }),
            },
        )?;
        Ok(TestEnvironment { environment, name, _sandbox: self })
    }

    /// Creates an environment with no services.
    pub fn create_empty_environment(&self, name: impl Into<String>) -> Result<TestEnvironment<'_>> {
        self.create_environment(name, NO_SERVICES)
    }

    /// Creates an environment with Netstack services.
    pub fn create_netstack_environment<N, S>(&self, name: S) -> Result<TestEnvironment<'_>>
    where
        N: Netstack,
        S: Into<String>,
    {
        self.create_netstack_environment_with::<N, _, _>(name, NO_SERVICES)
    }

    /// Creates an environment with the base Netstack services plus additional
    /// ones in `services`.
    pub fn create_netstack_environment_with<N, S, I>(
        &self,
        name: S,
        services: I,
    ) -> Result<TestEnvironment<'_>>
    where
        S: Into<String>,
        N: Netstack,
        I: IntoIterator,
        I::Item: Into<netemul_environment::LaunchService>,
    {
        self.create_environment(
            name,
            [
                KnownServices::Stack(N::VERSION),
                KnownServices::Netstack(N::VERSION),
                KnownServices::SocketProvider(N::VERSION),
                KnownServices::MockCobalt,
                KnownServices::Stash,
            ]
            .iter()
            .map(netemul_environment::LaunchService::from)
            .chain(services.into_iter().map(Into::into)),
        )
    }

    /// Connects to the sandbox's NetworkContext.
    fn get_network_context(&self) -> Result<netemul_network::NetworkContextProxy> {
        let (ctx, server) =
            fidl::endpoints::create_proxy::<netemul_network::NetworkContextMarker>()?;
        let () = self.sandbox.get_network_context(server)?;
        Ok(ctx)
    }

    /// Connects to the sandbox's NetworkManager.
    fn get_network_manager(&self) -> Result<netemul_network::NetworkManagerProxy> {
        let ctx = self.get_network_context()?;
        let (network_manager, server) =
            fidl::endpoints::create_proxy::<netemul_network::NetworkManagerMarker>()?;
        let () = ctx.get_network_manager(server)?;
        Ok(network_manager)
    }

    /// Connects to the sandbox's EndpointManager.
    fn get_endpoint_manager(&self) -> Result<netemul_network::EndpointManagerProxy> {
        let ctx = self.get_network_context()?;
        let (ep_manager, server) =
            fidl::endpoints::create_proxy::<netemul_network::EndpointManagerMarker>()?;
        let () = ctx.get_endpoint_manager(server)?;
        Ok(ep_manager)
    }

    /// Creates a new empty network with default configurations and `name`.
    pub async fn create_network(&self, name: impl Into<String>) -> Result<TestNetwork<'_>> {
        let name = name.into();
        let netm = self.get_network_manager()?;
        let (status, network) = netm
            .create_network(
                &name,
                netemul_network::NetworkConfig { latency: None, packet_loss: None, reorder: None },
            )
            .await
            .context("create_network FIDL error")?;
        let () = zx::Status::ok(status).context("create_network failed")?;
        let network = network
            .ok_or_else(|| anyhow::anyhow!("create_network didn't return a valid network"))?
            .into_proxy()?;
        Ok(TestNetwork { network, name, sandbox: self })
    }

    /// Creates a new unattached endpoint with default configurations and `name`.
    pub async fn create_endpoint(&self, name: impl Into<String>) -> Result<TestEndpoint<'_>> {
        let name = name.into();
        let epm = self.get_endpoint_manager()?;
        let (status, endpoint) = epm
            .create_endpoint(
                &name,
                &mut fidl_fuchsia_netemul_network::EndpointConfig {
                    mtu: 1500,
                    mac: None,
                    backing: fidl_fuchsia_netemul_network::EndpointBacking::Ethertap,
                },
            )
            .await
            .context("create_endpoint FIDL error")?;
        let () = zx::Status::ok(status).context("create_endpoint failed")?;
        let endpoint = endpoint
            .ok_or_else(|| anyhow::anyhow!("create_endpoint didn't return a valid endpoint"))?
            .into_proxy()?;
        Ok(TestEndpoint { endpoint, name, _sandbox: self })
    }

    /// Helper function to create a new Netstack environment and connect to a
    /// netstack service on it.
    pub fn new_netstack<N, S>(&self, name: &'static str) -> Result<(TestEnvironment<'_>, S::Proxy)>
    where
        N: Netstack,
        S: fidl::endpoints::ServiceMarker + fidl::endpoints::DiscoverableService,
    {
        let env = self
            .create_netstack_environment::<N, _>(name)
            .context("failed to create test environment")?;
        let netstack_proxy =
            env.connect_to_service::<S>().context("failed to connect to netstack")?;
        Ok((env, netstack_proxy))
    }

    /// Helper function to create a new Netstack environment and a new
    /// unattached endpoint.
    pub async fn new_netstack_and_device<N, S>(
        &self,
        name: &'static str,
    ) -> Result<(TestEnvironment<'_>, S::Proxy, TestEndpoint<'_>)>
    where
        N: Netstack,
        S: fidl::endpoints::ServiceMarker + fidl::endpoints::DiscoverableService,
    {
        let (env, stack) = self.new_netstack::<N, S>(name)?;
        let endpoint = self.create_endpoint(name).await.context("failed to create endpoint")?;
        Ok((env, stack, endpoint))
    }
}

/// Interface configuration used by [`TestEnvironment::join_network`].
pub enum InterfaceConfig {
    /// Interface is configured with a static address.
    StaticIp(fidl_fuchsia_net_stack::InterfaceAddress),
    /// Interface is configured to use DHCP to obtain an address.
    Dhcp,
    /// No address configuration is performed.
    None,
}

/// An environment within a netemul sandbox.
#[must_use]
pub struct TestEnvironment<'a> {
    environment: netemul_environment::ManagedEnvironmentProxy,
    name: String,
    _sandbox: &'a TestSandbox,
}

impl<'a> TestEnvironment<'a> {
    /// Connects to a service within the environment.
    pub fn connect_to_service<S>(&self) -> Result<S::Proxy>
    where
        S: fidl::endpoints::ServiceMarker + fidl::endpoints::DiscoverableService,
    {
        let (proxy, server) = zx::Channel::create()?;
        let () = self.environment.connect_to_service(S::SERVICE_NAME, server)?;
        let proxy = fuchsia_async::Channel::from_channel(proxy)?;
        Ok(<S::Proxy as fidl::endpoints::Proxy>::from_channel(proxy))
    }

    /// Gets this environment's launcher.
    ///
    /// All applications launched within a netemul environment will have their
    /// output (stdout, stderr, syslog) decorated with the environment name.
    pub fn get_launcher(&self) -> Result<fidl_fuchsia_sys::LauncherProxy> {
        let (launcher, server) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_sys::LauncherMarker>()
                .context("failed to create launcher proxy")?;
        let () = self.environment.get_launcher(server)?;
        Ok(launcher)
    }

    /// Joins `network` with `config`.
    ///
    /// `join_network` is a helper to create a new endpoint `ep_name` attached
    /// to `network` and configure it with `config`. Returns a [`TestInterface`]
    /// which is already added to this environment's netstack, link
    /// online, enabled,  and configured according to `config`.
    ///
    /// Note that this environment needs a Netstack for this operation to
    /// succeed. See [`TestSandbox::create_netstack_environment`] to create an
    /// environment with all the Netstack services.
    pub async fn join_network(
        &self,
        network: &TestNetwork<'a>,
        ep_name: impl Into<String>,
        config: InterfaceConfig,
    ) -> Result<TestInterface<'a>> {
        let endpoint =
            network.create_endpoint(ep_name).await.context("failed to create endpoint")?;
        let interface =
            endpoint.into_interface_in_environment(self).await.context("failed to add endpoint")?;
        let () = interface.set_link_up(true).await.context("failed to start endpoint")?;
        let () = match config {
            InterfaceConfig::StaticIp(addr) => {
                interface.add_ip_addr(addr).await.context("failed to add static IP")?
            }
            InterfaceConfig::Dhcp => {
                interface.start_dhcp().await.context("failed to start DHCP")?;
            }
            InterfaceConfig::None => (),
        };
        let () = interface.enable_interface().await.context("failed to enable interface")?;
        Ok(interface)
    }
}

/// A virtual Network.
///
/// `TestNetwork` is a single virtual broadcast domain backed by Netemul.
/// Created through [`TestSandbox::create_network`].
#[must_use]
pub struct TestNetwork<'a> {
    network: netemul_network::NetworkProxy,
    name: String,
    sandbox: &'a TestSandbox,
}

impl<'a> TestNetwork<'a> {
    /// Attaches `ep` to this network.
    pub async fn attach_endpoint(&self, ep: &TestEndpoint<'a>) -> Result<()> {
        Ok(zx::Status::ok(
            self.network.attach_endpoint(&ep.name).await.context("attach_endpoint FIDL error")?,
        )
        .context("attach_endpoint failed")?)
    }

    /// Creates a new endpoint with `name` attached to this network.
    pub async fn create_endpoint(&self, name: impl Into<String>) -> Result<TestEndpoint<'a>> {
        let ep = self
            .sandbox
            .create_endpoint(name)
            .await
            .with_context(|| format!("failed to create endpoint for network {}", self.name))?;
        let () = self.attach_endpoint(&ep).await.with_context(|| {
            format!("failed to attach endpoint {} to network {}", ep.name, self.name)
        })?;
        Ok(ep)
    }
}

/// A virtual network endpoint backed by Netemul.
#[must_use]
pub struct TestEndpoint<'a> {
    endpoint: netemul_network::EndpointProxy,
    name: String,
    _sandbox: &'a TestSandbox,
}

impl<'a> std::ops::Deref for TestEndpoint<'a> {
    type Target = netemul_network::EndpointProxy;

    fn deref(&self) -> &Self::Target {
        &self.endpoint
    }
}

impl<'a> TestEndpoint<'a> {
    /// Gets access to this device's virtual Ethernet device.
    ///
    /// Note that an error is returned if the Endpoint is a
    /// [`netemul_network::DeviceConnection::NetworkDevice`].
    pub async fn get_ethernet(
        &self,
    ) -> Result<fidl::endpoints::ClientEnd<fidl_fuchsia_hardware_ethernet::DeviceMarker>> {
        match self
            .get_device()
            .await
            .with_context(|| format!("failed to get device connection for {}", self.name))?
        {
            netemul_network::DeviceConnection::Ethernet(e) => Ok(e),
            netemul_network::DeviceConnection::NetworkDevice(_) => {
                Err(anyhow::anyhow!("Endpoint {} is not an Ethernet device", self.name))
            }
        }
    }

    /// Adds this endpoint to `stack`, returning the interface identifier.
    pub async fn add_to_stack(&self, stack: &net_stack::StackProxy) -> Result<u64> {
        Ok(match self.get_device().await.context("get_device failed")? {
            netemul_network::DeviceConnection::Ethernet(eth) => stack.add_ethernet_interface(&self.name, eth).await.squash_result()?,
            netemul_network::DeviceConnection::NetworkDevice(netdevice) => {
                todo!("(48907) Support NetworkDevice version of integration tests. Got unexpected NetworkDevice {:?}", netdevice)
            }
        })
    }

    /// Consumes this `TestEndpoint` and tries to add it to the Netstack in
    /// `environment`, returning a [`TestInterface`] on success.
    pub async fn into_interface_in_environment(
        self,
        environment: &TestEnvironment<'a>,
    ) -> Result<TestInterface<'a>> {
        let stack = environment.connect_to_service::<net_stack::StackMarker>()?;
        let netstack = environment.connect_to_service::<fidl_fuchsia_netstack::NetstackMarker>()?;
        let id = self.add_to_stack(&stack).await.with_context(|| {
            format!("failed to add {} to environment {}", self.name, environment.name)
        })?;
        Ok(TestInterface { endpoint: self, id, stack, netstack })
    }
}

/// A [`TestEndpoint`] that is installed in an environment's Netstack.
#[must_use]
pub struct TestInterface<'a> {
    endpoint: TestEndpoint<'a>,
    id: u64,
    stack: net_stack::StackProxy,
    netstack: fidl_fuchsia_netstack::NetstackProxy,
}

impl<'a> std::ops::Deref for TestInterface<'a> {
    type Target = netemul_network::EndpointProxy;

    fn deref(&self) -> &Self::Target {
        &self.endpoint
    }
}

impl<'a> TestInterface<'a> {
    /// Gets the interface identifier.
    pub fn id(&self) -> u64 {
        self.id
    }

    /// Enable interface.
    ///
    /// Equivalent to `stack.enable_interface(test_interface.id())`.
    pub async fn enable_interface(&self) -> Result<()> {
        self.stack.enable_interface(self.id).await.squash_result().with_context(|| {
            format!("stack.enable_interface for endpoint {} failed", self.endpoint.name)
        })
    }

    /// Add interface address.
    ///
    /// Equivalent to `stack.add_interface_address(test_interface.id(), &mut addr)`.
    pub async fn add_ip_addr(&self, mut addr: net_stack::InterfaceAddress) -> Result<()> {
        self.stack.add_interface_address(self.id, &mut addr).await.squash_result().with_context(
            || {
                format!(
                    "stack.add_interface_address({}, {:?}) for endpoint {} failed",
                    self.id, addr, self.endpoint.name
                )
            },
        )
    }

    /// Starts DHCP on this interface.
    pub async fn start_dhcp(&self) -> Result<()> {
        let (dhcp_client, server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_net_dhcp::ClientMarker>()
                .context("failed to create endpoints for fuchsia.net.dhcp.Client")?;

        let () = self
            .netstack
            .get_dhcp_client(self.id.try_into().expect("should fit"), server_end)
            .await
            .context("failed to call netstack.get_dhcp_client")?
            .map_err(zx::Status::from_raw)
            .context("failed to get dhcp client")?;

        let () = dhcp_client
            .start()
            .await
            .context("failed to call dhcp_client.start")?
            .map_err(zx::Status::from_raw)
            .context("failed to start dhcp client")?;

        Ok(())
    }
}
