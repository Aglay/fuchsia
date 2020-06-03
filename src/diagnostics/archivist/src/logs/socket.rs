// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

use super::error::StreamError;
use super::message::Message;
use fidl_fuchsia_sys_internal::SourceIdentity;
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::{
    io, ready,
    task::{Context, Poll},
    Stream,
};
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::Arc;

/// An `Encoding` is able to parse a `Message` from raw bytes.
pub trait Encoding {
    /// Attempt to parse a message from the given buffer
    fn parse_message(buf: &[u8]) -> Result<Message, StreamError>;
}

/// An encoding that can parse the legacy [logger/syslog wire format]
///
/// [logger/syslog wire format]: https://fuchsia.googlesource.com/fuchsia/+/master/zircon/system/ulib/syslog/include/lib/syslog/wire_format.h
pub struct LegacyEncoding;
/// An encoding that can parse the [structured log format]
///
/// [structured log format]: https://fuchsia.dev/fuchsia-src/development/logs/encodings
pub struct StructuredEncoding;

impl Encoding for LegacyEncoding {
    fn parse_message(buf: &[u8]) -> Result<Message, StreamError> {
        Message::from_logger(buf)
    }
}

impl Encoding for StructuredEncoding {
    fn parse_message(buf: &[u8]) -> Result<Message, StreamError> {
        Message::from_structured(buf)
    }
}

#[must_use = "don't drop logs on the floor please!"]
pub struct LogMessageSocket<E> {
    source: Arc<SourceIdentity>,
    socket: fasync::Socket,
    buffer: Vec<u8>,
    _encoder: PhantomData<E>,
}

impl<E> LogMessageSocket<E> {
    /// Description of the source of the items.
    pub fn source(&self) -> &SourceIdentity {
        &self.source
    }
}

impl LogMessageSocket<LegacyEncoding> {
    /// Creates a new `LogMessageSocket` from the given `socket` that reads the legacy format.
    pub fn new(socket: zx::Socket, source: Arc<SourceIdentity>) -> Result<Self, io::Error> {
        Ok(Self {
            socket: fasync::Socket::from_socket(socket)?,
            buffer: Vec::new(),
            source,
            _encoder: PhantomData,
        })
    }
}

impl LogMessageSocket<StructuredEncoding> {
    /// Creates a new `LogMessageSocket` from the given `socket` that reads the structured log
    /// format.
    pub fn new_structured(
        socket: zx::Socket,
        source: Arc<SourceIdentity>,
    ) -> Result<Self, io::Error> {
        Ok(Self {
            socket: fasync::Socket::from_socket(socket)?,
            buffer: Vec::new(),
            source,
            _encoder: PhantomData,
        })
    }
}

impl<E> Stream for LogMessageSocket<E>
where
    E: Encoding + Unpin,
{
    type Item = Result<Message, StreamError>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let &mut Self { ref mut socket, ref mut buffer, .. } = &mut *self;
        Poll::Ready(match ready!(socket.poll_datagram(cx, buffer)) {
            Ok(_) => {
                let res = E::parse_message(buffer.as_slice());
                buffer.clear();
                Some(res)
            }
            Err(zx::Status::PEER_CLOSED) => None,
            Err(e) => Some(Err(StreamError::Io { source: e.into_io_error() })),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::message::{
        fx_log_packet_t, LogHierarchy, LogProperty, Message, MessageLabel, Severity, METADATA_SIZE,
    };
    use super::*;
    use diagnostic_streams::{
        encode::Encoder, Argument, Record, Severity as StreamSeverity, Value,
    };
    use futures::stream::TryStreamExt;
    use std::io::Cursor;

    #[fasync::run_until_stalled(test)]
    async fn logger_stream_test() {
        let (sin, sout) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();
        let mut packet: fx_log_packet_t = Default::default();
        packet.metadata.pid = 1;
        packet.metadata.severity = 0x30; // INFO
        packet.data[0] = 5;
        packet.fill_data(1..6, 'A' as _);
        packet.fill_data(7..12, 'B' as _);

        let mut ls = LogMessageSocket::new(sout, Arc::new(SourceIdentity::empty())).unwrap();
        sin.write(packet.as_bytes()).unwrap();
        let mut expected_p = Message {
            size: METADATA_SIZE + 6 /* tag */+ 6, /* msg */
            time: zx::Time::from_nanos(packet.metadata.time),
            severity: Severity::Info,
            contents: LogHierarchy::new(
                "root",
                vec![
                    LogProperty::Uint(MessageLabel::ProcessId, packet.metadata.pid),
                    LogProperty::Uint(MessageLabel::ThreadId, packet.metadata.tid),
                    LogProperty::Uint(MessageLabel::Dropped, packet.metadata.dropped_logs as _),
                    LogProperty::string(MessageLabel::Tag, "AAAAA"),
                    LogProperty::string(MessageLabel::Msg, "BBBBB"),
                ],
                vec![],
            ),
        };
        expected_p.contents.sort();

        let mut result_message = ls.try_next().await.unwrap().unwrap();
        result_message.contents.sort();
        assert_eq!(result_message, expected_p);

        // write one more time
        sin.write(packet.as_bytes()).unwrap();

        let mut result_message = ls.try_next().await.unwrap().unwrap();
        result_message.contents.sort();
        assert_eq!(result_message, expected_p);
    }

    #[fasync::run_until_stalled(test)]
    async fn structured_logger_stream_test() {
        let (sin, sout) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();
        let record = Record {
            timestamp: 107,
            severity: StreamSeverity::Fatal,
            arguments: vec![
                Argument { name: "key".to_string(), value: Value::Text("value".to_string()) },
                Argument { name: "__tag".to_string(), value: Value::Text("tag-a".to_string()) },
            ],
        };
        let mut buffer = Cursor::new(vec![0u8; 1024]);
        let mut encoder = Encoder::new(&mut buffer);
        encoder.write_record(&record).unwrap();
        let encoded = &buffer.get_ref()[..buffer.position() as usize];

        let mut expected_p = Message {
            size: encoded.len(),
            time: zx::Time::from_nanos(107),
            severity: Severity::Fatal,
            contents: LogHierarchy::new(
                "root",
                vec![
                    LogProperty::string(MessageLabel::Other("key".to_string()), "value"),
                    LogProperty::string(MessageLabel::Tag, "tag-a"),
                ],
                vec![],
            ),
        };
        expected_p.contents.sort();

        let mut stream =
            LogMessageSocket::new_structured(sout, Arc::new(SourceIdentity::empty())).unwrap();

        sin.write(encoded).unwrap();
        let mut result_message = stream.try_next().await.unwrap().unwrap();
        result_message.contents.sort();
        assert_eq!(result_message, expected_p);

        // write again
        sin.write(encoded).unwrap();
        let mut result_message = stream.try_next().await.unwrap().unwrap();
        result_message.contents.sort();
        assert_eq!(result_message, expected_p);
    }
}
