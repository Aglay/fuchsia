// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{err_msg, Error, ResultExt},
    fidl::endpoints::{create_proxy, ClientEnd},
    fidl_fuchsia_test_breakpoints as fbreak,
    fuchsia_component::client::*,
    futures::future::BoxFuture,
};

/// A wrapper over the BreakpointSystem FIDL proxy.
/// Provides all of the FIDL methods with a cleaner, simpler interface.
/// Refer to breakpoints.fidl for a detailed description of this protocol.
pub struct BreakpointSystemClient {
    proxy: fbreak::BreakpointSystemProxy,
}

impl BreakpointSystemClient {
    /// Connects to the BreakpointSystem service at its default location
    /// The default location is presumably "/svc/fuchsia.test.breakpoints.BreakpointSystem"
    pub fn new() -> Result<Self, Error> {
        let proxy = connect_to_service::<fbreak::BreakpointSystemMarker>()
            .context("could not connect to BreakpointSystem service")?;
        Ok(BreakpointSystemClient::from_proxy(proxy))
    }

    /// Wraps a provided BreakpointSystem proxy
    pub fn from_proxy(proxy: fbreak::BreakpointSystemProxy) -> Self {
        Self { proxy }
    }

    pub async fn register(
        &self,
        event_types: Vec<fbreak::EventType>,
    ) -> Result<InvocationReceiverClient, Error> {
        let (proxy, server_end) = create_proxy::<fbreak::InvocationReceiverMarker>()?;
        self.proxy
            .register(&mut event_types.into_iter(), server_end)
            .await
            .context("could not register breakpoints")?;
        Ok(InvocationReceiverClient::new(proxy))
    }

    pub async fn start_component_manager(&self) -> Result<(), Error> {
        self.proxy.start_component_manager().await.context("could not start component manager")?;
        Ok(())
    }
}

/// A wrapper over the InvocationReceiver FIDL proxy.
/// Provides convenience methods that build on InvocationReceiver::Next
pub struct InvocationReceiverClient {
    proxy: fbreak::InvocationReceiverProxy,
}

impl InvocationReceiverClient {
    fn new(proxy: fbreak::InvocationReceiverProxy) -> Self {
        Self { proxy }
    }

    pub async fn next(&self) -> Result<fbreak::Invocation, Error> {
        let invocation = self.proxy.next().await.context("could not get next breakpoint")?;
        Ok(invocation)
    }

    /// Expects the next invocation to be of a particular type.
    /// Returns the casted type if successful and an error otherwise.
    pub async fn expect_type<T: Invocation>(&self) -> Result<T, Error> {
        let invocation = self.proxy.next().await?;
        T::from_fidl(invocation)
    }

    /// Expects the next invocation to be of a particular type and moniker.
    /// Returns the casted type if successful and an error otherwise.
    pub async fn expect_exact<T: Invocation>(&self, expected_moniker: &str) -> Result<T, Error> {
        let invocation = self.expect_type::<T>().await?;
        if expected_moniker == invocation.target_moniker() {
            Ok(invocation)
        } else {
            Err(err_msg("Incorrect moniker"))
        }
    }

    /// Waits for an invocation of a particular type.
    /// Implicitly resumes all other invocations.
    /// Returns the casted type if successful and an error otherwise.
    pub async fn wait_until_type<T: Invocation>(&self) -> Result<T, Error> {
        loop {
            let invocation = self.next().await?;
            if let Ok(invocation) = T::from_fidl(invocation) {
                return Ok(invocation);
            }
        }
    }

    /// Waits for an invocation of a particular type and moniker.
    /// Implicitly resumes all other invocations.
    /// Returns the casted type if successful and an error otherwise.
    pub async fn wait_until_exact<T: Invocation>(
        &self,
        expected_moniker: &str,
    ) -> Result<T, Error> {
        loop {
            let invocation = self.wait_until_type::<T>().await?;
            if invocation.target_moniker() == expected_moniker {
                return Ok(invocation);
            }
            invocation.resume().await?;
        }
    }

