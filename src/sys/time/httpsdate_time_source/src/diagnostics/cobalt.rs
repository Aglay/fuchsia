// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::datatypes::HttpsSample;
use crate::diagnostics::Diagnostics;
use fidl_fuchsia_cobalt::HistogramBucket;
use fuchsia_cobalt::{CobaltConnector, CobaltSender, ConnectionType};
use fuchsia_zircon as zx;
use futures::Future;
use httpdate_hyper::HttpsDateError;
use parking_lot::Mutex;
use time_metrics_registry::{
    HttpsdateBoundSizeMetricDimensionPhase as HttpsAlgorithmPhase, HTTPSDATE_BOUND_SIZE_METRIC_ID,
    HTTPSDATE_POLL_LATENCY_INT_BUCKETS_FLOOR, HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS,
    HTTPSDATE_POLL_LATENCY_INT_BUCKETS_STEP_SIZE, HTTPSDATE_POLL_LATENCY_METRIC_ID, PROJECT_ID,
};

/// A `Diagnostics` implementation that uploads diagnostics metrics to Cobalt.
pub struct CobaltDiagnostics {
    /// Client connection to Cobalt.
    sender: Mutex<CobaltSender>,
}

impl CobaltDiagnostics {
    /// Create a new `CobaltDiagnostics`, and future that must be polled to upload to Cobalt.
    pub fn new() -> (Self, impl Future<Output = ()>) {
        let (sender, fut) =
            CobaltConnector::default().serve(ConnectionType::project_id(PROJECT_ID));
        (Self { sender: Mutex::new(sender) }, fut)
    }

    /// Calculate the bucket number in the latency metric for a given duration.
    fn round_trip_time_bucket(duration: &zx::Duration) -> u32 {
        // bucket index 0 is reserved for underflow. Notably there are NUM_BUCKETS + 1 buckets,
        // and the last bucket is reserved for overflow.
        const OVERFLOW_THRESHOLD: i64 = HTTPSDATE_POLL_LATENCY_INT_BUCKETS_FLOOR
            + ((HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS - 1)
                * HTTPSDATE_POLL_LATENCY_INT_BUCKETS_STEP_SIZE) as i64;
        if duration.into_micros() < HTTPSDATE_POLL_LATENCY_INT_BUCKETS_FLOOR {
            0
        } else if duration.into_micros() > OVERFLOW_THRESHOLD {
            HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS
        } else {
            ((duration.into_micros() - HTTPSDATE_POLL_LATENCY_INT_BUCKETS_FLOOR) as u32)
                / HTTPSDATE_POLL_LATENCY_INT_BUCKETS_STEP_SIZE
                + 1
        }
    }
}

impl Diagnostics for CobaltDiagnostics {
    fn success(&self, sample: &HttpsSample) {
        let mut sender = self.sender.lock();
        sender.log_event_count(
            HTTPSDATE_BOUND_SIZE_METRIC_ID,
            [HttpsAlgorithmPhase::Maintain],
            0i64, // period_duration, not used
            sample.final_bound_size.into_micros(),
        );

        let mut bucket_counts = [0u64; HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS as usize + 1];
        for bucket_idx in sample.round_trip_times.iter().map(Self::round_trip_time_bucket) {
            bucket_counts[bucket_idx as usize] += 1;
        }
        let histogram_buckets = bucket_counts
            .iter()
            .enumerate()
            .filter(|(_, count)| **count > 0)
            .map(|(index, count)| HistogramBucket { index: index as u32, count: *count })
            .collect::<Vec<_>>();
        sender.log_int_histogram(HTTPSDATE_POLL_LATENCY_METRIC_ID, (), histogram_buckets);
    }

