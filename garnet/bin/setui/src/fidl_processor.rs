// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use std::sync::Arc;

use anyhow::Error;
use fidl::endpoints::{Request, ServiceMarker};
use fuchsia_async as fasync;
use futures::future::LocalBoxFuture;
use futures::lock::Mutex;
use futures::{FutureExt, StreamExt, TryStreamExt};

use crate::switchboard::base::{SettingResponse, SettingType, SwitchboardClient};
use crate::switchboard::hanging_get_handler::{HangingGetHandler, Sender};
use crate::ExitSender;

pub type RequestResultCreator<'a, S> = LocalBoxFuture<'a, Result<Option<Request<S>>, Error>>;

type ChangeFunction<T> = Box<dyn Fn(&T, &T) -> bool + Send + Sync + 'static>;

/// The RequestContext is passed to each request callback to provide resources,
/// such as the switchboard and hanging get functionality. Note that we do not
/// directly expose the hanging get handler so that we can better control its
/// lifetime.
pub struct RequestContext<T, ST>
where
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    pub switchboard_client: SwitchboardClient,
    hanging_get_handler: Arc<Mutex<HangingGetHandler<T, ST>>>,
    exit_tx: ExitSender,
}

impl<T, ST> RequestContext<T, ST>
where
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    pub async fn watch(&self, responder: ST, close_on_error: bool) {
        let mut hanging_get_lock = self.hanging_get_handler.lock().await;
        hanging_get_lock
            .watch(responder, if close_on_error { Some(self.exit_tx.clone()) } else { None })
            .await;
    }

    pub async fn watch_with_change_fn(
        &self,
        change_function: ChangeFunction<T>,
        responder: ST,
        close_on_error: bool,
    ) {
        let mut hanging_get_lock = self.hanging_get_handler.lock().await;
        hanging_get_lock
            .watch_with_change_fn(
                change_function,
                responder,
                if close_on_error { Some(self.exit_tx.clone()) } else { None },
            )
            .await;
    }
}

impl<T, ST> Clone for RequestContext<T, ST>
where
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    fn clone(&self) -> RequestContext<T, ST> {
        RequestContext {
            switchboard_client: self.switchboard_client.clone(),
            hanging_get_handler: self.hanging_get_handler.clone(),
            exit_tx: self.exit_tx.clone(),
        }
    }
}

/// RequestCallback closures are handed a request and the surrounding
/// context. They are expected to hand back a future that returns when the
/// request is processed. The returned value is a result with an optional
/// request, containing None if not processed and the original request
/// otherwise.
pub type RequestCallback<S, T, ST> =
    Box<dyn Fn(RequestContext<T, ST>, Request<S>) -> RequestResultCreator<'static, S>>;
pub type RequestStream<S> = <S as ServiceMarker>::RequestStream;

/// A processing unit is an entity that is able to process a stream request and
/// indicate whether the request was consumed.
trait ProcessingUnit<S>
where
    S: ServiceMarker,
{
    fn process(
        &self,
        switchboard: SwitchboardClient,
        request: Request<S>,
        exit_tx: ExitSender,
    ) -> RequestResultCreator<'static, S>;
}

/// SettingProcessingUnit is a concrete implementation of the ProcessingUnit
/// trait, allowing a RequestCallback to participate in stream request
/// processing. The SettingProcessingUnit maintains a hanging get handler keyed
/// to the constructed type.
struct SettingProcessingUnit<S, T, ST>
where
    S: ServiceMarker,
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    callback: RequestCallback<S, T, ST>,
    hanging_get_handler: Arc<Mutex<HangingGetHandler<T, ST>>>,
}

impl<S, T, ST> SettingProcessingUnit<S, T, ST>
where
    S: ServiceMarker,
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    async fn new(
        setting_type: SettingType,
        switchboard_client: SwitchboardClient,
        callback: RequestCallback<S, T, ST>,
    ) -> Self {
        Self {
            callback: callback,
            hanging_get_handler: HangingGetHandler::create(switchboard_client, setting_type).await,
        }
    }
}

impl<S, T, ST> Drop for SettingProcessingUnit<S, T, ST>
where
    S: ServiceMarker,
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    fn drop(&mut self) {
        let hanging_get_handler = self.hanging_get_handler.clone();
        fasync::spawn_local(async move {
            hanging_get_handler.lock().await.close();
        });
    }
}

