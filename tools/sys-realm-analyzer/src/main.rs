// Copyright 2021 The Fuchsia Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    argh::FromArgs,
    serde::Deserialize,
    serde::Serialize,
    serde_json,
    std::{
        collections::HashMap,
        io::{self, Write},
        process::Command,
    },
};

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct BuildTarget {
    pub board: String,
    pub arch: String,
}

#[derive(Debug, Clone, Deserialize, Serialize, Eq, PartialOrd, PartialEq, Ord)]
struct ComponentManifest {
    pub url: String,
    pub manifest: String,
    pub features: ManifestContent,
}

#[derive(Debug, Deserialize, Clone, Serialize, Eq, PartialEq, PartialOrd, Ord)]
struct ManifestContent {
    features: Option<Vec<String>>,
}

#[derive(Debug, FromArgs)]
/// Performs one or more builds based the on the arguments provided and uses
/// the `scrutiny` tool via `fx` to get information about the v1 sys realm of
/// each build. The tool does further analysis to determine how many components
/// use each v1 component feature.
struct Options {
    #[argh(option, default = "\"fx\".to_string()")]
    /// path for the `fx` command
    fx_path: String,

    #[argh(option)]
    /// the architectures to compose products from, if specified, '--boards' must also be provided
    arches: Option<String>,

    #[argh(option)]
    /// the boards to compose products from, if specified, '--arches' must also be provided
    boards: Option<String>,

    #[argh(option)]
    /// the products to inspect,
    products: Option<String>,
}

fn main() -> Result<(), u8> {
    let args: Options = argh::from_env();
    let targets = {
        let mut composed_products = get_target_sets(args.arches.as_ref(), args.boards.as_ref());
        composed_products.append(&mut get_target_products(args.products.as_ref()));
        if composed_products.is_empty() {
            composed_products.append(&mut get_default_build_targets());
        }
        composed_products
    };

    if targets.is_empty() {
        println!("No target products specified if architectures or boards are specified, make sure the other is as well.");
        return Ok(());
    } else {
        println!("Selected targets: {:?}", targets);
    }

    let fx_path = &args.fx_path;
    for t in targets {
        println!("{}.{}", t.board, t.arch);
        let out_dir = format!("out/{}-{}", t.board, t.arch);
        let cmd_out = Command::new(fx_path)
            .arg("--dir")
            .arg(out_dir.clone())
            .arg("set")
            .arg(format!("{}.{}", t.board, t.arch))
            .arg("--release")
            .arg("--with")
            .arg("//src/security/scrutiny")
            .output()
            .expect("Snap! `fx set` failed");

        match cmd_out.status.code().as_ref() {
            Some(0) => {
                println!("Successfully configured build for {}.{}", t.board, t.arch);
            }
            _ => {
                println!(
                    "Failed to configure build for {}.{}:\n{}",
                    t.board,
                    t.arch,
                    String::from_utf8_lossy(&cmd_out.stderr)
                );
                return Err(1);
            }
        }

        let build_cmd = Command::new(fx_path)
            .arg("build")
            .arg("build/images:config-data")
            .output()
            .expect("build failed");

        if build_cmd.status.code().unwrap() != 0 {
            println!("Build failed! {}", build_cmd.status.code().unwrap());
        }

        let mut scrutiny_cmd = Command::new(fx_path);
        scrutiny_cmd.arg("scrutiny").arg("-c").arg("sys.realm");

        let scrutiny_result = scrutiny_cmd.output().expect("getting command output failed");
        let out_str = String::from_utf8_lossy(&scrutiny_result.stdout);
        let component_list = serde_json::from_str::<'_, Vec<ComponentManifest>>(&out_str).unwrap();
        let mut index = group_by_feature(&component_list);
        output_result(&component_list, &mut index, io::stdout());
    }

    Ok(())
}

fn group_by_feature<'a>(
    component_list: &'a Vec<ComponentManifest>,
) -> HashMap<Option<String>, Vec<&'a ComponentManifest>> {
    let mut index = HashMap::<Option<String>, Vec<&ComponentManifest>>::new();

    for component in component_list {
        match &component.features.features {
            Some(features) => {
                if features.len() == 0 {
                    index.entry(None).or_insert_with(|| vec![]).push(component);
                } else {
                    for feature in features {
                        index
                            .entry(Some(feature.clone()))
                            .or_insert_with(|| vec![])
                            .push(component);
                    }
                }
            }
            None => {
                index.entry(None).or_insert_with(|| vec![]).push(component);
            }
        }
    }

    index
}

fn output_result<T: Write>(
    list: &Vec<ComponentManifest>,
    index: &mut HashMap<Option<String>, Vec<&ComponentManifest>>,
    mut stdout: T,
) {
    stdout
        .write(format!("{} components in the sys realm with these URLs:\n", list.len()).as_bytes())
        .unwrap();
    let mut urls = list.iter().map(|manifest| manifest.url.clone()).collect::<Vec<String>>();
    urls.sort();
    for url in urls {
        stdout.write(format!("  {}\n", url).as_bytes()).unwrap();
    }

    stdout.write("\n\n".as_bytes()).unwrap();

    let mut keys: Vec<Option<String>> = index.keys().map(|k| k.clone()).collect();
    keys.sort();

    for k in keys {
        let matches = index.get_mut(&k).unwrap();
        match k {
            None => {
                stdout
                    .write(format!("{} components use no features\n", matches.len()).as_bytes())
                    .unwrap();
            }
            Some(key) => {
                stdout
                    .write(
                        format!("feature {} is used by {} components\n", key, matches.len())
                            .as_bytes(),
                    )
                    .unwrap();
            }
        }
        matches.sort();
        for component in matches {
            stdout.write(format!("  {}\n", component.url).as_bytes()).unwrap();
        }
    }
}

