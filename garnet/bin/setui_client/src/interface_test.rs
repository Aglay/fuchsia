// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![allow(dead_code)]

use {
    failure::{Error, ResultExt},
    fidl_fuchsia_settings::*,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    futures::prelude::*,
};

mod accessibility;
mod audio;
mod client;
mod device;
mod display;
mod do_not_disturb;
mod intl;
mod privacy;
mod setup;
mod system;

enum Services {
    SetUi(fidl_fuchsia_setui::SetUiServiceRequestStream),
    Accessibility(AccessibilityRequestStream),
    Audio(AudioRequestStream),
    Device(DeviceRequestStream),
    Display(DisplayRequestStream),
    DoNotDisturb(DoNotDisturbRequestStream),
    Intl(IntlRequestStream),
    Privacy(PrivacyRequestStream),
    Setup(SetupRequestStream),
    System(SystemRequestStream),
}

struct ExpectedStreamSettingsStruct {
    stream: Option<fidl_fuchsia_media::AudioRenderUsage>,
    source: Option<fidl_fuchsia_settings::AudioStreamSettingSource>,
    level: Option<f32>,
    volume_muted: Option<bool>,
    input_muted: Option<bool>,
}

const ENV_NAME: &str = "setui_client_test_environment";
const TEST_BUILD_TAG: &str = "0.20190909.1.0";

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    println!("account mutation tests");
    validate_account_mutate(
        "autologinguest".to_string(),
        fidl_fuchsia_setui::LoginOverride::AutologinGuest,
    )
    .await?;
    validate_account_mutate("auth".to_string(), fidl_fuchsia_setui::LoginOverride::AuthProvider)
        .await?;
    validate_account_mutate("none".to_string(), fidl_fuchsia_setui::LoginOverride::None).await?;

    println!("accessibility service tests");
    println!("  client calls set accessibility watch");
    validate_accessibility(None, None, None, None, None).await?;

    println!("  client calls set audio_description");
    validate_accessibility(Some(true), None, None, None, None).await?;

    println!("  client calls set screen_reader");
    validate_accessibility(None, Some(true), None, None, None).await?;

    println!("  client calls set color_inversion");
    validate_accessibility(None, None, Some(true), None, None).await?;

    println!("  client calls set enable_magnification");
    validate_accessibility(None, None, None, Some(true), None).await?;

    println!("  client calls set color_correction");
    validate_accessibility(
        None,
        None,
        None,
        None,
        Some(fidl_fuchsia_settings::ColorBlindnessType::Protanomaly),
    )
    .await?;

    println!("audio service tests");
    println!("  client calls audio watch");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
        source: Some(fidl_fuchsia_settings::AudioStreamSettingSource::User),
        level: Some(0.6),
        volume_muted: Some(false),
        input_muted: Some(true),
    })
    .await?;

    println!("  client calls set audio input - stream");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: Some(fidl_fuchsia_media::AudioRenderUsage::Background),
        source: None,
        level: None,
        volume_muted: None,
        input_muted: None,
    })
    .await?;

    println!("  client calls set audio input - source");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: None,
        source: Some(fidl_fuchsia_settings::AudioStreamSettingSource::System),
        level: None,
        volume_muted: None,
        input_muted: None,
    })
    .await?;

    println!("  client calls set audio input - level");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: None,
        source: None,
        level: Some(0.3),
        volume_muted: None,
        input_muted: None,
    })
    .await?;

    println!("  client calls set audio input - volume_muted");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: None,
        source: None,
        level: None,
        volume_muted: Some(true),
        input_muted: None,
    })
    .await?;

    println!("  client calls set audio input - input_muted");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: None,
        source: None,
        level: None,
        volume_muted: None,
        input_muted: Some(false),
    })
    .await?;

    println!("  client calls set audio input - multiple");
    validate_audio(&ExpectedStreamSettingsStruct {
        stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
        source: Some(fidl_fuchsia_settings::AudioStreamSettingSource::User),
        level: Some(0.6),
        volume_muted: Some(false),
        input_muted: Some(true),
    })
    .await?;

    println!("device service tests");
    println!("  client calls device watch");
    validate_device().await?;

    println!("display service tests");
    println!("  client calls display watch");
    validate_display(None, None).await?;

    println!("  client calls set brightness");
    validate_display(Some(0.5), None).await?;

    println!("  client calls set auto brightness");
    validate_display(None, Some(true)).await?;

    println!("do not disturb service tests");
    println!("  client calls dnd watch");
    validate_dnd(Some(false), Some(false)).await?;

    println!("  client calls set user initiated do not disturb");
    validate_dnd(Some(true), Some(false)).await?;

    println!("  client calls set night mode initiated do not disturb");
    validate_dnd(Some(false), Some(true)).await?;

    println!("intl service tests");
    println!("  client calls set temperature unit");
    validate_temperature_unit().await?;

    println!("privacy service tests");
    println!("  client calls privacy watch");
    validate_privacy(None).await?;

    println!("  client calls set user_data_sharing_consent");
    validate_privacy(Some(true)).await?;

    println!("  set() output");
    validate_privacy_set_output(true).await?;
    validate_privacy_set_output(false).await?;

    println!("  watch() output");
    validate_privacy_watch_output(None).await?;
    validate_privacy_watch_output(Some(true)).await?;
    validate_privacy_watch_output(Some(false)).await?;

    println!("system service tests");
    println!("  client calls set login mode");
    validate_system_override().await?;

    println!("setup service tests");
    println!(" client calls set config interfaces");
    validate_setup().await?;

    Ok(())
}

