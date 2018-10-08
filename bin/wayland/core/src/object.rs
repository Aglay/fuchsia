// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
///! This module provides types for managing the set of wayland objects for a
///! connection. The |ObjectMap| associates a numeric object id with a
///! |MessageReceiver| that can intepret the message and provide the logic to
///! implement the interface contract for that object.
///!
///! At a high level, the |RequestReceiver<I:Interface>| trait allows for a
///! decoded request to be interacted with. In the middle we provide the
///! |RequestDispatcher| struct that implements the |MessageReceiver| trait
///! by decoding the |Message| into the concrete |Request| type for the
///! |Interface|.
///!
///! Consumers should mostly have to only concern themselves with the
///! |RequestReceiver<I:Interface>| trait, with the other types being mostly the
///! glue and dispatch logic.
///!
///! Ex:
///!    let mut map = ObjectMap::new();
///!    // Add the global wl_display object (object ID 0).
///!    map.add_object(WlDisplay, 0, |request| {
///!        match request {
///!        WlDisplayRequest::GetRegistry {..} => { ... },
///!        WlDispayRequest::Sync {..} => { ... },
///!        }
///!    });
use std::any::Any;
use std::collections::hash_map::{Entry, HashMap};
use std::marker::PhantomData;

use failure::{format_err, Error, Fail};

use crate::{Arg, FromArgs, Interface, Message, MessageGroupSpec, ObjectId};

/// The |ObjectMap| holds the state of active objects for a single connection.
///
/// When a new connection is established, the server should populate the
/// |ObjectMap| with the "wl_display" singleton object. From the the client can
/// query the registry and bind new objects to the interfaces the client
/// understands.
pub struct ObjectMap {
    /// The set of active objects. This holds the descriptors of supported
    /// messages for each object, as well as the |MessageReceiver| that's
    /// capable of handling the requests.
    objects: HashMap<u32, ObjectMapEntry>,
}

struct ObjectMapEntry {
    /// The opcodes and method signatures accepted by this object.
    request_spec: &'static MessageGroupSpec,
    /// The handler for this object.
    receiver: Box<dyn MessageReceiver>,
}

#[derive(Copy, Clone, Debug, Fail)]
pub enum ObjectMapError {
    /// An error raised if a message is received with the 'sender' field set
    /// to an unknown object (ex: either the object has not been created yet
    /// or it has already been deleted).
    #[fail(display = "Object with id {} is not found", _0)]
    InvalidObjectId(u32),

    /// An error raised if a message is delivered to an object with an
    /// unsupported or unknown opcode.
    #[fail(display = "Opcode {} is not supported", _0)]
    InvalidOpcode(u16),

    /// An error raised if a request is dispatched to object ID that does not
    /// have an assocated |MessageReceiver|.
    #[fail(display = "Object is not implemented")]
    ObjectNotImplemented,
}

/// Errors generated when looking up objects from the map.
#[derive(Debug, Fail)]
pub enum ObjectLookupError {
    #[fail(display = "Object with id does not exist")]
    ObjectDoesNotExist,
    #[fail(display = "Failed to downcast")]
    DowncastFailed,
}

/// When the concrete type of an object is known statically, we can provide
/// an ObjectRef wrapper around the ObjectId in order to make downcasting
/// simpler.
///
/// This is primarily useful when vending self references to MessageReceviers.
#[derive(Copy, Clone, Debug)]
pub struct ObjectRef<T: 'static>(PhantomData<T>, ObjectId);

impl<T> ObjectRef<T> {
    /// Provides an immutable reference to an object, downcasted to |T|.
    pub fn get<'a>(&self, map: &'a ObjectMap) -> Result<&'a T, ObjectLookupError> {
        map.get(self.1)
    }

    /// Provides a mutable reference to an object, downcasted to |T|.
    pub fn get_mut<'a>(&self, map: &'a mut ObjectMap) -> Result<&'a mut T, ObjectLookupError> {
        map.get_mut(self.1)
    }
}

