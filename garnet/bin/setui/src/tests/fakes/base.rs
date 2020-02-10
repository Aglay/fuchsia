// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::registry::base::Command;
use crate::registry::base::GenerateHandler;
use crate::registry::device_storage::DeviceStorageFactory;
use anyhow::Error;
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::StreamExt;

/// Trait for providing a service.
pub trait Service {
  /// Returns true if this service can process the given service name, false
  /// otherwise.
  fn can_handle_service(&self, service_name: &str) -> bool;

  /// Processes the request stream within the specified channel. Ok is returned
  /// on success, an error otherwise.
  fn process_stream(&self, service_name: &str, channel: zx::Channel) -> Result<(), Error>;
}

/// A helper function for creating a simple setting handler.
pub fn create_setting_handler<T: DeviceStorageFactory + Send + Sync + 'static>(
  command_handler: Box<dyn Fn(Command) + Send + Sync + 'static>,
) -> GenerateHandler<T> {
  let (handler_tx, mut handler_rx) = futures::channel::mpsc::unbounded::<Command>();

  fasync::spawn(async move {
    while let Some(command) = handler_rx.next().await {
      (command_handler)(command);
    }
  });

  let handler_tx_clone = handler_tx.clone();

  return Box::new(move |_| handler_tx_clone.clone());
}
