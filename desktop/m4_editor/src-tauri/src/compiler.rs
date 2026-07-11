use crate::model::{validate, Project, ValidationIssue, MAX_BUNDLE_BYTES};
use serde::Serialize;
use sha2::{Digest, Sha256};
use thiserror::Error;

const SDB3_MAGIC: u32 = 0x3342_4453;

#[derive(Debug, Error)]
pub enum CompileError {
    #[error("project validation failed: {0}")]
    Invalid(String),
    #[error("could not serialize project: {0}")]
    Serialize(#[from] serde_json::Error),
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
    let payload = serde_json::to_vec(project)?;
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