// Creates a service in an environment for a given setting type.
// Usage: create_service!(service_enum_name,
//          request_name => {code block},
//          request2_name => {code_block}
//          ... );
macro_rules! create_service  {
    ($setting_type:path, $( $request:pat => $callback:block ),*) => {{

        let mut fs = ServiceFs::new();
        fs.add_fidl_service($setting_type);
        let env = fs.create_nested_environment(ENV_NAME)?;

        fasync::spawn(fs.for_each_concurrent(None, move |connection| {
            async move {
                #![allow(unreachable_patterns)]
                match connection {
                    $setting_type(stream) => {
                        stream
                            .err_into::<failure::Error>()
                            .try_for_each(|req| async move {
                                match req {
                                    $($request => $callback)*
                                    _ => panic!("Incorrect command to service"),
                                }
                                Ok(())
                            })
                            .unwrap_or_else(|e: failure::Error| panic!(
                                "error running setui server: {:?}",
                                e
                            )).await;
                    }
                    _ => {
                        panic!("Unexpected service");
                    }
                }
            }
        }));
        env
    }};
}

async fn validate_system_override() -> Result<(), Error> {
    let env = create_service!(Services::System,
        SystemRequest::Set { settings, responder } => {
            if let Some(login_override) = settings.mode {
                assert_eq!(login_override, LoginOverride::AuthProvider);
                responder.send(&mut Ok(()))?;
            } else {
                panic!("Wrong call to set");
            }
    });

    let system_service =
        env.connect_to_service::<SystemMarker>().context("Failed to connect to intl service")?;

    system::command(system_service, Some("auth".to_string())).await?;

    Ok(())
}

async fn validate_temperature_unit() -> Result<(), Error> {
    let env = create_service!(Services::Intl,
        IntlRequest::Set { settings, responder } => {
            if let Some(temperature_unit) = settings.temperature_unit {
            assert_eq!(
                temperature_unit,
                fidl_fuchsia_intl::TemperatureUnit::Celsius
            );
            responder.send(&mut Ok(()))?;
            } else {
                panic!("Wrong call to set");
            }
    });

    let intl_service =
        env.connect_to_service::<IntlMarker>().context("Failed to connect to intl service")?;

    intl::command(intl_service, None, Some(fidl_fuchsia_intl::TemperatureUnit::Celsius), vec![])
        .await?;

    Ok(())
}

async fn validate_device() -> Result<(), Error> {
    let env = create_service!(Services::Device,
        DeviceRequest::Watch { responder } => {
            responder.send(DeviceSettings {
                build_tag: Some(TEST_BUILD_TAG.to_string()),
            })?;
        }
    );

    let device_service =
        env.connect_to_service::<DeviceMarker>().context("Failed to connect to device service")?;

    device::command(device_service).await?;

    Ok(())
}

