// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::switchboard::base::*, failure::Error, fuchsia_async as fasync, futures::lock::Mutex,
    futures::stream::StreamExt, parking_lot::RwLock, std::marker::PhantomData, std::sync::Arc,
};

/// Handler for hanging gets within the switchboard.
/// We never use the data type T directly, but it is used to constrain ST as the sender
/// for that type.
/// To use, one should implement a sender, as well as a way to convert SettingResponse into
/// something that sender can use.
pub struct HangingGetHandler<T, ST> {
    switchboard_handle: Arc<RwLock<dyn Switchboard + Send + Sync>>,
    _listen_session: Box<dyn ListenSession + Send + Sync>,
    sent_latest_value: bool,
    hanging_get: Option<ST>,
    data_type: PhantomData<T>,
    setting_type: SettingType,
}

/// Trait that should be implemented to send data to the hanging get watcher.
pub trait Sender<T> {
    fn send_response(self, data: T);
}

impl<T, ST> HangingGetHandler<T, ST>
where
    T: From<SettingResponse> + Send + Sync + 'static,
    ST: Sender<T> + Send + Sync + 'static,
{
    pub fn create(
        switchboard_handle: Arc<RwLock<dyn Switchboard + Send + Sync>>,
        setting_type: SettingType,
    ) -> Arc<Mutex<HangingGetHandler<T, ST>>> {
        let (on_change_sender, mut on_change_receiver) =
            futures::channel::mpsc::unbounded::<SettingType>();

        let hanging_get_handler = Arc::new(Mutex::new(HangingGetHandler::<T, ST> {
            switchboard_handle: switchboard_handle.clone(),
            _listen_session: switchboard_handle
                .clone()
                .write()
                .listen(setting_type, on_change_sender)
                .expect("started listening successfully"),
            sent_latest_value: false,
            hanging_get: None,
            data_type: PhantomData,
            setting_type: setting_type,
        }));

        {
            let hanging_get_handler_clone = hanging_get_handler.clone();
            fasync::spawn(async move {
                while let Some(setting_type) = on_change_receiver.next().await {
                    assert_eq!(setting_type, setting_type);
                    let mut handler_lock = hanging_get_handler_clone.lock().await;
                    handler_lock.on_change().await;
                }
            });
        }

        hanging_get_handler
    }

    /// Park a new hanging get in the handler
    pub async fn watch(&mut self, responder: ST) {
        if let None = self.hanging_get {
            self.hanging_get = Some(responder);
            if self.sent_latest_value == false {
                self.on_change().await;
            }
        } else {
            panic!("Inconsistent state; watcher already registered");
        }
    }

    /// Called when receiving a notification that value has changed.
    async fn on_change(&mut self) {
        let mut responder_swap = None;
        std::mem::swap(&mut self.hanging_get, &mut responder_swap);

        if let Some(responder) = responder_swap {
            let value = self.get().await.expect("got current value");
            responder.send_response(value);
            self.hanging_get = None;
            self.sent_latest_value = true;
        } else {
            self.sent_latest_value = false;
        }
    }

    async fn get(&self) -> Result<T, Error> {
        let (response_tx, response_rx) =
            futures::channel::oneshot::channel::<SettingResponseResult>();
        {
            let mut switchboard = self.switchboard_handle.write();
            switchboard
                .request(self.setting_type, SettingRequest::Get, response_tx)
                .expect("made request");
        }
        if let Ok(Some(setting_response)) = response_rx.await.expect("got result") {
            Ok(T::from(setting_response))
        } else {
            panic!("incorrect value sent to display");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use failure::Error;
    use futures::channel::mpsc::UnboundedSender;

    const ID1: f32 = 1.0;
    const ID2: f32 = 2.0;
    const SETTING_TYPE: SettingType = SettingType::Display;

    struct TestStruct {
        id: f32,
    }

    struct TestSender {
        sender: UnboundedSender<TestStruct>,
    }

    struct TestListenSession {}

    impl ListenSession for TestListenSession {
        fn close(&mut self) {}
    }

    impl Drop for TestListenSession {
        fn drop(&mut self) {}
    }

    struct TestSwitchboard {
        id_to_send: Arc<RwLock<f32>>,
        setting_type: Option<SettingType>,
        listener: Option<UnboundedSender<SettingType>>,
    }

    impl TestSwitchboard {
        fn notify_listener(&self) {
            if let Some(setting_type_value) = self.setting_type {
                if let Some(listener_sender) = self.listener.clone() {
                    listener_sender.unbounded_send(setting_type_value).unwrap();
                    return;
                }
            }
            panic!("Missing listener to notify");
        }
    }

    impl Sender<TestStruct> for TestSender {
        fn send_response(self, data: TestStruct) {
            self.sender.unbounded_send(data).unwrap();
        }
    }

    impl From<SettingResponse> for TestStruct {
        fn from(response: SettingResponse) -> Self {
            if let SettingResponse::Brightness(info) = response {
                return TestStruct { id: info.manual_brightness_value };
            }
            panic!("bad response");
        }
    }

    impl Switchboard for TestSwitchboard {
        fn request(
            &mut self,
            setting_type: SettingType,
            request: SettingRequest,
            callback: futures::channel::oneshot::Sender<Result<Option<SettingResponse>, Error>>,
        ) -> Result<(), Error> {
            assert_eq!(setting_type, SETTING_TYPE);
            assert_eq!(request, SettingRequest::Get);

            let value = *self.id_to_send.read();
            callback
                .send(Ok(Some(SettingResponse::Brightness(DisplayInfo::new(false, value)))))
                .unwrap();
            Ok(())
        }

        fn listen(
            &mut self,
            setting_type: SettingType,
            listener: UnboundedSender<SettingType>,
        ) -> Result<Box<dyn ListenSession + Send + Sync>, Error> {
            self.setting_type = Some(setting_type);
            self.listener = Some(listener);
            Ok(Box::new(TestListenSession {}))
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_hanging_get() {
        let current_id = Arc::new(RwLock::new(ID1));
        let switchboard_handle = Arc::new(RwLock::new(TestSwitchboard {
            id_to_send: current_id.clone(),
            listener: None,
            setting_type: None,
        }));

        let hanging_get_handler: Arc<Mutex<HangingGetHandler<TestStruct, TestSender>>> =
            HangingGetHandler::create(switchboard_handle.clone(), SettingType::Display);

        let (hanging_get_responder, mut hanging_get_listener) =
            futures::channel::mpsc::unbounded::<TestStruct>();

        {
            let mut hanging_get_lock = hanging_get_handler.lock().await;
            hanging_get_lock.watch(TestSender { sender: hanging_get_responder.clone() }).await;
        }

        // First get should return immediately
        let data = hanging_get_listener.next().await.unwrap();
        assert_eq!(data.id, ID1);

        // Subsequent one should wait until new value is notified
        {
            let mut hanging_get_lock = hanging_get_handler.lock().await;
            hanging_get_lock.watch(TestSender { sender: hanging_get_responder.clone() }).await;
        }

        {
            *current_id.write() = ID2;
        }

        {
            let switchboard = switchboard_handle.read();
            switchboard.notify_listener();
        }

        let data = hanging_get_listener.next().await.unwrap();
        assert_eq!(data.id, ID2);
    }
}
