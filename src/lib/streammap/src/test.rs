// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::*;
use failure;
use fuchsia_async as fasync;
use futures::{
    self,
    channel::mpsc,
    executor::block_on,
    prelude::*,
    task::{Context, Poll},
};
use futures_test::task::new_count_waker;
use std::collections::HashSet;
use test_util::assert_matches;

#[fasync::run_until_stalled]
#[test]
async fn ready_items_are_yielded() -> Result<(), failure::Error> {
    let mut stream_map = StreamMap::new();
    let mut expected_output = HashSet::new();

    for i in 0..100usize {
        let (mut sender, receiver) = mpsc::channel::<usize>(1);
        sender.send(i).await?;
        stream_map.insert(i, receiver).await;
        expected_output.insert(i);
    }

    for i in 0..100usize {
        let next_fut = stream_map.next();
        fasync::pin_mut!(next_fut);
        assert_eq!(futures::poll!(next_fut), Poll::Ready(Some(i)));
    }

    Ok(())
}

#[fasync::run(4)]
#[test]
async fn concurrently_ready_items_are_yielded() -> Result<(), failure::Error> {
    let mut stream_map = StreamMap::new();
    let mut expected_output = HashSet::new();

    for i in 0..1000usize {
        let (mut sender, receiver) = mpsc::channel::<usize>(1);
        fasync::spawn(async move {
            sender.send(i).await.expect("Sending message");
        });
        stream_map.insert(i, receiver).await;
        expected_output.insert(i);
    }

    for _ in 0..1000usize {
        let next_fut = stream_map.next();
        fasync::pin_mut!(next_fut);
        let poll_result = futures::poll!(next_fut);
        assert_matches!(poll_result, Poll::Ready(Some(_)));
    }

    Ok(())
}

#[fasync::run_until_stalled]
#[test]
async fn removed_streams_are_terminated() -> Result<(), failure::Error> {
    let mut stream_map = StreamMap::new();
    let (mut sender1, receiver1) = mpsc::channel(1);
    let (mut sender2, receiver2) = mpsc::channel(1);
    sender1.send(1u32).await?;
    sender2.send(2u32).await?;

    let insert1 = stream_map.insert(1u32, receiver1).await;
    let insert2 = stream_map.insert(2u32, receiver2).await;

    assert_matches!(insert1, None);
    assert_matches!(insert2, None);
    assert_matches!(stream_map.remove(1u32).await, Some(_));

    for expected in vec![Poll::Ready(Some(2)), Poll::Pending] {
        let next_fut = stream_map.next();
        fasync::pin_mut!(next_fut);
        assert_eq!(futures::poll!(next_fut), expected);
    }

    Ok(())
}

#[fasync::run_until_stalled]
#[test]
async fn with_elem_has_effect() -> Result<(), failure::Error> {
    let mut stream_map = StreamMap::new();
    let (_sender, receiver) = mpsc::channel::<u32>(1);

    stream_map.insert(1u32, receiver).await;

    let mut run_for_present_element = false;
    assert!(
        stream_map
            .with_elem(1u32, |_| {
                run_for_present_element = true;
            })
            .await
    );
    assert!(run_for_present_element);

    let mut run_for_absent_element = false;
    assert!(
        !stream_map
            .with_elem(2u32, |_| {
                run_for_absent_element = true;
            })
            .await
    );
    assert!(!run_for_absent_element);

    Ok(())
}

#[test]
fn awoken_for_mutex_guard() -> Result<(), failure::Error> {
    let mut stream_map = StreamMap::new();

    let (_sender1, receiver1) = mpsc::channel::<usize>(1);
    block_on(stream_map.insert(0usize, receiver1));

    let (_sender2, receiver2) = mpsc::channel::<usize>(1);
    block_on(stream_map.insert(0usize, receiver2));

    let store_handle = stream_map.store();
    let guard = block_on(store_handle.lock());

    let (waker, wake_count) = new_count_waker();
    let mut ctx = Context::from_waker(&waker);

    let next_fut = stream_map.next();
    fasync::pin_mut!(next_fut);
    assert_eq!(next_fut.poll(&mut ctx), Poll::Pending);

    assert_eq!(wake_count.get(), 0);
    drop(guard);
    assert_eq!(wake_count.get(), 1);

    Ok(())
}

#[fasync::run_singlethreaded]
#[test]
async fn ended_stream_is_removed_from_map() -> Result<(), failure::Error> {
    let mut stream_map = StreamMap::new();

    let (sender, receiver) = mpsc::channel::<usize>(1);
    stream_map.insert(0usize, receiver).await;

    let store = stream_map.store();
    let stream_count = || async { store.lock().await.len() };

    let next_fut = stream_map.next();
    fasync::pin_mut!(next_fut);
    assert_eq!(futures::poll!(next_fut), Poll::Pending);
    assert_eq!(stream_count().await, 1);

    drop(sender);
    let next_fut = stream_map.next();
    fasync::pin_mut!(next_fut);
    assert_eq!(futures::poll!(next_fut), Poll::Pending);
    assert_eq!(stream_count().await, 0);

    Ok(())
}
