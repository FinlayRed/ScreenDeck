use crate::model::Project;
use base64::{engine::general_purpose::STANDARD, Engine};
use std::{fs::File, io::{Read, Write}, path::Path};
use thiserror::Error;
use zip::{write::SimpleFileOptions, ZipArchive, ZipWriter};

#[derive(Debug, Error)]
pub enum ArchiveError {
    #[error("project archive I/O failed: {0}")] Io(#[from] std::io::Error),
    #[error("project archive is invalid: {0}")] Zip(#[from] zip::result::ZipError),
    #[error("project source is invalid: {0}")] Json(#[from] serde_json::Error),
    #[error("asset '{0}' does not contain a valid data URL")] Asset(String),
}

fn safe_name(value: &str) -> String { value.chars().map(|c| if c.is_ascii_alphanumeric() || ".-_".contains(c) { c } else { '_' }).collect() }

pub fn save(path: &Path, project: &Project) -> Result<(), ArchiveError> {
    let file = File::create(path)?;
    let mut zip = ZipWriter::new(file);
    let options = SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    let mut source = project.clone();
    for asset in &mut source.assets { asset.data_url.clear(); }
    zip.start_file("project.json", options)?;
    zip.write_all(&serde_json::to_vec_pretty(&source)?)?;
    for asset in &project.assets {
        let (_, encoded) = asset.data_url.split_once(',').ok_or_else(|| ArchiveError::Asset(asset.name.clone()))?;
        let bytes = STANDARD.decode(encoded).map_err(|_| ArchiveError::Asset(asset.name.clone()))?;
        zip.start_file(format!("assets/{}/{}", safe_name(&asset.id), safe_name(&asset.name)), options)?;
        zip.write_all(&bytes)?;
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
        let path = format!("assets/{}/{}", safe_name(&asset.id), safe_name(&asset.name));
        let mut bytes = Vec::new();
        zip.by_name(&path)?.read_to_end(&mut bytes)?;
        asset.data_url = format!("data:{};base64,{}", asset.media_type, STANDARD.encode(bytes));
    }
    Ok(project)
}
