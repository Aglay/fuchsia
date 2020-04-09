// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod template_helpers;
mod util;

use anyhow::{anyhow, bail};
use chrono::{Datelike, Utc};
use handlebars::Handlebars;
use serde::Serialize;
use std::collections::HashMap;
use std::path::{Component, Path, PathBuf};
use std::str::FromStr;
use std::{env, fmt, fs, io};
use structopt::StructOpt;
use tempfile::tempdir;
use termion::{color, style};

const LANG_AGNOSTIC_EXTENSION: &'static str = "tmpl";

fn main() -> Result<(), anyhow::Error> {
    let args = CreateArgs::from_args();
    if args.project_name.contains("_") {
        bail!("project-name cannot contain underscores");
    }
    let templates_dir_path = util::get_templates_dir_path()?;

    // Collect the template files for this project type and language.
    let template_tree = TemplateTree::from_json_file(
        &templates_dir_path.join("templates.json"),
        &args.project_type,
        &args.lang,
        &StdFs,
    )?;

    // Create the set of variables accessible to template files.
    let template_args = TemplateArgs::from_create_args(&args)?;

    // Register the template engine and execute the templates.
    let mut handlebars = Handlebars::new();
    handlebars.set_strict_mode(true);
    template_helpers::register_helpers(&mut handlebars);
    let project = template_tree.render(&mut handlebars, &template_args)?;

    // Write the rendered files to a temp directory.
    let dir = tempdir()?;
    let tmp_out_path = dir.path().join(&args.project_name);
    project.write(&tmp_out_path)?;

    // Rename the temp directory project to the final location.
    let dest_project_path = env::current_dir()?.join(&args.project_name);
    fs::rename(&tmp_out_path, &dest_project_path)?;

    println!("Project created at {}.", dest_project_path.to_string_lossy());

    // Find the parent BUILD.gn file and suggest adding the test target.
    let parent_build =
        dest_project_path.parent().map(|p| p.join("BUILD.gn")).filter(|b| b.exists());
    if let Some(parent_build) = parent_build {
        println!(
            "{}note:{} Don't forget to include the {}{}:tests{} GN target in the parent {}tests{} target ({}).",
            color::Fg(color::Yellow), color::Fg(color::Reset),
            style::Bold, &args.project_name, style::Reset,
            style::Bold, style::Reset,
            parent_build.to_string_lossy()
        );
    }

    Ok(())
}

#[derive(Debug, StructOpt)]
#[structopt(name = "fx-create", about = "Creates scaffolding for new projects.")]
struct CreateArgs {
    /// The type of project to create.
    ///
    /// This can be one of:
    ///
    /// - component-v2: A V2 component launched with Component Manager,
    #[structopt(name = "project-type")]
    project_type: String,

    /// The name of the new project.
    ///
    /// This will be the name of the GN target and directory for the project.
    /// The name should not contain any underscores.
    #[structopt(name = "project-name")]
    project_name: String,

    /// The programming language.
    #[structopt(short, long)]
    lang: Language,
}

/// Supported languages for project creation.
#[derive(Debug)]
enum Language {
    Rust,
    Cpp,
}

impl Language {
    /// Returns the language's template extension. Template
    /// files that match this extension belong to this language.
    fn template_extension(&self) -> &'static str {
        match self {
            Self::Rust => "tmpl-rust",
            Self::Cpp => "tmpl-cpp",
        }
    }

    // Check if the file's extension matches the language-specific template
    // extension or the general template extension.
    fn matches(&self, path: &Path) -> bool {
        if let Some(ext) = path.extension() {
            ext == self.template_extension() || ext == LANG_AGNOSTIC_EXTENSION
        } else {
            false
        }
    }
}

impl FromStr for Language {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(match s {
            "rust" => Self::Rust,
            "cpp" => Self::Cpp,
            _ => return Err(format!("unrecognized language \"{}\"", s)),
        })
    }
}

impl fmt::Display for Language {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Rust => "rust",
            Self::Cpp => "cpp",
        })
    }
}

/// The arguments passed in during template execution.
/// The fields defined here represent what variables can be present in template files.
/// Add a field here and populate it to make it accessible in a template.
///
/// NOTE: The fields are serialized to JSON when passed to templates. The serialization
/// process renames all fields to UPPERCASE.
#[derive(Debug, Serialize)]
#[serde(rename_all = "UPPERCASE")]
struct TemplateArgs {
    /// The current year, for use in copyright headers.
    /// Reference from a template with `{{COPYRIGHT_YEAR}}`.
    copyright_year: String,

    /// The project name, as given on the command line.
    /// Reference from a template with `{{PROJECT_NAME}}`.
    project_name: String,

