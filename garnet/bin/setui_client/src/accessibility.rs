// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {failure::Error, fidl_fuchsia_settings::*};

pub async fn command(
    proxy: AccessibilityProxy,
    audio_description: Option<bool>,
    color_correction: Option<ColorBlindnessType>,
) -> Result<String, Error> {
    let mut output = String::new();

    if let Some(audio_description_value) = audio_description {
        let mut settings = AccessibilitySettings::empty();
        settings.audio_description = Some(audio_description_value);

        let mutate_result = proxy.set(settings).await?;
        match mutate_result {
            Ok(_) => output.push_str(&format!(
                "Successfully set audio_description to {}",
                audio_description_value
            )),
            Err(err) => output.push_str(&format!("{:?}", err)),
        }
    } else if let Some(color_correction_value) = color_correction {
        let mut settings = AccessibilitySettings::empty();
        settings.color_correction = Some(color_correction_value);

        let mutate_result = proxy.set(settings).await?;
        match mutate_result {
            Ok(_) => output.push_str(&format!(
                "Successfully set color_correction to {}",
                describe_color_blindness_type(&color_correction_value)
            )),
            Err(err) => output.push_str(&format!("{:?}", err)),
        }
    } else {
        let setting = proxy.watch().await?;

        match setting {
            Ok(setting_value) => {
                let setting_string = describe_accessibility_setting(&setting_value);
                output.push_str(&setting_string);
            }
            Err(err) => output.push_str(&format!("{:?}", err)),
        }
    }

    Ok(output)
}

fn describe_accessibility_setting(accessibility_setting: &AccessibilitySettings) -> String {
    let mut output = String::new();

    output.push_str("Accessibility { ");

    if let Some(audio_description) = accessibility_setting.audio_description {
        output.push_str(&format!("audio_description: {} ", audio_description))
    }

    if let Some(color_correction) = accessibility_setting.color_correction {
        output.push_str(&format!(
            "color_correction: {} ",
            describe_color_blindness_type(&color_correction)
        ))
    }

    output.push_str("}");

    return output;
}

fn describe_color_blindness_type(color_blindness_type: &ColorBlindnessType) -> String {
    match color_blindness_type {
        fidl_fuchsia_settings::ColorBlindnessType::None => "none".into(),
        fidl_fuchsia_settings::ColorBlindnessType::Protanomaly => "protanomaly".into(),
        fidl_fuchsia_settings::ColorBlindnessType::Deuteranomaly => "deuteranomaly".into(),
        fidl_fuchsia_settings::ColorBlindnessType::Tritanomaly => "tritanomaly".into(),
    }
}
