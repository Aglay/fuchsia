// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub use {
    crate::FONTS_CMX,
    failure::{Error, ResultExt},
    fidl_fuchsia_fonts as fonts,
    fidl_fuchsia_fonts_ext::DecodableExt,
    fidl_fuchsia_intl as intl, fuchsia_async as fasync,
    fuchsia_component::client::{launch, launch_with_options, launcher, App, LaunchOptions},
    fuchsia_zircon as zx,
    fuchsia_zircon::AsHandleRef,
};

#[macro_export]
macro_rules! assert_buf_eq {
    ($typeface_info_a:ident, $typeface_info_b:ident) => {
        assert!(
            $typeface_info_a.buffer_id == $typeface_info_b.buffer_id,
            "{}.buffer_id == {}.buffer_id\n{0}: {:?}\n{1}: {:?}",
            stringify!($typeface_info_a),
            stringify!($typeface_info_b),
            $typeface_info_a,
            $typeface_info_b
        )
    };
}

pub fn start_provider_with_default_fonts() -> Result<(App, fonts::ProviderProxy), Error> {
    let launcher = launcher().context("Failed to open launcher service")?;
    let app = launch(&launcher, FONTS_CMX.to_string(), None)
        .context("Failed to launch fonts::Provider")?;

    let font_provider = app
        .connect_to_service::<fonts::ProviderMarker>()
        .context("Failed to connect to fonts::Provider")?;

    Ok((app, font_provider))
}

pub fn start_provider_with_test_fonts() -> Result<(App, fonts::ProviderProxy), Error> {
    let mut launch_options = LaunchOptions::new();
    launch_options.add_dir_to_namespace(
        "/test_fonts".to_string(),
        std::fs::File::open("/pkg/data/testdata/test_fonts")?,
    )?;

    let launcher = launcher().context("Failed to open launcher service")?;
    let app = launch_with_options(
        &launcher,
        FONTS_CMX.to_string(),
        Some(vec!["--font-manifest".to_string(), "/test_fonts/manifest.json".to_string()]),
        launch_options,
    )
    .context("Failed to launch fonts::Provider")?;

    let font_provider = app
        .connect_to_service::<fonts::ProviderMarker>()
        .context("Failed to connect to fonts::Provider")?;

    Ok((app, font_provider))
}

#[derive(Debug, Eq, PartialEq)]
pub struct TypefaceInfo {
    pub vmo_koid: zx::Koid,
    pub buffer_id: u32,
    pub size: u64,
    pub index: u32,
}

pub async fn get_typeface_info(
    font_provider: &fonts::ProviderProxy,
    name: Option<String>,
    languages: Option<Vec<String>>,
    code_points: Option<Vec<char>>,
) -> Result<TypefaceInfo, Error> {
    let typeface = font_provider
        .get_typeface(fonts::TypefaceRequest {
            query: Some(fonts::TypefaceQuery {
                family: name.as_ref().map(|name| fonts::FamilyName { name: name.to_string() }),
                style: Some(fonts::Style2 {
                    weight: Some(fonts::WEIGHT_NORMAL),
                    width: Some(fonts::Width::SemiExpanded),
                    slant: Some(fonts::Slant::Upright),
                }),
                code_points: code_points
                    .map(|code_points| code_points.into_iter().map(|ch| ch as u32).collect()),
                languages: languages.map(|languages| {
                    languages
                        .into_iter()
                        .map(|lang_code| intl::LocaleId { id: lang_code })
                        .collect()
                }),
                fallback_family: None,
            }),
            flags: Some(fonts::TypefaceRequestFlags::empty()),
        })
        .await?;

    assert!(!typeface.is_empty(), "Received empty response for {:?}", name);
    let buffer = typeface.buffer.unwrap();
    assert!(buffer.size > 0);
    assert!(buffer.size <= buffer.vmo.get_size()?);

    let vmo_koid = buffer.vmo.as_handle_ref().get_koid()?;
    Ok(TypefaceInfo {
        vmo_koid,
        buffer_id: typeface.buffer_id.unwrap(),
        size: buffer.size,
        index: typeface.font_index.unwrap(),
    })
}

pub async fn get_typeface_info_basic(
    font_provider: &fonts::ProviderProxy,
    name: Option<String>,
) -> Result<TypefaceInfo, Error> {
    get_typeface_info(font_provider, name, None, None).await
}
