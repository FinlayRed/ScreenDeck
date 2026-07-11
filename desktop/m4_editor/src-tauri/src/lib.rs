mod archive;
mod compiler;
mod device;
mod model;

use model::Project;
use std::{fs, path::Path};

#[tauri::command]
fn validate_project(project: Project) -> compiler::CompileSummary { compiler::summarize(&project) }

#[tauri::command]
fn save_archive(path: String, project: Project) -> Result<(), String> { archive::save(Path::new(&path), &project).map_err(|error| error.to_string()) }

#[tauri::command]
fn open_archive(path: String) -> Result<Project, String> {
    let project = archive::open(Path::new(&path)).map_err(|error| error.to_string())?;
    let issues = model::validate(&project);
    if issues.is_empty() { Ok(project) } else { Err(issues.into_iter().map(|item| format!("{}: {}", item.path, item.message)).collect::<Vec<_>>().join("; ")) }
}

#[tauri::command]
fn backup_bundle(path: String, project: Project) -> Result<(), String> {
    let bundle = compiler::compile(&project).map_err(|error| error.to_string())?;
    fs::write(path, bundle).map_err(|error| format!("could not write backup: {error}"))
}

#[tauri::command]
fn device_status() -> device::DeviceStatus { device::status() }

#[tauri::command]
fn sync_project(project: Project) -> Result<device::SyncResult, String> {
    let summary = compiler::summarize(&project);
    let bundle = compiler::compile(&project).map_err(|error| error.to_string())?;
    device::sync(&bundle, summary.fingerprint).map_err(|error| error.to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![validate_project, save_archive, open_archive, backup_bundle, device_status, sync_project])
        .run(tauri::generate_context!())
        .expect("failed to run Screendeck editor");
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn project() -> Project {
        serde_json::from_value(json!({
            "schemaVersion": 1, "name": "Round trip", "assets": [],
            "macros": [{"id":"m1","name":"F13","steps":[{"kind":"key_down","key":"F13"},{"kind":"delay","durationMs":25},{"kind":"key_up","key":"F13"}]}],
            "profiles": [{"id":"p1","name":"Default","pages":[{"id":"g1","name":"Main","buttons":(0..32).map(|i| if i == 0 { json!({"action":"macro","macroId":"m1","accent":"#5566ff"}) } else { json!({"action":"none","accent":"#222222"}) }).collect::<Vec<_>>() }]}]
        })).unwrap()
    }

    #[test]
    fn crc_matches_physical_m3_smoke_vector() {
        let payload: Vec<u8> = (0..96).map(|value| (value * 37 + 11) as u8).collect();
        assert_eq!(compiler::crc32(&payload), 0xBDDF_EACA);
    }

    #[test]
    fn compiler_emits_valid_sdb3_header() {
        let bundle = compiler::compile(&project()).unwrap();
        assert_eq!(&bundle[0..4], &0x3342_4453u32.to_le_bytes());
        assert_eq!(u32::from_le_bytes(bundle[8..12].try_into().unwrap()) as usize, bundle.len());
        assert_eq!(u32::from_le_bytes(bundle[12..16].try_into().unwrap()), compiler::crc32(&bundle[16..]));
    }

    #[test]
    fn portable_archive_round_trips_editable_source() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("sample.sdeck");
        archive::save(&path, &project()).unwrap();
        let restored = archive::open(&path).unwrap();
        assert_eq!(restored.name, "Round trip");
        assert!(model::validate(&restored).is_empty());
    }

    #[test]
    #[ignore = "requires a physical Screendeck connected to the USB-OTG port"]
    fn physical_winusb_round_trip_and_sync() {
        let before = device::status();
        assert!(before.connected, "{}", before.detail);
        assert_eq!(before.capabilities & 0x1f, 0x1f);
        let sample = project();
        let summary = compiler::summarize(&sample);
        let bundle = compiler::compile(&sample).unwrap();
        let result = device::sync(&bundle, summary.fingerprint).unwrap();
        assert!(result.generation > 0);
        let after = device::status();
        assert!(after.connected, "{}", after.detail);
        assert_eq!(after.generation, result.generation);
    }
}
