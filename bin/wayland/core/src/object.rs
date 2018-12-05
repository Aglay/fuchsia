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
use std::any::Any;
use std::collections::hash_map::{Entry, HashMap};
use std::fmt::{self, Debug, Display};
use std::marker::PhantomData;

use failure::{format_err, Error, Fail};

use crate::{
    Arg, Client, FromArgs, Interface, MessageGroupSpec, MessageHeader, MessageSpec, MessageType,
    ObjectId,
};

/// The |ObjectMap| holds the state of active objects for a single connection.
///
/// When a new connection is established, the server should populate the
/// |ObjectMap| with the "wl_display" singleton object. From the the client can
/// query the registry and bind new objects to the interfaces the client
/// understands.
pub(crate) struct ObjectMap {
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

    /// Looks up the recevier function and the message structure from the map.
    pub(crate) fn lookup_internal(
        &self, header: &MessageHeader,
    ) -> Result<(MessageReceiverFn, &'static MessageSpec), Error> {
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

        Ok((receiver.receiver(), spec))
    }

    /// Adds a new object into the map that will handle messages with the sender
    /// set to |id|. When a message is received with the corresponding |id|, the
    /// message will be decoded and forwarded to the |RequestReceiver|.
    ///
    /// Returns Err if there is already an object for |id| in this |ObjectMap|.
    pub fn add_object<I: Interface + 'static, R: RequestReceiver<I> + 'static>(
        &mut self, id: u32, receiver: R,
    ) -> Result<ObjectRef<R>, Error> {
        self.add_object_raw(id, Box::new(RequestDispatcher::new(receiver)), &I::REQUESTS)?;
        Ok(ObjectRef::from_id(id))
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

/// When the concrete type of an object is known statically, we can provide
/// an ObjectRef wrapper around the ObjectId in order to make downcasting
/// simpler.
///
/// This is primarily useful when vending self references to MessageReceviers.
pub struct ObjectRef<T: 'static>(PhantomData<T>, ObjectId);

// We cannot just derive these since that will place the corresponding trait
// bound on `T`.
impl<T: 'static> Copy for ObjectRef<T> {}
impl<T: 'static> Clone for ObjectRef<T> {
    fn clone(&self) -> Self {
        ObjectRef(PhantomData, self.1)
    }
}
impl<T: 'static> Debug for ObjectRef<T> {
    fn fmt(&self, f: &mut fmt::Formatter) -> Result<(), fmt::Error> {
        write!(f, "ObjectRef({})", self.1)
    }
}
impl<T> From<ObjectId> for ObjectRef<T> {
    fn from(id: ObjectId) -> Self {
        Self::from_id(id)
    }
}

impl<T> ObjectRef<T> {
    pub fn from_id(id: ObjectId) -> Self {
        ObjectRef(PhantomData, id)
    }

    pub fn id(&self) -> ObjectId {
        self.1
    }

    /// Provides an immutable reference to an object, downcasted to |T|.
    pub fn get<'a>(&self, client: &'a Client) -> Result<&'a T, ObjectLookupError> {
        client.get_object(self.1)
    }

    /// Provides a mutable reference to an object, downcasted to |T|.
    pub fn get_mut<'a>(&self, client: &'a mut Client) -> Result<&'a mut T, ObjectLookupError> {
        client.get_object_mut(self.1)
    }

    /// Returns `true` iff the underlying object is still valid.
    ///
    /// Here 'valid' means that the object_id exists and refers to an object
    /// of type `T`. This method will still return `true` if the associated
    /// object_id has been deleted and recreated with the same type `T. If this
    /// is not desirable then the caller must track instance with some state
    /// embedded into `T`.
    ///
    /// This can be useful to verify prior to sending events using the
    /// associated object_id but the host object itself is not required.
    pub fn is_valid(&self, client: &mut Client) -> bool {
        self.get(client).is_ok()
    }
}

/// A `NewObject` is a type-safe wrapper around a 'new_id' argument that has
/// a static wayland interface. This wrapper will enforce that the object is
/// only implemented by types that can receive wayland messages for the
/// expected interface.
pub struct NewObject<I: Interface + 'static>(PhantomData<I>, ObjectId);
impl<I: Interface + 'static> Display for NewObject<I> {
    fn fmt(&self, f: &mut fmt::Formatter) -> Result<(), fmt::Error> {
        write!(f, "NewObject<{}>({})", I::NAME, self.1)
    }
}
impl<I: Interface + 'static> Debug for NewObject<I> {
    fn fmt(&self, f: &mut fmt::Formatter) -> Result<(), fmt::Error> {
        write!(f, "{}", self)
    }
}

