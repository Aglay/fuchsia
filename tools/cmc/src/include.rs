// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::error::Error,
    crate::merge::merge_json,
    crate::util::{json_or_json5_from_file, write_depfile},
    serde_json::Value,
    std::{
        collections::HashSet,
        fs,
        io::{BufRead, BufReader, Write},
        iter::FromIterator,
        path::PathBuf,
    },
};

/// Read in the provided JSON file and add includes.
/// If the JSON file is an object with a key "include" that references an array of strings then
/// the strings are treated as paths to JSON files to be merged with the input file.
/// Returns any includes encountered.
/// If a depfile is provided, also writes includes encountered to the depfile.
pub fn merge_includes(
    file: &PathBuf,
    output: Option<&PathBuf>,
    depfile: Option<&PathBuf>,
    includepath: &PathBuf,
) -> Result<(), Error> {
    let includes = transitive_includes(&file, &includepath)?;
    let mut v: Value = json_or_json5_from_file(&file)?;
    v.as_object_mut().and_then(|v| v.remove("include"));

    for include in &includes {
        let path = includepath.join(&include);
        let mut includev: Value = json_or_json5_from_file(&path).map_err(|e| {
            Error::parse(
                format!("Couldn't read include {}: {}", &path.display(), e),
                None,
                Some(&file),
            )
        })?;
        includev.as_object_mut().and_then(|v| v.remove("include"));
        merge_json(&mut v, &includev).map_err(|e| {
            Error::parse(format!("Failed to merge with {:?}: {}", path, e), None, Some(&file))
        })?;
    }

    // Write postprocessed JSON
    if let Some(output_path) = output.as_ref() {
        fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(output_path)?
            .write_all(format!("{:#}", v).as_bytes())?;
    } else {
        println!("{:#}", v);
    }

    // Write includes to depfile
    if let Some(depfile_path) = depfile {
        write_depfile(depfile_path, output, &includes, includepath)?;
    }

    Ok(())
}

const CHECK_INCLUDES_URL: &str =
    "https://fuchsia.dev/fuchsia-src/development/components/build#component-manifest-includes";

/// Read in the provided JSON file and ensure that it contains all expected includes.
pub fn check_includes(
    file: &PathBuf,
    mut expected_includes: Vec<String>,
    // If specified, this is a path to newline-delimited `expected_includes`
    fromfile: Option<&PathBuf>,
    depfile: Option<&PathBuf>,
    stamp: Option<&PathBuf>,
    includepath: &PathBuf,
) -> Result<(), Error> {
    if let Some(path) = fromfile {
        let reader = BufReader::new(fs::File::open(path)?);
        for line in reader.lines() {
            match line {
                Ok(value) => expected_includes.push(String::from(value)),
                Err(e) => return Err(Error::invalid_args(format!("Invalid --fromfile: {}", e))),
            }
        }
    }
    if expected_includes.is_empty() {
        if let Some(depfile_path) = depfile {
            if depfile_path.exists() {
                // Delete stale depfile
                fs::remove_file(depfile_path)?;
            }
        }
        return Ok(());
    }

    let actual = transitive_includes(&file, &includepath)?;
    for expected in expected_includes {
        if !actual.contains(&expected) {
            return Err(Error::Validate {
                schema_name: None,
                err: format!(
                    "{:?} must include {}.\nSee: {}",
                    &file, &expected, CHECK_INCLUDES_URL
                ),
                filename: file.to_str().map(String::from),
            });
        }
    }

    // Write includes to depfile
    if let Some(depfile_path) = depfile {
        write_depfile(depfile_path, stamp, &actual, includepath)?;
    }

    Ok(())
}

