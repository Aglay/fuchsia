// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{err_msg, Error};
use fidl::endpoints::{RequestStream, ServerEnd};
use fidl_fuchsia_stash::{
    GetIteratorMarker, GetIteratorRequest, GetIteratorRequestStream, ListIteratorMarker,
    ListIteratorRequest, ListIteratorRequestStream, Value, MAX_KEY_SIZE, MAX_STRING_SIZE,
    ListItem, KeyValue
};
use fuchsia_async as fasync;
use fuchsia_syslog::{fx_log_err, fx_log_info};
use fuchsia_zircon_sys;
use futures::{TryFutureExt, TryStreamExt};
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::Arc;

use crate::store;

// 16 byte overhead for vectors
// 16 byte overhead per string
// 1 byte used for the uint8 type enum
const LIST_PREFIX_CHUNK_SIZE: usize = (((fuchsia_zircon_sys::ZX_CHANNEL_MAX_MSG_BYTES as u64) - 16)
                                            / (MAX_KEY_SIZE + 16 + 1)) as usize;

// 16 byte overhead for vectors
// 16 byte overhead per string
const GET_PREFIX_CHUNK_SIZE: usize = (((fuchsia_zircon_sys::ZX_CHANNEL_MAX_MSG_BYTES as u64) - 16)
                                            / (MAX_KEY_SIZE + 16 + MAX_STRING_SIZE + 16)) as usize;

/// Accessors are used by clients to view and interact with the store. In this situation, that
/// means handling transactional logic and using the store_manager to perform store
/// lookups/manipulations.
pub struct Accessor {
    store_manager: Arc<Mutex<store::StoreManager>>,
    client_name: String,
    enable_bytes: bool,
    read_only: bool,
    fields_updated: Arc<Mutex<HashMap<store::Key, Option<Value>>>>,
}

