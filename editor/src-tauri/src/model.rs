// SPDX-License-Identifier: GPL-3.0-or-later

use serde::{Deserialize, Serialize};
use std::collections::HashSet;

pub const MAX_PROFILES: usize = 8;
pub const MAX_PAGES_PER_PROFILE: usize = 16;
pub const BUTTONS_PER_PAGE: usize = 32;
pub const MAX_MACROS: usize = 128;
pub const MAX_STEPS: usize = 64;
pub const MAX_BUNDLE_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Project {
    pub schema_version: u16,
    pub name: String,
    #[serde(default = "default_screensaver_timeout")]
    pub screensaver_timeout_seconds: u32,
    #[serde(default = "default_brightness")]
    pub brightness_percent: u8,
    #[serde(default = "default_orientation")]
    pub orientation: String,
    #[serde(default = "default_true")]
    pub screensaver_enabled: bool,
    #[serde(default = "default_empty_button_style")]
    pub empty_button_style: String,
    pub profiles: Vec<Profile>,
    pub macros: Vec<Macro>,
    pub assets: Vec<Asset>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub pages: Vec<Page>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Page {
    pub id: String,
    pub name: String,
    pub buttons: Vec<Button>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Button {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub image_fit: Option<String>,
    pub action: ActionKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub macro_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radial: Option<RadialMenu>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RadialMenu {
    pub size: u8,
    pub items: Vec<RadialItem>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RadialItem {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub image_fit: Option<String>,
    #[serde(default = "default_radial_action")]
    pub action: ActionKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub macro_id: Option<String>,
}

fn default_screensaver_timeout() -> u32 {
    15
}
fn default_brightness() -> u8 {
    80
}
fn default_orientation() -> String {
    "landscape".into()
}
fn default_true() -> bool {
    true
}
fn default_empty_button_style() -> String {
    "grey".into()
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum ActionKind {
    Macro,
    PageNext,
    PagePrevious,
    ProfileNext,
    None,
}

fn default_radial_action() -> ActionKind {
    ActionKind::Macro
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Macro {
    pub id: String,
    pub name: String,
    pub steps: Vec<MacroStep>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MacroStep {
    pub kind: StepKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub key: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub duration_ms: Option<u32>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub modifiers: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum StepKind {
    KeyPress,
    KeyDown,
    KeyUp,
    Delay,
    Consumer,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Asset {
    pub id: String,
    pub name: String,
    pub media_type: String,
    #[serde(default)]
    pub data_url: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub source_name: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub source_media_type: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub source_data_url: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub animation_data_url: String,
    #[serde(default)]
    pub animation_fps: u8,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ValidationIssue {
    pub path: String,
    pub message: String,
    pub severity: &'static str,
}

fn issue(path: impl Into<String>, message: impl Into<String>) -> ValidationIssue {
    ValidationIssue {
        path: path.into(),
        message: message.into(),
        severity: "error",
    }
}

fn is_keyboard_key(value: &str) -> bool {
    (value.len() == 1 && value.as_bytes()[0].is_ascii_uppercase())
        || value
            .strip_prefix('F')
            .and_then(|number| number.parse::<u8>().ok())
            .is_some_and(|number| (13..=24).contains(&number))
        || matches!(
            value,
            "ENTER"
                | "ESCAPE"
                | "TAB"
                | "SPACE"
                | "BACKSPACE"
                | "DELETE"
                | "LEFT"
                | "RIGHT"
                | "UP"
                | "DOWN"
                | "CTRL"
                | "SHIFT"
                | "ALT"
                | "GUI"
        )
}

fn is_press_key(value: &str) -> bool {
    is_keyboard_key(value) && !matches!(value, "CTRL" | "SHIFT" | "ALT" | "GUI")
}

fn is_consumer_key(value: &str) -> bool {
    matches!(
        value,
        "VOLUME_UP" | "VOLUME_DOWN" | "MUTE" | "PLAY_PAUSE" | "NEXT_TRACK" | "PREVIOUS_TRACK"
    )
}

pub fn migrate(project: &mut Project) {
    if project.schema_version == 2 {
        project.schema_version = 3;
        if project.orientation.is_empty() {
            project.orientation = "landscape".into();
        }
    }
    if project.empty_button_style == "black" {
        project.empty_button_style = "grey".into();
    }
}

pub fn validate(project: &Project) -> Vec<ValidationIssue> {
    let mut issues = Vec::new();
    if project.schema_version != 3 {
        issues.push(issue(
            "schemaVersion",
            "Only project schema version 3 is supported.",
        ));
    }
    if project.name.trim().is_empty() || project.name.len() > 80 {
        issues.push(issue("name", "Project name must contain 1–80 characters."));
    }
    if !(5..=3600).contains(&project.screensaver_timeout_seconds) {
        issues.push(issue(
            "screensaverTimeoutSeconds",
            "Screensaver delay must be between 5 seconds and 60 minutes.",
        ));
    }
    if project.brightness_percent > 100 {
        issues.push(issue(
            "brightnessPercent",
            "Brightness must be between 0% and 100%.",
        ));
    }
    if !matches!(
        project.orientation.as_str(),
        "landscape" | "landscape_flipped"
    ) {
        issues.push(issue(
            "orientation",
            "Orientation must be landscape or landscape flipped.",
        ));
    }
    if !matches!(project.empty_button_style.as_str(), "grey" | "hidden") {
        issues.push(issue(
            "emptyButtonStyle",
            "Empty key appearance must be grey or hidden.",
        ));
    }
    if project.profiles.is_empty() || project.profiles.len() > MAX_PROFILES {
        issues.push(issue(
            "profiles",
            format!("A project needs 1–{MAX_PROFILES} profiles."),
        ));
    }
    if project.macros.len() > MAX_MACROS {
        issues.push(issue(
            "macros",
            format!("At most {MAX_MACROS} macros are supported."),
        ));
    }

    let macro_ids: HashSet<_> = project.macros.iter().map(|item| item.id.as_str()).collect();
    let asset_ids: HashSet<_> = project.assets.iter().map(|item| item.id.as_str()).collect();
    if macro_ids.len() != project.macros.len() {
        issues.push(issue("macros", "Macro IDs must be unique."));
    }
    if asset_ids.len() != project.assets.len() {
        issues.push(issue("assets", "Asset IDs must be unique."));
    }
    for (index, asset) in project.assets.iter().enumerate() {
        if asset.data_url.is_empty() {
            issues.push(issue(
                format!("assets[{index}].dataUrl"),
                "A device-ready image is required.",
            ));
        }
        let source_fields = [
            &asset.source_name,
            &asset.source_media_type,
            &asset.source_data_url,
        ];
        if source_fields.iter().any(|value| !value.is_empty())
            && source_fields.iter().any(|value| value.is_empty())
        {
            issues.push(issue(
                format!("assets[{index}].source"),
                "Original asset name, media type and data must be stored together.",
            ));
        }
        if !asset.animation_data_url.is_empty() && asset.animation_fps != 15 {
            issues.push(issue(
                format!("assets[{index}].animationFps"),
                "Animated icons must use the deterministic 15 FPS preset.",
            ));
        }
    }

    for (pi, profile) in project.profiles.iter().enumerate() {
        if profile.pages.is_empty() || profile.pages.len() > MAX_PAGES_PER_PROFILE {
            issues.push(issue(
                format!("profiles[{pi}].pages"),
                format!("A profile needs 1–{MAX_PAGES_PER_PROFILE} pages."),
            ));
        }
        for (pgi, page) in profile.pages.iter().enumerate() {
            if page.buttons.len() != BUTTONS_PER_PAGE {
                issues.push(issue(
                    format!("profiles[{pi}].pages[{pgi}].buttons"),
                    "A page must contain exactly 32 buttons.",
                ));
            }
            for (bi, button) in page.buttons.iter().enumerate() {
                let path = format!("profiles[{pi}].pages[{pgi}].buttons[{bi}]");
                if button.action == ActionKind::Macro
                    && button
                        .macro_id
                        .as_deref()
                        .is_none_or(|id| !macro_ids.contains(id))
                {
                    issues.push(issue(
                        path.clone(),
                        "The button references a missing macro.",
                    ));
                }
                if button
                    .icon_id
                    .as_deref()
                    .is_some_and(|id| !asset_ids.contains(id))
                {
                    issues.push(issue(path.clone(), "The button references a missing icon."));
                }
                if let Some(radial) = &button.radial {
                    if !matches!(radial.size, 4 | 6 | 8)
                        || radial.items.len() != radial.size as usize
                    {
                        issues.push(issue(
                            format!("{path}.radial"),
                            "A radial menu must contain exactly 4, 6, or 8 items.",
                        ));
                    }
                    for (ri, item) in radial.items.iter().enumerate() {
                        if item.action == ActionKind::Macro
                            && item
                                .macro_id
                                .as_deref()
                                .is_none_or(|id| !macro_ids.contains(id))
                        {
                            issues.push(issue(
                                format!("{path}.radial.items[{ri}].macroId"),
                                "The radial item references a missing macro.",
                            ));
                        }
                        if item
                            .icon_id
                            .as_deref()
                            .is_some_and(|id| !asset_ids.contains(id))
                        {
                            issues.push(issue(
                                format!("{path}.radial.items[{ri}].iconId"),
                                "The radial item references a missing icon.",
                            ));
                        }
                        if item
                            .image_fit
                            .as_deref()
                            .is_some_and(|fit| fit != "cover" && fit != "contain")
                        {
                            issues.push(issue(
                                format!("{path}.radial.items[{ri}].imageFit"),
                                "Artwork display must be cover or contain.",
                            ));
                        }
                    }
                }
                if button
                    .image_fit
                    .as_deref()
                    .is_some_and(|fit| fit != "cover" && fit != "contain")
                {
                    issues.push(issue(
                        format!("profiles[{pi}].pages[{pgi}].buttons[{bi}].imageFit"),
                        "Artwork display must be cover or contain.",
                    ));
                }
            }
        }
    }
    for (mi, item) in project.macros.iter().enumerate() {
        let mut held_keys = HashSet::new();
        if item.steps.len() > MAX_STEPS {
            issues.push(issue(
                format!("macros[{mi}].steps"),
                format!("A macro can contain at most {MAX_STEPS} steps."),
            ));
        }
        for (si, step) in item.steps.iter().enumerate() {
            let path = format!("macros[{mi}].steps[{si}]");
            if step.kind == StepKind::Delay
                && !(1..=60_000).contains(&step.duration_ms.unwrap_or(0))
            {
                issues.push(issue(
                    path.clone(),
                    "Delay must be between 1 and 60,000 ms.",
                ));
            }
            if step.kind == StepKind::KeyPress
                && step.key.as_deref().is_none_or(|key| !is_press_key(key))
            {
                issues.push(issue(path.clone(), "Choose a non-modifier key; use the Ctrl, Shift, Alt, and Win toggles for modifiers."));
            } else if matches!(step.kind, StepKind::KeyDown | StepKind::KeyUp)
                && step.key.as_deref().is_none_or(|key| !is_keyboard_key(key))
            {
                issues.push(issue(path.clone(), "Choose a supported keyboard HID key."));
            } else if step.kind == StepKind::Consumer
                && step.key.as_deref().is_none_or(|key| !is_consumer_key(key))
            {
                issues.push(issue(
                    path.clone(),
                    "Choose a supported consumer-control HID key.",
                ));
            }
            if step.kind == StepKind::KeyPress {
                if !(1..=1000).contains(&step.duration_ms.unwrap_or(25)) {
                    issues.push(issue(
                        path.clone(),
                        "Key press duration must be between 1 and 1,000 ms.",
                    ));
                }
                if step
                    .modifiers
                    .iter()
                    .any(|value| !matches!(value.as_str(), "CTRL" | "SHIFT" | "ALT" | "GUI"))
                {
                    issues.push(issue(
                        path,
                        "Choose only Ctrl, Shift, Alt, or Win modifiers.",
                    ));
                }
            } else if !step.modifiers.is_empty() {
                issues.push(issue(
                    path,
                    "Modifiers are only supported by Key press steps.",
                ));
            }
            if let Some(key) = step.key.as_deref().filter(|key| is_keyboard_key(key)) {
                if step.kind == StepKind::KeyDown && !held_keys.insert(key) {
                    issues.push(issue(
                        format!("macros[{mi}].steps[{si}]"),
                        format!("{key} is already held by this macro."),
                    ));
                } else if step.kind == StepKind::KeyUp && !held_keys.remove(key) {
                    issues.push(issue(
                        format!("macros[{mi}].steps[{si}]"),
                        format!("{key} is not currently held by this macro."),
                    ));
                }
            }
        }
        let mut unreleased: Vec<_> = held_keys.into_iter().collect();
        unreleased.sort_unstable();
        for key in unreleased {
            issues.push(issue(
                format!("macros[{mi}].steps"),
                format!("{key} must be released before the macro ends."),
            ));
        }
    }
    issues
}