/// Returns all includes of a file relative to `includepath`.
/// Follows transitive includes.
/// Detects cycles.
/// Includes are returned in sorted order.
pub fn transitive_includes(file: &PathBuf, includepath: &PathBuf) -> Result<Vec<String>, Error> {
    fn helper(
        file: &PathBuf,
        includepath: &PathBuf,
        doc: &Value,
        entered: &mut HashSet<String>,
        exited: &mut HashSet<String>,
    ) -> Result<(), Error> {
        if let Some(includes) = doc.get("include").and_then(|v| v.as_array()) {
            for include in includes.into_iter().filter_map(|v| v.as_str().map(String::from)) {
                // Avoid visiting the same include more than once
                if !entered.insert(include.clone()) {
                    if !exited.contains(&include) {
                        return Err(Error::parse(
                            format!("Includes cycle at {}", include),
                            None,
                            Some(&file),
                        ));
                    }
                } else {
                    let path = includepath.join(&include);
                    let include_doc = json_or_json5_from_file(&path).map_err(|e| {
                        Error::parse(
                            format!("Couldn't read include {}: {}", &path.display(), e),
                            None,
                            Some(&file),
                        )
                    })?;
                    helper(&file, &includepath, &include_doc, entered, exited)?;
                    exited.insert(include);
                }
            }
        }
        Ok(())
    }

    let root = json_or_json5_from_file(&file)?;
    let mut entered = HashSet::new();
    let mut exited = HashSet::new();
    helper(&file, &includepath, &root, &mut entered, &mut exited)?;
    let mut includes = Vec::from_iter(exited);
    includes.sort();
    Ok(includes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use matches::assert_matches;
    use serde_json::json;
    use std::fmt::Display;
    use std::fs::File;
    use std::io::{LineWriter, Read};
    use tempfile::TempDir;

    fn tmp_file(tmp_dir: &TempDir, name: &str, contents: impl Display) -> PathBuf {
        let path = tmp_dir.path().join(name);
        File::create(tmp_dir.path().join(name))
            .unwrap()
            .write_all(format!("{:#}", contents).as_bytes())
            .unwrap();
        return path;
    }

    fn assert_eq_file(file: PathBuf, contents: impl Display) {
        let mut out = String::new();
        File::open(file).unwrap().read_to_string(&mut out).unwrap();
        assert_eq!(out, format!("{:#}", contents));
    }

    #[test]
    fn test_include_cmx() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": ["shard.cmx"],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(
            &tmp_dir,
            "shard.cmx",
            json!({
                "sandbox": {
                    "services": ["fuchsia.foo.Bar"]
                }
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let cmx_depfile_path = tmp_dir.path().join("cmx.d");
        merge_includes(&cmx_path, Some(&out_cmx_path), Some(&cmx_depfile_path), &include_path)
            .unwrap();

        assert_eq_file(
            out_cmx_path,
            json!({
                "program": {
                    "binary": "bin/hello_world"
                },
                "sandbox": {
                    "services": ["fuchsia.foo.Bar"]
                }
            }),
        );
        let mut deps = String::new();
        File::open(&cmx_depfile_path).unwrap().read_to_string(&mut deps).unwrap();
        assert_eq!(
            deps,
            format!("{tmp}/out.cmx: {tmp}/shard.cmx\n", tmp = tmp_dir.path().display())
        );
    }

    #[test]
    fn test_include_cml() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cml_path = tmp_file(
            &tmp_dir,
            "some.cml",
            "{include: [\"shard.cml\"], program: {binary: \"bin/hello_world\"}}",
        );
        tmp_file(&tmp_dir, "shard.cml", "{use: [{ protocol: [\"fuchsia.foo.Bar\"]}]}");

        let out_cml_path = tmp_dir.path().join("out.cml");
        let cml_depfile_path = tmp_dir.path().join("cml.d");
        merge_includes(&cml_path, Some(&out_cml_path), Some(&cml_depfile_path), &include_path)
            .unwrap();

        assert_eq_file(
            out_cml_path,
            json!({
                "program": {
                    "binary": "bin/hello_world"
                },
                "use": [{
                    "protocol": ["fuchsia.foo.Bar"]
                }]
            }),
        );
        let mut deps = String::new();
        File::open(&cml_depfile_path).unwrap().read_to_string(&mut deps).unwrap();
        assert_eq!(
            deps,
            format!("{tmp}/out.cml: {tmp}/shard.cml\n", tmp = tmp_dir.path().display())
        );
    }

    #[test]
    fn test_include_multiple_shards() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": ["shard1.cmx", "shard2.cmx"],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(
            &tmp_dir,
            "shard1.cmx",
            json!({
                "sandbox": {
                    "services": ["fuchsia.foo.Bar"]
                }
            }),
        );
        tmp_file(
            &tmp_dir,
            "shard2.cmx",
            json!({
                "sandbox": {
                    "services": ["fuchsia.foo.Qux"]
                }
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let cmx_depfile_path = tmp_dir.path().join("cmx.d");
        merge_includes(&cmx_path, Some(&out_cmx_path), Some(&cmx_depfile_path), &include_path)
            .unwrap();

        assert_eq_file(
            out_cmx_path,
            json!({
                "program": {
                    "binary": "bin/hello_world"
                },
                "sandbox": {
                    "services": ["fuchsia.foo.Bar", "fuchsia.foo.Qux"]
                }
            }),
        );
        let mut deps = String::new();
        File::open(&cmx_depfile_path).unwrap().read_to_string(&mut deps).unwrap();
        assert_eq!(
            deps,
            format!(
                "{tmp}/out.cmx: {tmp}/shard1.cmx {tmp}/shard2.cmx\n",
                tmp = tmp_dir.path().display()
            )
        );
    }

    #[test]
    fn test_include_recursively() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": ["shard1.cmx"],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(
            &tmp_dir,
            "shard1.cmx",
            json!({
                "include": ["shard2.cmx"],
                "sandbox": {
                    "services": ["fuchsia.foo.Bar"]
                }
            }),
        );
        tmp_file(
            &tmp_dir,
            "shard2.cmx",
            json!({
                "sandbox": {
                    "services": ["fuchsia.foo.Qux"]
                }
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let cmx_depfile_path = tmp_dir.path().join("cmx.d");
        merge_includes(&cmx_path, Some(&out_cmx_path), Some(&cmx_depfile_path), &include_path)
            .unwrap();

        assert_eq_file(
            out_cmx_path,
            json!({
                "program": {
                    "binary": "bin/hello_world"
                },
                "sandbox": {
                    "services": ["fuchsia.foo.Bar", "fuchsia.foo.Qux"]
                }
            }),
        );
        let mut deps = String::new();
        File::open(&cmx_depfile_path).unwrap().read_to_string(&mut deps).unwrap();
        assert_eq!(
            deps,
            format!(
                "{tmp}/out.cmx: {tmp}/shard1.cmx {tmp}/shard2.cmx\n",
                tmp = tmp_dir.path().display()
            )
        );
    }

    #[test]
    fn test_include_nothing() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": [],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let cmx_depfile_path = tmp_dir.path().join("cmx.d");
        merge_includes(&cmx_path, Some(&out_cmx_path), Some(&cmx_depfile_path), &include_path)
            .unwrap();

        assert_eq_file(
            out_cmx_path,
            json!({
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        assert_eq!(cmx_depfile_path.exists(), false);
    }

    #[test]
    fn test_no_includes() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let cmx_depfile_path = tmp_dir.path().join("cmx.d");
        merge_includes(&cmx_path, Some(&out_cmx_path), Some(&cmx_depfile_path), &include_path)
            .unwrap();

        assert_eq_file(
            out_cmx_path,
            json!({
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        assert_eq!(cmx_depfile_path.exists(), false);
    }

    #[test]
    fn test_invalid_include() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": ["doesnt_exist.cmx"],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let cmx_depfile_path = tmp_dir.path().join("cmx.d");
        let result =
            merge_includes(&cmx_path, Some(&out_cmx_path), Some(&cmx_depfile_path), &include_path);

        assert_matches!(result, Err(Error::Parse { err, .. })
                        if err.starts_with("Couldn't read include ") && err.contains("doesnt_exist.cmx"));
    }

    #[test]
    fn test_include_detect_cycle() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some1.cmx",
            json!({
                "include": ["some2.cmx"],
            }),
        );
        tmp_file(
            &tmp_dir,
            "some2.cmx",
            json!({
                "include": ["some1.cmx"],
            }),
        );

        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let result = merge_includes(&cmx_path, Some(&out_cmx_path), None, &include_path);
        assert_matches!(result, Err(Error::Parse { err, .. }) if err.contains("Includes cycle"));
    }

    #[test]
    fn test_include_a_diamond_is_not_a_cycle() {
        //   A
        //  / \
        // B   C
        //  \ /
        //   D
        // The above is fine.
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let a_path = tmp_file(
            &tmp_dir,
            "a.cmx",
            json!({
                "include": ["b.cmx", "c.cmx"],
            }),
        );
        tmp_file(
            &tmp_dir,
            "b.cmx",
            json!({
                "include": ["d.cmx"],
            }),
        );
        tmp_file(
            &tmp_dir,
            "c.cmx",
            json!({
                "include": ["d.cmx"],
            }),
        );
        tmp_file(&tmp_dir, "d.cmx", json!({}));
        let out_cmx_path = tmp_dir.path().join("out.cmx");
        let result = merge_includes(&a_path, Some(&out_cmx_path), None, &include_path);
        assert_matches!(result, Ok(()));
    }

    #[test]
    fn test_expect_nothing() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx1_path = tmp_file(
            &tmp_dir,
            "some1.cmx",
            json!({
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        let check_depfile_path = tmp_dir.path().join("check.d");
        let stamp_path = tmp_dir.path().join("stamp");
        assert_matches!(
            check_includes(
                &cmx1_path,
                vec![],
                None,
                Some(&check_depfile_path),
                Some(&stamp_path),
                &include_path
            ),
            Ok(())
        );
        // Don't generate depfile (or delete existing) if no includes found
        assert_eq!(false, check_depfile_path.exists());

        let cmx2_path = tmp_file(
            &tmp_dir,
            "some2.cmx",
            json!({
                "include": [],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        assert_matches!(
            check_includes(&cmx2_path, vec![], None, None, None, &include_path),
            Ok(())
        );

        let cmx3_path = tmp_file(
            &tmp_dir,
            "some3.cmx",
            json!({
                "include": [ "foo.cmx" ],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        assert_matches!(
            check_includes(&cmx3_path, vec![], None, None, None, &include_path),
            Ok(())
        );
    }

    #[test]
    fn test_expect_something_present() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": [ "foo.cmx", "bar.cmx" ],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(&tmp_dir, "foo.cmx", json!({}));
        tmp_file(&tmp_dir, "bar.cmx", json!({}));
        let check_depfile_path = tmp_dir.path().join("check.d");
        let stamp_path = tmp_dir.path().join("stamp");
        assert_matches!(
            check_includes(
                &cmx_path,
                vec!["bar.cmx".into()],
                None,
                Some(&check_depfile_path),
                Some(&stamp_path),
                &include_path
            ),
            Ok(())
        );
        let mut deps = String::new();
        File::open(&check_depfile_path).unwrap().read_to_string(&mut deps).unwrap();
        assert_eq!(
            deps,
            format!("{tmp}/stamp: {tmp}/bar.cmx {tmp}/foo.cmx\n", tmp = tmp_dir.path().display())
        );
    }

    #[test]
    fn test_expect_something_missing() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx1_path = tmp_file(
            &tmp_dir,
            "some1.cmx",
            json!({
                "include": [ "foo.cmx", "bar.cmx" ],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(&tmp_dir, "foo.cmx", json!({}));
        tmp_file(&tmp_dir, "bar.cmx", json!({}));
        assert_matches!(check_includes(&cmx1_path, vec!["qux.cmx".into()], None, None, None, &include_path),
                        Err(Error::Validate { filename, .. }) if filename == cmx1_path.to_str().map(String::from));

        let cmx2_path = tmp_file(
            &tmp_dir,
            "some2.cmx",
            json!({
                // No includes
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        assert_matches!(check_includes(&cmx2_path, vec!["qux.cmx".into()], None, None, None, &include_path),
                        Err(Error::Validate { filename, .. }) if filename == cmx2_path.to_str().map(String::from));
    }

    #[test]
    fn test_expect_something_transitive() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": [ "foo.cmx" ],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(&tmp_dir, "foo.cmx", json!({"include": [ "bar.cmx" ]}));
        tmp_file(&tmp_dir, "bar.cmx", json!({}));
        assert_matches!(
            check_includes(&cmx_path, vec!["bar.cmx".into()], None, None, None, &include_path),
            Ok(())
        );
    }

    #[test]
    fn test_expect_fromfile() {
        let tmp_dir = TempDir::new().unwrap();
        let include_path = tmp_dir.path().to_path_buf();
        let cmx_path = tmp_file(
            &tmp_dir,
            "some.cmx",
            json!({
                "include": [ "foo.cmx", "bar.cmx" ],
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );
        tmp_file(&tmp_dir, "foo.cmx", json!({}));
        tmp_file(&tmp_dir, "bar.cmx", json!({}));

        let fromfile_path = tmp_dir.path().join("fromfile");
        let mut fromfile = LineWriter::new(File::create(fromfile_path.clone()).unwrap());
        writeln!(fromfile, "foo.cmx").unwrap();
        writeln!(fromfile, "bar.cmx").unwrap();
        assert_matches!(
            check_includes(&cmx_path, vec![], Some(&fromfile_path), None, None, &include_path),
            Ok(())
        );

        // Add another include that's missing
        writeln!(fromfile, "qux.cmx").unwrap();
        assert_matches!(check_includes(&cmx_path, vec![], Some(&fromfile_path), None, None, &include_path),
                        Err(Error::Validate { filename, .. }) if filename == cmx_path.to_str().map(String::from));
    }
}