impl Accessor {
    /// Create a new accessor for interacting with the store. The client_name field determines what
    /// namespace in the store will be used.
    pub fn new(
        store_manager: Arc<Mutex<store::StoreManager>>,
        enable_bytes: bool,
        read_only: bool,
        client_name: String,
    ) -> Accessor {
        Accessor {
            store_manager,
            enable_bytes,
            client_name,
            read_only,
            fields_updated: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    /// Retrieves a value from the store. If a value has been modified and commit() hasn't been
    /// called yet, the modified version will be returned.
    pub fn get_value(&self, key: &str) -> Result<Option<Value>, Error> {
        fx_log_info!("retrieving value for key: {}", key);
        let store_manager = self.store_manager.lock();

        if let Some(o_val) = self.fields_updated.lock().get(key) {
            // This accessor has an uncommited update for the field, so let's return that.
            match (self.enable_bytes, o_val) {
                (false, Some(Value::Bytesval(_))) => {
                    // Bytes are disabled. Return an error instead.
                    return Err(err_msg(
                        "client attempted to access bytes when the type is disabled",
                    ));
                }
                (_, Some(val)) => {
                    return Ok(Some(store::clone_value(val)?));
                }
                (_, None) => return Ok(None),
            }
        }

        let res = store_manager.get_value(&self.client_name, &key);

        match res {
            None => Ok(None),
            Some(Value::Bytesval(_)) if !self.enable_bytes => Err(err_msg(
                "client attempted to access bytes when the type is disabled",
            )),
            Some(val) => Ok(Some(store::clone_value(val)?)),
        }
    }

    /// Sets a value in the store. commit() must be called for this to take effect.
    pub fn set_value(&mut self, key: String, val: Value) -> Result<(), Error> {
        if self.read_only {
            return Err(err_msg(
                "client attempted to set a value with a read-only accessor",
            ));
        }
        if !self.enable_bytes {
            if let Value::Bytesval(_) = val {
                return Err(err_msg(
                    "client attempted to set bytes when the type is disabled",
                ));
            }
        }

        self.fields_updated.lock().insert(key, Some(val));

        Ok(())
    }

    /// Deletes a value from the store. commit() must be called for this to take effect.
    pub fn delete_value(&mut self, key: String) -> Result<(), Error> {
        if self.read_only {
            return Err(err_msg(
                "client attempted to delete a value with a read-only accessor",
            ));
        }

        self.fields_updated.lock().insert(key, None);
        Ok(())
    }

    /// Given a server endpoint, answers calls to get_next on the endpoint to return key/type pairs
    /// where the key contains the given prefix.
    pub fn list_prefix(
        &mut self,
        prefix: String,
        server_end: ServerEnd<ListIteratorMarker>,
    ) {
        let mut list_results = self
            .store_manager
            .lock()
            .list_prefix(&self.client_name, &prefix);

        // Merge the results with the pending updated fields
        let fields_updated = self.fields_updated.lock();
        if !fields_updated.is_empty() {
            // Delete any fields that have been marked for removal
            list_results.retain(|li| field_has_been_deleted(fields_updated.get(&li.key)));
            // Update any fields that have been updated/created
            for (key, o_val) in fields_updated.iter() {
                let mut key_was_added = false;
                for li in list_results.iter_mut() {
                    if let Some(v) = o_val {
                        if &li.key == key {
                            key_was_added = true;
                            li.type_ = store::value_to_type(&v);
                        }
                    }
                }
                // This is a new key, append it to the results.
                if !key_was_added {
                    if let Some(v) = o_val {
                        list_results.push(ListItem {
                            key: key.clone(),
                            type_: store::value_to_type(&v),
                        });
                    }
                }
            }
        }

        fasync::spawn(async move {
            let server_chan = fasync::Channel::from_channel(server_end.into_channel())?;
            let mut stream = ListIteratorRequestStream::from_channel(server_chan);
            while let Some(ListIteratorRequest::GetNext{ responder })
                = await!(stream.try_next())?
            {
                let split_at = list_results.len() - LIST_PREFIX_CHUNK_SIZE.min(list_results.len());
                let mut chunk = list_results.split_off(split_at);
                responder.send(&mut chunk.iter_mut())?;
            }
            Ok(())
        }.unwrap_or_else(|e: failure::Error| fx_log_err!("error running list prefix interface: {:?}", e)));
    }

    /// Given a server endpoint, answers calls to get_next on the endpoint to return key/value
    /// pairs where the key contains the given prefix.
    pub fn get_prefix(
        &mut self,
        prefix: String,
        server_end: ServerEnd<GetIteratorMarker>,
    ) -> Result<(), Error> {

        let mut get_results = self
            .store_manager
            .lock()
            .get_prefix(&self.client_name, &prefix)?;

        // Merge the results with the pending updated fields
        let fields_updated = self.fields_updated.lock();
        if !fields_updated.is_empty() {
            // Delete any fields that have been marked for removal
            get_results.retain(|kv| field_has_been_deleted(fields_updated.get(&kv.key)));
            // Update any fields that have been updated/created
            for (key, o_val) in fields_updated.iter() {
                let mut key_was_added = false;
                for kv in get_results.iter_mut() {
                    if let Some(v) = o_val {
                        if &kv.key == key {
                            key_was_added = true;
                            kv.val = store::clone_value(v)?;
                        }
                    }
                }
                // This is a new key, append it to the results.
                if !key_was_added {
                    if let Some(v) = o_val {
                        get_results.push(KeyValue {
                            key: key.clone(),
                            val: store::clone_value(v)?,
                        });
                    }
                }
            }
        }

        let enable_bytes = self.enable_bytes;

        fasync::spawn(async move {
            let server_chan = fasync::Channel::from_channel(server_end.into_channel())?;
            let mut stream = GetIteratorRequestStream::from_channel(server_chan);
            while let Some(GetIteratorRequest::GetNext{ responder })
                = await!(stream.try_next())?
            {
                let split_at = get_results.len() - GET_PREFIX_CHUNK_SIZE.min(get_results.len());
                let mut chunk = get_results.split_off(split_at);
                if !enable_bytes {
                    for item in chunk.iter() {
                        if let Value::Bytesval(_) = item.val {
                            Err(err_msg("client attempted to access bytes when the type is disabled"))?;
                        }
                    }
                }
                responder.send(&mut chunk.iter_mut())?;
            }
            Ok(())
        }.unwrap_or_else(|e: failure::Error| fx_log_err!("error running get prefix interface: {:?}", e)));
        Ok(())
    }

    /// Deletes all keys under a given prefix. commit() must be called for this to take effect.
    pub fn delete_prefix(&mut self, prefix: String) -> Result<(), Error> {
        if self.read_only {
            return Err(err_msg(
                "client attempted to delete a prefix with a read-only accessor",
            ));
        }

        let sm = self.store_manager.lock();
        let mut fields_updated = self.fields_updated.lock();

        let list_results = sm.list_prefix(&self.client_name, &prefix);

        for (key, o_val) in fields_updated.iter_mut() {
            if key.starts_with(&prefix) {
                *o_val = None;
            }
        }
        for li in list_results {
            fields_updated.insert(li.key, None);
        }
        Ok(())
    }

    /// Causes all state modifications to take effect and be written to disk atomically.
    pub fn commit(&mut self) -> Result<(), Error> {
        if self.read_only {
            return Err(err_msg(
                "client attempted to commit with a read-only accessor",
            ));
        }

        let mut store_manager = self.store_manager.lock();
        let mut fields_updated = self.fields_updated.lock();
        let mut old_values: HashMap<String, Option<Value>> = HashMap::new();

        // Iterate over each key updated. For each key, record the current value and then set or
        // delete it as appropriate. If an error is encountered, break out of the loop and use the
        // values we just recorded to undo the changes we made.

        let commit_result = (|| {
            for (key, o_val) in fields_updated.iter() {
                {
                    let o_old_value = store_manager.get_value(&self.client_name, key);
                    let old_value = match o_old_value {
                        Some(val_ref) => Some(store::clone_value(val_ref)?),
                        None => None,
                    };
                    old_values.insert(key.clone(), old_value);
                }

                match o_val {
                    Some(val) => store_manager.set_value(
                        &self.client_name,
                        key.clone(),
                        store::clone_value(val)?,
                    )?,
                    None => store_manager.delete_value(&self.client_name, key)?,
                }
            }
            Ok(())
        })();
        fields_updated.clear();
        if commit_result.is_ok() {
            return commit_result;
        }
        // There was a problem. Revert the changes.
        for (key, o_val) in old_values.iter() {
            match o_val {
                // TODO: on an error here, should we bail out immediately (current response),
                // or continue and attempt to revert as many changes as we can? Does a
                // partially-committed accessor mean that the store is now corrupted?
                Some(val) => {
                    store_manager.set_value(
                        &self.client_name,
                        key.clone(),
                        store::clone_value(val)?,
                    )?;
                }
                None => {
                    store_manager.delete_value(&self.client_name, key)?;
                }
            }
        }
        return commit_result;
    }
}

// A helper function for use in list_prefix and get_prefix
fn field_has_been_deleted<T>(f: Option<&Option<T>>) -> bool {
    match f {
        // The key has not been updated, keep it
        None => true,
        // The key has been marked for removal, drop it
        Some(None) => false,
        // The key has been updated to a new value, keep it
        Some(Some(_)) => true,
    }
}

#[cfg(test)]
mod tests {
    use crate::accessor::*;
    use crate::store;
    use fidl::client::QueryResponseFut;
    use fidl::endpoints::create_proxy;
    use fidl_fuchsia_stash::{ListItem, Value};
    use fuchsia_async as fasync;
    use parking_lot::Mutex;
    use std::sync::Arc;
    use tempfile::TempDir;

    fn get_tmp_store_manager(tmp_dir: &TempDir) -> store::StoreManager {
        store::StoreManager::new(tmp_dir.path().join("stash.store")).unwrap()
    }

    async fn drain_stash_iterator<T, F>(mut f: F) -> Vec<T>
    where
        F: FnMut() -> QueryResponseFut<Vec<T>>
    {
        let mut res = Vec::new();
        loop {
            let mut subset = await!(f()).unwrap();
            if subset.len() == 0 {
                break;
            }
            res.append(&mut subset);
        }
        res
    }

    #[test]
    fn test_get_value() {
        let test_client_name = "test_client".to_string();
        let test_key = "test_key".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        sm.lock()
            .set_value(&test_client_name, test_key.clone(), Value::Boolval(true))
            .unwrap();

        let fetched_val = acc.get_value(&test_key).unwrap();
        assert_eq!(Some(Value::Boolval(true)), fetched_val);
        assert_eq!(0, acc.fields_updated.lock().len());
    }

    #[test]
    fn test_set_value() {
        let test_client_name = "test_client".to_string();
        let test_key = "test_key".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        let test_val = Value::Boolval(true);
        let expected_val = Value::Boolval(true);

        acc.set_value(test_key.clone(), test_val).unwrap();
        assert_eq!(None, sm.lock().get_value(&test_client_name, &test_key));
        assert_eq!(1, acc.fields_updated.lock().len());
        assert_eq!(
            &Some(expected_val),
            acc.fields_updated.lock().get(&test_key).unwrap()
        );

        assert_eq!(None, sm.lock().get_value(&test_client_name, &test_key));

        acc.commit().unwrap();
        assert_eq!(
            &Value::Boolval(true),
            sm.lock().get_value(&test_client_name, &test_key).unwrap()
        );
        assert_eq!(0, acc.fields_updated.lock().len());
    }

    #[test]
    fn test_delete_value() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        sm.lock()
            .set_value(&test_client_name, "a".to_string(), Value::Boolval(true))
            .unwrap();

        assert_eq!(0, acc.fields_updated.lock().len());
        acc.delete_value("b".to_string()).unwrap();
        assert_eq!(1, acc.fields_updated.lock().len());

        acc.delete_value("a".to_string()).unwrap();
        assert_eq!(2, acc.fields_updated.lock().len());
        assert_eq!(&None, acc.fields_updated.lock().get("a").unwrap());

        acc.delete_value("a".to_string()).unwrap();
        assert_eq!(2, acc.fields_updated.lock().len());
        assert_eq!(&None, acc.fields_updated.lock().get("a").unwrap());

        assert_eq!(
            Some(&Value::Boolval(true)),
            sm.lock().get_value(&test_client_name, &"a".to_string())
        );

        acc.commit().unwrap();
        assert_eq!(
            None,
            sm.lock().get_value(&test_client_name, &"a".to_string())
        );
        assert_eq!(0, acc.fields_updated.lock().len());
    }

    #[test]
    fn test_list_prefix() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        let mut executor = fasync::Executor::new().unwrap();

        for key in vec!["a", "a/a", "a/b", "a/a/b", "b", "b/c", "bbbbb"] {
            sm.lock()
                .set_value(&test_client_name, key.to_string(), Value::Boolval(true))
                .unwrap();
        }

        let mut run_test = |prefix: &str, mut expected: Vec<&str>| {
            let (list_iterator, server_end) = create_proxy().unwrap();
            acc.list_prefix(prefix.to_string(), server_end);

            let actual = executor.run_singlethreaded(drain_stash_iterator(|| list_iterator.get_next()));
            let mut actual : Vec<String> = actual.iter().map(|li| li.key.clone()).collect();

            expected.sort_unstable();
            actual.sort_unstable();
            assert_eq!(expected, actual);
        };

        run_test("", vec!["a", "a/a", "a/b", "a/a/b", "b", "b/c", "bbbbb"]);
        run_test("a", vec!["a", "a/a", "a/b", "a/a/b"]);
        run_test("a/a", vec!["a/a", "a/a/b"]);
        run_test("b", vec!["b", "b/c", "bbbbb"]);
        run_test("bb", vec!["bbbbb"]);
        run_test("c", vec![]);
    }