    /// Waits for a UseCapability invocation of a particular moniker and capability path.
    /// Implicitly resumes all other invocations.
    /// Returns the casted type if successful and an error otherwise.
    pub async fn wait_until_use_capability(
        &self,
        expected_moniker: &str,
        expected_capability_path: &str,
    ) -> Result<UseCapability, Error> {
        loop {
            let invocation = self.wait_until_exact::<UseCapability>(expected_moniker).await?;
            if expected_capability_path == invocation.capability {
                return Ok(invocation);
            }
            invocation.resume().await?;
        }
    }

    /// Waits for a RouteFrameworkCapability invocation of a particular moniker and capability path.
    /// Implicitly resumes all other invocations.
    /// Returns the casted type if successful and an error otherwise.
    pub async fn wait_until_route_framework_capability(
        &self,
        expected_moniker: &str,
        expected_capability: &str,
    ) -> Result<RouteFrameworkCapability, Error> {
        loop {
            let invocation =
                self.wait_until_exact::<RouteFrameworkCapability>(expected_moniker).await?;
            if expected_capability == invocation.capability {
                return Ok(invocation);
            }
            invocation.resume().await?;
        }
    }
}

/// Common features of any invocation - event type, target moniker, conversion function
pub trait Invocation: Handler {
    const TYPE: fbreak::EventType;
    fn target_moniker(&self) -> &str;
    fn from_fidl(inv: fbreak::Invocation) -> Result<Self, Error>;
}

/// Basic handler that resumes/unblocks from an Invocation
#[must_use = "invoke resume() otherwise component manager will be halted indefinitely!"]
pub trait Handler: Sized {
    fn handler_proxy(self) -> fbreak::HandlerProxy;

    #[must_use = "futures do nothing unless you await on them!"]
    fn resume<'a>(self) -> BoxFuture<'a, Result<(), fidl::Error>> {
        let proxy = self.handler_proxy();
        Box::pin(async move { proxy.resume().await })
    }
}

/// Implemented on fbreak::Invocation for resuming a generic invocation
impl Handler for fbreak::Invocation {
    fn handler_proxy(self) -> fbreak::HandlerProxy {
        self.handler
            .expect("Could not find handler in Invocation object")
            .into_proxy()
            .expect("Could not convert into proxy")
    }
}

/// A protocol that allows routing capabilities over FIDL.
pub trait RoutingProtocol {
    fn protocol_proxy(&self) -> fbreak::RoutingProtocolProxy;

    fn route<'a>(
        &self,
        client_end: ClientEnd<fbreak::CapabilityProviderMarker>,
    ) -> BoxFuture<'a, Result<(), fidl::Error>> {
        let proxy = self.protocol_proxy();
        Box::pin(async move { proxy.route(client_end).await })
    }
}

/// The macro defined below will automatically create event classes corresponding
/// to their breakpoints.fidl and hooks.rs counterparts. Every event class implements
/// the Invocation and Handler traits. These minimum requirements allow every event to
/// be handled by the breakpoints client library.

