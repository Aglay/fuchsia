// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

use {
    crate::container::ComponentIdentity,
    fidl_fuchsia_diagnostics::{Interest, StringSelector},
    fidl_fuchsia_logger::{LogInterestSelector, LogSinkControlHandle},
    std::collections::HashMap,
    std::sync::Weak,
    tracing::warn,
};

/// Type used to identify the intended component target specified by an
/// interest selector.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct Component {
    name: String,
}

/// Interest dispatcher to handle the communication of `Interest` changes
/// to their intended `LogSink` client listeners.
#[derive(Debug, Default)]
pub struct InterestDispatcher {
    /// Map of LogSinkControlHandles associated with a given component name
    /// (possibly multiple instances). Any provided selector specification
    /// will apply universally to all matching instances.
    interest_listeners: HashMap<Component, Vec<Weak<LogSinkControlHandle>>>,
    /// List of LogInterestSelectors that couple the specified interest with
    /// the intended target component.
    selectors: Vec<LogInterestSelector>,
}

impl InterestDispatcher {
    /// Add a LogSinkControlHandle corresponding to the given component
    /// (source) as an interest listener. If one or more control handles
    /// are already associated with this component (i.e. in the case of
    /// multiple instances) this handle will be apended to the list.
    pub fn add_interest_listener(
        &mut self,
        source: &ComponentIdentity,
        handle: Weak<LogSinkControlHandle>,
    ) {
        let component = Component { name: source.to_string() };

        let component_listeners =
            self.interest_listeners.entry(component.clone()).or_insert(vec![]);
        component_listeners.push(handle);

        // check to see if we have a selector specified for this component and
        // if so, send the interest notification.
        let mut interest: Option<Interest> = None;
        self.selectors.iter().for_each(|s| {
            if let Some(segments) = &s.selector.moniker_segments {
                segments.iter().for_each(|segment| {
                    match segment {
                        StringSelector::StringPattern(name) => {
                            // TODO(fxbug.dev/54198): Interest listener matching based
                            // on strict name comparison look at using moniker
                            // heuristics via selectors API.
                            if name == &component.name {
                                interest = Some(Interest {
                                    min_severity: s.interest.min_severity,
                                    ..Interest::EMPTY
                                });
                            }
                        }
                        _ => warn!(?segment, "Unexpected component selector moniker segment"),
                    };
                });
            };
        });
        if let Some(i) = interest {
            self.notify_listeners_for_component(component, |l| {
                let _ = l.send_on_register_interest(Interest {
                    min_severity: i.min_severity,
                    ..Interest::EMPTY
                });
            });
        }
    }

    /// Update the set of selectors that archivist uses to control the log
    /// levels associated with any active LogSink clients.
    pub fn update_selectors<'a>(&mut self, selectors: Vec<LogInterestSelector>) {
        if !self.selectors.is_empty() {
            warn!(existing = ?self.selectors, new = ?selectors, "Overriding selectors");
        }
        selectors.iter().for_each(|s| {
            if let Some(segments) = &s.selector.moniker_segments {
                segments.iter().for_each(|segment| {
                    match segment {
                        // string_pattern results from selectors created with
                        // selectors::parse_component_selector. Potentially
                        // handle additional cases (exact_match?) if needs be.
                        StringSelector::StringPattern(name) => {
                            self.notify_listeners_for_component(
                                Component { name: name.to_string() },
                                |l| {
                                    let _ = l.send_on_register_interest(Interest {
                                        min_severity: s.interest.min_severity,
                                        ..Interest::EMPTY
                                    });
                                },
                            );
                        }
                        _ => warn!(?segment, "Unexpected component selector moniker segment"),
                    };
                });
            };
        });
        self.selectors = selectors;
    }

    fn notify_listeners_for_component<F>(&mut self, component: Component, mut f: F)
    where
        F: FnMut(&LogSinkControlHandle) -> (),
    {
        if let Some(component_listeners) = self.interest_listeners.get_mut(&component) {
            component_listeners.retain(|listener| match listener.upgrade() {
                Some(listener_) => {
                    f(&listener_);
                    true
                }
                None => false,
            });
        } else {
            warn!(
                ?component,
                "Failed to notify interest listener - unable to find LogSinkControlHandle"
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::logs::message::TEST_IDENTITY;
    use fidl::endpoints::{create_request_stream, RequestStream};
    use std::sync::Arc;

    /// This test does not await any futures but it creates FIDL types which expect to have
    /// an executor available.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn interest_listeners() {
        let source_arc = TEST_IDENTITY.clone();

        let mut dispatcher = InterestDispatcher::default();
        let (_logsink_client, log_request_stream) =
            create_request_stream::<fidl_fuchsia_logger::LogSinkMarker>()
                .expect("failed to create LogSink proxy");
        let handle = log_request_stream.control_handle();

        // add the interest listener (component_id + weak handle)
        dispatcher.add_interest_listener(&source_arc, Arc::downgrade(&Arc::new(handle)));

        // check listener addition successful
        let c = Component { name: source_arc.to_string() };
        let preclose_listeners = &dispatcher.interest_listeners.get(&c);
        assert_eq!(preclose_listeners.is_some(), true);
        if let Some(l) = preclose_listeners {
            assert_eq!(l.len(), 1);
            if let Some(handle) = l[0].upgrade() {
                // close the channel
                handle.shutdown();
            }
            // notify checks the channel/retains
            dispatcher.notify_listeners_for_component(c.clone(), |listener| {
                let _ = listener
                    .send_on_register_interest(Interest { min_severity: None, ..Interest::EMPTY });
            });

            // check that the listener is no longer active.
            let postclose_listeners = &dispatcher.interest_listeners.get(&c);
            assert_eq!(postclose_listeners.is_some(), true);
            match postclose_listeners {
                Some(l) => assert_eq!(l.len(), 0),
                None => {}
            }
        }
    }
}