    fn failure(&self, _error: &HttpsDateError) {
        // Currently, no failure events are registered with cobalt.
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fuchsia_cobalt::{CobaltEvent, CountEvent, EventPayload};
    use fuchsia_async as fasync;
    use futures::{channel::mpsc, stream::StreamExt};
    use lazy_static::lazy_static;
    use std::{collections::HashSet, iter::FromIterator};

    const TEST_BOUND_SIZE: zx::Duration = zx::Duration::from_millis(101);
    const TEST_STANDARD_DEVIATION: zx::Duration = zx::Duration::from_millis(20);
    const BUCKET_SIZE: zx::Duration =
        zx::Duration::from_micros(HTTPSDATE_POLL_LATENCY_INT_BUCKETS_STEP_SIZE as i64);
    const BUCKET_FLOOR: zx::Duration =
        zx::Duration::from_micros(HTTPSDATE_POLL_LATENCY_INT_BUCKETS_FLOOR);
    const ONE_MICROS: zx::Duration = zx::Duration::from_micros(1);
    lazy_static! {
        static ref TEST_TIME: zx::Time = zx::Time::from_nanos(123_456_789);
        static ref BUCKET_1_RTT: zx::Duration = BUCKET_FLOOR + ONE_MICROS;
        static ref BUCKET_5_RTT_1: zx::Duration = *BUCKET_1_RTT + BUCKET_SIZE * 4;
        static ref BUCKET_5_RTT_2: zx::Duration = BUCKET_FLOOR + BUCKET_SIZE * 5 - ONE_MICROS;
        static ref OVERFLOW_RTT: zx::Duration =
            BUCKET_FLOOR + BUCKET_SIZE * (HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS + 2);
        static ref OVERFLOW_ADJACENT_RTT: zx::Duration = BUCKET_FLOOR
            + BUCKET_SIZE * (HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS - 1)
            - ONE_MICROS;
        static ref UNDERFLOW_RTT: zx::Duration = BUCKET_FLOOR - ONE_MICROS;
    }

    /// Create a `CobaltDiagnostics` and a receiver to inspect events it produces.
    fn diagnostics_for_test() -> (CobaltDiagnostics, mpsc::Receiver<CobaltEvent>) {
        let (send, recv) = mpsc::channel(10);
        (CobaltDiagnostics { sender: Mutex::new(CobaltSender::new(send)) }, recv)
    }

    #[test]
    fn test_round_trip_time_bucket() {
        assert_eq!(CobaltDiagnostics::round_trip_time_bucket(&*BUCKET_1_RTT), 1);
        assert_eq!(CobaltDiagnostics::round_trip_time_bucket(&*BUCKET_5_RTT_1), 5);
        assert_eq!(
            CobaltDiagnostics::round_trip_time_bucket(&*OVERFLOW_RTT),
            HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS
        );
        assert_eq!(
            CobaltDiagnostics::round_trip_time_bucket(&*OVERFLOW_ADJACENT_RTT),
            HTTPSDATE_POLL_LATENCY_INT_BUCKETS_NUM_BUCKETS - 1
        );
        assert_eq!(CobaltDiagnostics::round_trip_time_bucket(&*UNDERFLOW_RTT), 0);
    }

    #[fasync::run_until_stalled(test)]
    async fn test_success_single_rtt() {
        let (cobalt, event_recv) = diagnostics_for_test();
        cobalt.success(&HttpsSample {
            utc: *TEST_TIME,
            monotonic: *TEST_TIME,
            standard_deviation: TEST_STANDARD_DEVIATION,
            final_bound_size: TEST_BOUND_SIZE,
            round_trip_times: vec![*BUCKET_1_RTT],
        });
        assert_eq!(
            event_recv.take(2).collect::<Vec<_>>().await,
            vec![
                CobaltEvent {
                    metric_id: HTTPSDATE_BOUND_SIZE_METRIC_ID,
                    event_codes: vec![HttpsAlgorithmPhase::Maintain as u32],
                    component: None,
                    payload: EventPayload::EventCount(CountEvent {
                        period_duration_micros: 0,
                        count: TEST_BOUND_SIZE.into_micros()
                    })
                },
                CobaltEvent {
                    metric_id: HTTPSDATE_POLL_LATENCY_METRIC_ID,
                    event_codes: vec![],
                    component: None,
                    payload: EventPayload::IntHistogram(vec![HistogramBucket {
                        index: 1,
                        count: 1
                    }])
                }
            ]
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn test_success_multiple_rtt() {
        let (cobalt, event_recv) = diagnostics_for_test();
        cobalt.success(&HttpsSample {
            utc: *TEST_TIME,
            monotonic: *TEST_TIME,
            standard_deviation: TEST_STANDARD_DEVIATION,
            final_bound_size: TEST_BOUND_SIZE,
            round_trip_times: vec![*BUCKET_1_RTT, *BUCKET_5_RTT_1, *BUCKET_5_RTT_2],
        });
        let mut events = event_recv.take(2).collect::<Vec<_>>().await;
        assert_eq!(
            events[0],
            CobaltEvent {
                metric_id: HTTPSDATE_BOUND_SIZE_METRIC_ID,
                event_codes: vec![HttpsAlgorithmPhase::Maintain as u32],
                component: None,
                payload: EventPayload::EventCount(CountEvent {
                    period_duration_micros: 0,
                    count: TEST_BOUND_SIZE.into_micros()
                })
            }
        );
        assert_eq!(events[1].metric_id, HTTPSDATE_POLL_LATENCY_METRIC_ID);
        assert!(events[1].event_codes.is_empty());
        assert!(events[1].component.is_none());
        match events.remove(1).payload {
            EventPayload::IntHistogram(buckets) => {
                let expected_buckets: HashSet<HistogramBucket> = HashSet::from_iter(vec![
                    HistogramBucket { index: 1, count: 1 },
                    HistogramBucket { index: 5, count: 2 },
                ]);
                assert_eq!(expected_buckets, HashSet::from_iter(buckets));
            }
            p => panic!("Got unexpected payload: {:?}", p),
        }
    }
}
