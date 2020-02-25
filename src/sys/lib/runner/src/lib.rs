// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod component;

use {
    fidl_fuchsia_component_runner as fcrunner, fidl_fuchsia_data as fdata, std::path::Path,
    thiserror::Error,
};

/// An error encountered operating on `ComponentStartInfo`.
#[derive(Debug, PartialEq, Eq, Error)]
pub enum StartInfoError {
    #[error("missing url")]
    MissingUrl,
}

/// An error encountered trying to get entry out of `ComponentStartInfo->program`.
#[derive(Debug, PartialEq, Eq, Error)]
pub enum StartInfoProgramError {
    #[error("\"program.binary\" must be specified")]
    MissingBinary,

    #[error("the value of \"program.binary\" must be a string")]
    InValidBinaryType,

    #[error("the value of \"program.binary\" must be a relative path")]
    BinaryPathNotRelative,

    #[error("invalid type in arguments")]
    InvalidArguments,

    #[error("\"program\" must be specified")]
    NotFound,
}

// Retrieves component URL from start_info or errors out if not found.
pub fn get_resolved_url(
    start_info: &fcrunner::ComponentStartInfo,
) -> Result<String, StartInfoError> {
    match &start_info.resolved_url {
        Some(url) => Ok(url.to_string()),
        _ => Err(StartInfoError::MissingUrl),
    }
}

fn find<'a>(dict: &'a fdata::Dictionary, key: &str) -> Option<&'a fdata::DictionaryValue> {
    match &dict.entries {
        Some(entries) => {
            for entry in entries {
                if entry.key == key {
                    return entry.value.as_ref().map(|val| &**val);
                }
            }
            None
        }
        _ => None,
    }
}

/// Retrieves program.binary from ComponentStartInfo and makes sure that path is relative.
pub fn get_program_binary(
    start_info: &fcrunner::ComponentStartInfo,
) -> Result<String, StartInfoProgramError> {
    if let Some(program) = &start_info.program {
        if let Some(val) = find(program, "binary") {
            if let fdata::DictionaryValue::Str(bin) = val {
                if !Path::new(bin).is_absolute() {
                    Ok(bin.to_string())
                } else {
                    Err(StartInfoProgramError::BinaryPathNotRelative)
                }
            } else {
                Err(StartInfoProgramError::InValidBinaryType)
            }
        } else {
            Err(StartInfoProgramError::MissingBinary)
        }
    } else {
        Err(StartInfoProgramError::NotFound)
    }
}

/// Retrieves program.args from ComponentStartInfo and validates them.
pub fn get_program_args(
    start_info: &fcrunner::ComponentStartInfo,
) -> Result<Vec<String>, StartInfoProgramError> {
    if let Some(program) = &start_info.program {
        if let Some(args) = find(program, "args") {
            if let fdata::DictionaryValue::StrVec(vec) = args {
                return vec.iter().map(|v| Ok(v.clone())).collect();
            }
        }
    }
    Ok(vec![])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn get_resolved_url_test() {
        let new_start_info = |url: Option<String>| fcrunner::ComponentStartInfo {
            resolved_url: url,
            program: None,
            ns: None,
            outgoing_dir: None,
            runtime_dir: None,
        };
        assert_eq!(
            Ok("some_url".to_string()),
            get_resolved_url(&new_start_info(Some("some_url".to_owned()))),
        );

        assert_eq!(Err(StartInfoError::MissingUrl), get_resolved_url(&new_start_info(None)));
    }

    #[test]
    fn get_program_binary_test() {
        let new_start_info = |binary_name: Option<&str>| fcrunner::ComponentStartInfo {
            program: Some(fdata::Dictionary {
                entries: Some(vec![fdata::DictionaryEntry {
                    key: "binary".to_string(),
                    value: binary_name
                        .and_then(|s| Some(Box::new(fdata::DictionaryValue::Str(s.to_string())))),
                }]),
            }),
            ns: None,
            outgoing_dir: None,
            runtime_dir: None,
            resolved_url: None,
        };
        assert_eq!(
            Ok("bin/myexecutable".to_string()),
            get_program_binary(&new_start_info(Some("bin/myexecutable"))),
        );
        assert_eq!(
            Err(StartInfoProgramError::BinaryPathNotRelative),
            get_program_binary(&new_start_info(Some("/bin/myexecutable")))
        );
        assert_eq!(
            Err(StartInfoProgramError::MissingBinary),
            get_program_binary(&new_start_info(None))
        );
    }

    fn new_args_set(args: Vec<String>) -> fcrunner::ComponentStartInfo {
        fcrunner::ComponentStartInfo {
            program: Some(fdata::Dictionary {
                entries: Some(vec![fdata::DictionaryEntry {
                    key: "args".to_string(),
                    value: Some(Box::new(fdata::DictionaryValue::StrVec(args))),
                }]),
            }),
            ns: None,
            outgoing_dir: None,
            runtime_dir: None,
            resolved_url: None,
        }
    }

    #[test]
    fn get_program_args_test() {
        let e: Vec<String> = vec![];

        assert_eq!(
            e,
            get_program_args(&fcrunner::ComponentStartInfo {
                program: Some(fdata::Dictionary { entries: Some(vec![]) }),
                ns: None,
                outgoing_dir: None,
                runtime_dir: None,
                resolved_url: None,
            })
            .unwrap()
        );

        assert_eq!(e, get_program_args(&new_args_set(vec![])).unwrap());

        assert_eq!(
            Ok(vec!["a".to_string()]),
            get_program_args(&new_args_set(vec!["a".to_string()]))
        );

        assert_eq!(
            Ok(vec!["a".to_string(), "b".to_string()]),
            get_program_args(&new_args_set(vec!["a".to_string(), "b".to_string()]))
        );
    }
}
