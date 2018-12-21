// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod errors;

use lazy_static::lazy_static;
use regex::Regex;
use std::fmt;
use url::percent_encoding::percent_decode;
use url::Url;

pub use crate::errors::ParseError;

lazy_static! {
    static ref NAME_RE: Regex = Regex::new(r"^[0-9a-z\-\._]{1,100}$").unwrap();
    static ref HASH_RE: Regex = Regex::new(r"^[0-9a-z]{64}$").unwrap();
}

/// Decoded representation of a fuchsia-pkg URI.
///
/// Depending on which segments are included, the URI may identify a package
/// repository, a package within a repository (with optional variant and hash),
/// or a resource within a package.
///
/// Repository identifier:
/// - fuchsia-pkg://example.com/
///
/// Package identifier:
/// - fuchsia-pkg://example.com/some-package
/// - fuchsia-pkg://example.com/some-package/some-variant
/// - fuchsia-pkg://example.com/some-package/some-variant/<some-hash>
///
/// Resource identifier:
/// - fuchsia-pkg://example.com/some-package#path/to/resource
/// - fuchsia-pkg://example.com/some-package/some-variant#path/to/resource
/// - fuchsia-pkg://example.com/some-package/some-variant/<some-hash>#path/to/resource
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PackageUri {
    host: String,
    name: Option<String>,
    variant: Option<String>,
    hash: Option<String>,
    path: Option<String>,
}

impl PackageUri {
    pub fn parse(input: &str) -> Result<Self, ParseError> {
        let uri = Url::parse(input)?;

        let scheme = uri.scheme();
        if scheme != "fuchsia-pkg" {
            return Err(ParseError::InvalidScheme);
        }

        let host = if let Some(host) = uri.host() {
            host.to_string()
        } else {
            return Err(ParseError::InvalidHost);
        };
        if host.is_empty() {
            return Err(ParseError::InvalidHost);
        }

        if uri.port().is_some() {
            return Err(ParseError::CannotContainPort);
        }

        if !uri.username().is_empty() {
            return Err(ParseError::CannotContainUsername);
        }

        if uri.password().is_some() {
            return Err(ParseError::CannotContainPassword);
        }

        let (name, variant, hash) = parse_path(uri.path())?;

        if uri.query().is_some() {
            return Err(ParseError::CannotContainQueryParameters);
        }

        let path = if let Some(path) = uri.fragment() {
            let path = match percent_decode(path.as_bytes()).decode_utf8() {
                Ok(path) => path,
                Err(_) => {
                    return Err(ParseError::InvalidResourcePath);
                }
            };

            if path.is_empty() {
                None
            } else if check_path(&path) {
                Some(path.to_string())
            } else {
                return Err(ParseError::InvalidResourcePath);
            }
        } else {
            None
        };

        Ok(PackageUri {
            host: host,
            name: name,
            variant: variant,
            hash: hash,
            path: path,
        })
    }

    pub fn host(&self) -> &str {
        &self.host
    }

    pub fn name(&self) -> Option<&str> {
        self.name.as_ref().map(|s| &**s)
    }

    pub fn variant(&self) -> Option<&str> {
        self.variant.as_ref().map(|s| &**s)
    }

    pub fn hash(&self) -> Option<&str> {
        self.hash.as_ref().map(|s| &**s)
    }

    pub fn path(&self) -> Option<&str> {
        self.path.as_ref().map(|s| &**s)
    }

    pub fn new_repository(host: String) -> Result<PackageUri, ParseError> {
        if host.is_empty() {
            return Err(ParseError::InvalidHost);
        }
        Ok(PackageUri {
            host: host,
            name: None,
            variant: None,
            hash: None,
            path: None,
        })
    }

    pub fn new_package(
        host: String, name: String, variant: Option<String>, hash: Option<String>,
    ) -> Result<PackageUri, ParseError> {
        let mut uri = PackageUri::new_repository(host)?;
        if !NAME_RE.is_match(&name) {
            return Err(ParseError::InvalidName);
        }
        if let Some(ref v) = variant {
            if !NAME_RE.is_match(v) {
                return Err(ParseError::InvalidVariant);
            }
        }
        if let Some(ref h) = hash {
            if variant.is_none() {
                return Err(ParseError::InvalidVariant);
            }
            if !HASH_RE.is_match(h) {
                return Err(ParseError::InvalidHash);
            }
        }
        uri.name = Some(name);
        uri.variant = variant;
        uri.hash = hash;
        Ok(uri)
    }

    pub fn new_resource(
        host: String, name: String, variant: Option<String>, hash: Option<String>, path: String,
    ) -> Result<PackageUri, ParseError> {
        let mut uri = PackageUri::new_package(host, name, variant, hash)?;
        if path.is_empty() || !check_path(&path) {
            return Err(ParseError::InvalidResourcePath);
        }
        uri.path = Some(path);
        Ok(uri)
    }
}