/// Given comma-delimited strings for processor architectures and boards,
/// return a vector with the cross product of these lists.
fn get_target_sets(arch_str: Option<&String>, board_str: Option<&String>) -> Vec<BuildTarget> {
    let arches = match arch_str {
        Some(arch_str) => arch_str.split(",").map(|a| a.trim().clone()).collect(),
        None => vec![],
    };
    let boards = match board_str {
        Some(board_str) => board_str.split(",").map(|b| b.trim().clone()).collect(),
        None => vec![],
    };

    let mut targets = vec![];
    for board in &boards {
        for arch in &arches {
            targets.push(BuildTarget { board: board.to_string(), arch: arch.to_string() });
        }
    }

    targets
}

/// Get a default set of build targets.
fn get_default_build_targets() -> Vec<BuildTarget> {
    let boards = vec!["workstation", "bringup", "core"];
    let arches = vec!["x64", "arm64"];

    let mut targets = vec![];
    for board in &boards {
        for arch in &arches {
            targets.push(BuildTarget { board: board.to_string(), arch: arch.to_string() });
        }
    }

    targets
}

/// Given a comma-delimited set of product targets, return a list of
/// `BuildTarget`s.
fn get_target_products(products_str: Option<&String>) -> Vec<BuildTarget> {
    let product_strs = match products_str {
        Some(products_str) => products_str.split(",").map(|b| b.trim().clone()).collect(),
        None => vec![],
    };
    let mut build_targets = vec![];

    for product in product_strs {
        let parts: Vec<&str> = product.split(".").collect();
        if parts.len() != 2 {
            panic!("Product string '{:?}' looks wrong.", parts);
        }
        build_targets.push(BuildTarget { board: parts[0].to_string(), arch: parts[1].to_string() });
    }
    build_targets
}

#[cfg(test)]
mod test {

    use {
        super::{
            get_target_products, get_target_sets, group_by_feature, BuildTarget, ComponentManifest,
            ManifestContent,
        },
        std::collections::HashMap,
    };

    #[test]
    fn test_feature_grouping() {
        let component_list = vec![
            ComponentManifest {
                url: String::from("foo"),
                manifest: String::from("empty"),
                features: ManifestContent { features: None },
            },
            ComponentManifest {
                url: String::from("bar"),
                manifest: String::from("empty"),
                features: ManifestContent { features: Some(vec![String::from("config-data")]) },
            },
        ];

        let mut expected: HashMap<Option<String>, Vec<&ComponentManifest>> = HashMap::new();
        expected.insert(None, vec![&component_list.get(0).unwrap()]);
        expected.insert(Some(String::from("config-data")), vec![&component_list.get(1).unwrap()]);
        assert_eq!(expected, group_by_feature(&component_list));

        let component_list = vec![
            ComponentManifest {
                url: String::from("foo"),
                manifest: String::from("empty"),
                features: ManifestContent { features: None },
            },
            ComponentManifest {
                url: String::from("bar"),
                manifest: String::from("empty"),
                features: ManifestContent { features: Some(vec![String::from("config-data")]) },
            },
            ComponentManifest {
                url: String::from("buzz"),
                manifest: String::from("empty"),
                features: ManifestContent {
                    features: Some(vec![
                        String::from("config-data"),
                        String::from("isolated-storage"),
                    ]),
                },
            },
        ];

        let mut expected: HashMap<Option<String>, Vec<&ComponentManifest>> = HashMap::new();
        expected.insert(None, vec![&component_list.get(0).unwrap()]);
        expected.insert(
            Some(String::from("config-data")),
            vec![&component_list.get(1).unwrap(), &component_list.get(2).unwrap()],
        );
        expected
            .insert(Some(String::from("isolated-storage")), vec![&component_list.get(2).unwrap()]);
        assert_eq!(expected, group_by_feature(&component_list));
    }

    #[test]
    fn test_product_targets() {
        let mut results = get_target_products(Some(&String::from("core.x64")));
        assert_eq!(
            vec![BuildTarget { arch: String::from("x64"), board: String::from("core") }],
            results
        );

        results = get_target_products(Some(&String::from("core.x64,workstation.arm64")));
        assert_eq!(
            vec![
                BuildTarget { arch: String::from("x64"), board: String::from("core") },
                BuildTarget { arch: String::from("arm64"), board: String::from("workstation") }
            ],
            results
        );

        results = get_target_products(None);
        assert_eq!(Vec::<BuildTarget>::new(), results);
    }

    #[test]
    #[should_panic]
    fn test_invalid_target() {
        get_target_products(Some(&String::from("core_x64")));
    }

    #[test]
    fn test_target_set() {
        let mut expected =
            vec![BuildTarget { arch: String::from("x64"), board: String::from("workstation") }];
        let mut results =
            get_target_sets(Some(&String::from("x64")), Some(&String::from("workstation")));
        assert_eq!(expected, results);

        expected = vec![
            BuildTarget { arch: String::from("x64"), board: String::from("workstation") },
            BuildTarget { arch: String::from("x64"), board: String::from("core") },
            BuildTarget { arch: String::from("arm64"), board: String::from("workstation") },
            BuildTarget { arch: String::from("x64"), board: String::from("core") },
        ];
        results = get_target_sets(
            Some(&String::from("x64,arm64")),
            Some(&String::from("workstation,core")),
        );
        assert_eq!(expected.sort(), results.sort());

        expected = vec![];
        results = get_target_sets(Some(&String::from("x64,arm64")), None);
        assert_eq!(expected, results);
    }
}
