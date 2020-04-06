// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A portable representation of handle-like objects for fidl.

#[cfg(target_os = "fuchsia")]
pub use fuchsia_handles::*;

#[cfg(not(target_os = "fuchsia"))]
pub use non_fuchsia_handles::*;

/// Fuchsia implementation of handles just aliases the zircon library
#[cfg(target_os = "fuchsia")]
pub mod fuchsia_handles {
    use crate::invoke_for_handle_types;

    use fuchsia_async as fasync;
    use fuchsia_zircon as zx;

    pub use zx::AsHandleRef;
    pub use zx::Handle;
    pub use zx::HandleBased;
    pub use zx::HandleRef;
    pub use zx::MessageBuf;

    macro_rules! fuchsia_handle {
        ($x:tt, Stub) => {
            /// Stub implementation of Zircon handle type $x.
            #[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
            #[repr(transparent)]
            pub struct $x(zx::Handle);

            impl zx::AsHandleRef for $x {
                fn as_handle_ref(&self) -> HandleRef<'_> {
                    self.0.as_handle_ref()
                }
            }
            impl From<Handle> for $x {
                fn from(handle: Handle) -> Self {
                    $x(handle)
                }
            }
            impl From<$x> for Handle {
                fn from(x: $x) -> Handle {
                    x.0
                }
            }
            impl zx::HandleBased for $x {}
        };
        ($x:tt, $availability:tt) => {
            pub use zx::$x;
        };
    }

    invoke_for_handle_types!(fuchsia_handle);

    pub use fasync::Channel as AsyncChannel;
    pub use fasync::Socket as AsyncSocket;

    pub use zx::SocketOpts;
}

/// Non-Fuchsia implementation of handles
#[cfg(not(target_os = "fuchsia"))]
pub mod non_fuchsia_handles {
    use crate::invoke_for_handle_types;

    use fuchsia_zircon_status as zx_status;
    use futures::future::poll_fn;
    use parking_lot::Mutex;
    use slab::Slab;
    use std::{
        borrow::BorrowMut,
        collections::VecDeque,
        pin::Pin,
        sync::Arc,
        task::{Context, Poll, Waker},
    };

    /// Invalid handle value
    pub const INVALID_HANDLE: u32 = 0xffff_ffff;

    /// The type of a handle
    #[derive(Debug, PartialEq)]
    pub enum FidlHdlType {
        /// An invalid handle
        Invalid,
        /// A channel
        Channel,
        /// A socket
        Socket,
    }

    lazy_static::lazy_static! {
        static ref HANDLE_WAKEUPS: Mutex<Vec<Option<Waker>>> = Mutex::new(Vec::new());
    }

    /// Awaken a handle by index.
    ///
    /// Wakeup flow:
    ///   There are no wakeups issued unless hdl_need_read is called.
    ///   hdl_need_read arms the wakeup, and no wakeups will occur until that call is made.
    fn hdl_awaken(hdl: u32) {
        let mut w = HANDLE_WAKEUPS.lock();
        let i = hdl as usize;
        if i >= w.len() {
            return;
        }
        w[i].take().map(Waker::wake);
    }

    #[must_use]
    fn hdl_need_wakeup<R>(hdl: u32, cx: &mut Context<'_>) -> Poll<R> {
        let mut w = HANDLE_WAKEUPS.lock();
        let i = hdl as usize;
        while w.len() <= i {
            w.push(None);
        }
        w[i] = Some(cx.waker().clone());
        Poll::Pending
    }

