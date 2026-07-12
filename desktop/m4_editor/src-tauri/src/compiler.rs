use crate::model::{validate, ActionKind, Project, ValidationIssue, MAX_BUNDLE_BYTES};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use thiserror::Error;

const SDB3_MAGIC: u32 = 0x3342_4453;
const M5UI_MAGIC: u32 = 0x4955_354D;
const M5UI_HEADER_BYTES: usize = 32;
const M5UI_PROFILE_BYTES: usize = 8;
const M5UI_BUTTON_BYTES: usize = 8;
const M5UI_ASSET_BYTES: usize = 12;

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
pub struct CompileSummary { pub bundle_bytes: usize, pub payload_crc32: u32, pub fingerprint: String, pub issues: Vec<ValidationIssue> }

pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0u32;
    for &value in data {
        crc ^= u32::from(value);
        for _ in 0..8 { crc = (crc >> 1) ^ if crc & 1 != 0 { 0xEDB8_8320 } else { 0 }; }
    }
    !crc
}

pub fn compile(project: &Project) -> Result<Vec<u8>, CompileError> {
    let issues = validate(project);
    if !issues.is_empty() { return Err(CompileError::Invalid(issues.iter().map(|item| format!("{}: {}", item.path, item.message)).collect::<Vec<_>>().join("; "))); }
    let page_count: usize = project.profiles.iter().map(|profile| profile.pages.len()).sum();
    if project.assets.len() > u16::MAX as usize || page_count > u16::MAX as usize {
        return Err(CompileError::Invalid("too many pages or icon assets for the device bundle".into()));
    }
    let profiles_offset = M5UI_HEADER_BYTES;
    let pages_offset = profiles_offset + project.profiles.len() * M5UI_PROFILE_BYTES;
    let assets_offset = pages_offset + page_count * 32 * M5UI_BUTTON_BYTES;
    let blob_offset = assets_offset + project.assets.len() * M5UI_ASSET_BYTES;
    let mut payload = Vec::with_capacity(blob_offset);
    payload.extend_from_slice(&M5UI_MAGIC.to_le_bytes());
    payload.extend_from_slice(&1u16.to_le_bytes());
    payload.extend_from_slice(&(M5UI_HEADER_BYTES as u16).to_le_bytes());
    payload.extend_from_slice(&(project.profiles.len() as u16).to_le_bytes());
    payload.extend_from_slice(&(page_count as u16).to_le_bytes());
    payload.extend_from_slice(&(project.assets.len() as u16).to_le_bytes());
    payload.extend_from_slice(&32u16.to_le_bytes());
    payload.extend_from_slice(&(profiles_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(pages_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(assets_offset as u32).to_le_bytes());
    payload.extend_from_slice(&(blob_offset as u32).to_le_bytes());

    let mut first_page = 0u16;
    for profile in &project.profiles {
        payload.extend_from_slice(&first_page.to_le_bytes());
        payload.extend_from_slice(&(profile.pages.len() as u16).to_le_bytes());
        payload.extend_from_slice(&0u32.to_le_bytes());
        first_page += profile.pages.len() as u16;
    }
    let asset_indices: HashMap<&str, u16> = project.assets.iter().enumerate()
        .map(|(index, asset)| (asset.id.as_str(), index as u16)).collect();
    for profile in &project.profiles {
        for page in &profile.pages {
            for button in &page.buttons {
                let accent = u32::from_str_radix(button.accent.trim_start_matches('#'), 16).unwrap_or(0x2A2C33);
                let asset = button.icon_id.as_deref().and_then(|id| asset_indices.get(id).copied()).unwrap_or(u16::MAX);
                let action = match button.action {
                    ActionKind::None => 0, ActionKind::Macro => 1, ActionKind::PageNext => 2,
                    ActionKind::PagePrevious => 3, ActionKind::ProfileNext => 4,
                };
                payload.extend_from_slice(&accent.to_le_bytes());
                payload.extend_from_slice(&asset.to_le_bytes());
                payload.push(action);
                payload.push(u8::from(button.image_fit.as_deref() == Some("contain")));
            }
        }
    }
    let mut asset_blobs = Vec::with_capacity(project.assets.len());
    let mut next_blob = blob_offset;
    for asset in &project.assets {
        let (_, encoded) = asset.data_url.split_once(',').ok_or_else(|| CompileError::Asset(asset.name.clone()))?;
        let bytes = STANDARD.decode(encoded).map_err(|_| CompileError::Asset(asset.name.clone()))?;
        payload.extend_from_slice(&(next_blob as u32).to_le_bytes());
        payload.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        payload.push(1); payload.extend_from_slice(&[0, 0, 0]);
        next_blob += bytes.len();
        asset_blobs.push(bytes);
    }
    for bytes in asset_blobs { payload.extend_from_slice(&bytes); }
    let total = payload.len() + 16;
    if total > MAX_BUNDLE_BYTES { return Err(CompileError::TooLarge(total)); }
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
            (bundle.len(), u32::from_le_bytes(bundle[12..16].try_into().unwrap()), format!("{:x}", hash)[..12].to_owned())
        }
        Err(_) => (0, 0, String::new()),
    };
    CompileSummary { bundle_bytes, payload_crc32, fingerprint, issues }
}
