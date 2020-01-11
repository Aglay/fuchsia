// Copyright 2019 The Fuchsia Authors. All right reserved.
// Use of this source code is goverend by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::error::ModelError,
    anyhow::format_err,
    fidl_fuchsia_io::{self as fio},
    fidl_fuchsia_io2::{self as fio2},
    lazy_static::lazy_static,
    std::cmp::Ordering,
    std::collections::HashSet,
    std::convert::From,
};

/// List of all available fio2::Operation rights.
const ALL_RIGHTS: [fio2::Operations; 10] = [
    fio2::Operations::Connect,
    fio2::Operations::ReadBytes,
    fio2::Operations::GetAttributes,
    fio2::Operations::Traverse,
    fio2::Operations::Enumerate,
    fio2::Operations::WriteBytes,
    fio2::Operations::UpdateAttributes,
    fio2::Operations::ModifyDirectory,
    fio2::Operations::Execute,
    fio2::Operations::Admin,
];

// TODO(benwright) - const initialization of bitflag types with or are not supported. Change when
// supported.
lazy_static! {
    /// All the fio2 rights required to represent fio::OPEN_RIGHT_READABLE.
    pub static ref READ_RIGHTS: fio2::Operations =
        fio2::Operations::ReadBytes
        | fio2::Operations::GetAttributes
        | fio2::Operations::Traverse
        | fio2::Operations::Enumerate;
    /// All the fio2 rights required to represent fio::OPEN_RIGHT_WRITABLE.
    pub static ref WRITE_RIGHTS: fio2::Operations =
        fio2::Operations::WriteBytes
        | fio2::Operations::UpdateAttributes
        | fio2::Operations::ModifyDirectory;
    /// All the fio2 rights required to represent fio::OPEN_RIGHT_EXECUTABLE.
    pub static ref EXECUTE_RIGHTS: fio2::Operations = fio2::Operations::Execute;
}

/// Opaque rights type to define new traits like PartialOrd on.
#[derive(Debug, PartialEq, Eq, Clone)]
pub struct Rights(fio2::Operations);

impl Rights {
    /// Converts fuchsia.io2 directory rights to fuchsio.io compatible rights. This will be removed
    /// once fuchsia.io2 is supported by component manager.
    pub fn into_legacy(&self) -> u32 {
        let mut flags: u32 = 0;
        let rights = self.0;
        if rights.intersects(
            fio2::Operations::ReadBytes
                | fio2::Operations::GetAttributes
                | fio2::Operations::Traverse
                | fio2::Operations::Enumerate,
        ) {
            flags |= fio::OPEN_RIGHT_READABLE;
        }
        if rights.intersects(
            fio2::Operations::WriteBytes
                | fio2::Operations::UpdateAttributes
                | fio2::Operations::ModifyDirectory,
        ) {
            flags |= fio::OPEN_RIGHT_WRITABLE;
        }
        if rights.contains(fio2::Operations::Execute) {
            flags |= fio::OPEN_RIGHT_EXECUTABLE;
        }
        if rights.contains(fio2::Operations::Admin) {
            flags |= fio::OPEN_RIGHT_ADMIN;
        }
        // TODO(benwright) - Since there is no direct translation for connect in CV1 we must
        // explicitly define it here as both flags.
        if flags == 0 && rights.contains(fio2::Operations::Connect) {
            flags |= fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE;
        }
        flags
    }
}

/// Returns equal if and only if the operations are idential. Less than is returned if self is a
/// strict subset of other. Greater is returned if self is a superset of other. If the two types
/// contain different values None is returned as their is no possible ordering.
impl PartialOrd for Rights {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        if self.0 == other.0 {
            return Some(Ordering::Equal);
        }
        let mut lhs = HashSet::new();
        let mut rhs = HashSet::new();
        for right in ALL_RIGHTS.iter() {
            if self.0.contains(*right) {
                lhs.insert(right);
            }
            if other.0.contains(*right) {
                rhs.insert(right);
            }
        }
        if lhs.is_subset(&rhs) {
            return Some(Ordering::Less);
        }
        if lhs.is_superset(&rhs) {
            return Some(Ordering::Greater);
        }
        None
    }
}

/// Allows creating rights from fio2::Operations.
impl From<fio2::Operations> for Rights {
    fn from(operations: fio2::Operations) -> Self {
        Rights(operations)
    }
}