// Can only check one mutate option at once
async fn validate_display(
    expected_brightness: Option<f32>,
    expected_auto_brightness: Option<bool>,
) -> Result<(), Error> {
    let env = create_service!(
        Services::Display, DisplayRequest::Set { settings, responder, } => {
            if let (Some(brightness_value), Some(expected_brightness_value)) =
              (settings.brightness_value, expected_brightness) {
                assert_eq!(brightness_value, expected_brightness_value);
                responder.send(&mut Ok(()))?;
            } else if let (Some(auto_brightness), Some(expected_auto_brightness_value)) =
              (settings.auto_brightness, expected_auto_brightness) {
                assert_eq!(auto_brightness, expected_auto_brightness_value);
                responder.send(&mut Ok(()))?;
            } else {
                panic!("Unexpected call to set");
            }
        },
        DisplayRequest::Watch { responder } => {
            responder.send(&mut Ok(DisplaySettings {
                auto_brightness: Some(false),
                brightness_value: Some(0.5),
            }))?;
        }
    );

    let display_service = env
        .connect_to_service::<DisplayMarker>()
        .context("Failed to connect to display service")?;

    display::command(display_service, expected_brightness, expected_auto_brightness).await?;

    Ok(())
}

async fn validate_accessibility(
    expected_audio_description: Option<bool>,
    expected_screen_reader: Option<bool>,
    expected_color_inversion: Option<bool>,
    expected_enable_magnification: Option<bool>,
    expected_color_correction: Option<fidl_fuchsia_settings::ColorBlindnessType>,
) -> Result<(), Error> {
    let env = create_service!(
        Services::Accessibility, AccessibilityRequest::Set { settings, responder, } => {
            if let (Some(audio_description), Some(expected_audio_description_value)) =
                (settings.audio_description, expected_audio_description) {
                assert_eq!(audio_description, expected_audio_description_value);
                responder.send(&mut Ok(()))?;
            } else if let (Some(screen_reader), Some(expected_screen_reader_value)) =
                (settings.screen_reader, expected_screen_reader) {
                assert_eq!(screen_reader, expected_screen_reader_value);
                responder.send(&mut Ok(()))?;
            } else if let (Some(color_inversion), Some(expected_color_inversion_value)) =
                (settings.color_inversion, expected_color_inversion) {
                assert_eq!(color_inversion, expected_color_inversion_value);
                responder.send(&mut Ok(()))?;
            } else if let (Some(enable_magnification), Some(expected_enable_magnification_value)) =
                (settings.enable_magnification, expected_enable_magnification) {
                assert_eq!(enable_magnification, expected_enable_magnification_value);
                responder.send(&mut Ok(()))?;
            } else if let (Some(color_correction), Some(expected_color_correction_value)) =
                (settings.color_correction, expected_color_correction) {
                assert_eq!(color_correction, expected_color_correction_value);
                responder.send(&mut Ok(()))?;
            } else {
                panic!("Unexpected call to set");
            }
        },
        AccessibilityRequest::Watch { responder } => {
            responder.send(&mut Ok(AccessibilitySettings::empty()))?;
        }
    );

    let accessibility_service = env
        .connect_to_service::<AccessibilityMarker>()
        .context("Failed to connect to accessibility service")?;

    accessibility::command(
        accessibility_service,
        false,
        expected_audio_description,
        expected_screen_reader,
        expected_color_inversion,
        expected_enable_magnification,
        expected_color_correction,
    )
    .await?;

    Ok(())
}

async fn validate_audio(expected: &'static ExpectedStreamSettingsStruct) -> Result<(), Error> {
    let env = create_service!(Services::Audio,
        AudioRequest::Set { settings, responder } => {
            if let Some(streams) = settings.streams {
                verify_streams(streams, expected);
                responder.send(&mut (Ok(())))?;
            } else if let Some(input) = settings.input {
                if let (Some(input_muted), Some(expected_input_muted)) =
                    (input.muted, expected.input_muted) {
                    assert_eq!(input_muted, expected_input_muted);
                    responder.send(&mut (Ok(())))?;
                }
            }
        },
        AudioRequest::Watch { responder } => {
            responder.send(&mut Ok(AudioSettings {
                streams: Some(vec![AudioStreamSettings {
                    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
                    source: Some(fidl_fuchsia_settings::AudioStreamSettingSource::User),
                    user_volume: Some(Volume {
                        level: Some(0.6),
                        muted: Some(false)
                    }),
                }]),
                input: Some(AudioInput {
                    muted: Some(true)
                }),
            }))?;
        }
    );

    let audio_service =
        env.connect_to_service::<AudioMarker>().context("Failed to connect to audio service")?;

    audio::command(
        audio_service,
        expected.stream,
        expected.source,
        expected.level,
        expected.volume_muted,
        expected.input_muted,
    )
    .await?;
    Ok(())
}

