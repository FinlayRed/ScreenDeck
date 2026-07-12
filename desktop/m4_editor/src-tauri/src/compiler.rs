use crate::model::{validate, ActionKind, Project, StepKind, ValidationIssue, MAX_BUNDLE_BYTES};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use thiserror::Error;

const SDB3_MAGIC: u32 = 0x3342_4453;
const M5UI_MAGIC: u32 = 0x4955_354D;
const M5UI_HEADER_BYTES: usize = 56;
const M5UI_PROFILE_BYTES: usize = 8;
const M5UI_BUTTON_BYTES: usize = 8;
const M5UI_ASSET_BYTES: usize = 20;
const MACRO_DESCRIPTOR_BYTES: usize = 8;
const MACRO_STEP_BYTES: usize = 8;

fn keyboard_usage(name: &str) -> Option<u16> {
    if name.len() == 1 {
        let value = name.as_bytes()[0];
        if value.is_ascii_uppercase() {
            return Some(u16::from(value - b'A') + 0x04);
        }
    }
    if let Some(number) = name
        .strip_prefix('F')
        .and_then(|value| value.parse::<u16>().ok())
    {
        if (13..=24).contains(&number) {
            return Some(0x68 + number - 13);
        }
    }
    Some(match name {
        "ENTER" => 0x28,
        "ESCAPE" => 0x29,
        "BACKSPACE" => 0x2A,
        "TAB" => 0x2B,
        "SPACE" => 0x2C,
        "DELETE" => 0x4C,
        "RIGHT" => 0x4F,
        "LEFT" => 0x50,
        "DOWN" => 0x51,
        "UP" => 0x52,
        "CTRL" => 0xE0,
        "SHIFT" => 0xE1,
        "ALT" => 0xE2,
        "GUI" => 0xE3,
        _ => return None,
    })
}

fn consumer_usage(name: &str) -> Option<u16> {
    Some(match name {
        "PLAY_PAUSE" => 0x00CD,
        "MUTE" => 0x00E2,
        "VOLUME_UP" => 0x00E9,
        "VOLUME_DOWN" => 0x00EA,
        "NEXT_TRACK" => 0x00B5,
        "PREVIOUS_TRACK" => 0x00B6,
        _ => return None,
    })
}

pub fn mjpeg_frame_count(bytes: &[u8]) -> Option<u16> {
    let mut frames = 0u16;
    let mut in_frame = false;
    let mut previous = 0u8;
    for &current in bytes {
        if !in_frame && previous == 0xFF && current == 0xD8 {
            in_frame = true;
        } else if in_frame && previous == 0xFF && current == 0xD9 {
            frames = frames.checked_add(1)?;
            in_frame = false;
        }
        previous = current;
    }
    (!in_frame && frames != 0).then_some(frames)
}