impl<S, T, ST> ProcessingUnit<S> for SettingProcessingUnit<S, T, ST>
where
    S: ServiceMarker,
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    fn process(
        &self,
        switchboard_client: SwitchboardClient,
        request: Request<S>,
        exit_tx: ExitSender,
    ) -> RequestResultCreator<'static, S> {
        let context = RequestContext {
            switchboard_client: switchboard_client.clone(),
            hanging_get_handler: self.hanging_get_handler.clone(),
            exit_tx: exit_tx.clone(),
        };

        return (self.callback)(context.clone(), request);
    }
}

/// The FidlProcessor delegates request processing across a number of processing
/// units. There should be a single FidlProcessor per stream.
pub struct FidlProcessor<S>
where
    S: ServiceMarker,
{
    request_stream: RequestStream<S>,
    switchboard_client: SwitchboardClient,
    processing_units: Vec<Box<dyn ProcessingUnit<S>>>,
}

impl<S> FidlProcessor<S>
where
    S: ServiceMarker,
{
    pub fn new(stream: RequestStream<S>, switchboard_client: SwitchboardClient) -> Self {
        Self {
            request_stream: stream,
            switchboard_client: switchboard_client,
            processing_units: Vec::new(),
        }
    }

    pub async fn register<V, SV>(
        &mut self,
        setting_type: SettingType,
        callback: RequestCallback<S, V, SV>,
    ) where
        V: From<SettingResponse> + Send + Sync + 'static,
        SV: Sender<V> + Send + Sync + 'static,
    {
        let processing_unit = Box::new(
            SettingProcessingUnit::<S, V, SV>::new(
                setting_type,
                self.switchboard_client.clone(),
                callback,
            )
            .await,
        );
        self.processing_units.push(processing_unit);
    }

    // Process the stream. Note that we pass in the processor here as it cannot
    // be used again afterwards.
    pub async fn process(mut self) {
        let (exit_tx, mut exit_rx) = futures::channel::mpsc::unbounded::<()>();
        loop {
            // Note that we create a fuse outside the select! to prevent it from
            // being called from outside the select! macro.
            let mut fused_stream = self.request_stream.try_next().fuse();
            futures::select! {
                request = fused_stream => {
                    if let Ok(Some(mut req)) = request {
                        for processing_unit in &self.processing_units {
                            // If the processing unit consumes the request (a non-empty
                            // result is returned) or an error occurs, exit processing this
                            // request. Otherwise, hand the request to the next processing
                            // unit
                            match processing_unit.process(
                                    self.switchboard_client.clone(),
                                    req, exit_tx.clone()).await {
                                Ok(Some(return_request)) => {
                                    req = return_request;
                                }
                                _ => {
                                    break
                                }
                            }
                        }
                    } else {
                       return;
                    }
                }
                exit = exit_rx.next() => {
                    return;
                }
            }
        }
    }
}

pub fn process_stream<S, T, ST>(
    stream: RequestStream<S>,
    switchboard: SwitchboardClient,
    setting_type: SettingType,
    callback: RequestCallback<S, T, ST>,
) where
    S: ServiceMarker,
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    fasync::spawn_local(async move {
        let mut processor = FidlProcessor::<S>::new(stream, switchboard);
        processor.register::<T, ST>(setting_type, callback).await;
        processor.process().await;
    });
}

/// Process the request stream, enabling both watch() and watch2()
/// to be handled.
// TODO(fxb/52593): Remove when clients are ported to watch2.
pub fn process_stream_both_watches<S, T, ST1, ST2>(
    stream: RequestStream<S>,
    switchboard: SwitchboardClient,
    setting_type: SettingType,
    watch_callback: RequestCallback<S, T, ST1>,
    watch_2_callback: RequestCallback<S, T, ST2>,
) where
    S: ServiceMarker,
    T: From<SettingResponse> + Send + Sync + 'static,
    ST1: Sender<T> + Send + Sync + 'static,
    ST2: Sender<T> + Send + Sync + 'static,
{
    fasync::spawn_local(async move {
        let mut processor = FidlProcessor::<S>::new(stream, switchboard);
        processor.register::<T, ST1>(setting_type, watch_callback).await;
        processor.register::<T, ST2>(setting_type, watch_2_callback).await;
        processor.process().await;
    });
}
