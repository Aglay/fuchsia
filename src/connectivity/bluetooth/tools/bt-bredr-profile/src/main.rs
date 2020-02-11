// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context, Error},
    fidl_fuchsia_bluetooth_bredr::*,
    fuchsia_async::{self as fasync, futures::select},
    fuchsia_component::client::connect_to_service,
    fuchsia_zircon as zx,
    futures::{
        channel::mpsc::{channel, SendError},
        FutureExt, Sink, SinkExt, Stream, StreamExt, TryStreamExt,
    },
    pin_utils::pin_mut,
    rustyline::{error::ReadlineError, CompletionType, Config, Editor},
    std::{cmp::PartialEq, collections::HashMap, fmt::Debug, thread},
};

use crate::commands::{Cmd, CmdHelper, ReplControl};

mod commands;

/// Prompt to be shown for tool's REPL
pub static PROMPT: &str = "\x1b[34mprofile>\x1b[0m ";

/// Escape code to clear the pty line on which the cursor is located.
/// Used when evented output is intermingled with the REPL prompt.
pub static CLEAR_LINE: &str = "\x1b[2K";

/// Listen on the control event channel for new events.
async fn profile_listener(mut stream: ProfileEventStream) -> Result<(), Error> {
    while let Some(_event) = stream.try_next().await? {
        print!("{}", CLEAR_LINE);
        // TODO(40025): handle events
    }
    Ok(())
}

#[derive(Debug, PartialEq)]
struct L2capChannel {
    socket: zx::Socket,
    mode: ChannelMode,
    max_tx_sdu_size: u16,
}

/// Tracks all state local to the command line tool.
#[derive(Debug, PartialEq)]
struct ChannelState<T> {
    next_chan_id: u32,
    channels: HashMap<u32, T>,
}

impl<T: Debug + PartialEq> ChannelState<T> {
    pub fn new() -> ChannelState<T> {
        ChannelState { next_chan_id: 0, channels: HashMap::new() }
    }

    pub fn channels(&self) -> &HashMap<u32, T> {
        &self.channels
    }

    /// Returns id assigned to channel.
    pub fn add_channel(&mut self, channel: T) -> u32 {
        let chan_id = self.next_chan_id;
        self.next_chan_id += 1;
        assert_eq!(None, self.channels.insert(chan_id, channel));
        chan_id
    }

    pub fn remove_channel(&mut self, channel_id: u32) -> Option<T> {
        self.channels.remove(&channel_id)
    }
}

fn channels(state: &mut ChannelState<L2capChannel>) {
    for (chan_id, chan) in state.channels() {
        print!(
            "Channel:\n  Id: {}\n  Mode: {:?}\n  Max Tx Sdu Size: {}\n",
            chan_id, chan.mode, chan.max_tx_sdu_size
        );
    }
}

async fn connect_l2cap(
    profile_svc: &ProfileProxy,
    state: &mut ChannelState<L2capChannel>,
    args: &Vec<String>,
) -> Result<(), Error> {
    if args.len() != 4 {
        return Err(anyhow!("Invalid number of arguments"));
    }
    let peer_id = &args[0];
    let psm = args[1].parse::<u16>().map_err(|_| anyhow!("Psm must be [0, 65535]"))?;
    let channel_mode = match args[2].as_ref() {
        "basic" => ChannelMode::Basic,
        "ertm" => ChannelMode::EnhancedRetransmission,
        arg => return Err(anyhow!("Invalid channel mode: {}", arg)),
    };
    let max_rx_sdu_size =
        args[3].parse::<u16>().map_err(|_| anyhow!("max-sdu-size must be [0, 65535]"))?;
    let params = ChannelParameters {
        channel_mode: Some(channel_mode),
        max_rx_sdu_size: Some(max_rx_sdu_size),
    };

    let (status, channel) = profile_svc.connect_l2cap(peer_id, psm, params).await?;

    let _ = status
        .error
        .map_or(Ok(()), |e| Err(anyhow!("Could not connect to {}: {:?}", peer_id, e)))?;

    let mode = match channel.channel_mode {
        Some(m) => m,
        None => return Err(anyhow!("Missing channel mode in response")),
    };

    let max_tx_sdu_size = match channel.max_tx_sdu_size {
        Some(s) => s,
        None => return Err(anyhow!("Missing max tx sdu size in response")),
    };

    let chan_id = match channel.socket {
        Some(socket) => state.add_channel(L2capChannel { socket, mode, max_tx_sdu_size }),
        None => {
            println!("Error: failed to receive a socket");
            return Ok(());
        }
    };

    print!(
        "Channel:\n  Id: {}\n  Mode: {:?}\n  Max Tx Sdu Size: {}\n",
        chan_id, mode, max_tx_sdu_size
    );

    Ok(())
}

fn disconnect_l2cap(
    state: &mut ChannelState<L2capChannel>,
    args: &Vec<String>,
) -> Result<(), Error> {
    if args.len() != 1 {
        return Err(anyhow!("Invalid number of arguments"));
    }

    let chan_id = args[0].parse::<u32>().map_err(|_| anyhow!("channel-id must be an integer"))?;

    match state.remove_channel(chan_id) {
        Some(_) => println!("Channel {} disconnected", chan_id),
        None => println!("No channel with id {} exists", chan_id),
    }
    Ok(())
}

enum ParsedCmd {
    Valid(Cmd, Vec<String>),
    Empty,
}