fn verify_streams(
    streams: Vec<AudioStreamSettings>,
    expected: &'static ExpectedStreamSettingsStruct,
) {
    let extracted_stream_settings = streams.get(0).unwrap();
    if let (Some(stream), Some(expected_stream)) =
        (extracted_stream_settings.stream, expected.stream)
    {
        assert_eq!(stream, expected_stream);
    }
    if let (Some(source), Some(expected_source)) =
        (extracted_stream_settings.source, expected.source)
    {
        assert_eq!(source, expected_source);
    }
    if let Some(user_volume) = extracted_stream_settings.user_volume.as_ref() {
        if let (Some(level), Some(expected_level)) = (user_volume.level, expected.level) {
            assert_eq!(level, expected_level);
        }
        if let (Some(volume_muted), Some(expected_volume_muted)) =
            (user_volume.muted, expected.volume_muted)
        {
            assert_eq!(volume_muted, expected_volume_muted);
        }
    }
}

async fn validate_dnd(
    expected_user_dnd: Option<bool>,
    expected_night_mode_dnd: Option<bool>,
) -> Result<(), Error> {
    let env = create_service!(Services::DoNotDisturb,
        DoNotDisturbRequest::Set { settings, responder } => {
            if let(Some(user_dnd), Some(expected_user_dnd)) =
                (settings.user_initiated_do_not_disturb, expected_user_dnd) {
                assert_eq!(user_dnd, expected_user_dnd);
                responder.send(&mut Ok(()))?;
            } else if let (Some(night_mode_dnd), Some(expected_night_mode_dnd)) =
                (settings.night_mode_initiated_do_not_disturb, expected_night_mode_dnd) {
                assert_eq!(night_mode_dnd, expected_night_mode_dnd);
                responder.send(&mut (Ok(())))?;
            } else {
                panic!("Unexpected call to set");
            }
        },
        DoNotDisturbRequest::Watch { responder } => {
            responder.send(DoNotDisturbSettings {
                user_initiated_do_not_disturb: Some(false),
                night_mode_initiated_do_not_disturb: Some(false),
            })?;
        }
    );

    let do_not_disturb_service = env
        .connect_to_service::<DoNotDisturbMarker>()
        .context("Failed to connect to do not disturb service")?;

    do_not_disturb::command(do_not_disturb_service, expected_user_dnd, expected_night_mode_dnd)
        .await?;

    Ok(())
}

async fn validate_privacy(expected_user_data_sharing_consent: Option<bool>) -> Result<(), Error> {
    let env = create_service!(
        Services::Privacy, PrivacyRequest::Set { settings, responder, } => {
            if let (Some(user_data_sharing_consent), Some(expected_user_data_sharing_consent_value)) =
                (settings.user_data_sharing_consent, expected_user_data_sharing_consent) {
                assert_eq!(user_data_sharing_consent, expected_user_data_sharing_consent_value);
                responder.send(&mut Ok(()))?;
            } else {
                panic!("Unexpected call to set");
            }
        },
        PrivacyRequest::Watch { responder } => {
            responder.send(&mut Ok(PrivacySettings {
                user_data_sharing_consent: Some(false),
            }))?;
        }
    );

    let privacy_service = env
        .connect_to_service::<PrivacyMarker>()
        .context("Failed to connect to privacy service")?;

    privacy::command(privacy_service, expected_user_data_sharing_consent).await?;

    Ok(())
}

async fn validate_privacy_set_output(
    expected_user_data_sharing_consent: bool,
) -> Result<(), Error> {
    let env = create_service!(
        Services::Privacy, PrivacyRequest::Set { settings: _, responder, } => {
            responder.send(&mut Ok(()))?;
        },
        PrivacyRequest::Watch { responder } => {
            responder.send(&mut Ok(PrivacySettings {
                user_data_sharing_consent: Some(expected_user_data_sharing_consent),
            }))?;
        }
    );

    let privacy_service = env
        .connect_to_service::<PrivacyMarker>()
        .context("Failed to connect to privacy service")?;

    let output =
        privacy::command(privacy_service, Some(expected_user_data_sharing_consent)).await?;

    assert_eq!(
        output,
        format!(
            "Successfully set user_data_sharing_consent to {}",
            expected_user_data_sharing_consent
        )
    );

    Ok(())
}

