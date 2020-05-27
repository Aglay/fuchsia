// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {anyhow, serde_json, std::str::FromStr, thiserror::Error};

#[derive(Error, Debug)]
pub enum Error {
    #[error("Error while fetching data: {}", _0)]
    Fetch(anyhow::Error),

    #[error("Invalid format: {}", _0)]
    InvalidFormat(String),

    #[error("Invalid arguments: {}", _0)]
    InvalidArguments(String),

    #[error("The archivist returned invalid JSON")]
    ArchiveInvalidJson,

    #[error("The archivist didn't return expected property: {}", _0)]
    ArchiveMissingProperty(String),

    #[error("Failed formatting the command response: {}", _0)]
    InvalidCommandResponse(serde_json::Error),
}

impl Error {
    pub fn invalid_format(format: impl Into<String>) -> Error {
        Error::InvalidFormat(format.into())
    }

    pub fn invalid_arguments(msg: impl Into<String>) -> Error {
        Error::InvalidArguments(msg.into())
    }

    pub fn archive_missing_property(name: impl Into<String>) -> Error {
        Error::ArchiveMissingProperty(name.into())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Format {
    Text,
    Json,
}

impl FromStr for Format {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.to_lowercase().as_ref() {
            "json" => Ok(Format::Json),
            "text" => Ok(Format::Text),
            f => Err(Error::invalid_format(f)),
        }
    }
}