impl fmt::Display for PackageUri {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "fuchsia-pkg://{}", self.host)?;
        if let Some(ref name) = self.name {
            write!(f, "/{}", name)?;

            if let Some(ref variant) = self.variant {
                write!(f, "/{}", variant)?;

                if let Some(ref hash) = self.hash {
                    write!(f, "/{}", hash)?;
                }
            }
        }

        if let Some(ref path) = self.path {
            write!(
                f,
                "#{}",
                url::percent_encoding::utf8_percent_encode(
                    path,
                    url::percent_encoding::DEFAULT_ENCODE_SET
                )
            )?;
        }

        Ok(())
    }
}

fn parse_path(
    mut path: &str,
) -> Result<(Option<String>, Option<String>, Option<String>), ParseError> {
    let mut name = None;
    let mut variant = None;
    let mut hash = None;

    if path.starts_with('/') {
        path = &path[1..];

        if !path.is_empty() {
            let mut iter = path.split('/').fuse();

            if let Some(s) = iter.next() {
                if NAME_RE.is_match(s) {
                    name = Some(s.to_string());
                } else {
                    return Err(ParseError::InvalidName);
                }
            }

            if let Some(s) = iter.next() {
                if NAME_RE.is_match(s) {
                    variant = Some(s.to_string());
                } else {
                    return Err(ParseError::InvalidVariant);
                }
            }

            if let Some(s) = iter.next() {
                if HASH_RE.is_match(s) {
                    hash = Some(s.to_string());
                } else {
                    return Err(ParseError::InvalidHash);
                }
            }

            if let Some(_) = iter.next() {
                return Err(ParseError::ExtraPathSegments);
            }
        }
    }

    Ok((name, variant, hash))
}

fn check_path(input: &str) -> bool {
    for segment in input.split('/') {
        if segment.is_empty() || segment == "." || segment == ".." {
            return false;
        }

        if segment.bytes().find(|c| *c == b'\x00').is_some() {
            return false;
        }
    }

    true
}

#[cfg(test)]
mod tests {
    use super::*;

