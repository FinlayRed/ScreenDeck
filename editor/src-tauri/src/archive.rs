// SPDX-License-Identifier: GPL-3.0-or-later

use crate::model::{Asset, Project};
use base64::{engine::general_purpose::STANDARD, Engine};
use std::{
    fs::File,
    io::{self, Read, Write},
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};
use thiserror::Error;
use zip::{write::SimpleFileOptions, ZipArchive, ZipWriter};

#[derive(Debug, Error)]
pub enum ArchiveError {
    #[error("project archive I/O failed: {0}")]
    Io(#[from] std::io::Error),
    #[error("project archive is invalid: {0}")]
    Zip(#[from] zip::result::ZipError),
    #[error("project source is invalid: {0}")]
    Json(#[from] serde_json::Error),
    #[error("asset '{0}' does not contain a valid data URL")]
    Asset(String),
}

fn safe_name(value: &str) -> String {
    value
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || ".-_".contains(c) {
                c
            } else {
                '_'
            }
        })
        .collect()
}

/// Atomically replaces `destination` with `temporary`. On Windows this uses
/// `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` so the destination is never
/// missing or truncated; elsewhere it falls back to rename with a remove pass.
pub(crate) fn atomic_replace(destination: &Path, temporary: &Path) -> io::Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        const MOVEFILE_REPLACE_EXISTING: u32 = 0x1;
        #[link(name = "kernel32")]
        extern "system" {
            fn MoveFileExW(existing: *const u16, replacement: *const u16, flags: u32) -> i32;
        }
        let existing: Vec<u16> = temporary.as_os_str().encode_wide().chain(Some(0)).collect();
        let replacement: Vec<u16> = destination
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect();
        unsafe {
            if MoveFileExW(
                existing.as_ptr(),
                replacement.as_ptr(),
                MOVEFILE_REPLACE_EXISTING,
            ) == 0
            {
                return Err(io::Error::last_os_error());
            }
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        std::fs::rename(temporary, destination).or_else(|_| {
            let _ = std::fs::remove_file(destination);
            std::fs::rename(temporary, destination)
        })
    }
}

fn unique_temporary(destination: &Path) -> io::Result<(std::path::PathBuf, std::path::PathBuf)> {
    let directory = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let name = destination
        .file_name()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "destination has no file name"))?
        .to_string_lossy()
        .into_owned();
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    Ok((
        directory.join(format!(".{name}.{}.{stamp}.tmp", std::process::id())),
        directory.to_path_buf(),
    ))
}

pub fn save(path: &Path, project: &Project) -> Result<(), ArchiveError> {
    // E3: build the archive in a uniquely named temporary file in the
    // destination directory, flush it, then atomically replace the target so
    // a disk, decoding, or ZIP failure never destroys the previous archive.
    let (temporary, _) = unique_temporary(path)?;
    let result = (|| -> Result<(), ArchiveError> {
        let file = File::create(&temporary)?;
        let mut zip = ZipWriter::new(file);
        let json_options =
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        let media_options =
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
        let source = Project {
            schema_version: project.schema_version,
            name: project.name.clone(),
            screensaver_timeout_seconds: project.screensaver_timeout_seconds,
            brightness_percent: project.brightness_percent,
            orientation: project.orientation.clone(),
            screensaver_enabled: project.screensaver_enabled,
            empty_button_style: project.empty_button_style.clone(),
            profiles: project.profiles.clone(),
            macros: project.macros.clone(),
            assets: project
                .assets
                .iter()
                .map(|asset| Asset {
                    id: asset.id.clone(),
                    name: asset.name.clone(),
                    media_type: asset.media_type.clone(),
                    data_url: String::new(),
                    source_name: asset.source_name.clone(),
                    source_media_type: asset.source_media_type.clone(),
                    source_data_url: String::new(),
                    animation_data_url: String::new(),
                    animation_fps: asset.animation_fps,
                })
                .collect(),
        };
        zip.start_file("project.json", json_options)?;
        zip.write_all(&serde_json::to_vec_pretty(&source)?)?;
        for asset in &project.assets {
            let (_, encoded) = asset
                .data_url
                .split_once(',')
                .ok_or_else(|| ArchiveError::Asset(asset.name.clone()))?;
            let bytes = STANDARD
                .decode(encoded)
                .map_err(|_| ArchiveError::Asset(asset.name.clone()))?;
            zip.start_file(
                format!(
                    "assets/{}/device/{}",
                    safe_name(&asset.id),
                    safe_name(&asset.name)
                ),
                media_options,
            )?;
            zip.write_all(&bytes)?;
            if !asset.source_data_url.is_empty() {
                let (_, encoded) = asset
                    .source_data_url
                    .split_once(',')
                    .ok_or_else(|| ArchiveError::Asset(asset.source_name.clone()))?;
                let original = STANDARD
                    .decode(encoded)
                    .map_err(|_| ArchiveError::Asset(asset.source_name.clone()))?;
                zip.start_file(
                    format!(
                        "assets/{}/original/{}",
                        safe_name(&asset.id),
                        safe_name(&asset.source_name)
                    ),
                    media_options,
                )?;
                zip.write_all(&original)?;
            }
            if !asset.animation_data_url.is_empty() {
                let (_, encoded) = asset
                    .animation_data_url
                    .split_once(',')
                    .ok_or_else(|| ArchiveError::Asset(asset.name.clone()))?;
                let animation = STANDARD
                    .decode(encoded)
                    .map_err(|_| ArchiveError::Asset(asset.name.clone()))?;
                zip.start_file(
                    format!("assets/{}/device/animation.mjpg", safe_name(&asset.id)),
                    media_options,
                )?;
                zip.write_all(&animation)?;
            }
        }
        let file = zip.finish()?;
        file.sync_all()?;
        atomic_replace(path, &temporary)?;
        Ok(())
    })();
    if result.is_err() {
        // The destination was never touched; remove the abandoned staging file.
        let _ = std::fs::remove_file(&temporary);
    }
    result
}

pub fn open(path: &Path) -> Result<Project, ArchiveError> {
    let file = File::open(path)?;
    let mut zip = ZipArchive::new(file)?;
    let mut source = String::new();
    zip.by_name("project.json")?.read_to_string(&mut source)?;
    let mut project: Project = serde_json::from_str(&source)?;
    for asset in &mut project.assets {
        let path = format!(
            "assets/{}/device/{}",
            safe_name(&asset.id),
            safe_name(&asset.name)
        );
        let mut bytes = Vec::new();
        zip.by_name(&path)?.read_to_end(&mut bytes)?;
        asset.data_url = format!(
            "data:{};base64,{}",
            asset.media_type,
            STANDARD.encode(bytes)
        );
        if !asset.source_name.is_empty() {
            let original_path = format!(
                "assets/{}/original/{}",
                safe_name(&asset.id),
                safe_name(&asset.source_name)
            );
            let mut original = Vec::new();
            zip.by_name(&original_path)?.read_to_end(&mut original)?;
            asset.source_data_url = format!(
                "data:{};base64,{}",
                asset.source_media_type,
                STANDARD.encode(original)
            );
        }
        if asset.animation_fps != 0 {
            let animation_path = format!("assets/{}/device/animation.mjpg", safe_name(&asset.id));
            let mut animation = Vec::new();
            zip.by_name(&animation_path)?.read_to_end(&mut animation)?;
            asset.animation_data_url = format!(
                "data:video/x-motion-jpeg;base64,{}",
                STANDARD.encode(animation)
            );
        }
    }
    Ok(project)
}