/// Parse a single raw input command from a user into (command type, argument list)
fn parse_cmd(line: String) -> Result<ParsedCmd, Error> {
    let components: Vec<_> = line.trim().split_whitespace().collect();
    match components.split_first() {
        Some((raw_cmd, args)) => match raw_cmd.parse() {
            Ok(cmd) => {
                let args = args.into_iter().map(|s| s.to_string()).collect();
                Ok(ParsedCmd::Valid(cmd, args))
            }
            Err(_) => Err(anyhow!("\"{}\" is not a valid command", raw_cmd)),
        },
        None => Ok(ParsedCmd::Empty),
    }
}

async fn handle_cmd(
    profile_svc: &ProfileProxy,
    state: &mut ChannelState<L2capChannel>,
    cmd: Cmd,
    args: Vec<String>,
) -> Result<ReplControl, Error> {
    match cmd {
        Cmd::Channels => {
            channels(state);
        }
        Cmd::ConnectL2cap => {
            connect_l2cap(profile_svc, state, &args).await?;
        }
        Cmd::DisconnectL2cap => {
            disconnect_l2cap(state, &args)?;
        }
        Cmd::Help => {
            println!("{}", Cmd::help_msg());
        }
        Cmd::Exit | Cmd::Quit => return Ok(ReplControl::Break),
    };
    Ok(ReplControl::Continue)
}

/// Generates a rustyline `Editor` in a separate thread to manage user input. This input is returned
/// as a `Stream` of lines entered by the user.
///
/// The thread exits and the `Stream` is exhausted when an error occurs on stdin or the user
/// sends a ctrl-c or ctrl-d sequence.
///
/// Because rustyline shares control over output to the screen with other parts of the system, a
/// `Sink` is passed to the caller to send acknowledgements that a command has been processed and
/// that rustyline should handle the next line of input.
fn cmd_stream() -> (impl Stream<Item = String>, impl Sink<(), Error = SendError>) {
    let (mut cmd_sender, cmd_receiver) = channel(512);
    let (ack_sender, mut ack_receiver) = channel(512);

    thread::spawn(move || -> Result<(), Error> {
        let mut exec = fasync::Executor::new().context("error creating readline event loop")?;

        let fut = async {
            let config = Config::builder()
                .auto_add_history(true)
                .history_ignore_space(true)
                .completion_type(CompletionType::List)
                .build();
            let mut rl = Editor::<CmdHelper>::with_config(config);
            rl.set_helper(Some(CmdHelper::new()));

            loop {
                let readline = rl.readline(PROMPT);
                match readline {
                    Ok(line) => {
                        cmd_sender.try_send(line)?;
                    }
                    Err(ReadlineError::Eof) | Err(ReadlineError::Interrupted) => {
                        return Ok(());
                    }
                    Err(e) => {
                        println!("Error: {:?}", e);
                        return Err(e.into());
                    }
                }

                // Wait for processing thread to finish evaluating last command.
                if ack_receiver.next().await == None {
                    return Ok(());
                }
            }
        };

        exec.run_singlethreaded(fut)
    });
    (cmd_receiver, ack_sender)
}

/// Wait for raw commands from rustyline thread, and then parse and handle them.
async fn run_repl(profile_svc: ProfileProxy) -> Result<(), Error> {
    // `cmd_stream` blocks on input in a separate thread and passes commands and acks back to
    // the main thread via async channels.
    let (mut commands, mut acks) = cmd_stream();

    let mut state = ChannelState::<L2capChannel>::new();

    while let Some(raw_cmd) = commands.next().await {
        match parse_cmd(raw_cmd) {
            Ok(ParsedCmd::Valid(cmd, args)) => {
                match handle_cmd(&profile_svc, &mut state, cmd, args).await {
                    Ok(ReplControl::Continue) => {}
                    Ok(ReplControl::Break) => break,
                    Err(e) => println!("Error handling command: {}", e),
                }
            }
            Ok(ParsedCmd::Empty) => {}
            Err(err) => println!("Error parsing command: {}", err),
        }
        // Notify readline loop that command has been evaluated.
        acks.send(()).await?
    }

    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let profile_svc = connect_to_service::<ProfileMarker>()
        .context("failed to connect to bluetooth profile service")?;
    let event_stream = profile_svc.take_event_stream();

    let listener = profile_listener(event_stream);
    let repl = run_repl(profile_svc);

    pin_mut!(listener);
    pin_mut!(repl);

    select! {
        r = repl.fuse() => r,
        l = listener.fuse() => l,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_add_channel() {
        let mut state = ChannelState::<i32>::new();
        assert_eq!(0, state.add_channel(0));
        assert_eq!(1, state.add_channel(1));

        assert_eq!(2, state.channels().len());
        assert_eq!(Some(&0i32), state.channels().get(&0u32));
        assert_eq!(Some(&1i32), state.channels().get(&1u32));
    }

    #[test]
    fn test_disconnect_l2cap() {
        let mut state = ChannelState::new();
        let (s, _) = fidl::Socket::create(fidl::SocketOpts::STREAM).unwrap();
        assert_eq!(
            0,
            state.add_channel(L2capChannel {
                socket: s,
                mode: ChannelMode::Basic,
                max_tx_sdu_size: 672
            })
        );
        assert_eq!(1, state.channels().len());
        let args = vec!["0".to_string()];
        assert!(disconnect_l2cap(&mut state, &args).is_ok());
        assert!(state.channels().is_empty());

        // Disconnecting an already disconnected channel should not fail.
        // (It should only print a message)
        assert!(disconnect_l2cap(&mut state, &args).is_ok());
    }
}