    macro_rules! test_parse_ok {
        (
            $(
                $test_name:ident => {
                    uri = $pkg_uri:expr,
                    host = $pkg_host:expr,
                    name = $pkg_name:expr,
                    variant = $pkg_variant:expr,
                    hash = $pkg_hash:expr,
                    path = $pkg_path:expr,
                }
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    let pkg_uri = $pkg_uri.to_string();
                    assert_eq!(
                        PackageUri::parse(&pkg_uri),
                        Ok(PackageUri {
                            host: $pkg_host,
                            name: $pkg_name,
                            variant: $pkg_variant,
                            hash: $pkg_hash,
                            path: $pkg_path,
                        })
                    );
                }
            )+
        }
    }

    macro_rules! test_parse_err {
        (
            $(
                $test_name:ident => {
                    uris = $uris:expr,
                    err = $err:expr,
                }
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    for uri in &$uris {
                        assert_eq!(
                            PackageUri::parse(uri),
                            Err($err),
                        );
                    }
                }
            )+
        }
    }

    macro_rules! test_format {
        (
            $(
                $test_name:ident => {
                    parsed = $parsed:expr,
                    formatted = $formatted:expr,
                }
            )+
        ) => {
            $(
                #[test]
                fn $test_name() {
                    assert_eq!(
                        format!("{}", $parsed),
                        $formatted
                    );
                }
            )+
        }
    }

    test_parse_ok! {
        test_parse_host => {
            uri = "fuchsia-pkg://fuchsia.com",
            host = "fuchsia.com".to_string(),
            name = None,
            variant = None,
            hash = None,
            path = None,
        }
        test_parse_host_name => {
            uri = "fuchsia-pkg://fuchsia.com/fonts",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = None,
            hash = None,
            path = None,
        }
        test_parse_host_name_special_chars => {
            uri = "fuchsia-pkg://fuchsia.com/abc123-._",
            host = "fuchsia.com".to_string(),
            name = Some("abc123-._".to_string()),
            variant = None,
            hash = None,
            path = None,
        }
        test_parse_host_name_variant => {
            uri = "fuchsia-pkg://fuchsia.com/fonts/stable",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = Some("stable".to_string()),
            hash = None,
            path = None,
        }
        test_parse_host_name_variant_hash => {
            uri = "fuchsia-pkg://fuchsia.com/fonts/stable/80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = Some("stable".to_string()),
            hash = Some("80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a".to_string()),
            path = None,
        }
        test_parse_ignoring_empty_path => {
            uri = "fuchsia-pkg://fuchsia.com/fonts#",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = None,
            hash = None,
            path = None,
        }
        test_parse_path => {
            uri = "fuchsia-pkg://fuchsia.com/fonts#foo/bar",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = None,
            hash = None,
            path = Some("foo/bar".to_string()),
        }
        test_parse_path_decodes_percent_encoding => {
            uri = "fuchsia-pkg://fuchsia.com/fonts#foo%23bar",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = None,
            hash = None,
            path = Some("foo#bar".to_string()),
        }
        test_parse_path_ignores_nul_chars => {
            uri = "fuchsia-pkg://fuchsia.com/fonts#foo\x00bar",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = None,
            hash = None,
            path = Some("foobar".to_string()),
        }
        test_parse_path_allows_encoded_control_chars => {
            uri = "fuchsia-pkg://fuchsia.com/fonts#foo%09bar",
            host = "fuchsia.com".to_string(),
            name = Some("fonts".to_string()),
            variant = None,
            hash = None,
            path = Some("foo\tbar".to_string()),
        }
    }

    test_parse_err! {
        test_parse_host_cannot_be_absent => {
            uris = [
                "fuchsia-pkg://",
            ],
            err = ParseError::InvalidHost,
        }
        test_parse_host_cannot_be_empty => {
            uris = [
                "fuchsia-pkg:///",
            ],
            err = ParseError::InvalidHost,
        }
        test_parse_name_cannot_be_empty => {
            uris = [
                "fuchsia-pkg://fuchsia.com//",
            ],
            err = ParseError::InvalidName,
        }
        test_parse_name_cannot_be_longer_than_100_chars => {
            uris = [
                "fuchsia-pkg://fuchsia.com/12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901/",
            ],
            err = ParseError::InvalidName,
        }
        test_parse_name_cannot_have_invalid_characters => {
            uris = [
                "fuchsia-pkg://fuchsia.com/$",
                "fuchsia-pkg://fuchsia.com/foo$bar",
            ],
            err = ParseError::InvalidName,
        }
        test_parse_variant_cannot_have_invalid_characters => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts/$",
                "fuchsia-pkg://fuchsia.com/fonts/foo$bar",
            ],
            err = ParseError::InvalidVariant,
        }
        test_parse_hash_cannot_be_empty => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts/stable/",
            ],
            err = ParseError::InvalidHash,
        }
        test_parse_hash_cannot_have_invalid_characters => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts/stable/8$e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a",
                "fuchsia-pkg://fuchsia.com/fonts/stable/80E8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a",
            ],
            err = ParseError::InvalidHash,
        }
        test_parse_hash_must_be_64_chars => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts/stable/80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4",
                "fuchsia-pkg://fuchsia.com/fonts/stable/80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4aa",
            ],
            err = ParseError::InvalidHash,
        }
        test_parse_hash_cannot_have_extra_segments => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts/stable/80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a/foo",
            ],
            err = ParseError::ExtraPathSegments,
        }
        test_parse_path_cannot_be_slash => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts#/",
            ],
            err = ParseError::InvalidResourcePath,
        }
        test_parse_path_cannot_start_with_slash => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts#/foo",
                "fuchsia-pkg://fuchsia.com/fonts#/foo/bar",
            ],
            err = ParseError::InvalidResourcePath,
        }
        test_parse_path_cannot_end_with_slash => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts#foo/",
                "fuchsia-pkg://fuchsia.com/fonts#foo/bar/",
            ],
            err = ParseError::InvalidResourcePath,
        }
        test_parse_path_cannot_contain_dot_dot => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts#foo/../bar",
            ],
            err = ParseError::InvalidResourcePath,
        }
        test_parse_path_cannot_contain_empty_segments => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts#foo//bar",
            ],
            err = ParseError::InvalidResourcePath,
        }
        test_parse_path_cannot_contain_percent_encoded_nul_chars => {
            uris = [
                "fuchsia-pkg://fuchsia.com/fonts#foo%00bar",
            ],
            err = ParseError::InvalidResourcePath,
        }
        test_parse_path_rejects_port => {
            uris = [
                "fuchsia-pkg://fuchsia.com:1234",
            ],
            err = ParseError::CannotContainPort,
        }
        test_parse_path_rejects_username => {
            uris = [
                "fuchsia-pkg://user@fuchsia.com",
                "fuchsia-pkg://user:password@fuchsia.com",
            ],
            err = ParseError::CannotContainUsername,
        }
        test_parse_path_rejects_password => {
            uris = [
                "fuchsia-pkg://:password@fuchsia.com",
            ],
            err = ParseError::CannotContainPassword,
        }
        test_parse_rejects_query_params => {
            uris = [
                "fuchsia-pkg://fuchsia.com?foo=bar",
            ],
            err = ParseError::CannotContainQueryParameters,
        }
    }

    test_format! {
        test_format_repository_uri => {
            parsed = PackageUri::new_repository("fuchsia.com".to_string()).unwrap(),
            formatted = "fuchsia-pkg://fuchsia.com",
        }
        test_format_package_uri => {
            parsed = PackageUri::new_package(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                None,
                None,
            ).unwrap(),
            formatted = "fuchsia-pkg://fuchsia.com/fonts",
        }
        test_format_package_uri_with_variant => {
            parsed = PackageUri::new_package(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                Some("stable".to_string()),
                None,
            ).unwrap(),
            formatted = "fuchsia-pkg://fuchsia.com/fonts/stable",
        }
        test_format_package_uri_with_hash => {
            parsed = PackageUri::new_package(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                Some("stable".to_string()),
                Some("80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a".to_string()),
            ).unwrap(),
            formatted = "fuchsia-pkg://fuchsia.com/fonts/stable/80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a",
        }
        test_format_resource_uri => {
            parsed = PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                None,
                None,
                "foo#bar".to_string(),
            ).unwrap(),
            formatted = "fuchsia-pkg://fuchsia.com/fonts#foo%23bar",
        }
    }

    #[test]
    fn test_new_repository() {
        let uri = PackageUri::new_repository("fuchsia.com".to_string()).unwrap();
        assert_eq!("fuchsia.com", uri.host());
        assert_eq!(None, uri.name());
        assert_eq!(None, uri.variant());
        assert_eq!(None, uri.hash());
        assert_eq!(None, uri.path());

        assert_eq!(
            PackageUri::new_repository("".to_string()),
            Err(ParseError::InvalidHost)
        )
    }

    #[test]
    fn test_new_package() {
        let uri = PackageUri::new_package(
            "fuchsia.com".to_string(),
            "fonts".to_string(),
            Some("stable".to_string()),
            Some("80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a".to_string()),
        )
        .unwrap();
        assert_eq!("fuchsia.com", uri.host());
        assert_eq!(Some("fonts"), uri.name());
        assert_eq!(Some("stable"), uri.variant());
        assert_eq!(
            Some("80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a"),
            uri.hash()
        );
        assert_eq!(None, uri.path());

        assert_eq!(
            PackageUri::new_package("".to_string(), "fonts".to_string(), None, None),
            Err(ParseError::InvalidHost)
        );
        assert_eq!(
            PackageUri::new_package("fuchsia.com".to_string(), "".to_string(), None, None),
            Err(ParseError::InvalidName)
        );
        assert_eq!(
            PackageUri::new_package(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                Some("$".to_string()),
                None
            ),
            Err(ParseError::InvalidVariant)
        );
        assert_eq!(
            PackageUri::new_package(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                None,
                Some(
                    "80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a".to_string()
                )
            ),
            Err(ParseError::InvalidVariant)
        );
        assert_eq!(
            PackageUri::new_package(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                Some("stable".to_string()),
                Some("$".to_string())
            ),
            Err(ParseError::InvalidHash)
        );
    }

    #[test]
    fn test_new_resource() {
        let uri = PackageUri::new_resource(
            "fuchsia.com".to_string(),
            "fonts".to_string(),
            Some("stable".to_string()),
            Some("80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a".to_string()),
            "foo/bar".to_string(),
        )
        .unwrap();
        assert_eq!("fuchsia.com", uri.host());
        assert_eq!(Some("fonts"), uri.name());
        assert_eq!(Some("stable"), uri.variant());
        assert_eq!(
            Some("80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a"),
            uri.hash()
        );
        assert_eq!(Some("foo/bar"), uri.path());

        assert_eq!(
            PackageUri::new_resource(
                "".to_string(),
                "fonts".to_string(),
                None,
                None,
                "foo/bar".to_string()
            ),
            Err(ParseError::InvalidHost)
        );
        assert_eq!(
            PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "".to_string(),
                None,
                None,
                "foo/bar".to_string()
            ),
            Err(ParseError::InvalidName)
        );
        assert_eq!(
            PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                Some("$".to_string()),
                None,
                "foo/bar".to_string()
            ),
            Err(ParseError::InvalidVariant)
        );
        assert_eq!(
            PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                None,
                Some(
                    "80e8721f4eba5437c8b6e1604f6ee384f42aed2b6dfbfd0b616a864839cd7b4a".to_string()
                ),
                "foo/bar".to_string()
            ),
            Err(ParseError::InvalidVariant)
        );
        assert_eq!(
            PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                Some("stable".to_string()),
                Some("$".to_string()),
                "foo/bar".to_string()
            ),
            Err(ParseError::InvalidHash)
        );
        assert_eq!(
            PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                None,
                None,
                "".to_string()
            ),
            Err(ParseError::InvalidResourcePath)
        );
        assert_eq!(
            PackageUri::new_resource(
                "fuchsia.com".to_string(),
                "fonts".to_string(),
                None,
                None,
                "a//b".to_string()
            ),
            Err(ParseError::InvalidResourcePath)
        );
    }
}
