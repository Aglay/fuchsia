// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use serde_derive::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug, Hash, Clone, PartialOrd, Ord, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum TargetArchitecture {
    Arm64,
    X64,
}
