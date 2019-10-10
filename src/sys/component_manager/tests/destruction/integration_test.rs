// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    component_manager_lib::{
        model::{
            self,
            hooks::*,
            testing::{
                test_helpers::DestroyHook,
                test_hook::{Lifecycle, TestHook},
            },
        },
        startup,
    },
    failure::{Error, ResultExt},
    fuchsia_async as fasync, fuchsia_syslog as syslog,
    futures::prelude::*,
};

// TODO: This is a white box test so that we can use hooks. Really this should be a black box test,
// but we need to implement stopping and/or external hooks for that to be possible.
#[fasync::run_singlethreaded(test)]
async fn destruction() -> Result<(), Error> {
    syslog::init_with_tags(&[]).context("could not initialize logging")?;

    // Set up model and hooks.
    let root_component_url =
        "fuchsia-pkg://fuchsia.com/destruction_integration_test#meta/collection_realm.cm"
            .to_string();
    let args = startup::Arguments {
        use_builtin_process_launcher: false,
        use_builtin_vmex: false,
        root_component_url,
    };
    let model = startup::model_setup(&args).await?;
    let test_hook = TestHook::new();
    let (destroy_hook, _, mut destroy_recv) = DestroyHook::new(vec!["coll:root:1"].into());
    model.root_realm.hooks.install(test_hook.hooks()).await;
    model
        .root_realm
        .hooks
        .install(vec![HookRegistration {
            event_type: EventType::PostDestroyInstance,
            callback: destroy_hook.clone(),
        }])
        .await;

    model.look_up_and_bind_instance(model::AbsoluteMoniker::root()).await?;

    // Wait for `coll:root` to be destroyed.
    destroy_recv.next().await.expect("failed to destroy notification");

    // Assert that root component has no children.
    let children: Vec<_> = model
        .root_realm
        .lock_state()
        .await
        .as_ref()
        .expect("not resolved")
        .all_child_realms()
        .keys()
        .map(|m| m.clone())
        .collect();
    assert!(children.is_empty());

    // Assert the expected lifecycle events. The leaves can be stopped/destroyed in either order.
    let mut events: Vec<_> = test_hook
        .lifecycle()
        .into_iter()
        .filter_map(|e| match e {
            Lifecycle::Stop(_) | Lifecycle::Destroy(_) => Some(e),
            _ => None,
        })
        .collect();

    let mut next: Vec<_> = events.drain(0..2).collect();
    next.sort_unstable();
    let expected: Vec<_> = vec![
        Lifecycle::Stop(vec!["coll:root:1", "trigger_a:0"].into()),
        Lifecycle::Stop(vec!["coll:root:1", "trigger_b:0"].into()),
    ];
    assert_eq!(next, expected);
    let next: Vec<_> = events.drain(0..1).collect();
    assert_eq!(next, vec![Lifecycle::Stop(vec!["coll:root:1"].into())]);

    let mut next: Vec<_> = events.drain(0..2).collect();
    next.sort_unstable();
    let expected: Vec<_> = vec![
        Lifecycle::Destroy(vec!["coll:root:1", "trigger_a:0"].into()),
        Lifecycle::Destroy(vec!["coll:root:1", "trigger_b:0"].into()),
    ];
    assert_eq!(next, expected);
    assert_eq!(events, vec![Lifecycle::Destroy(vec!["coll:root:1"].into())]);

    Ok(())
}