/// Support turning raw `ObjectId`s into `NewObject`s.
///
/// Ex:
///   let id: ObjectId = 3;
///   let new_object: NewObject<MyInterface> = id.into();
impl<I: Interface + 'static> From<ObjectId> for NewObject<I> {
    fn from(id: ObjectId) -> Self {
        Self::from_id(id)
    }
}

impl<I: Interface + 'static> NewObject<I> {
    pub fn from_id(id: ObjectId) -> Self {
        NewObject(PhantomData, id)
    }

    pub fn id(&self) -> ObjectId {
        self.1
    }

    pub fn implement<R: RequestReceiver<I>>(
        self, client: &mut Client, receiver: R,
    ) -> Result<ObjectRef<R>, Error> {
        client.add_object(self.1, receiver)
    }
}

/// A |MessageReceiver| is a type that can accept in-bound messages from a
/// client.
///
/// The server will dispatch |Message|s to the appropriate |MessageReceiver|
/// by reading the sender field in the message header.
type MessageReceiverFn =
    fn(this: ObjectId, opcode: u16, args: Vec<Arg>, client: &mut Client) -> Result<(), Error>;
pub trait MessageReceiver {
    /// Returns a function pointer that will be called to handle requests
    /// targeting this object.
    fn receiver(&self) -> MessageReceiverFn;

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
    ///                  client: &mut Client
    ///       ) -> Result<(), Error> {
    ///           let this = self.get()?;
    ///           let this_mut = self.get_mut()?;
    ///       }
    ///   }
    fn receive(
        this: ObjectRef<Self>, request: I::Request, client: &mut Client,
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
pub struct RequestDispatcher<I: Interface, R: RequestReceiver<I>> {
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
    this: ObjectId, opcode: u16, args: Vec<Arg>, client: &mut Client,
) -> Result<(), Error> {
    let request = I::Request::from_args(opcode, args)?;
    if client.protocol_logging() {
        println!("--r-> {}", request.log(this));
    }
    R::receive(ObjectRef(PhantomData, this), request, client)?;
    Ok(())
}

/// Convert the raw Message into the appropriate request type by delegating
/// to the associated |Request| type of |Interface|, and then invoke the
/// receiver.
impl<I: Interface, R: RequestReceiver<I>> MessageReceiver for RequestDispatcher<I, R> {
    fn receiver(&self) -> MessageReceiverFn {
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

    use fuchsia_async as fasync;
    use fuchsia_zircon as zx;
    use parking_lot::Mutex;
    use std::sync::Arc;

    use crate::test_protocol::*;
    use crate::{IntoMessage, RegistryBuilder};

    fn create_client() -> Result<Client, Error> {
        let _executor = fasync::Executor::new();
        let registry = Arc::new(Mutex::new(RegistryBuilder::new().build()));
        let (c1, _c2) = zx::Channel::create()?;
        Ok(Client::new(fasync::Channel::from_channel(c1)?, registry))
    }

    #[test]
    fn dispatch_message_to_request_receiver() -> Result<(), Error> {
        let mut client = create_client()?;
        client.add_object(0, TestReceiver::new())?;

        // Send a sync message; verify it's received.
        client.receive_message(TestMessage::Message1.into_message(0)?)?;
        assert_eq!(1, client.get_object::<TestReceiver>(0)?.count());
        Ok(())
    }

    #[test]
    fn add_object_duplicate_id() -> Result<(), Error> {
        let mut client = create_client()?;
        assert!(client.add_object(0, TestReceiver::new()).is_ok());
        assert!(client.add_object(0, TestReceiver::new()).is_err());
        Ok(())
    }

    #[test]
    fn dispatch_message_to_invalid_id() -> Result<(), Error> {
        // Send a message to an empty map.
        let mut client = create_client()?;

        assert!(client
            .receive_message(TestMessage::Message1.into_message(0)?)
            .is_err());
        Ok(())
    }
}
