// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

extern crate url;

use std::collections::BTreeMap;
use url::form_urlencoded;

pub use crate::env_info::*;
pub use crate::user_status::*;

const GA_PROPERTY_KEY: &str = "tid";

#[cfg(test)]
const GA_PROPERTY_ID: &str = "UA-175659118-1";
#[cfg(not(test))]
const GA_PROPERTY_ID: &str = "UA-127897021-9";

const GA_CLIENT_KEY: &str = "cid";

const GA_EVENT_CATEGORY_KEY: &str = "ec";
const GA_EVENT_CATEGORY_DEFAULT: &str = "general";

// TODO - match zxdb by changing category and action for analytics commands
const GA_EVENT_CATEGORY_ANALYTICS: &str = "analytics";
const GA_EVENT_CATEGORY_ANALYTICS_ACTION_ENABLE: &str = "manual-enable";
const GA_EVENT_CATEGORY_ANALYTICS_ACTION_DISABLE: &str = "disable";

const GA_EVENT_ACTION_KEY: &str = "ea";

const GA_EVENT_LABELS_KEY: &str = "el";
const GA_DATA_TYPE_KEY: &str = "t";
const GA_DATA_TYPE_EVENT_KEY: &str = "event";

const GA_PROTOCOL_KEY: &str = "v";
const GA_PROTOCOL_VAL: &str = "1";

//  TODO fx uses bash or zsh. Is that what we want?
const GA_APP_NAME_KEY: &str = "an";
const GA_APP_VERSION_KEY: &str = "av";
const GA_APP_VERSION_DEFAULT: &str = "unknown";

const GA_CUSTOM_DIMENSION_1_KEY: &str = "cd1";

pub type UuidBuilder = fn() -> String;

pub fn make_body_with_hash(
    app_name: &str,
    app_version: Option<&str>,
    category: Option<&str>,
    action: Option<&str>,
    labels: Option<&str>,
    custom_dimensions: BTreeMap<&str, String>,
    uuid_builder: &UuidBuilder,
) -> String {
    let uuid_pre = &(uuid_builder)();
    let uname = os_and_release_desc();

    let mut params = BTreeMap::new();
    params.insert(GA_PROTOCOL_KEY, GA_PROTOCOL_VAL);
    params.insert(GA_PROPERTY_KEY, GA_PROPERTY_ID);
    params.insert(GA_CLIENT_KEY, &uuid_pre);
    params.insert(GA_DATA_TYPE_KEY, GA_DATA_TYPE_EVENT_KEY);
    params.insert(GA_APP_NAME_KEY, app_name);
    params.insert(
        GA_APP_VERSION_KEY,
        match app_version {
            Some(ver) => ver,
            None => GA_APP_VERSION_DEFAULT,
        },
    );
    params.insert(
        GA_EVENT_CATEGORY_KEY,
        match category {
            Some(value) => value,
            None => GA_EVENT_CATEGORY_DEFAULT,
        },
    );
    insert_if_present(GA_EVENT_ACTION_KEY, &mut params, action);
    insert_if_present(GA_EVENT_LABELS_KEY, &mut params, labels);

    for (&key, value) in custom_dimensions.iter() {
        params.insert(key, value);
    }
    params.insert(GA_CUSTOM_DIMENSION_1_KEY, &uname);

    let body = to_kv_post_body(&params);
    //println!("body = {}", &body);
    body
}

fn insert_if_present<'a>(
    key: &'a str,
    params: &mut BTreeMap<&'a str, &'a str>,
    value: Option<&'a str>,
) {
    match value {
        Some(val) => {
            if !val.is_empty() {
                params.insert(key, val);
            }
        }
        None => (),
    };
}

fn to_kv_post_body(params: &BTreeMap<&str, &str>) -> String {
    let mut serializer = form_urlencoded::Serializer::new(String::new());
    serializer.extend_pairs(params.iter());
    serializer.finish()
}

#[cfg(test)]
mod test {
    use super::*;

    fn test_uuid() -> String {
        "test_cid".to_string()
    }

    #[test]
    fn make_post_body() {
        let args = "config analytics enable";
        let args_encoded = "config+analytics+enable";
        let app_name = "ffx";
        let app_version = "1";
        let uname = os_and_release_desc().replace(" ", "+");
        let cid = test_uuid();
        let expected = format!(
            "an={}&av={}&cd1={}&cid={}&ea={}&ec=general&el={}&t=event&tid=UA-175659118-1&v=1",
            &app_name, &app_version, &uname, &cid, &args_encoded, &args_encoded
        );
        assert_eq!(
            expected,
            make_body_with_hash(
                app_name,
                Some(app_version),
                None,
                Some(args),
                Some(args),
                BTreeMap::new(),
                &(test_uuid as UuidBuilder)
            )
        );
    }

    #[test]
    fn make_post_body_with_labels() {
        let args = "config analytics enable";
        let args_encoded = "config+analytics+enable";
        let labels = "labels";
        let app_name = "ffx";
        let app_version = "1";
        let uname = os_and_release_desc().replace(" ", "+");
        let cid = test_uuid();
        let expected = format!(
            "an={}&av={}&cd1={}&cid={}&ea={}&ec=general&el={}&t=event&tid=UA-175659118-1&v=1",
            &app_name, &app_version, &uname, &cid, &args_encoded, &labels
        );
        assert_eq!(
            expected,
            make_body_with_hash(
                app_name,
                Some(app_version),
                None,
                Some(args),
                Some(labels),
                BTreeMap::new(),
                &(test_uuid as UuidBuilder)
            )
        );
    }

    #[test]
    fn make_post_body_with_custom_dimensions() {
        let args = "config analytics enable";
        let args_encoded = "config+analytics+enable";
        let labels = "labels";
        let app_name = "ffx";
        let app_version = "1";
        let uname = os_and_release_desc().replace(" ", "+");
        let cd3_val = "foo".to_string();
        let cid = test_uuid();
        let expected = format!(
            "an={}&av={}&cd1={}&cd3={}&cid={}&ea={}&ec=general&el={}&t=event&tid=UA-175659118-1&v=1",
            &app_name, &app_version, &uname, &cd3_val, &cid, &args_encoded, &labels
        );
        let mut custom_dimensions = BTreeMap::new();
        custom_dimensions.insert("cd3", cd3_val);
        assert_eq!(
            expected,
            make_body_with_hash(
                app_name,
                Some(app_version),
                None,
                Some(args),
                Some(labels),
                custom_dimensions,
                &(test_uuid as UuidBuilder)
            )
        );
    }

    #[test]
    fn insert_if_present_some() {
        let mut map2 = BTreeMap::new();
        let val_str = "property1";
        let val = Some(val_str);
        let key = "key1";
        insert_if_present(key, &mut map2, val);
        assert_eq!(val_str, *map2.get(key).unwrap());
    }

    #[test]
    fn insert_if_present_none() {
        let mut map2 = BTreeMap::new();
        let val = None;
        let key = "key1";
        insert_if_present(key, &mut map2, val);
        assert_eq!(None, map2.get(key));
    }

    #[test]
    fn insert_if_present_empty() {
        let mut map2 = BTreeMap::new();
        let val = "";
        let key = "key1";
        insert_if_present(key, &mut map2, Some(val));
        assert_eq!(None, map2.get(key));
    }
}