impl ObjectMap {
    pub fn new() -> Self {
        ObjectMap {
            objects: HashMap::new(),
        }
    }

    /// Looks up an object in the map and returns a downcasted reference to
    /// the implementation.
    pub fn get<T: Any>(&self, id: ObjectId) -> Result<&T, ObjectLookupError> {
        match self.objects.get(&id) {
            Some(entry) => match entry.receiver.data().downcast_ref() {
                Some(t) => Ok(t),
                None => Err(ObjectLookupError::DowncastFailed),
            },
            None => Err(ObjectLookupError::ObjectDoesNotExist),
        }
    }

    /// Looks up an object in the map and returns a downcasted mutable
    /// reference to the implementation.
    pub fn get_mut<T: Any>(&mut self, id: ObjectId) -> Result<&mut T, ObjectLookupError> {
        match self.objects.get_mut(&id) {
            Some(entry) => match entry.receiver.data_mut().downcast_mut() {
                Some(t) => Ok(t),
                None => Err(ObjectLookupError::DowncastFailed),
            },
            None => Err(ObjectLookupError::ObjectDoesNotExist),
        }
    }

    /// Reads the message header to find the target for this message and then
    /// forwards the message to the associated |MessageReceiver|.
    ///
    /// Returns Err if no object is associated with the sender field in the
    /// message header, or if the objects receiver itself fails.
    pub fn receive_message(&mut self, mut message: Message) -> Result<(), Error> {
        let header = message.read_header()?;
        // Lookup the table entry for this object & fail if there is no entry
        // found.
        let (receiver, spec) = {
            let ObjectMapEntry {
                request_spec,
                receiver,
            } = self
                .objects
                .get(&header.sender)
                .ok_or(ObjectMapError::InvalidObjectId(header.sender))?;
            let spec = request_spec
                .0
                .get(header.opcode as usize)
                .ok_or(ObjectMapError::InvalidOpcode(header.opcode))?;

            (receiver.receiver(), spec)
        };

        // Decode the argument stream and invoke the |MessageReceiver|.
        let args = message.read_args(spec.0)?;
        receiver(header.sender, header.opcode, args, self)?;
        Ok(())
    }

    /// Adds a new object into the map that will handle messages with the sender
    /// set to |id|. When a message is received with the corresponding |id|, the
    /// message will be decoded and forwarded to the |RequestReceiver|.
    ///
    /// Returns Err if there is already an object for |id| in this |ObjectMap|.
    pub fn add_object<I: Interface + 'static, R: RequestReceiver<I> + 'static>(
        &mut self, _: I, id: u32, receiver: R,
    ) -> Result<(), Error> {
        self.add_object_raw(id, Box::new(RequestDispatcher::new(receiver)), &I::REQUESTS)
    }

    /// Adds an object to the map using the low-level primitives. It's favorable
    /// to use instead |add_object| if the wayland interface for the object is
    /// statically known.
    pub fn add_object_raw(
        &mut self, id: ObjectId, receiver: Box<MessageReceiver>,
        request_spec: &'static MessageGroupSpec,
    ) -> Result<(), Error> {
        if let Entry::Vacant(entry) = self.objects.entry(id) {
            entry.insert(ObjectMapEntry {
                receiver,
                request_spec,
            });
            Ok(())
        } else {
            Err(format_err!("Can't add duplicate object with id {}. ", id))
        }
    }

    pub fn delete(&mut self, id: ObjectId) -> Result<(), Error> {
        if self.objects.remove(&id).is_some() {
            // TODO: Send wl_display::delete_id.
            Ok(())
        } else {
            Err(format_err!("Item {} does not exist", id))
        }
    }
}

/// A |MessageReceiver| is a type that can accept in-bound messages from a
/// client.
///
/// The server will dispatch |Message|s to the appropriate |MessageReceiver|
/// by reading the sender field in the message header.
pub trait MessageReceiver {
    /// Returns a function pointer that will be called to handle requests
    /// targeting this object.
    fn receiver(
        &self,
    ) -> fn(this: ObjectId, opcode: u16, args: Vec<Arg>, object_map: &mut ObjectMap)
        -> Result<(), Error>;