    /// A borrowed reference to an underlying handle
    pub struct HandleRef<'a>(u32, std::marker::PhantomData<&'a u32>);

    /// A trait to get a reference to the underlying handle of an object.
    pub trait AsHandleRef {
        /// Get a reference to the handle.
        fn as_handle_ref(&self) -> HandleRef;

        /// Return true if this handle is invalid
        fn is_invalid(&self) -> bool {
            self.as_handle_ref().0 == INVALID_HANDLE
        }

        /// Interpret the reference as a raw handle (an integer type). Two distinct
        /// handles will have different raw values (so it can perhaps be used as a
        /// key in a data structure).
        fn raw_handle(&self) -> u32 {
            self.as_handle_ref().0
        }

        /// Non-fuchsia only: return the type of a handle
        fn handle_type(&self) -> FidlHdlType {
            if self.is_invalid() {
                FidlHdlType::Invalid
            } else {
                with_handle(self.as_handle_ref().0, |obj| match obj {
                    FidlHandle::LeftChannel(_, _) => FidlHdlType::Channel,
                    FidlHandle::RightChannel(_, _) => FidlHdlType::Channel,
                    FidlHandle::LeftStreamSocket(_, _) => FidlHdlType::Socket,
                    FidlHandle::RightStreamSocket(_, _) => FidlHdlType::Socket,
                    FidlHandle::LeftDatagramSocket(_, _) => FidlHdlType::Socket,
                    FidlHandle::RightDatagramSocket(_, _) => FidlHdlType::Socket,
                })
            }
        }

        /// Non-fuchsia only: return a reference to the other end of a handle
        fn related(&self) -> HandleRef {
            if self.is_invalid() {
                HandleRef(INVALID_HANDLE, std::marker::PhantomData)
            } else {
                with_handle(self.as_handle_ref().0, |obj| match obj {
                    FidlHandle::LeftChannel(_, x) => HandleRef(*x, std::marker::PhantomData),
                    FidlHandle::RightChannel(_, x) => HandleRef(*x, std::marker::PhantomData),
                    FidlHandle::LeftStreamSocket(_, x) => HandleRef(*x, std::marker::PhantomData),
                    FidlHandle::RightStreamSocket(_, x) => HandleRef(*x, std::marker::PhantomData),
                    FidlHandle::LeftDatagramSocket(_, x) => HandleRef(*x, std::marker::PhantomData),
                    FidlHandle::RightDatagramSocket(_, x) => {
                        HandleRef(*x, std::marker::PhantomData)
                    }
                })
            }
        }
    }

    impl AsHandleRef for HandleRef<'_> {
        fn as_handle_ref(&self) -> HandleRef {
            HandleRef(self.0, std::marker::PhantomData)
        }
    }

    /// A trait implemented by all handle-based types.
    pub trait HandleBased: AsHandleRef + From<Handle> + Into<Handle> {
        /// Creates an instance of this type from a handle.
        ///
        /// This is a convenience function which simply forwards to the `From` trait.
        fn from_handle(handle: Handle) -> Self {
            Self::from(handle)
        }

        /// Converts the value into its inner handle.
        ///
        /// This is a convenience function which simply forwards to the `Into` trait.
        fn into_handle(self) -> Handle {
            self.into()
        }
    }

    /// Representation of a handle-like object
    #[derive(PartialEq, Eq, Debug, Ord, PartialOrd, Hash)]
    pub struct Handle(u32);

    impl Drop for Handle {
        fn drop(&mut self) {
            hdl_close(self.0);
        }
    }

    impl AsHandleRef for Handle {
        fn as_handle_ref(&self) -> HandleRef {
            HandleRef(self.0, std::marker::PhantomData)
        }
    }

    impl HandleBased for Handle {}

    impl Handle {
        /// Return an invalid handle
        pub fn invalid() -> Handle {
            Handle(INVALID_HANDLE)
        }

        /// If a raw handle is obtained from some other source, this method converts
        /// it into a type-safe owned handle.
        pub unsafe fn from_raw(hdl: u32) -> Handle {
            Handle(hdl)
        }

        /// Take this handle and return a new handle (leaves this handle invalid)
        pub fn take(&mut self) -> Handle {
            let h = Handle(self.0);
            self.0 = INVALID_HANDLE;
            h
        }

        /// Take this handle and return a new raw handle (leaves this handle invalid)
        pub unsafe fn raw_take(&mut self) -> u32 {
            let h = self.0;
            self.0 = INVALID_HANDLE;
            h
        }
    }

    macro_rules! declare_unsupported_fidl_handle {
        ($name:ident) => {
            /// A Zircon-like $name
            #[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Hash)]
            pub struct $name;

            impl From<$crate::handle::Handle> for $name {
                fn from(_: $crate::handle::Handle) -> $name {
                    $name
                }
            }
            impl From<$name> for Handle {
                fn from(_: $name) -> $crate::handle::Handle {
                    $crate::handle::Handle::invalid()
                }
            }
            impl HandleBased for $name {}
            impl AsHandleRef for $name {
                fn as_handle_ref(&self) -> HandleRef {
                    HandleRef(INVALID_HANDLE, std::marker::PhantomData)
                }
            }
        };
    }

    macro_rules! declare_fidl_handle {
        ($name:ident) => {
            /// A Zircon-like $name
            #[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Hash)]
            pub struct $name(u32);

            impl From<$crate::handle::Handle> for $name {
                fn from(mut hdl: $crate::handle::Handle) -> $name {
                    let out = $name(hdl.0);
                    hdl.0 = INVALID_HANDLE;
                    out
                }
            }
            impl From<$name> for Handle {
                fn from(mut hdl: $name) -> $crate::handle::Handle {
                    let out = unsafe { $crate::handle::Handle::from_raw(hdl.0) };
                    hdl.0 = INVALID_HANDLE;
                    out
                }
            }
            impl HandleBased for $name {}
            impl AsHandleRef for $name {
                fn as_handle_ref(&self) -> HandleRef {
                    HandleRef(self.0, std::marker::PhantomData)
                }
            }

            impl Drop for $name {
                fn drop(&mut self) {
                    hdl_close(self.0);
                }
            }
        };
    }

    macro_rules! host_handle {
        ($x:tt, Everywhere) => {
            declare_fidl_handle! {$x}
        };
        ($x:tt, $availability:ident) => {
            declare_unsupported_fidl_handle! {$x}
        };
    }

    invoke_for_handle_types!(host_handle);

    impl Channel {
        /// Create a channel, resulting in a pair of `Channel` objects representing both
        /// sides of the channel. Messages written into one maybe read from the opposite.
        pub fn create() -> Result<(Channel, Channel), zx_status::Status> {
            let cs = Arc::new(Mutex::new(ChannelState {
                open: true,
                left: HalfChannelState::new(),
                right: HalfChannelState::new(),
            }));
            let mut h = HANDLES.lock();
            let left = h.insert(FidlHandle::LeftChannel(cs.clone(), INVALID_HANDLE)) as u32;
            let right = h.insert(FidlHandle::RightChannel(cs, left)) as u32;
            if let FidlHandle::LeftChannel(_, peer) = &mut h[left as usize] {
                *peer = right;
            } else {
                unreachable!();
            }
            Ok((Channel(left), Channel(right)))
        }

        /// Read a message from a channel.
        pub fn read(&self, buf: &mut MessageBuf) -> Result<(), zx_status::Status> {
            let (bytes, handles) = buf.split_mut();
            self.read_split(bytes, handles)
        }

        /// Read a message from a channel into a separate byte vector and handle vector.
        pub fn read_split(
            &self,
            bytes: &mut Vec<u8>,
            handles: &mut Vec<Handle>,
        ) -> Result<(), zx_status::Status> {
            with_handle(self.0, |obj| match obj {
                FidlHandle::LeftChannel(cs, _) => {
                    let ChannelState { open, left: ref mut st, .. } = *cs.lock();
                    Self::dequeue_read(open, st, bytes, handles)
                }
                FidlHandle::RightChannel(cs, _) => {
                    let ChannelState { open, right: ref mut st, .. } = *cs.lock();
                    Self::dequeue_read(open, st, bytes, handles)
                }
                _ => panic!("Non channel passed to Channel::read_split"),
            })
        }

        fn dequeue_read(
            open: bool,
            st: &mut HalfChannelState,
            bytes: &mut Vec<u8>,
            handles: &mut Vec<Handle>,
        ) -> Result<(), zx_status::Status> {
            if let Some(mut msg) = st.messages.pop_front() {
                std::mem::swap(bytes, &mut msg.bytes);
                std::mem::swap(handles, &mut msg.handles);
                Ok(())
            } else if open {
                Err(zx_status::Status::SHOULD_WAIT)
            } else {
                Err(zx_status::Status::PEER_CLOSED)
            }
        }

        /// Write a message to a channel.
        pub fn write(
            &self,
            bytes: &[u8],
            handles: &mut Vec<Handle>,
        ) -> Result<(), zx_status::Status> {
            let wakeup = with_handle(self.0, |obj| match obj {
                FidlHandle::LeftChannel(cs, peer) => match *cs.lock() {
                    ChannelState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    ChannelState { right: ref mut st, .. } => {
                        Self::enqueue_write(st, *peer, bytes, handles)
                    }
                },
                FidlHandle::RightChannel(cs, peer) => match *cs.lock() {
                    ChannelState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    ChannelState { left: ref mut st, .. } => {
                        Self::enqueue_write(st, *peer, bytes, handles)
                    }
                },
                _ => panic!("Non channel passed to Channel::write"),
            })?;
            if wakeup != INVALID_HANDLE {
                hdl_awaken(wakeup);
            }
            Ok(())
        }

        fn enqueue_write(
            st: &mut HalfChannelState,
            peer: u32,
            bytes: &[u8],
            handles: &mut Vec<Handle>,
        ) -> Result<u32, zx_status::Status> {
            let mut b = Vec::new();
            b.extend_from_slice(bytes);
            st.messages.push_back(ChannelMessage {
                bytes: b,
                handles: std::mem::replace(handles, Vec::new()),
            });
            Ok(peer)
        }
    }

    /// An I/O object representing a `Channel`.
    #[derive(Debug)]
    pub struct AsyncChannel {
        channel: Channel,
    }

    impl AsyncChannel {
        /// Writes a message into the channel.
        pub fn write(
            &self,
            bytes: &[u8],
            handles: &mut Vec<Handle>,
        ) -> Result<(), zx_status::Status> {
            self.channel.write(bytes, handles)
        }

        /// Consumes self and returns the underlying Channel (named thusly for compatibility with
        /// fasync variant)
        pub fn into_zx_channel(self) -> Channel {
            self.channel
        }

        /// Receives a message on the channel and registers this `Channel` as
        /// needing a read on receiving a `io::std::ErrorKind::WouldBlock`.
        ///
        /// Identical to `recv_from` except takes separate bytes and handles buffers
        /// rather than a single `MessageBuf`.
        pub fn read(
            &self,
            cx: &mut Context<'_>,
            bytes: &mut Vec<u8>,
            handles: &mut Vec<Handle>,
        ) -> Poll<Result<(), zx_status::Status>> {
            match self.channel.read_split(bytes, handles) {
                Err(zx_status::Status::SHOULD_WAIT) => hdl_need_wakeup(self.channel.0, cx),
                x => Poll::Ready(x),
            }
        }

        /// Receives a message on the channel and registers this `Channel` as
        /// needing a read on receiving a `io::std::ErrorKind::WouldBlock`.
        pub fn recv_from(
            &self,
            ctx: &mut Context<'_>,
            buf: &mut MessageBuf,
        ) -> Poll<Result<(), zx_status::Status>> {
            let (bytes, handles) = buf.split_mut();
            self.read(ctx, bytes, handles)
        }

        /// Creates a future that receive a message to be written to the buffer
        /// provided.
        ///
        /// The returned future will return after a message has been received on
        /// this socket and been placed into the buffer.
        pub fn recv_msg<'a>(&'a self, buf: &'a mut MessageBuf) -> RecvMsg<'a> {
            RecvMsg { channel: self, buf }
        }

        /// Creates a new `AsyncChannel` from a previously-created `Channel`.
        pub fn from_channel(channel: Channel) -> std::io::Result<AsyncChannel> {
            Ok(AsyncChannel { channel })
        }
    }

    /// A future used to receive a message from a channel.
    ///
    /// This is created by the `Channel::recv_msg` method.
    #[must_use = "futures do nothing unless polled"]
    pub struct RecvMsg<'a> {
        channel: &'a AsyncChannel,
        buf: &'a mut MessageBuf,
    }

    impl<'a> futures::Future for RecvMsg<'a> {
        type Output = Result<(), zx_status::Status>;

        fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
            let this = &mut *self;
            this.channel.recv_from(cx, this.buf)
        }
    }

    /// Socket options available portable
    #[derive(Clone, Copy)]
    pub enum SocketOpts {
        /// A bytestream style socket
        STREAM,
        /// A datagram style socket
        DATAGRAM,
    }

    impl Socket {
        /// Create a pair of sockets
        pub fn create(sock_opts: SocketOpts) -> Result<(Socket, Socket), zx_status::Status> {
            match sock_opts {
                SocketOpts::STREAM => {
                    let cs = Arc::new(Mutex::new(StreamSocketState {
                        open: true,
                        left: HalfStreamSocketState::new(),
                        right: HalfStreamSocketState::new(),
                    }));
                    let mut h = HANDLES.lock();
                    let left =
                        h.insert(FidlHandle::LeftStreamSocket(cs.clone(), INVALID_HANDLE)) as u32;
                    let right = h.insert(FidlHandle::RightStreamSocket(cs, left)) as u32;
                    if let FidlHandle::LeftStreamSocket(_, peer) = &mut h[left as usize] {
                        *peer = right;
                    } else {
                        unreachable!();
                    }
                    Ok((Socket(left), Socket(right)))
                }
                SocketOpts::DATAGRAM => {
                    let cs = Arc::new(Mutex::new(DatagramSocketState {
                        open: true,
                        left: HalfDatagramSocketState::new(),
                        right: HalfDatagramSocketState::new(),
                    }));
                    let mut h = HANDLES.lock();
                    let left =
                        h.insert(FidlHandle::LeftDatagramSocket(cs.clone(), INVALID_HANDLE)) as u32;
                    let right = h.insert(FidlHandle::RightDatagramSocket(cs, left)) as u32;
                    if let FidlHandle::LeftDatagramSocket(_, peer) = &mut h[left as usize] {
                        *peer = right;
                    } else {
                        unreachable!();
                    }
                    Ok((Socket(left), Socket(right)))
                }
            }
        }

        /// Write the given bytes into the socket.
        /// Return value (on success) is number of bytes actually written.
        pub fn write(&self, bytes: &[u8]) -> Result<usize, zx_status::Status> {
            let (result, wakeup) = with_handle(self.0, |obj| match obj {
                FidlHandle::LeftStreamSocket(cs, peer) => match *cs.lock() {
                    StreamSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    StreamSocketState { right: ref mut st, .. } => {
                        Self::enqueue_stream_write(st, *peer, bytes)
                    }
                },
                FidlHandle::RightStreamSocket(cs, peer) => match *cs.lock() {
                    StreamSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    StreamSocketState { left: ref mut st, .. } => {
                        Self::enqueue_stream_write(st, *peer, bytes)
                    }
                },
                FidlHandle::LeftDatagramSocket(cs, peer) => match *cs.lock() {
                    DatagramSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    DatagramSocketState { right: ref mut st, .. } => {
                        Self::enqueue_datagram_write(st, *peer, bytes)
                    }
                },
                FidlHandle::RightDatagramSocket(cs, peer) => match *cs.lock() {
                    DatagramSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    DatagramSocketState { left: ref mut st, .. } => {
                        Self::enqueue_datagram_write(st, *peer, bytes)
                    }
                },
                _ => panic!("Non socket passed to Socket::write"),
            })?;
            if wakeup != INVALID_HANDLE {
                hdl_awaken(wakeup);
            }
            Ok(result)
        }

        fn enqueue_stream_write(
            st: &mut HalfStreamSocketState,
            peer: u32,
            bytes: &[u8],
        ) -> Result<(usize, u32), zx_status::Status> {
            if bytes.len() > 0 {
                st.bytes.extend(bytes);
                Ok((bytes.len(), peer))
            } else {
                Ok((bytes.len(), INVALID_HANDLE))
            }
        }

        fn enqueue_datagram_write(
            st: &mut HalfDatagramSocketState,
            peer: u32,
            bytes: &[u8],
        ) -> Result<(usize, u32), zx_status::Status> {
            st.bytes.push_back(bytes.to_vec());
            Ok((bytes.len(), peer))
        }

        /// Return how many bytes are buffered in the socket
        pub fn outstanding_read_bytes(&self) -> Result<usize, zx_status::Status> {
            with_handle(self.0, |obj| match obj {
                FidlHandle::LeftStreamSocket(cs, _) => match *cs.lock() {
                    StreamSocketState { left: ref mut st, .. } if st.bytes.len() > 0 => {
                        Ok(st.bytes.len())
                    }
                    StreamSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    StreamSocketState { .. } => Ok(0),
                },
                FidlHandle::RightStreamSocket(cs, _) => match *cs.lock() {
                    StreamSocketState { right: ref mut st, .. } if st.bytes.len() > 0 => {
                        Ok(st.bytes.len())
                    }
                    StreamSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    StreamSocketState { .. } => Ok(0),
                },
                FidlHandle::LeftDatagramSocket(cs, _) => match *cs.lock() {
                    DatagramSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    DatagramSocketState { left: ref mut st, .. } => {
                        Ok(st.bytes.front().map(|frame| frame.len()).unwrap_or(0))
                    }
                },
                FidlHandle::RightDatagramSocket(cs, _) => match *cs.lock() {
                    DatagramSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    DatagramSocketState { right: ref mut st, .. } => {
                        Ok(st.bytes.front().map(|frame| frame.len()).unwrap_or(0))
                    }
                },
                _ => panic!("Non socket passed to Socket::read"),
            })
        }

        /// Read bytes from the socket.
        /// Return value (on success) is number of bytes actually read.
        pub fn read(&self, bytes: &mut [u8]) -> Result<usize, zx_status::Status> {
            with_handle(self.0, |obj| match obj {
                FidlHandle::LeftStreamSocket(cs, _) => match *cs.lock() {
                    StreamSocketState { open, left: ref mut st, .. } => {
                        Self::dequeue_stream_read(open, st, bytes)
                    }
                },
                FidlHandle::RightStreamSocket(cs, _) => match *cs.lock() {
                    StreamSocketState { open, right: ref mut st, .. } => {
                        Self::dequeue_stream_read(open, st, bytes)
                    }
                },
                FidlHandle::LeftDatagramSocket(cs, _) => match *cs.lock() {
                    DatagramSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    DatagramSocketState { left: ref mut st, .. } => {
                        Self::dequeue_datagram_read(st, bytes)
                    }
                },
                FidlHandle::RightDatagramSocket(cs, _) => match *cs.lock() {
                    DatagramSocketState { open: false, .. } => Err(zx_status::Status::PEER_CLOSED),
                    DatagramSocketState { right: ref mut st, .. } => {
                        Self::dequeue_datagram_read(st, bytes)
                    }
                },
                _ => panic!("Non socket passed to Socket::read"),
            })
        }

        fn dequeue_stream_read(
            open: bool,
            st: &mut HalfStreamSocketState,
            bytes: &mut [u8],
        ) -> Result<usize, zx_status::Status> {
            if bytes.len() == 0 {
                if !open {
                    return Err(zx_status::Status::PEER_CLOSED);
                }
                return Ok(0);
            }
            let copy_bytes = std::cmp::min(bytes.len(), st.bytes.len());
            if copy_bytes == 0 {
                return Err(zx_status::Status::SHOULD_WAIT);
            }
            for (i, b) in st.bytes.drain(..copy_bytes).enumerate() {
                bytes[i] = b;
            }
            Ok(copy_bytes)
        }

        fn dequeue_datagram_read(
            st: &mut HalfDatagramSocketState,
            bytes: &mut [u8],
        ) -> Result<usize, zx_status::Status> {
            if let Some(frame) = st.bytes.pop_front() {
                let n = std::cmp::min(bytes.len(), frame.len());
                bytes[..n].clone_from_slice(&frame[..n]);
                Ok(n)
            } else {
                Err(zx_status::Status::SHOULD_WAIT)
            }
        }
    }

    /// An I/O object representing a `Socket`.
    #[derive(Debug)]
    pub struct AsyncSocket {
        socket: Socket,
    }

    impl AsyncSocket {
        /// Construct an `AsyncSocket` from an existing `Socket`
        pub fn from_socket(socket: Socket) -> std::io::Result<AsyncSocket> {
            Ok(AsyncSocket { socket })
        }

        /// Convert AsyncSocket back into a regular socket
        pub fn into_zx_socket(self) -> Result<Socket, AsyncSocket> {
            Ok(self.socket)
        }

        /// Polls for the next data on the socket, appending it to the end of |out| if it has arrived.
        /// Not very useful for a non-datagram socket as it will return all available data
        /// on the socket.
        pub fn poll_datagram(
            &self,
            cx: &mut Context<'_>,
            out: &mut Vec<u8>,
        ) -> Poll<Result<usize, zx_status::Status>> {
            let avail = self.socket.outstanding_read_bytes()?;
            let len = out.len();
            out.resize(len + avail, 0);
            let (_, mut tail) = out.split_at_mut(len);
            match self.socket.read(&mut tail) {
                Err(zx_status::Status::SHOULD_WAIT) => hdl_need_wakeup(self.socket.0, cx),
                Err(zx_status::Status::PEER_CLOSED) => Poll::Ready(Ok(0)),
                Err(e) => Poll::Ready(Err(e)),
                Ok(bytes) => {
                    if bytes == avail {
                        Poll::Ready(Ok(bytes))
                    } else {
                        Poll::Ready(Err(zx_status::Status::BAD_STATE))
                    }
                }
            }
        }

        /// Reads the next datagram that becomes available onto the end of |out|.  Note: Using this
        /// multiple times concurrently is an error and the first one will never complete.
        pub async fn read_datagram<'a>(
            &'a self,
            out: &'a mut Vec<u8>,
        ) -> Result<usize, zx_status::Status> {
            poll_fn(move |cx| self.poll_datagram(cx, out)).await
        }
    }

    impl futures::io::AsyncWrite for AsyncSocket {
        fn poll_write(
            self: Pin<&mut Self>,
            _cx: &mut std::task::Context<'_>,
            bytes: &[u8],
        ) -> Poll<Result<usize, std::io::Error>> {
            Poll::Ready(self.socket.write(bytes).map_err(|e| e.into()))
        }

        fn poll_flush(
            self: Pin<&mut Self>,
            _cx: &mut std::task::Context<'_>,
        ) -> Poll<Result<(), std::io::Error>> {
            Poll::Ready(Ok(()))
        }

        fn poll_close(
            mut self: Pin<&mut Self>,
            _cx: &mut std::task::Context<'_>,
        ) -> Poll<Result<(), std::io::Error>> {
            self.borrow_mut().socket = Socket::from_handle(Handle::invalid());
            Poll::Ready(Ok(()))
        }
    }

    impl futures::io::AsyncRead for AsyncSocket {
        fn poll_read(
            self: Pin<&mut Self>,
            cx: &mut std::task::Context<'_>,
            bytes: &mut [u8],
        ) -> Poll<Result<usize, std::io::Error>> {
            match self.socket.read(bytes) {
                Err(zx_status::Status::SHOULD_WAIT) => hdl_need_wakeup(self.socket.0, cx),
                Err(zx_status::Status::PEER_CLOSED) => Poll::Ready(Ok(0)),
                Ok(x) => Poll::Ready(Ok(x)),
                Err(x) => Poll::Ready(Err(x.into())),
            }
        }
    }

    /// A buffer for _receiving_ messages from a channel.
    ///
    /// A `MessageBuf` is essentially a byte buffer and a vector of
    /// handles, but move semantics for "taking" handles requires special handling.
    ///
    /// Note that for sending messages to a channel, the caller manages the buffers,
    /// using a plain byte slice and `Vec<Handle>`.
    #[derive(Debug, Default)]
    pub struct MessageBuf {
        bytes: Vec<u8>,
        handles: Vec<Handle>,
    }

    impl MessageBuf {
        /// Create a new, empty, message buffer.
        pub fn new() -> Self {
            Default::default()
        }

        /// Create a new non-empty message buffer.
        pub fn new_with(v: Vec<u8>, h: Vec<Handle>) -> Self {
            Self { bytes: v, handles: h }
        }

        /// Splits apart the message buf into a vector of bytes and a vector of handles.
        pub fn split_mut(&mut self) -> (&mut Vec<u8>, &mut Vec<Handle>) {
            (&mut self.bytes, &mut self.handles)
        }

        /// Splits apart the message buf into a vector of bytes and a vector of handles.
        pub fn split(self) -> (Vec<u8>, Vec<Handle>) {
            (self.bytes, self.handles)
        }

        /// Ensure that the buffer has the capacity to hold at least `n_bytes` bytes.
        pub fn ensure_capacity_bytes(&mut self, n_bytes: usize) {
            ensure_capacity(&mut self.bytes, n_bytes);
        }

        /// Ensure that the buffer has the capacity to hold at least `n_handles` handles.
        pub fn ensure_capacity_handles(&mut self, n_handles: usize) {
            ensure_capacity(&mut self.handles, n_handles);
        }

        /// Ensure that at least n_bytes bytes are initialized (0 fill).
        pub fn ensure_initialized_bytes(&mut self, n_bytes: usize) {
            if n_bytes <= self.bytes.len() {
                return;
            }
            self.bytes.resize(n_bytes, 0);
        }

        /// Get a reference to the bytes of the message buffer, as a `&[u8]` slice.
        pub fn bytes(&self) -> &[u8] {
            self.bytes.as_slice()
        }

        /// The number of handles in the message buffer. Note this counts the number
        /// available when the message was received; `take_handle` does not affect
        /// the count.
        pub fn n_handles(&self) -> usize {
            self.handles.len()
        }

        /// Take the handle at the specified index from the message buffer. If the
        /// method is called again with the same index, it will return `None`, as
        /// will happen if the index exceeds the number of handles available.
        pub fn take_handle(&mut self, index: usize) -> Option<Handle> {
            self.handles.get_mut(index).and_then(|handle| {
                if handle.is_invalid() {
                    None
                } else {
                    Some(std::mem::replace(handle, Handle::invalid()))
                }
            })
        }

        /// Clear the bytes and handles contained in the buf. This will drop any
        /// contained handles, resulting in their resources being freed.
        pub fn clear(&mut self) {
            self.bytes.clear();
            self.handles.clear();
        }
    }

    fn ensure_capacity<T>(vec: &mut Vec<T>, size: usize) {
        let len = vec.len();
        if size > len {
            vec.reserve(size - len);
        }
    }

    struct ChannelMessage {
        bytes: Vec<u8>,
        handles: Vec<Handle>,
    }

    struct HalfChannelState {
        messages: VecDeque<ChannelMessage>,
    }

    impl HalfChannelState {
        fn new() -> HalfChannelState {
            HalfChannelState { messages: VecDeque::new() }
        }
    }

    struct ChannelState {
        open: bool,
        left: HalfChannelState,
        right: HalfChannelState,
    }

    struct HalfStreamSocketState {
        bytes: VecDeque<u8>,
    }

    impl HalfStreamSocketState {
        fn new() -> HalfStreamSocketState {
            HalfStreamSocketState { bytes: VecDeque::new() }
        }
    }

    struct HalfDatagramSocketState {
        bytes: VecDeque<Vec<u8>>,
    }

    impl HalfDatagramSocketState {
        fn new() -> HalfDatagramSocketState {
            HalfDatagramSocketState { bytes: VecDeque::new() }
        }
    }

    struct StreamSocketState {
        open: bool,
        left: HalfStreamSocketState,
        right: HalfStreamSocketState,
    }

    struct DatagramSocketState {
        open: bool,
        left: HalfDatagramSocketState,
        right: HalfDatagramSocketState,
    }

    enum FidlHandle {
        LeftChannel(Arc<Mutex<ChannelState>>, u32),
        RightChannel(Arc<Mutex<ChannelState>>, u32),
        LeftStreamSocket(Arc<Mutex<StreamSocketState>>, u32),
        RightStreamSocket(Arc<Mutex<StreamSocketState>>, u32),
        LeftDatagramSocket(Arc<Mutex<DatagramSocketState>>, u32),
        RightDatagramSocket(Arc<Mutex<DatagramSocketState>>, u32),
    }

    lazy_static::lazy_static! {
        static ref HANDLES: Mutex<Slab<FidlHandle>> = Mutex::new(Slab::new());
    }

    fn with_handle<R>(hdl: u32, f: impl FnOnce(&mut FidlHandle) -> R) -> R {
        f(&mut HANDLES.lock()[hdl as usize])
    }

    /// Close the handle: no action if hdl==INVALID_HANDLE
    fn hdl_close(hdl: u32) {
        if hdl == INVALID_HANDLE {
            return;
        }
        let wakeup = match HANDLES.lock().remove(hdl as usize) {
            FidlHandle::LeftChannel(cs, peer) => {
                let st = &mut *cs.lock();
                let wakeup = st.open;
                st.open = false;
                if wakeup {
                    peer
                } else {
                    INVALID_HANDLE
                }
            }
            FidlHandle::RightChannel(cs, peer) => {
                let st = &mut *cs.lock();
                let wakeup = st.open;
                st.open = false;
                if wakeup {
                    peer
                } else {
                    INVALID_HANDLE
                }
            }
            FidlHandle::LeftStreamSocket(cs, peer) => {
                let st = &mut *cs.lock();
                let wakeup = st.open;
                st.open = false;
                if wakeup {
                    peer
                } else {
                    INVALID_HANDLE
                }
            }
            FidlHandle::RightStreamSocket(cs, peer) => {
                let st = &mut *cs.lock();
                let wakeup = st.open;
                st.open = false;
                if wakeup {
                    peer
                } else {
                    INVALID_HANDLE
                }
            }
            FidlHandle::LeftDatagramSocket(cs, peer) => {
                let st = &mut *cs.lock();
                let wakeup = st.open;
                st.open = false;
                if wakeup {
                    peer
                } else {
                    INVALID_HANDLE
                }
            }
            FidlHandle::RightDatagramSocket(cs, peer) => {
                let st = &mut *cs.lock();
                let wakeup = st.open;
                st.open = false;
                if wakeup {
                    peer
                } else {
                    INVALID_HANDLE
                }
            }
        };
        if wakeup != INVALID_HANDLE {
            hdl_awaken(wakeup);
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use fuchsia_zircon_status as zx_status;
    use futures::io::{AsyncReadExt, AsyncWriteExt};
    use futures::task::{noop_waker, Context};
    use futures::Future;
    use std::pin::Pin;
    use zx_status::Status;

    #[cfg(target_os = "fuchsia")]
    use fuchsia_async as fasync;

    #[cfg(not(target_os = "fuchsia"))]
    use futures::executor::block_on;

    #[cfg(target_os = "fuchsia")]
    fn run_async_test(f: impl Future<Output = ()>) {
        fasync::Executor::new().unwrap().run_singlethreaded(f);
    }

    #[cfg(not(target_os = "fuchsia"))]
    fn run_async_test(f: impl Future<Output = ()>) {
        block_on(f);
    }

    #[test]
    fn channel_write_read() {
        let (a, b) = Channel::create().unwrap();
        let (c, d) = Channel::create().unwrap();
        let mut incoming = MessageBuf::new();

        assert_eq!(b.read(&mut incoming).err().unwrap(), Status::SHOULD_WAIT);
        d.write(&[4, 5, 6], &mut vec![]).unwrap();
        a.write(&[1, 2, 3], &mut vec![c.into(), d.into()]).unwrap();

        b.read(&mut incoming).unwrap();
        assert_eq!(incoming.bytes(), &[1, 2, 3]);
        assert_eq!(incoming.n_handles(), 2);
        let c: Channel = incoming.take_handle(0).unwrap().into();
        let d: Channel = incoming.take_handle(1).unwrap().into();
        c.read(&mut incoming).unwrap();
        drop(d);
        assert_eq!(incoming.bytes(), &[4, 5, 6]);
        assert_eq!(incoming.n_handles(), 0);
    }

    #[test]
    fn socket_write_read() {
        let (a, b) = Socket::create(SocketOpts::STREAM).unwrap();
        a.write(&[1, 2, 3]).unwrap();
        let mut buf = [0u8; 128];
        assert_eq!(b.read(&mut buf).unwrap(), 3);
        assert_eq!(&buf[0..3], &[1, 2, 3]);
    }

    #[test]
    fn async_channel_write_read() {
        run_async_test(async move {
            let (a, b) = Channel::create().unwrap();
            let (a, b) =
                (AsyncChannel::from_channel(a).unwrap(), AsyncChannel::from_channel(b).unwrap());
            let mut buf = MessageBuf::new();

            let waker = noop_waker();
            let mut cx = Context::from_waker(&waker);

            let mut rx = b.recv_msg(&mut buf);
            assert_eq!(Pin::new(&mut rx).poll(&mut cx), std::task::Poll::Pending);
            a.write(&[1, 2, 3], &mut vec![]).unwrap();
            rx.await.unwrap();
            assert_eq!(buf.bytes(), &[1, 2, 3]);

            let mut rx = a.recv_msg(&mut buf);
            assert!(Pin::new(&mut rx).poll(&mut cx).is_pending());
            b.write(&[1, 2, 3], &mut vec![]).unwrap();
            rx.await.unwrap();
            assert_eq!(buf.bytes(), &[1, 2, 3]);
        })
    }

    #[test]
    fn async_socket_write_read() {
        run_async_test(async move {
            let (a, b) = Socket::create(SocketOpts::STREAM).unwrap();
            let (mut a, mut b) =
                (AsyncSocket::from_socket(a).unwrap(), AsyncSocket::from_socket(b).unwrap());
            let mut buf = [0u8; 128];

            let waker = noop_waker();
            let mut cx = Context::from_waker(&waker);

            let mut rx = b.read(&mut buf);
            assert!(Pin::new(&mut rx).poll(&mut cx).is_pending());
            assert!(Pin::new(&mut a.write(&[1, 2, 3])).poll(&mut cx).is_ready());
            rx.await.unwrap();
            assert_eq!(&buf[0..3], &[1, 2, 3]);

            let mut rx = a.read(&mut buf);
            assert!(Pin::new(&mut rx).poll(&mut cx).is_pending());
            assert!(Pin::new(&mut b.write(&[1, 2, 3])).poll(&mut cx).is_ready());
            rx.await.unwrap();
            assert_eq!(&buf[0..3], &[1, 2, 3]);
        })
    }

    #[test]
    fn channel_basic() {
        let (p1, p2) = Channel::create().unwrap();

        let mut empty = vec![];
        assert!(p1.write(b"hello", &mut empty).is_ok());

        let mut buf = MessageBuf::new();
        assert!(p2.read(&mut buf).is_ok());
        assert_eq!(buf.bytes(), b"hello");
    }

    #[test]
    fn channel_send_handle() {
        let hello_length: usize = 5;

        // Create a pair of channels and a pair of sockets.
        let (p1, p2) = Channel::create().unwrap();
        let (s1, s2) = Socket::create(SocketOpts::STREAM).unwrap();

        // Send one socket down the channel
        let mut handles_to_send: Vec<Handle> = vec![s1.into_handle()];
        assert!(p1.write(b"", &mut handles_to_send).is_ok());
        // Handle should be removed from vector.
        assert!(handles_to_send.is_empty());

        // Read the handle from the receiving channel.
        let mut buf = MessageBuf::new();
        assert!(p2.read(&mut buf).is_ok());
        assert_eq!(buf.n_handles(), 1);
        // Take the handle from the buffer.
        let received_handle = buf.take_handle(0).unwrap();
        // Should not affect number of handles.
        assert_eq!(buf.n_handles(), 1);
        // Trying to take it again should fail.
        assert!(buf.take_handle(0).is_none());

        // Now to test that we got the right handle, try writing something to it...
        let received_socket = Socket::from(received_handle);
        assert!(received_socket.write(b"hello").is_ok());

        // ... and reading it back from the original VMO.
        let mut read_vec = vec![0; hello_length];
        assert!(s2.read(&mut read_vec).is_ok());
        assert_eq!(read_vec, b"hello");
    }

    #[test]
    fn socket_basic() {
        let (s1, s2) = Socket::create(SocketOpts::STREAM).unwrap();

        // Write two packets and read from other end
        assert_eq!(s1.write(b"hello").unwrap(), 5);
        assert_eq!(s1.write(b"world").unwrap(), 5);

        let mut read_vec = vec![0; 11];
        assert_eq!(s2.read(&mut read_vec).unwrap(), 10);
        assert_eq!(&read_vec[0..10], b"helloworld");

        // Try reading when there is nothing to read.
        assert_eq!(s2.read(&mut read_vec), Err(Status::SHOULD_WAIT));
    }

    #[cfg(not(target_os = "fuchsia"))]
    #[test]
    fn handle_type_is_correct() {
        let (c1, c2) = Channel::create().unwrap();
        let (s1, s2) = Socket::create(SocketOpts::STREAM).unwrap();
        assert_eq!(c1.into_handle().handle_type(), FidlHdlType::Channel);
        assert_eq!(c2.into_handle().handle_type(), FidlHdlType::Channel);
        assert_eq!(s1.into_handle().handle_type(), FidlHdlType::Socket);
        assert_eq!(s2.into_handle().handle_type(), FidlHdlType::Socket);
    }
}