async fn validate_privacy_watch_output(
    expected_user_data_sharing_consent: Option<bool>,
) -> Result<(), Error> {
    let env = create_service!(
        Services::Privacy, PrivacyRequest::Set { settings: _, responder, } => {
            responder.send(&mut Ok(()))?;
        },
        PrivacyRequest::Watch { responder } => {
            responder.send(&mut Ok(PrivacySettings {
                user_data_sharing_consent: expected_user_data_sharing_consent,
            }))?;
        }
    );

    let privacy_service = env
        .connect_to_service::<PrivacyMarker>()
        .context("Failed to connect to privacy service")?;

    // Pass in None to call Watch() on the service.
    let output = privacy::command(privacy_service, None).await?;

    assert_eq!(
        output,
        format!(
            "{:?}",
            PrivacySettings { user_data_sharing_consent: expected_user_data_sharing_consent }
        )
    );

    Ok(())
}

fn create_setup_setting(interfaces: ConfigurationInterfaces) -> SetupSettings {
    let mut settings = SetupSettings::empty();
    settings.enabled_configuration_interfaces = Some(interfaces);

    settings
}

async fn validate_setup() -> Result<(), Error> {
    let expected_set_interfaces = ConfigurationInterfaces::Ethernet;
    let expected_watch_interfaces =
        ConfigurationInterfaces::Wifi | ConfigurationInterfaces::Ethernet;
    let env = create_service!(
        Services::Setup, SetupRequest::Set { settings, responder, } => {
            if let Some(interfaces) = settings.enabled_configuration_interfaces {
                assert_eq!(interfaces, expected_set_interfaces);
                responder.send(&mut Ok(()))?;
            } else {
                panic!("Unexpected call to set");
            }
        },
        SetupRequest::Watch { responder } => {
            responder.send(create_setup_setting(expected_watch_interfaces))?;
        }
    );

    let setup_service =
        env.connect_to_service::<SetupMarker>().context("Failed to connect to setup service")?;

    setup::command(setup_service.clone(), Some(expected_set_interfaces)).await?;

    let watch_result = setup::command(setup_service.clone(), None).await?;

    assert_eq!(
        watch_result,
        setup::describe_setup_setting(&create_setup_setting(expected_watch_interfaces))
    );

    Ok(())
}

async fn validate_account_mutate(
    specified_type: String,
    expected_override: fidl_fuchsia_setui::LoginOverride,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.add_fidl_service(Services::SetUi);
    let env = fs.create_nested_environment(ENV_NAME)?;

    fasync::spawn(fs.for_each_concurrent(None, move |req| {
        async move {
            match req {
                Services::SetUi(stream) => {
                    serve_check_login_override_mutate(stream, expected_override).await
                }
                _ => {}
            }
        }
    }));

    let setui = env
        .connect_to_service::<fidl_fuchsia_setui::SetUiServiceMarker>()
        .context("Failed to connect to setui service")?;

    client::mutate(setui, "login".to_string(), specified_type).await?;
    Ok(())
}

fn serve_check_login_override_mutate(
    stream: fidl_fuchsia_setui::SetUiServiceRequestStream,
    expected_override: fidl_fuchsia_setui::LoginOverride,
) -> impl Future<Output = ()> {
    stream
        .err_into::<failure::Error>()
        .try_for_each(move |req| {
            async move {
                match req {
                    fidl_fuchsia_setui::SetUiServiceRequest::Mutate {
                        setting_type,
                        mutation,
                        responder,
                    } => {
                        assert_eq!(setting_type, fidl_fuchsia_setui::SettingType::Account);

                        match mutation {
                            fidl_fuchsia_setui::Mutation::AccountMutationValue(
                                account_mutation,
                            ) => {
                                if let (Some(login_override), Some(operation)) =
                                    (account_mutation.login_override, account_mutation.operation)
                                {
                                    assert_eq!(login_override, expected_override);
                                    assert_eq!(
                                        operation,
                                        fidl_fuchsia_setui::AccountOperation::SetLoginOverride
                                    );
                                }
                            }
                            _ => {
                                panic!("unexpected data for account mutation");
                            }
                        }
                        responder
                            .send(&mut fidl_fuchsia_setui::MutationResponse {
                                return_code: fidl_fuchsia_setui::ReturnCode::Ok,
                            })
                            .context("sending response")?;
                    }
                    _ => {}
                };
                Ok(())
            }
        })
        .unwrap_or_else(|e: failure::Error| panic!("error running setui server: {:?}", e))
}