    fn data(&self) -> &Any;

    fn data_mut(&mut self) -> &mut Any;
}

/// The |RequestReceiver| trait is what high level code will use to work with
/// request messages for a given type.
pub trait RequestReceiver<I: Interface>: Any + Sized {
    /// Handle a decoded message for the associated |Interface|.
    ///
    /// |self| is not directly provided, but instead is provided as an
    /// |ObjectId| that can be used to get a reference to self.
    ///
    /// Ex:
    ///   struct MyReceiver;
    ///
    ///   impl RequestReceiver<MyInterface> for MyReceiver {
    ///       fn receive(mut this: ObjectRef<Self>,
    ///                  request: MyInterfaceRequest,
    ///                  object_map: &mut ObjectMap
    ///       ) -> Result<(), Error> {
    ///           let this = self.get()?;
    ///           let this_mut = self.get_mut()?;
    ///       }
    ///   }
    fn receive(
        this: ObjectRef<Self>, request: I::Request, object_map: &mut ObjectMap,
    ) -> Result<(), Error>;
}

/// Implements a |MessageReceiver| that can decode a request into the
/// appropriate request type for an |Interface|, and then invoke an
/// |Implementation|
///
/// This struct essentially is the glue that sits in between the generic
/// |MessageReceiver| trait that is used to dispatch raw message buffers and
/// the higher level |RequestReceiver| that operates on the decoded request
/// enums.
pub(crate) struct RequestDispatcher<I: Interface, R: RequestReceiver<I>> {
    _marker: PhantomData<I>,
    receiver: R,
}

impl<I: Interface, R: RequestReceiver<I>> RequestDispatcher<I, R> {
    pub fn new(receiver: R) -> Self {
        RequestDispatcher {
            receiver,
            _marker: PhantomData,
        }
    }
}

fn receive_message<I: Interface, R: RequestReceiver<I>>(
    this: ObjectId, opcode: u16, args: Vec<Arg>, object_map: &mut ObjectMap,
) -> Result<(), Error> {
    let request = I::Request::from_args(opcode, args)?;
    R::receive(ObjectRef(PhantomData, this), request, object_map)?;
    Ok(())
}

/// Convert the raw Message into the appropriate request type by delegating
/// to the associated |Request| type of |Interface|, and then invoke the
/// receiver.
impl<I: Interface, R: RequestReceiver<I>> MessageReceiver for RequestDispatcher<I, R> {
    fn receiver(
        &self,
    ) -> fn(this: ObjectId, opcode: u16, args: Vec<Arg>, object_map: &mut ObjectMap)
        -> Result<(), Error> {
        receive_message::<I, R>
    }

    fn data(&self) -> &Any {
        &self.receiver
    }

    fn data_mut(&mut self) -> &mut Any {
        &mut self.receiver
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::test_protocol::*;
    use crate::IntoMessage;

    #[test]
    fn dispatch_message_to_request_receiver() -> Result<(), Error> {
        let mut objects = ObjectMap::new();
        objects.add_object(TestInterface, 0, TestReceiver::new())?;

        // Send a sync message; verify it's received.
        objects.receive_message(TestMessage::Message1.into_message(0)?)?;
        assert_eq!(1, objects.get::<TestReceiver>(0)?.count());
        Ok(())
    }

    #[test]
    fn add_object_duplicate_id() -> Result<(), Error> {
        let mut objects = ObjectMap::new();
        assert!(
            objects
                .add_object(TestInterface, 0, TestReceiver::new())
                .is_ok()
        );
        assert!(
            objects
                .add_object(TestInterface, 0, TestReceiver::new())
                .is_err()
        );
        Ok(())
    }

    #[test]
    fn dispatch_message_to_invalid_id() -> Result<(), Error> {
        // Send a message to an empty map.
        let mut objects = ObjectMap::new();

        assert!(
            objects
                .receive_message(TestMessage::Message1.into_message(0)?)
                .is_err()
        );
        Ok(())
    }
}
