use crate::model::{
    validate, ActionKind, Asset, Button, Macro, MacroStep, Page, Profile, Project, RadialItem, RadialMenu, StepKind,
    ValidationIssue, MAX_BUNDLE_BYTES,
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use thiserror::Error;

const SDB3_MAGIC: u32 = 0x3342_4453;
const M5UI_MAGIC: u32 = 0x4955_354D;
const M5UI_HEADER_BYTES: usize = 72;
const M5UI_PROFILE_BYTES: usize = 8;
const M5UI_BUTTON_BYTES: usize = 8;
const M5UI_ASSET_BYTES: usize = 20;
const MACRO_DESCRIPTOR_BYTES: usize = 8;
const MACRO_STEP_BYTES: usize = 8;
const RADIAL_DESCRIPTOR_BYTES: usize = 4;
const RADIAL_ITEM_BYTES: usize = 4;
const RADIAL_ACTION_PROFILE_NEXT: u16 = u16::MAX - 3;
const RADIAL_ACTION_PAGE_PREVIOUS: u16 = u16::MAX - 2;
const RADIAL_ACTION_PAGE_NEXT: u16 = u16::MAX - 1;
const RADIAL_ACTION_NONE: u16 = u16::MAX;

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
    compile_validated(project)
}

fn compile_validated(project: &Project) -> Result<Vec<u8>, CompileError> {
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
    let radial_count: usize = project.profiles.iter().flat_map(|p| &p.pages).flat_map(|p| &p.buttons).filter(|b| b.radial.is_some()).count();
    let radial_item_count: usize = project.profiles.iter().flat_map(|p| &p.pages).flat_map(|p| &p.buttons).filter_map(|b| b.radial.as_ref()).map(|r| r.items.len()).sum();
    let button_macro_refs_offset = assets_offset + project.assets.len() * M5UI_ASSET_BYTES;
    let macro_descriptors_offset = button_macro_refs_offset + button_ref_count * 2;
    let macro_steps_offset =
        macro_descriptors_offset + project.macros.len() * MACRO_DESCRIPTOR_BYTES;
    let radial_descriptors_offset = macro_steps_offset + step_count * MACRO_STEP_BYTES;
    let radial_items_offset = radial_descriptors_offset + radial_count * RADIAL_DESCRIPTOR_BYTES;
    let blob_offset = radial_items_offset + radial_item_count * RADIAL_ITEM_BYTES;
    let mut payload = Vec::with_capacity(blob_offset);
    payload.extend_from_slice(&M5UI_MAGIC.to_le_bytes());
    payload.extend_from_slice(&3u16.to_le_bytes());
    payload.extend_from_slice(&(M5UI_HEADER_BYTES as u16).to_le_bytes());
    payload.extend_from_slice(&(project.profiles.len() as u16).to_le_bytes());
    payload.extend_from_slice(&(page_count as u16).to_le_bytes());
    payload.extend_from_slice(&(project.assets.len() as u16).to_le_bytes());
    payload.extend_from_slice(&32u16.to_le_bytes());
    payload.extend_from_slice(&(project.macros.len() as u16).to_le_bytes());
    payload.extend_from_slice(&(radial_count as u16).to_le_bytes());
    payload.extend_from_slice(&(step_count as u32).to_le_bytes());
    payload.extend_from_slice(&(profiles_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(pages_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(assets_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(button_macro_refs_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(macro_descriptors_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(macro_steps_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(blob_offset as u32).to_le_bytes());
    payload.extend_from_slice(&project.screensaver_timeout_seconds.to_le_bytes());
    payload.extend_from_slice(&(radial_descriptors_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(radial_items_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(radial_item_count as u32).to_le_bytes());
    let settings = u32::from(project.brightness_percent)
        | (u32::from(project.orientation == "landscape_flipped") << 8)
        | (u32::from(project.screensaver_enabled) << 9);
    payload.extend_from_slice(&settings.to_le_bytes());

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
    let mut next_radial = 0u32;
    for profile in &project.profiles {
        for page in &profile.pages {
            for button in &page.buttons {
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
                payload.extend_from_slice(&button.radial.as_ref().map(|_| { let value = next_radial; next_radial += 1; value }).unwrap_or(u32::MAX).to_le_bytes());
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
                StepKind::KeyPress => {
                    let mask = step.modifiers.iter().fold(0u8, |mask, value| {
                        mask | match value.as_str() {
                            "CTRL" => 1,
                            "SHIFT" => 2,
                            "ALT" => 4,
                            "GUI" => 8,
                            _ => 0,
                        }
                    });
                    (
                        5,
                        mask,
                        keyboard_usage(step.key.as_deref().unwrap_or("")),
                        step.duration_ms.unwrap_or(25),
                    )
                }
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
    let mut first_radial_item = 0u16;
    for button in project.profiles.iter().flat_map(|p| &p.pages).flat_map(|p| &p.buttons) {
        if let Some(radial) = &button.radial {
            payload.extend_from_slice(&first_radial_item.to_le_bytes());
            payload.push(radial.size);
            let cover_mask = radial.items.iter().enumerate().fold(0u8, |mask, (index, item)| {
                mask | (u8::from(item.image_fit.as_deref() == Some("cover")) << index)
            });
            payload.push(cover_mask);
            first_radial_item += radial.items.len() as u16;
        }
    }
    for item in project.profiles.iter().flat_map(|p| &p.pages).flat_map(|p| &p.buttons).filter_map(|b| b.radial.as_ref()).flat_map(|r| &r.items) {
        let asset = item.icon_id.as_deref().and_then(|id| asset_indices.get(id).copied()).unwrap_or(u16::MAX);
        let action_ref = match item.action {
            ActionKind::Macro => item.macro_id.as_deref().and_then(|id| macro_indices.get(id).copied()).unwrap_or(RADIAL_ACTION_NONE),
            ActionKind::PageNext => RADIAL_ACTION_PAGE_NEXT,
            ActionKind::PagePrevious => RADIAL_ACTION_PAGE_PREVIOUS,
            ActionKind::ProfileNext => RADIAL_ACTION_PROFILE_NEXT,
            ActionKind::None => RADIAL_ACTION_NONE,
        };
        payload.extend_from_slice(&asset.to_le_bytes());
        payload.extend_from_slice(&action_ref.to_le_bytes());
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
    let (bundle_bytes, payload_crc32, fingerprint) = if issues.is_empty() {
        match compile_validated(project) {
            Ok(bundle) => (
                bundle.len(),
                u32::from_le_bytes(bundle[12..16].try_into().unwrap()),
                fingerprint(&bundle),
            ),
            Err(_) => (0, 0, String::new()),
        }
    } else {
        (0, 0, String::new())
    };
    CompileSummary {
        bundle_bytes,
        payload_crc32,
        fingerprint,
        issues,
    }
}

pub fn fingerprint(bundle: &[u8]) -> String {
    let hash = Sha256::digest(bundle);
    format!("{:x}", hash)[..12].to_owned()
}

fn u16_at(data: &[u8], offset: usize) -> Result<u16, String> {
    data.get(offset..offset + 2)
        .ok_or("truncated bundle".into())
        .map(|v| u16::from_le_bytes(v.try_into().unwrap()))
}
fn u32_at(data: &[u8], offset: usize) -> Result<u32, String> {
    data.get(offset..offset + 4)
        .ok_or("truncated bundle".into())
        .map(|v| u32::from_le_bytes(v.try_into().unwrap()))
}
fn keyboard_name(usage: u16) -> Option<String> {
    if (0x04..=0x1d).contains(&usage) {
        return Some(char::from(b'A' + (usage - 0x04) as u8).to_string());
    }
    if (0x68..=0x73).contains(&usage) {
        return Some(format!("F{}", usage - 0x68 + 13));
    }
    Some(
        match usage {
            0x28 => "ENTER",
            0x29 => "ESCAPE",
            0x2a => "BACKSPACE",
            0x2b => "TAB",
            0x2c => "SPACE",
            0x4c => "DELETE",
            0x4f => "RIGHT",
            0x50 => "LEFT",
            0x51 => "DOWN",
            0x52 => "UP",
            0xe0 => "CTRL",
            0xe1 => "SHIFT",
            0xe2 => "ALT",
            0xe3 => "GUI",
            _ => return None,
        }
        .into(),
    )
}
fn consumer_name(usage: u16) -> Option<String> {
    Some(
        match usage {
            0xcd => "PLAY_PAUSE",
            0xe2 => "MUTE",
            0xe9 => "VOLUME_UP",
            0xea => "VOLUME_DOWN",
            0xb5 => "NEXT_TRACK",
            0xb6 => "PREVIOUS_TRACK",
            _ => return None,
        }
        .into(),
    )
}

pub fn decompile(bundle: &[u8]) -> Result<Project, String> {
    if bundle.len() < 72
        || u32_at(bundle, 0)? != SDB3_MAGIC
        || u32_at(bundle, 8)? as usize != bundle.len()
        || u32_at(bundle, 12)? != crc32(&bundle[16..])
    {
        return Err("invalid SDB3 bundle".into());
    }
    let p = &bundle[16..];
    if u32_at(p, 0)? != M5UI_MAGIC
        || u16_at(p, 4)? != 3
        || u16_at(p, 6)? as usize != M5UI_HEADER_BYTES
    {
        return Err("unsupported runtime bundle version".into());
    }
    let profile_count = u16_at(p, 8)? as usize;
    let page_count = u16_at(p, 10)? as usize;
    let asset_count = u16_at(p, 12)? as usize;
    let macro_count = u16_at(p, 16)? as usize;
    let radial_count = u16_at(p, 18)? as usize;
    let step_count = u32_at(p, 20)? as usize;
    let po = u32_at(p, 24)? as usize;
    let pgo = u32_at(p, 28)? as usize;
    let ao = u32_at(p, 32)? as usize;
    let ro = u32_at(p, 36)? as usize;
    let mo = u32_at(p, 40)? as usize;
    let so = u32_at(p, 44)? as usize;
    let rdo = u32_at(p, 56)? as usize;
    let rio = u32_at(p, 60)? as usize;
    let radial_item_count = u32_at(p, 64)? as usize;
    let range = |o: usize, n: usize, s: usize| {
        o.checked_add(n.checked_mul(s).ok_or("bundle range overflow")?)
            .filter(|e| *e <= p.len())
            .ok_or("truncated bundle")
    };
    range(po, profile_count, 8)?;
    range(pgo, page_count * 32, 8)?;
    range(ao, asset_count, 20)?;
    range(ro, page_count * 32, 2)?;
    range(mo, macro_count, 8)?;
    range(so, step_count, 8)?;
    range(rdo, radial_count, RADIAL_DESCRIPTOR_BYTES)?;
    range(rio, radial_item_count, RADIAL_ITEM_BYTES)?;
    let mut assets = Vec::new();
    for i in 0..asset_count {
        let o = ao + i * 20;
        let image_o = u32_at(p, o)? as usize;
        let image_n = u32_at(p, o + 4)? as usize;
        let anim_o = u32_at(p, o + 8)? as usize;
        let anim_n = u32_at(p, o + 12)? as usize;
        let fps = *p.get(o + 19).ok_or("truncated asset")?;
        range(image_o, image_n, 1)?;
        if anim_n > 0 {
            range(anim_o, anim_n, 1)?;
        }
        let mime = if p.get(image_o..image_o + 2) == Some(&[0xff, 0xd8]) {
            "image/jpeg"
        } else {
            "image/png"
        };
        assets.push(Asset {
            id: format!("device-asset-{i}"),
            name: format!(
                "Device icon {}.{}",
                i + 1,
                if mime == "image/jpeg" { "jpg" } else { "png" }
            ),
            media_type: mime.into(),
            data_url: format!(
                "data:{mime};base64,{}",
                STANDARD.encode(&p[image_o..image_o + image_n])
            ),
            source_name: String::new(),
            source_media_type: String::new(),
            source_data_url: String::new(),
            animation_data_url: if anim_n > 0 {
                format!(
                    "data:video/x-motion-jpeg;base64,{}",
                    STANDARD.encode(&p[anim_o..anim_o + anim_n])
                )
            } else {
                String::new()
            },
            animation_fps: fps,
        });
    }
    let mut macros = Vec::new();
    for i in 0..macro_count {
        let o = mo + i * 8;
        let first = u16_at(p, o)? as usize;
        let count = u16_at(p, o + 2)? as usize;
        if first + count > step_count {
            return Err("invalid macro step range".into());
        };
        let mut steps = Vec::new();
        for si in first..first + count {
            let x = so + si * 8;
            let kind = *p.get(x).ok_or("truncated macro")?;
            let usage = u16_at(p, x + 2)?;
            let duration = u32_at(p, x + 4)?;
            let step = match kind {
                1 => MacroStep {
                    kind: StepKind::KeyDown,
                    key: keyboard_name(usage),
                    duration_ms: None,
                    modifiers: Vec::new(),
                },
                2 => MacroStep {
                    kind: StepKind::KeyUp,
                    key: keyboard_name(usage),
                    duration_ms: None,
                    modifiers: Vec::new(),
                },
                3 => MacroStep {
                    kind: StepKind::Delay,
                    key: None,
                    duration_ms: Some(duration),
                    modifiers: Vec::new(),
                },
                4 => MacroStep {
                    kind: StepKind::Consumer,
                    key: consumer_name(usage),
                    duration_ms: None,
                    modifiers: Vec::new(),
                },
                5 => MacroStep {
                    kind: StepKind::KeyPress,
                    key: keyboard_name(usage),
                    duration_ms: Some(duration),
                    modifiers: ["CTRL", "SHIFT", "ALT", "GUI"]
                        .into_iter()
                        .enumerate()
                        .filter(|(bit, _)| p[x + 1] & (1 << bit) != 0)
                        .map(|(_, name)| name.to_string())
                        .collect(),
                },
                _ => return Err("unknown macro step".into()),
            };
            if step.kind != StepKind::Delay && step.key.is_none() {
                return Err("unsupported HID usage in device macro".into());
            }
            steps.push(step);
        }
        macros.push(Macro {
            id: format!("device-macro-{i}"),
            name: format!("Device macro {}", i + 1),
            steps,
        });
    }
    let mut all_pages = Vec::new();
    for pg in 0..page_count {
        let mut buttons = Vec::new();
        for bi in 0..32 {
            let ix = pg * 32 + bi;
            let o = pgo + ix * 8;
            let asset = u16_at(p, o + 4)?;
            let action = match p[o + 6] {
                0 => ActionKind::None,
                1 => ActionKind::Macro,
                2 => ActionKind::PageNext,
                3 => ActionKind::PagePrevious,
                4 => ActionKind::ProfileNext,
                _ => return Err("unknown button action".into()),
            };
            let mr = u16_at(p, ro + ix * 2)?;
            let radial_index = u32_at(p, o)?;
            let radial = if radial_index == u32::MAX {
                None
            } else {
                let descriptor = rdo + radial_index as usize * RADIAL_DESCRIPTOR_BYTES;
                if radial_index as usize >= radial_count { return Err("invalid radial descriptor".into()); }
                let first = u16_at(p, descriptor)? as usize;
                let size = *p.get(descriptor + 2).ok_or("truncated radial descriptor")?;
                let cover_mask = *p.get(descriptor + 3).ok_or("truncated radial descriptor")?;
                if !matches!(size, 4 | 6 | 8) || first + size as usize > radial_item_count { return Err("invalid radial item range".into()); }
                let mut items = Vec::with_capacity(size as usize);
                for ri in first..first + size as usize {
                    let item_offset = rio + ri * RADIAL_ITEM_BYTES;
                    let item_asset = u16_at(p, item_offset)?;
                    let item_action_ref = u16_at(p, item_offset + 2)?;
                    let (item_action, item_macro_id) = match item_action_ref {
                        RADIAL_ACTION_NONE => (ActionKind::None, None),
                        RADIAL_ACTION_PAGE_NEXT => (ActionKind::PageNext, None),
                        RADIAL_ACTION_PAGE_PREVIOUS => (ActionKind::PagePrevious, None),
                        RADIAL_ACTION_PROFILE_NEXT => (ActionKind::ProfileNext, None),
                        index if (index as usize) < macro_count => (ActionKind::Macro, Some(format!("device-macro-{index}"))),
                        _ => return Err("invalid radial action reference".into()),
                    };
                    if item_asset != u16::MAX && item_asset as usize >= asset_count { return Err("invalid radial reference".into()); }
                    items.push(RadialItem {
                        icon_id: (item_asset != u16::MAX).then(|| format!("device-asset-{item_asset}")),
                        image_fit: Some(if cover_mask & (1 << (ri - first)) != 0 { "cover" } else { "contain" }.into()),
                        action: item_action,
                        macro_id: item_macro_id,
                    });
                }
                Some(RadialMenu { size, items })
            };
            buttons.push(Button {
                icon_id: (asset != u16::MAX).then(|| format!("device-asset-{asset}")),
                image_fit: Some(if p[o + 7] == 1 { "contain" } else { "cover" }.into()),
                macro_id: (action == ActionKind::Macro && mr != u16::MAX)
                    .then(|| format!("device-macro-{mr}")),
                action,
                radial,
            });
        }
        all_pages.push(Page {
            id: format!("device-page-{pg}"),
            name: format!("Page {}", pg + 1),
            buttons,
        });
    }
    let mut profiles = Vec::new();
    for i in 0..profile_count {
        let o = po + i * 8;
        let first = u16_at(p, o)? as usize;
        let count = u16_at(p, o + 2)? as usize;
        if first + count > all_pages.len() {
            return Err("invalid profile page range".into());
        }
        profiles.push(Profile {
            id: format!("device-profile-{i}"),
            name: format!("Profile {}", i + 1),
            pages: all_pages[first..first + count].to_vec(),
        });
    }
    let project = Project {
        schema_version: 3,
        name: "Imported from Screendeck".into(),
        screensaver_timeout_seconds: match u32_at(p, 52)? {
            0 => 15,
            value => value,
        },
        brightness_percent: (u32_at(p, 68)? & 0xff) as u8,
        orientation: if u32_at(p, 68)? & (1 << 8) != 0 { "landscape_flipped" } else { "landscape" }.into(),
        screensaver_enabled: u32_at(p, 68)? & (1 << 9) != 0,
        profiles,
        macros,
        assets,
    };
    let issues = validate(&project);
    if !issues.is_empty() {
        return Err(issues
            .into_iter()
            .map(|x| x.message)
            .collect::<Vec<_>>()
            .join("; "));
    }
    Ok(project)
}