    #[test]
    fn test_list_prefix_paging() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        let mut executor = fasync::Executor::new().unwrap();

        for key in vec!["a", "a/a", "a/b", "a/a/b", "b", "b/c", "bbbbb"] {
            sm.lock()
                .set_value(&test_client_name, key.to_string(), Value::Boolval(true))
                .unwrap();
        }

        let mut run_test = |prefix: &str, mut expected: Vec<&str>| {
            let (list_iterator, server_end) = create_proxy().unwrap();
            acc.list_prefix(prefix.to_string(), server_end);

            let actual = executor.run_singlethreaded(drain_stash_iterator(|| list_iterator.get_next()));
            let mut actual : Vec<String> = actual.iter().map(|li| li.key.clone()).collect();

            expected.sort_unstable();
            actual.sort_unstable();
            assert_eq!(expected, actual);
        };

        run_test("", vec!["a", "a/a", "a/b", "a/a/b", "b", "b/c", "bbbbb"]);
        run_test("a", vec!["a", "a/a", "a/b", "a/a/b"]);
        run_test("a/a", vec!["a/a", "a/a/b"]);
        run_test("b", vec!["b", "b/c", "bbbbb"]);
        run_test("bb", vec!["bbbbb"]);
        run_test("c", vec![]);
    }

    #[test]
    fn test_get_prefix() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        let mut executor = fasync::Executor::new().unwrap();

        for (key, val) in vec![
            ("a", true),
            ("a/a", true),
            ("a/b", false),
            ("a/a/b", false),
            ("b", false),
            ("b/c", true),
            ("bbbbb", true),
        ] {
            sm.lock()
                .set_value(&test_client_name, key.to_string(), Value::Boolval(val))
                .unwrap();
        }

        let mut run_test = |prefix: &str, mut expected: Vec<(String, bool)>| {
            let (get_iterator, server_end) = create_proxy().unwrap();
            acc.get_prefix(prefix.to_string(), server_end).unwrap();

            let mut actual = Vec::new();
            loop {
                let subset = executor
                    .run_singlethreaded(get_iterator.get_next())
                    .unwrap();
                if subset.len() == 0 {
                    break;
                }
                for kv in subset.iter() {
                    let b = match kv.val {
                        Value::Boolval(b) => b,
                        _ => panic!("internal test error"),
                    };
                    actual.push((kv.key.clone(), b));
                }
            }
            expected.sort_unstable();
            actual.sort_unstable();
            assert_eq!(expected, actual);
        };

        run_test(
            "",
            vec![
                ("a".to_string(), true),
                ("a/a".to_string(), true),
                ("a/b".to_string(), false),
                ("a/a/b".to_string(), false),
                ("b".to_string(), false),
                ("b/c".to_string(), true),
                ("bbbbb".to_string(), true),
            ],
        );
        run_test(
            "a",
            vec![
                ("a".to_string(), true),
                ("a/a".to_string(), true),
                ("a/b".to_string(), false),
                ("a/a/b".to_string(), false),
            ],
        );
        run_test(
            "a/a",
            vec![("a/a".to_string(), true), ("a/a/b".to_string(), false)],
        );
        run_test(
            "b",
            vec![
                ("b".to_string(), false),
                ("b/c".to_string(), true),
                ("bbbbb".to_string(), true),
            ],
        );
        run_test("bb", vec![("bbbbb".to_string(), true)]);
        run_test("c", vec![]);
    }

    #[test]
    fn test_delete_prefix() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        let mut executor = fasync::Executor::new().unwrap();

        for key in vec!["a", "a/a", "a/b", "a/a/b", "b", "b/c", "bbbbb"] {
            sm.lock()
                .set_value(&test_client_name, key.to_string(), Value::Boolval(true))
                .unwrap();
        }

        acc.delete_prefix("a".to_string()).unwrap();
        assert_eq!(&None, acc.fields_updated.lock().get("a").unwrap());
        assert_eq!(&None, acc.fields_updated.lock().get("a/a").unwrap());
        assert_eq!(&None, acc.fields_updated.lock().get("a/b").unwrap());
        assert_eq!(&None, acc.fields_updated.lock().get("a/a/b").unwrap());
        assert_eq!(4, acc.fields_updated.lock().len());
        acc.commit().unwrap();

        let (list_iterator, server_end) = create_proxy().unwrap();
        acc.list_prefix("".to_string(), server_end);

        let mut actual = Vec::new();
        loop {
            let subset = executor
                .run_singlethreaded(list_iterator.get_next())
                .unwrap();
            if subset.len() == 0 {
                break;
            }
            for ListItem { key, type_: _ } in subset.iter() {
                actual.push(key.clone());
            }
        }
        actual.sort_unstable();
        assert_eq!(vec!["b", "b/c", "bbbbb"], actual);
    }

    #[test]
    fn test_commit_different_accessors() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc1 = Accessor::new(sm.clone(), true, false, test_client_name.clone());
        let mut acc2 = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        {
            let mut sm = sm.lock();
            sm.set_value(&test_client_name, "a".to_string(), Value::Boolval(true))
                .unwrap();
            sm.set_value(&test_client_name, "b".to_string(), Value::Boolval(false))
                .unwrap();
        }

        acc1.get_value("a").unwrap();
        acc1.set_value("b".to_string(), Value::Boolval(true))
            .unwrap();

        acc1.commit().unwrap();

        assert_eq!(
            &Value::Boolval(true),
            sm.lock()
                .get_value(&test_client_name, &"b".to_string())
                .unwrap()
        );

        acc2.get_value("a").unwrap();
        acc2.delete_value("b".to_string()).unwrap();

        acc2.commit().unwrap();

        assert_eq!(
            None,
            sm.lock().get_value(&test_client_name, &"b".to_string())
        );

        // Confirm that the fields touched and fields updated maps have been cleared
        assert_eq!(0, acc1.fields_updated.lock().len());
        assert_eq!(0, acc2.fields_updated.lock().len());
    }

    #[test]
    fn test_commit_reuse_accessor() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc1 = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        {
            let mut sm = sm.lock();
            sm.set_value(&test_client_name, "a".to_string(), Value::Boolval(true))
                .unwrap();
            sm.set_value(&test_client_name, "b".to_string(), Value::Boolval(false))
                .unwrap();
        }

        acc1.get_value("a").unwrap();
        acc1.set_value("b".to_string(), Value::Boolval(true))
            .unwrap();

        acc1.commit().unwrap();
        assert_eq!(0, acc1.fields_updated.lock().len());

        assert_eq!(
            &Value::Boolval(true),
            sm.lock()
                .get_value(&test_client_name, &"b".to_string())
                .unwrap()
        );

        acc1.get_value("a").unwrap();
        acc1.delete_value("b".to_string()).unwrap();

        acc1.commit().unwrap();
        assert_eq!(0, acc1.fields_updated.lock().len());

        assert_eq!(
            None,
            sm.lock().get_value(&test_client_name, &"b".to_string())
        );
    }

    #[test]
    fn test_delete_prefix_considers_current_commit() {
        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        for key in vec!["a", "aa", "aaa"] {
            acc.set_value(key.to_owned(), Value::Boolval(true)).unwrap();
        }
        acc.commit().unwrap();
        for key in vec!["b", "bb", "bbb"] {
            acc.set_value(key.to_owned(), Value::Boolval(true)).unwrap();
        }

        acc.delete_prefix("".to_string()).unwrap();

        let mut keys : Vec<String> = acc.fields_updated.lock().keys().map(|s| s.clone()).collect();
        keys.sort_unstable();
        assert_eq!(vec!["a", "aa", "aaa", "b", "bb" ,"bbb"], keys);
        for key in vec!["a", "aa", "aaa", "b", "bb" ,"bbb"] {
            assert_eq!(Some(&None), acc.fields_updated.lock().get(key));
        }
    }

    #[test]
    fn test_list_prefix_considers_current_commit() {
        let mut executor = fasync::Executor::new().unwrap();

        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        for key in vec!["a", "aa", "aaa"] {
            acc.set_value(key.to_owned(), Value::Boolval(true)).unwrap();
        }
        acc.commit().unwrap();
        for key in vec!["b", "bb", "bbb"] {
            acc.set_value(key.to_owned(), Value::Boolval(true)).unwrap();
        }

        let (list_iterator, server_end) = create_proxy().unwrap();
        acc.list_prefix("".to_string(), server_end);

        let res = executor.run_singlethreaded(drain_stash_iterator(|| list_iterator.get_next()));
        let mut res : Vec<String> = res.iter().map(|li| li.key.clone()).collect();

        res.sort_unstable();
        assert_eq!(vec!["a", "aa", "aaa", "b", "bb" ,"bbb"], res);
    }

    #[test]
    fn test_get_prefix_considers_current_commit() {
        let mut executor = fasync::Executor::new().unwrap();

        let test_client_name = "test_client".to_string();

        let tmp_dir = TempDir::new().unwrap();
        let sm = Arc::new(Mutex::new(get_tmp_store_manager(&tmp_dir)));
        let mut acc = Accessor::new(sm.clone(), true, false, test_client_name.clone());

        for key in vec!["a", "aa", "aaa"] {
            acc.set_value(key.to_owned(), Value::Boolval(true)).unwrap();
        }
        acc.commit().unwrap();
        for key in vec!["b", "bb", "bbb"] {
            acc.set_value(key.to_owned(), Value::Boolval(true)).unwrap();
        }

        let (get_iterator, server_end) = create_proxy().unwrap();
        acc.get_prefix("".to_string(), server_end).unwrap();

        let res = executor.run_singlethreaded(drain_stash_iterator(|| get_iterator.get_next()));
        let mut res : Vec<String> = res.iter().map(|kv| kv.key.clone()).collect();

        res.sort_unstable();
        assert_eq!(vec!["a", "aa", "aaa", "b", "bb" ,"bbb"], res);
    }
}