    /// The path to the new project, relative to the FUCHSIA_DIR environment variable.
    /// Reference from a template with `{{PROJECT_PATH}}`.
    project_path: String,

    /// The project-type, as specified on the command line. E.g. 'component-v2'.
    project_type: String,
}

impl TemplateArgs {
    /// Build TemplateArgs from the program args and environment.
    fn from_create_args(create_args: &CreateArgs) -> Result<Self, anyhow::Error> {
        Ok(TemplateArgs {
            copyright_year: Utc::now().year().to_string(),
            project_name: create_args.project_name.clone(),
            project_path: {
                let absolute_project_path = env::current_dir()?.join(&create_args.project_name);
                let fuchsia_root = util::get_fuchsia_root()?;
                absolute_project_path
                    .strip_prefix(&fuchsia_root)
                    .map_err(|_| {
                        anyhow!(
                            "current working directory must be a descendant of FUCHSIA_DIR ({:?})",
                            &fuchsia_root
                        )
                    })?
                    .to_str()
                    .ok_or_else(|| anyhow!("invalid path {:?}", &absolute_project_path))?
                    .to_string()
            },
            project_type: create_args.project_type.clone(),
        })
    }
}

/// The in-memory filtered template file tree.
#[derive(Debug, PartialEq)]
enum TemplateTree {
    /// A file and its template contents.
    File(String),

    /// A directory and its entries.
    Dir(HashMap<String, Box<TemplateTree>>),
}

impl TemplateTree {
    /// Populate a TemplateTree from a JSON file at `path`, filtering by `project_type` and `lang`.
    ///
    /// The JSON file must consist of a root list of strings, where each string is a path relative
    /// to the directory that contains the JSON file.
    ///
    /// E.g.
    ///
    /// `out/default/host-tools/create_templates/templates.json`:
    /// ```json
    /// [
    ///     "component-v2/BUILD.gn.tmpl-rust",
    ///     "component-v2/src/main.rs.tmpl-rust"
    /// ]
    /// ```
    ///
    /// This file is generated by the GN build rule in //tools/create/templates/BUILD.gn.
    fn from_json_file<FR>(
        path: &Path,
        project_type: &str,
        lang: &Language,
        file_reader: &FR,
    ) -> Result<Self, anyhow::Error>
    where
        FR: FileReader,
    {
        let template_files = {
            let json_contents = file_reader.read_to_string(path)?;
            let template_files: Vec<PathBuf> = serde_json::from_slice(json_contents.as_bytes())?;
            let mut template_files: Vec<PathBuf> = template_files
                .into_iter()
                .filter(|p| p.starts_with(project_type) && lang.matches(p))
                .collect();
            template_files.sort();
            template_files
        };
        if template_files.is_empty() {
            bail!("no templates found for project type \"{}\"", project_type);
        }
        let root_dir = path.parent().expect("no parent directory for JSON file");
        let mut tree = TemplateTree::Dir(HashMap::new());
        for path in &template_files {
            let contents = file_reader.read_to_string(root_dir.join(path))?;

            // Strip the project prefix from the path, which we know is there.
            let path = path.strip_prefix(project_type).unwrap();

            // Strip the .tmpl* extension. This will uncover the intended extension.
            // Eg: foo.rs.tmpl-rust -> foo.rs
            let path = path.with_extension("");

            tree.insert(&path, contents)?;
        }
        Ok(tree)
    }

    fn insert(&mut self, path: &Path, contents: String) -> Result<(), anyhow::Error> {
        let subtree = match self {
            Self::Dir(ref mut subtree) => subtree,
            Self::File(_) => bail!("cannot insert subtree into file"),
        };

        let mut path_iter = path.components();
        if let Some(Component::Normal(component)) = path_iter.next() {
            let name = util::filename_to_string(component)?;
            let rest = path_iter.as_path();
            if path_iter.next().is_some() {
                subtree
                    .entry(name)
                    .or_insert_with(|| Box::new(TemplateTree::Dir(HashMap::new())))
                    .insert(rest, contents)?;
            } else {
                if subtree.insert(name, Box::new(TemplateTree::File(contents))).is_some() {
                    bail!("duplicate paths");
                }
            }
            Ok(())
        } else {
            bail!("path must be relative and have no '..' or '.' components");
        }
    }

    /// Recursively renders this TemplateTree into a mirror type [`RenderedTree`],
    /// using `handlebars` as the template engine and `args` as the exported variables
    // accessible to the templates.
    fn render(
        &self,
        handlebars: &mut Handlebars,
        args: &TemplateArgs,
    ) -> Result<RenderedTree, handlebars::TemplateRenderError> {
        Ok(match self {
            Self::File(template_str) => {
                RenderedTree::File(handlebars.render_template(&template_str, args)?)
            }
            Self::Dir(nested_templates) => {
                let mut rendered_subtree = HashMap::new();
                for (filename, template) in nested_templates {
                    rendered_subtree.insert(
                        handlebars.render_template(&filename, args)?,
                        Box::new(template.render(handlebars, args)?),
                    );
                }
                RenderedTree::Dir(rendered_subtree)
            }
        })
    }
}