/// Creates an event class based on event type and an optional payload
/// * event_type -> FIDL name for event type
/// * payload -> If an event has a payload, describe the additional params:
///   * name -> FIDL name for the payload
///   * data -> If a payload contains data items, describe the additional params:
///     * name -> FIDL name for the data item
///     * ty -> Rust type for the data item
///   * protocol -> If a payload contains protocols, describe the additional params:
///     * name -> FIDL name for the protocol
///     * ty -> Rust type for the protocol proxy
///     * trait_name -> Rust name for the trait implementing this protocol
macro_rules! create_event {
    (
        event_type: $event_type:ident,
        payload: {
            name: $payload_name:ident,
            data: {$(
                {
                    name: $data_name:ident,
                    ty: $data_ty:ty,
                }
            )*},
            protocols: {$(
                {
                    name: $protocol_name:ident,
                    ty: $protocol_ty:ty,
                    trait_name: $protocol_trait_name:ident,
                }
            )*},
        }
    ) => {
        pub struct $event_type {
            target_moniker: String,
            handler: fbreak::HandlerProxy,
            $($protocol_name: $protocol_ty,)*
            $(pub $data_name: $data_ty,)*
        }

        impl Invocation for $event_type {
            const TYPE: fbreak::EventType = fbreak::EventType::$event_type;

            fn target_moniker(&self) -> &str {
                &self.target_moniker
            }

            fn from_fidl(inv: fbreak::Invocation) -> Result<Self, Error> {
                // Event type in invocation must match what is expected
                let event_type = inv.event_type.ok_or(
                    err_msg("Missing event_type from Invocation object")
                )?;
                if event_type != Self::TYPE {
                    return Err(err_msg("Incorrect event type"));
                }
                let target_moniker = inv.target_moniker.ok_or(
                    err_msg("Missing target_moniker from Invocation object")
                )?;
                let handler = inv.handler.ok_or(
                    err_msg("Missing handler from Invocation object")
                )?.into_proxy()?;

                // Extract the payload from the Invocation object.
                let event_payload = inv.event_payload.ok_or(
                    err_msg("Missing event_payload from Invocation object")
                )?;
                let $payload_name = event_payload.$payload_name.ok_or(
                    err_msg("Missing $payload_name from EventPayload object")
                )?;

                // Extract the additional data from the Payload object.
                $(
                    let $data_name: $data_ty = $payload_name.$data_name.ok_or(
                        err_msg("Missing $data_name from $payload_name object")
                    )?;
                )*

                // Extract the additional protocols from the Payload object.
                $(
                    let $protocol_name: $protocol_ty = $payload_name.$protocol_name.ok_or(
                        err_msg("Missing $protocol_name from $payload_name object")
                    )?.into_proxy()?;
                )*

                Ok($event_type { target_moniker, handler, $($data_name,)* $($protocol_name,)* })
            }
        }

        impl Handler for $event_type {
            fn handler_proxy(self) -> fbreak::HandlerProxy {
                self.handler
            }
        }

        $(
            impl $protocol_trait_name for $event_type {
                fn protocol_proxy(&self) -> $protocol_ty {
                    self.$protocol_name.clone()
                }
            }
        )*
    };
    ($event_type:ident) => {
        pub struct $event_type {
            target_moniker: String,
            handler: fbreak::HandlerProxy,
        }

        impl Invocation for $event_type {
            const TYPE: fbreak::EventType = fbreak::EventType::$event_type;

            fn target_moniker(&self) -> &str {
                &self.target_moniker
            }

            fn from_fidl(inv: fbreak::Invocation) -> Result<Self, Error> {
                // Event type in invocation must match what is expected
                let event_type = inv.event_type.ok_or(
                    err_msg("Missing event_type from Invocation object")
                )?;
                if event_type != Self::TYPE {
                    return Err(err_msg("Incorrect event type"));
                }
                let target_moniker = inv.target_moniker.ok_or(
                    err_msg("Missing target_moniker from Invocation object")
                )?;
                let handler = inv.handler.ok_or(
                    err_msg("Missing handler from Invocation object")
                )?.into_proxy()?;

                // There should be no payload for this event
                if inv.event_payload.is_some() {
                    return Err(err_msg("Unexpected event payload"));
                }

                Ok($event_type { target_moniker, handler, })
            }
        }

        impl Handler for $event_type {
            fn handler_proxy(self) -> fbreak::HandlerProxy {
                self.handler
            }
        }
    };
}

// To create a class for an event, use the above macro here.
create_event!(AddDynamicChild);
create_event!(PostDestroyInstance);
create_event!(PreDestroyInstance);
create_event!(RootComponentResolved);
create_event!(StartInstance);
create_event!(StopInstance);
create_event!(
    event_type: RouteBuiltinCapability,
    payload: {
        name: routing_payload,
        data: {
            {
                name: capability,
                ty: String,
            }
        },
        protocols: {
            {
                name: routing_protocol,
                ty: fbreak::RoutingProtocolProxy,
                trait_name: RoutingProtocol,
            }
        },
    }
);
create_event!(
    event_type: RouteFrameworkCapability,
    payload: {
        name: routing_payload,
        data: {
            {
                name: capability,
                ty: String,
            }
        },
        protocols: {
            {
                name: routing_protocol,
                ty: fbreak::RoutingProtocolProxy,
                trait_name: RoutingProtocol,
            }
        },
    }
);
create_event!(
    event_type: UseCapability,
    payload: {
        name: use_capability_payload,
        data: {
            {
                name: capability,
                ty: String,
            }
        },
        protocols: {},
    }
);