#[derive(Debug, Error)]
pub enum CompileError {
    #[error("project validation failed: {0}")]
    Invalid(String),
    #[error("could not serialize project: {0}")]
    Serialize(#[from] serde_json::Error),
    #[error("icon asset '{0}' does not contain valid base64 image data")]
    Asset(String),
    #[error("compiled bundle is {0} bytes; the device limit is 16 MiB")]
    TooLarge(usize),
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CompileSummary {
    pub bundle_bytes: usize,
    pub payload_crc32: u32,
    pub fingerprint: String,
    pub issues: Vec<ValidationIssue>,
}

pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0u32;
    for &value in data {
        crc ^= u32::from(value);
        for _ in 0..8 {
            crc = (crc >> 1) ^ if crc & 1 != 0 { 0xEDB8_8320 } else { 0 };
        }
    }
    !crc
}

pub fn compile(project: &Project) -> Result<Vec<u8>, CompileError> {
    let issues = validate(project);
    if !issues.is_empty() {
        return Err(CompileError::Invalid(
            issues
                .iter()
                .map(|item| format!("{}: {}", item.path, item.message))
                .collect::<Vec<_>>()
                .join("; "),
        ));
    }
    let page_count: usize = project
        .profiles
        .iter()
        .map(|profile| profile.pages.len())
        .sum();
    if project.assets.len() > u16::MAX as usize || page_count > u16::MAX as usize {
        return Err(CompileError::Invalid(
            "too many pages or icon assets for the device bundle".into(),
        ));
    }
    let profiles_offset = M5UI_HEADER_BYTES;
    let pages_offset = profiles_offset + project.profiles.len() * M5UI_PROFILE_BYTES;
    let assets_offset = pages_offset + page_count * 32 * M5UI_BUTTON_BYTES;
    let button_ref_count = page_count * 32;
    let step_count: usize = project.macros.iter().map(|item| item.steps.len()).sum();
    let button_macro_refs_offset = assets_offset + project.assets.len() * M5UI_ASSET_BYTES;
    let macro_descriptors_offset = button_macro_refs_offset + button_ref_count * 2;
    let macro_steps_offset =
        macro_descriptors_offset + project.macros.len() * MACRO_DESCRIPTOR_BYTES;
    let blob_offset = macro_steps_offset + step_count * MACRO_STEP_BYTES;
    let mut payload = Vec::with_capacity(blob_offset);
    payload.extend_from_slice(&M5UI_MAGIC.to_le_bytes());
    payload.extend_from_slice(&2u16.to_le_bytes());
    payload.extend_from_slice(&(M5UI_HEADER_BYTES as u16).to_le_bytes());
    payload.extend_from_slice(&(project.profiles.len() as u16).to_le_bytes());
    payload.extend_from_slice(&(page_count as u16).to_le_bytes());
    payload.extend_from_slice(&(project.assets.len() as u16).to_le_bytes());
    payload.extend_from_slice(&32u16.to_le_bytes());
    payload.extend_from_slice(&(project.macros.len() as u16).to_le_bytes());
    payload.extend_from_slice(&0u16.to_le_bytes());
    payload.extend_from_slice(&(step_count as u32).to_le_bytes());
    payload.extend_from_slice(&(profiles_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(pages_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(assets_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(button_macro_refs_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(macro_descriptors_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(macro_steps_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(blob_offset as u32).to_le_bytes());
    payload.extend_from_slice(&0u32.to_le_bytes());

    let mut first_page = 0u16;
    for profile in &project.profiles {
        payload.extend_from_slice(&first_page.to_le_bytes());
        payload.extend_from_slice(&(profile.pages.len() as u16).to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes());
        first_page += profile.pages.len() as u16;
    }
    let asset_indices: HashMap<&str, u16> = project
        .assets
        .iter()
        .enumerate()
        .map(|(index, asset)| (asset.id.as_str(), index as u16))
        .collect();
    let macro_indices: HashMap<&str, u16> = project
        .macros
        .iter()
        .enumerate()
        .map(|(index, item)| (item.id.as_str(), index as u16))
        .collect();
    let mut button_macro_refs = Vec::with_capacity(button_ref_count);
    for profile in &project.profiles {
        for page in &profile.pages {
            for button in &page.buttons {
                let accent = u32::from_str_radix(button.accent.trim_start_matches('#'), 16)
                    .unwrap_or(0x2A2C33);
                let asset = button
                    .icon_id
                    .as_deref()
                    .and_then(|id| asset_indices.get(id).copied())
                    .unwrap_or(u16::MAX);
                let action = match button.action {
                    ActionKind::None => 0,
                    ActionKind::Macro => 1,
                    ActionKind::PageNext => 2,
                    ActionKind::PagePrevious => 3,
                    ActionKind::ProfileNext => 4,
                };
                payload.extend_from_slice(&accent.to_le_bytes());
                payload.extend_from_slice(&asset.to_le_bytes());
                payload.push(action);
                payload.push(u8::from(button.image_fit.as_deref() == Some("contain")));
                button_macro_refs.push(if button.action == ActionKind::Macro {
                    button
                        .macro_id
                        .as_deref()
                        .and_then(|id| macro_indices.get(id).copied())
                        .unwrap_or(u16::MAX)
                } else {
                    u16::MAX
                });
            }
        }
    }
    let mut asset_blobs = Vec::with_capacity(project.assets.len() * 2);
    let mut next_blob = blob_offset;
    for asset in &project.assets {
        let (_, encoded) = asset
            .data_url
            .split_once(',')
            .ok_or_else(|| CompileError::Asset(asset.name.clone()))?;
        let bytes = STANDARD
            .decode(encoded)
            .map_err(|_| CompileError::Asset(asset.name.clone()))?;
        let static_offset = next_blob;
        next_blob += bytes.len();
        let mut animation_blob = None;
        let (animation_offset, animation_length, animation_frames, animation_fps) =
            if asset.animation_data_url.is_empty() {
                (0usize, 0usize, 0u16, 0u8)
            } else {
                let (_, encoded) = asset
                    .animation_data_url
                    .split_once(',')
                    .ok_or_else(|| CompileError::Asset(asset.name.clone()))?;
                let animation = STANDARD
                    .decode(encoded)
                    .map_err(|_| CompileError::Asset(asset.name.clone()))?;
                let frames = mjpeg_frame_count(&animation).ok_or_else(|| {
                    CompileError::Invalid(format!(
                        "animated icon '{}' is not a complete MJPEG stream",
                        asset.name
                    ))
                })?;
                let offset = next_blob;
                next_blob += animation.len();
                animation_blob = Some(animation);
                (offset, next_blob - offset, frames, asset.animation_fps)
            };
        payload.extend_from_slice(&(static_offset as u32).to_le_bytes());
        payload.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        payload.extend_from_slice(&(animation_offset as u32).to_le_bytes());
        payload.extend_from_slice(&(animation_length as u32).to_le_bytes());
        payload.extend_from_slice(&animation_frames.to_le_bytes());
        payload.push(if animation_length == 0 { 1 } else { 2 });
        payload.push(animation_fps);
        asset_blobs.push(bytes);
        if let Some(animation) = animation_blob {
            asset_blobs.push(animation);
        }
    }
    for reference in button_macro_refs {
        payload.extend_from_slice(&reference.to_le_bytes());
    }
    let mut first_step = 0u16;
    for item in &project.macros {
        payload.extend_from_slice(&first_step.to_le_bytes());
        payload.extend_from_slice(&(item.steps.len() as u16).to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes());
        first_step += item.steps.len() as u16;
    }
    for item in &project.macros {
        for step in &item.steps {
            let (kind, page, usage, duration) = match step.kind {
                StepKind::KeyDown => (
                    1,
                    0x07,
                    keyboard_usage(step.key.as_deref().unwrap_or("")),
                    0,
                ),
                StepKind::KeyUp => (
                    2,
                    0x07,
                    keyboard_usage(step.key.as_deref().unwrap_or("")),
                    0,
                ),
                StepKind::Delay => (3, 0, Some(0), step.duration_ms.unwrap_or(0)),
                StepKind::Consumer => (
                    4,
                    0x0C,
                    consumer_usage(step.key.as_deref().unwrap_or("")),
                    0,
                ),
            };
            let usage = usage.ok_or_else(|| {
                CompileError::Invalid(format!(
                    "unsupported {:?} key '{}' in macro '{}'",
                    step.kind,
                    step.key.as_deref().unwrap_or(""),
                    item.name
                ))
            })?;
            payload.push(kind);
            payload.push(page);
            payload.extend_from_slice(&usage.to_le_bytes());
            payload.extend_from_slice(&duration.to_le_bytes());
        }
    }
    debug_assert_eq!(payload.len(), blob_offset);
    for bytes in asset_blobs {
        payload.extend_from_slice(&bytes);
    }
    let total = payload.len() + 16;
    if total > MAX_BUNDLE_BYTES {
        return Err(CompileError::TooLarge(total));
    }
    let mut bundle = Vec::with_capacity(total);
    bundle.extend_from_slice(&SDB3_MAGIC.to_le_bytes());
    bundle.extend_from_slice(&1u16.to_le_bytes());
    bundle.extend_from_slice(&16u16.to_le_bytes());
    bundle.extend_from_slice(&(total as u32).to_le_bytes());
    bundle.extend_from_slice(&crc32(&payload).to_le_bytes());
    bundle.extend_from_slice(&payload);
    Ok(bundle)
}

pub fn summarize(project: &Project) -> CompileSummary {
    let issues = validate(project);
    let (bundle_bytes, payload_crc32, fingerprint) = match compile(project) {
        Ok(bundle) => {
            let hash = Sha256::digest(&bundle);
            (
                bundle.len(),
                u32::from_le_bytes(bundle[12..16].try_into().unwrap()),
                format!("{:x}", hash)[..12].to_owned(),
            )
        }
        Err(_) => (0, 0, String::new()),
    };
    CompileSummary {
        bundle_bytes,
        payload_crc32,
        fingerprint,
        issues,
    }
}