/// An in-memory representation of a file tree, where the paths and contents have
/// all been executed and rendered into their final form.
/// This is the mirror of [`TemplateTree`].
#[derive(Debug, PartialEq)]
enum RenderedTree {
    /// A file and its contents.
    File(String),

    /// A directory and its entries.
    Dir(HashMap<String, Box<RenderedTree>>),
}

impl RenderedTree {
    /// Write the RenderedTree to the `dest` path.
    fn write(&self, dest: &Path) -> io::Result<()> {
        match self {
            Self::File(contents) => {
                fs::write(dest, &contents)?;
            }
            Self::Dir(tree) => {
                fs::create_dir(dest)?;
                for (filename, subtree) in tree {
                    let dest = dest.join(filename);
                    subtree.write(&dest)?;
                }
            }
        }
        Ok(())
    }
}

/// Trait to enable testing of the template tree creation logic.
/// Allows mocking out reading from the file system.
trait FileReader {
    fn read_to_string(&self, p: impl AsRef<Path>) -> io::Result<String>;
}

/// Standard library filesystem implementation of FileReader.
struct StdFs;

impl FileReader for StdFs {
    fn read_to_string(&self, p: impl AsRef<Path>) -> io::Result<String> {
        fs::read_to_string(p)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matches::assert_matches;

    #[test]
    fn language_display_can_be_parsed() {
        assert_matches!(Language::Rust.to_string().parse(), Ok(Language::Rust));
        assert_matches!(Language::Cpp.to_string().parse(), Ok(Language::Cpp));
    }

    #[test]
    fn render_templates() {
        let templates = TemplateTree::Dir({
            let mut entries = HashMap::new();
            entries.insert(
                "file.h".to_string(),
                Box::new(TemplateTree::File("class {{PROJECT_NAME}};".to_string())),
            );
            entries.insert(
                "{{PROJECT_NAME}}_nested".to_string(),
                Box::new(TemplateTree::Dir({
                    let mut entries = HashMap::new();
                    entries.insert(
                        "file.h".to_string(),
                        Box::new(TemplateTree::File(
                            "#include \"{{PROJECT_PATH}}/file.h\"\n// `fx create {{PROJECT_TYPE}}`"
                                .to_string(),
                        )),
                    );
                    entries
                })),
            );
            entries
        });

        let mut handlebars = Handlebars::new();
        handlebars.set_strict_mode(true);
        let args = TemplateArgs {
            copyright_year: "2020".to_string(),
            project_name: "foo".to_string(),
            project_path: "bar/foo".to_string(),
            project_type: "component-v2".to_string(),
        };

        let rendered =
            templates.render(&mut handlebars, &args).expect("failed to render templates");
        assert_eq!(
            rendered,
            RenderedTree::Dir({
                let mut entries = HashMap::new();
                entries.insert(
                    "file.h".to_string(),
                    Box::new(RenderedTree::File("class foo;".to_string())),
                );
                entries.insert(
                    "foo_nested".to_string(),
                    Box::new(RenderedTree::Dir({
                        let mut entries = HashMap::new();
                        entries.insert(
                            "file.h".to_string(),
                            Box::new(RenderedTree::File(
                                "#include \"bar/foo/file.h\"\n// `fx create component-v2`"
                                    .to_string(),
                            )),
                        );
                        entries
                    })),
                );
                entries
            })
        );
    }

    impl<'a> FileReader for HashMap<&'a Path, &'a str> {
        fn read_to_string(&self, path: impl AsRef<Path>) -> io::Result<String> {
            self.get(path.as_ref())
                .map(|c| c.to_string())
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "file not found"))
        }
    }

    #[test]
    fn template_tree_from_json() {
        let mut files = HashMap::new();
        files.insert(
            Path::new("/out/default/host-tools/create-templates/templates.json"),
            r#"[ "component-v2/main.cc.tmpl-cpp" ]"#,
        );
        files.insert(
            Path::new("/out/default/host-tools/create-templates/component-v2/main.cc.tmpl-cpp"),
            r#"{{PROJECT_NAME}}"#,
        );
        let tree = TemplateTree::from_json_file(
            Path::new("/out/default/host-tools/create-templates/templates.json"),
            "component-v2",
            &Language::Cpp,
            &files,
        )
        .expect("failed");
        assert_matches!(tree, TemplateTree::Dir(nested) if nested.contains_key("main.cc"));
    }
}