/// RightWalkState contains all information required to traverse offer and expose chains in a tree
/// tracing routes from any point back to their originating source. This includes in the most
/// complex case traversing from a use to offer chain, back through to an expose chain.
#[derive(Debug, Clone)]
pub struct RightWalkState {
    is_first: bool,
    last_seen_rights: Option<Rights>,
    finished: bool,
}

#[allow(dead_code)]
impl RightWalkState {
    /// Constructs a new RightWalkState representing the start of a walk.
    pub fn new() -> RightWalkState {
        RightWalkState { is_first: true, last_seen_rights: None, finished: false }
    }

    /// Constructs a new RightWalkState representing the start of a walk after a
    /// hard coded initial node. Used to represent fraework rights with static right
    /// definitions.
    pub fn at(rights: Rights) -> RightWalkState {
        RightWalkState { is_first: false, last_seen_rights: Some(rights), finished: false }
    }

    /// Advances the RightWalkState if and only if the rights passed retain the required properties
    /// of a monotonic decreasing sequence. See Rights PartialOrd trait for information on how
    /// order is determined between elements.
    pub fn advance(&self, rights: Option<Rights>) -> Result<Self, ModelError> {
        if self.finished {
            return Err(ModelError::rights_error(format_err!(
                "Attempting to advance a finished RightWalkState"
            )));
        }
        if let Some(proposed_rights) = rights {
            if self.is_first {
                return Ok(RightWalkState {
                    is_first: false,
                    last_seen_rights: Some(proposed_rights.clone()),
                    finished: false,
                });
            }
            // Greater returns false if no ordering exists. This catches disjoint right
            // escalations.
            if *self.last_seen_rights.as_ref().unwrap() > proposed_rights {
                return Err(ModelError::rights_error(format_err!(
                    "Requested rights greater than provided rights"
                )));
            }
            return Ok(RightWalkState {
                is_first: self.is_first,
                last_seen_rights: Some(proposed_rights.clone()),
                finished: false,
            });
        }
        Ok(self.clone())
    }

    /// Finalizes the state preventing future modification, this is called when the walker arrives
    /// at a node with a source of Framework, Builtin or Self. The provided |rights| should always
    /// be the right at the CapabilitySource.
    pub fn finalize(&self, rights: Option<Rights>) -> Result<Self, ModelError> {
        if let Some(final_rights) = rights {
            if self.is_first {
                return Ok(RightWalkState {
                    is_first: false,
                    last_seen_rights: Some(final_rights.clone()),
                    finished: true,
                });
            }
            if *self.last_seen_rights.as_ref().unwrap() > final_rights {
                return Err(ModelError::rights_error(format_err!(
                    "Requested rights greater than provided rights"
                )));
            }
            return Ok(RightWalkState {
                is_first: self.is_first,
                last_seen_rights: Some(final_rights.clone()),
                finished: true,
            });
        }
        return Err(ModelError::rights_error(format_err!(
            "Directory routes must end at a source with a rights declaration"
        )));
    }

    /// Returns true if the walk state is finished.
    pub fn finished(&self) -> bool {
        self.finished
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn operation_ordering() {
        assert_eq!(Rights::from(*READ_RIGHTS), Rights::from(*READ_RIGHTS));
        assert_eq!(Rights::from(*READ_RIGHTS).partial_cmp(&Rights::from(*WRITE_RIGHTS)), None);
        assert_eq!(
            Rights::from(fio2::Operations::WriteBytes).partial_cmp(&Rights::from(*WRITE_RIGHTS)),
            Some(Ordering::Less)
        );
        assert_eq!(
            Rights::from(*WRITE_RIGHTS).partial_cmp(&Rights::from(fio2::Operations::WriteBytes)),
            Some(Ordering::Greater)
        );
    }

    #[test]
    fn into_legacy() {
        assert_eq!(Rights::from(*READ_RIGHTS).into_legacy(), fio::OPEN_RIGHT_READABLE);
        assert_eq!(Rights::from(*WRITE_RIGHTS).into_legacy(), fio::OPEN_RIGHT_WRITABLE);
        assert_eq!(Rights::from(*EXECUTE_RIGHTS).into_legacy(), fio::OPEN_RIGHT_EXECUTABLE);
        assert_eq!(
            Rights::from(*READ_RIGHTS | *WRITE_RIGHTS).into_legacy(),
            fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE
        );
        assert_eq!(
            Rights::from(*READ_RIGHTS | *WRITE_RIGHTS | *EXECUTE_RIGHTS).into_legacy(),
            fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE | fio::OPEN_RIGHT_EXECUTABLE
        );
    }
}
