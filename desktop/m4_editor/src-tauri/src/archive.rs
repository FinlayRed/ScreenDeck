use crate::model::{Asset, Project};
use base64::{engine::general_purpose::STANDARD, Engine};
use std::{
    fs::File,
    io::{Read, Write},
    path::Path,
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

pub fn save(path: &Path, project: &Project) -> Result<(), ArchiveError> {
    let file = File::create(path)?;
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
    zip.finish()?;
    Ok(())
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
