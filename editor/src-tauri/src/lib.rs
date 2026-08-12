// SPDX-License-Identifier: GPL-3.0-or-later

mod archive;
mod compiler;
mod device;
mod model;

use base64::{engine::general_purpose::STANDARD, Engine};
use model::Project;
use serde::Serialize;
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

const SCREENSAVER_FPS: u32 = 60;
const SCREENSAVER_MAX_SECONDS: u32 = 30;
const SCREENSAVER_MAX_FRAMES: u32 = SCREENSAVER_FPS * SCREENSAVER_MAX_SECONDS;

const DEVICE_ICON_PIXELS: u16 = 149;
const DEVICE_ICON_RADIUS: u16 = 12;

#[tauri::command]
fn validate_project(project: Project) -> compiler::CompileSummary {
    compiler::summarize(&project)
}

#[tauri::command]
async fn save_archive(path: String, project: Project) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        archive::save(Path::new(&path), &project).map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| format!("save worker failed: {error}"))?
}

/// E2: reject malformed or unsafe structure, but load well-formed work in
/// progress even when it exceeds device constraints (empty titles, extra
/// profiles, oversized media). Deployability issues stay in the editor and
/// block Sync, never Open or recovery.
fn structural_gate(project: &Project) -> Result<(), String> {
    if project.profiles.is_empty()
        || project
            .profiles
            .iter()
            .any(|profile| profile.pages.is_empty())
    {
        return Err("saved project has no profiles or an empty profile".into());
    }
    Ok(())
}

#[tauri::command]
async fn open_archive(path: String) -> Result<Project, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let mut project = archive::open(Path::new(&path)).map_err(|error| error.to_string())?;
        model::migrate(&mut project);
        structural_gate(&project)?;
        Ok(project)
    })
    .await
    .map_err(|error| format!("open worker failed: {error}"))?
}

fn workspace_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    use tauri::Manager;
    let directory = app
        .path()
        .app_data_dir()
        .map_err(|error| format!("could not locate app data: {error}"))?;
    fs::create_dir_all(&directory)
        .map_err(|error| format!("could not create app data directory: {error}"))?;
    Ok(directory.join("workspace.json"))
}

#[tauri::command]
fn save_workspace(
    app: tauri::AppHandle,
    mut project: Project,
    preserve_asset_data: bool,
) -> Result<(), String> {
    let path = workspace_path(&app)?;
    if preserve_asset_data {
        let bytes =
            fs::read(&path).map_err(|error| format!("could not read workspace assets: {error}"))?;
        let saved: Project = serde_json::from_slice(&bytes)
            .map_err(|error| format!("saved workspace assets are invalid: {error}"))?;
        let saved_assets: std::collections::HashMap<_, _> = saved
            .assets
            .into_iter()
            .map(|asset| (asset.id.clone(), asset))
            .collect();
        for asset in &mut project.assets {
            let saved = saved_assets
                .get(&asset.id)
                .ok_or_else(|| format!("saved workspace is missing asset {}", asset.id))?;
            asset.data_url.clone_from(&saved.data_url);
            asset.source_data_url.clone_from(&saved.source_data_url);
            asset
                .animation_data_url
                .clone_from(&saved.animation_data_url);
        }
    }
    let bytes = serde_json::to_vec(&project)
        .map_err(|error| format!("could not serialize workspace: {error}"))?;
    let temporary = path.with_extension("tmp");
    let write_result = (|| -> Result<(), String> {
        let mut file = std::fs::File::create(&temporary)
            .map_err(|error| format!("could not save workspace: {error}"))?;
        std::io::Write::write_all(&mut file, &bytes)
            .map_err(|error| format!("could not save workspace: {error}"))?;
        file.sync_all()
            .map_err(|error| format!("could not flush saved workspace: {error}"))?;
        archive::atomic_replace(&path, &temporary)
            .map_err(|error| format!("could not activate saved workspace: {error}"))
    })();
    if write_result.is_err() {
        let _ = std::fs::remove_file(&temporary);
    }
    write_result
}

#[tauri::command]
fn load_workspace(app: tauri::AppHandle) -> Result<Option<Project>, String> {
    let path = workspace_path(&app)?;
    if !path.exists() {
        return Ok(None);
    }
    let bytes =
        fs::read(path).map_err(|error| format!("could not read saved workspace: {error}"))?;
    let mut project: Project = serde_json::from_slice(&bytes)
        .map_err(|error| format!("saved workspace is invalid: {error}"))?;
    model::migrate(&mut project);
    structural_gate(&project)?;
    Ok(Some(project))
}

#[tauri::command]
async fn clear_workspace(app: tauri::AppHandle) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        let path = workspace_path(&app)?;
        match fs::remove_file(&path) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(format!("could not clear saved workspace: {error}")),
        }
    })
    .await
    .map_err(|error| format!("clear workspace worker failed: {error}"))?
}

#[tauri::command]
async fn backup_bundle(path: String, project: Project) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        let bundle = compiler::compile(&project).map_err(|error| error.to_string())?;
        fs::write(path, bundle).map_err(|error| format!("could not write backup: {error}"))
    })
    .await
    .map_err(|error| format!("backup worker failed: {error}"))?
}

#[tauri::command]
async fn device_status() -> device::DeviceStatus {
    tauri::async_runtime::spawn_blocking(device::status)
        .await
        .unwrap_or_else(|error| device::DeviceStatus {
            connected: false,
            generation: 0,
            capabilities: 0,
            detail: format!("device status worker failed: {error}"),
        })
}

#[tauri::command]
fn exit_application() {
    std::process::exit(0);
}

#[tauri::command]
async fn test_screensaver() -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(|| {
        device::test_screensaver().map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| format!("screensaver test worker failed: {error}"))?
}

#[tauri::command]
async fn sync_project(project: Project) -> Result<device::SyncResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let bundle = compiler::compile(&project).map_err(|error| error.to_string())?;
        let fingerprint = compiler::fingerprint(&bundle);
        device::sync(&bundle, fingerprint).map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| format!("sync worker failed: {error}"))?
}

#[tauri::command]
async fn sync_from_device() -> Result<Project, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let bundle = device::download().map_err(|error| error.to_string())?;
        compiler::decompile(&bundle)
    })
    .await
    .map_err(|error| format!("device download worker failed: {error}"))?
}

#[tauri::command]
async fn upload_screensaver(path: String) -> Result<device::ScreensaverResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let media = prepare_screensaver(Path::new(&path))?;
        device::upload_screensaver(&media).map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| format!("screensaver worker failed: {error}"))?
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct IconConversion {
    poster_data_url: String,
    animation_data_url: String,
    frame_count: u16,
}

#[tauri::command]
async fn prepare_icon_animation(name: String, data_url: String) -> Result<IconConversion, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let (_, encoded) = data_url.split_once(',').ok_or("animated icon is not a data URL")?;
        let source = STANDARD.decode(encoded).map_err(|_| "animated icon base64 is invalid")?;
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos();
        let extension = Path::new(&name).extension().and_then(|value| value.to_str()).unwrap_or("bin");
        let input = std::env::temp_dir().join(format!("screendeck-icon-{stamp}.{extension}"));
        let output = std::env::temp_dir().join(format!("screendeck-icon-{stamp}.mjpg"));
        fs::write(&input, source).map_err(|error| format!("could not stage animated icon: {error}"))?;
        let result = Command::new(ffmpeg_path())
            .args(["-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-threads", "1", "-i"])
            .arg(&input)
            .args(["-map_metadata", "-1", "-fflags", "+bitexact", "-flags:v", "+bitexact", "-an", "-sn", "-dn",
                "-vf", &format!("fps=15,scale={0}:{0}:force_original_aspect_ratio=decrease,pad={0}:{0}:(ow-iw)/2:(oh-ih)/2:black,format=rgb24,geq=r='if(gte(min(X,W-1-X),{1})+gte(min(Y,H-1-Y),{1})+lte(pow({1}-min(X,W-1-X),2)+pow({1}-min(Y,H-1-Y),2),{2}),r(X,Y),32)':g='if(gte(min(X,W-1-X),{1})+gte(min(Y,H-1-Y),{1})+lte(pow({1}-min(X,W-1-X),2)+pow({1}-min(Y,H-1-Y),2),{2}),g(X,Y),33)':b='if(gte(min(X,W-1-X),{1})+gte(min(Y,H-1-Y),{1})+lte(pow({1}-min(X,W-1-X),2)+pow({1}-min(Y,H-1-Y),2),{2}),b(X,Y),38)',format=yuvj420p", DEVICE_ICON_PIXELS, DEVICE_ICON_RADIUS, DEVICE_ICON_RADIUS * DEVICE_ICON_RADIUS),
                "-frames:v", "120", "-c:v", "mjpeg", "-q:v", "5", "-pix_fmt", "yuvj420p", "-f", "mjpeg"])
            .arg(&output).output().map_err(|error| format!("could not start FFmpeg: {error}"))?;
        let _ = fs::remove_file(&input);
        if !result.status.success() {
            let _ = fs::remove_file(&output);
            return Err(format!("animated icon conversion failed: {}", String::from_utf8_lossy(&result.stderr).trim()));
        }
        let animation = fs::read(&output).map_err(|error| format!("could not read animated icon: {error}"))?;
        let _ = fs::remove_file(&output);
        let frame_count = compiler::mjpeg_frame_count(&animation).ok_or("FFmpeg produced an invalid icon MJPEG stream")?;
        let end = animation.windows(2).position(|pair| pair == [0xFF, 0xD9]).map(|index| index + 2).ok_or("animated icon has no poster frame")?;
        Ok(IconConversion {
            poster_data_url: format!("data:image/jpeg;base64,{}", STANDARD.encode(&animation[..end])),
            animation_data_url: if frame_count > 1 { format!("data:video/x-motion-jpeg;base64,{}", STANDARD.encode(&animation)) } else { String::new() },
            frame_count,
        })
    }).await.map_err(|error| format!("animated icon worker failed: {error}"))?
}

fn prepare_screensaver(path: &Path) -> Result<Vec<u8>, String> {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let output =
        std::env::temp_dir().join(format!("screendeck-{stamp}-{}.mjpeg", std::process::id()));
    let ffmpeg = ffmpeg_path();
    let video_filter = format!(
        "fps={SCREENSAVER_FPS},scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black,transpose=clock,format=yuvj420p"
    );
    let max_frames = SCREENSAVER_MAX_FRAMES.to_string();
    for quality in ["10", "20", "31"] {
        let result = Command::new(&ffmpeg)
            .args(["-hide_banner", "-loglevel", "error", "-y", "-i"])
            .arg(path)
            .args([
                "-an",
                "-vf",
                &video_filter,
                "-frames:v",
                &max_frames,
                "-c:v",
                "mjpeg",
                "-q:v",
                quality,
                "-f",
                "mjpeg",
            ])
            .arg(&output)
            .output()
            .map_err(|error| format!(
                "could not start FFmpeg ({error}). Install FFmpeg or place ffmpeg.exe beside the Screendeck app"
            ))?;

        if !result.status.success() {
            let _ = fs::remove_file(&output);
            let detail = String::from_utf8_lossy(&result.stderr);
            return Err(format!("FFmpeg conversion failed: {}", detail.trim()));
        }
        let size = fs::metadata(&output)
            .map_err(|error| format!("could not inspect converted screensaver: {error}"))?
            .len();
        if size <= 16 * 1024 * 1024 {
            let media = fs::read(&output)
                .map_err(|error| format!("could not read converted screensaver: {error}"))?;
            if largest_mjpeg_frame(&media).is_some_and(|largest| largest <= 2 * 1024 * 1024) {
                let _ = fs::remove_file(&output);
                return Ok(media);
            }
            let _ = fs::remove_file(&output);
            continue;
        }
        let _ = fs::remove_file(&output);
    }
    Err("converted screensaver exceeds the device's 16 MiB stream or 2 MiB per-frame limit even at maximum compression; choose a shorter or less detailed source".into())
}

fn largest_mjpeg_frame(media: &[u8]) -> Option<usize> {
    let mut start = None;
    let mut largest = 0usize;
    for (index, pair) in media.windows(2).enumerate() {
        if start.is_none() && pair == [0xFF, 0xD8] {
            start = Some(index);
        } else if pair == [0xFF, 0xD9] {
            let frame_start = start.take()?;
            largest = largest.max(index + 2 - frame_start);
        }
    }
    (start.is_none() && largest != 0).then_some(largest)
}

fn ffmpeg_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|parent| parent.join("ffmpeg.exe")))
        .filter(|path| path.is_file())
        .unwrap_or_else(|| PathBuf::from("ffmpeg"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            validate_project,
            save_archive,
            open_archive,
            save_workspace,
            load_workspace,
            clear_workspace,
            backup_bundle,
            device_status,
            exit_application,
            test_screensaver,
            sync_project,
            sync_from_device,
            upload_screensaver,
            prepare_icon_animation
        ])
        .run(tauri::generate_context!())
        .expect("failed to run Screendeck editor");
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn project() -> Project {
        serde_json::from_value(json!({
            "schemaVersion": 3, "name": "Round trip", "screensaverTimeoutSeconds": 30, "brightnessPercent": 80, "orientation": "landscape", "screensaverEnabled": true, "assets": [],
            "macros": [{"id":"m1","name":"F13","steps":[{"kind":"key_down","key":"F13"},{"kind":"delay","durationMs":25},{"kind":"key_up","key":"F13"}]}],
            "profiles": [{"id":"p1","name":"Default","pages":[{"id":"g1","name":"Main","buttons":(0..32).map(|i| if i == 0 { json!({"action":"macro","macroId":"m1"}) } else { json!({"action":"none"}) }).collect::<Vec<_>>() }]}]
        })).unwrap()
    }

    #[test]
    fn crc_matches_physical_m3_smoke_vector() {
        let payload: Vec<u8> = (0..96).map(|value| (value * 37 + 11) as u8).collect();
        assert_eq!(compiler::crc32(&payload), 0xBDDF_EACA);
    }

    #[test]
    #[ignore = "requires a physical Screendeck connected to the USB-OTG port"]
    fn physical_screensaver_test_command() {
        device::test_screensaver().expect("screensaver test command failed");
    }

    #[test]
    fn compiler_emits_valid_sdb3_header() {
        let bundle = compiler::compile(&project()).unwrap();
        assert_eq!(compiler::summarize(&project()).bundle_bytes, bundle.len());
        assert_eq!(&bundle[0..4], &0x3342_4453u32.to_le_bytes());
        assert_eq!(
            u32::from_le_bytes(bundle[8..12].try_into().unwrap()) as usize,
            bundle.len()
        );
        assert_eq!(
            u32::from_le_bytes(bundle[12..16].try_into().unwrap()),
            compiler::crc32(&bundle[16..])
        );
        assert_eq!(&bundle[16..20], &0x4955_354Du32.to_le_bytes());
        assert_eq!(u16::from_le_bytes(bundle[20..22].try_into().unwrap()), 3);
    }

    #[test]
    fn compiler_round_trips_device_empty_button_style() {
        for (style, encoded) in [("grey", 0u32), ("hidden", 2)] {
            let mut source = project();
            source.empty_button_style = style.into();
            let bundle = compiler::compile(&source).unwrap();
            let settings = u32::from_le_bytes(bundle[84..88].try_into().unwrap());
            assert_eq!((settings >> 10) & 0x3, encoded);
            assert_eq!(
                compiler::decompile(&bundle).unwrap().empty_button_style,
                style
            );
        }
    }

    fn animated_asset_with(frames: u16) -> model::Asset {
        // A frame is a marker-only JPEG: the compiler and firmware both count
        // SOI/EOI pairs, and compile-level validation is structural.
        let mut stream = Vec::new();
        for _ in 0..frames {
            stream.extend_from_slice(&[0xff, 0xd8, 0xff, 0xd9]);
        }
        model::Asset {
            id: "anim".into(),
            name: "anim.mjpg".into(),
            media_type: "image/png".into(),
            data_url: "data:image/png;base64,REVWSUNF".into(),
            source_name: String::new(),
            source_media_type: String::new(),
            source_data_url: String::new(),
            animation_data_url: format!(
                "data:video/x-motion-jpeg;base64,{}",
                base64::engine::general_purpose::STANDARD.encode(stream)
            ),
            animation_fps: 15,
        }
    }

    #[test]
    fn animated_icon_frame_count_matches_device_contract() {
        // I4: the device accepts only 2..=120 complete frames. Compilation and
        // decompilation must agree so a crafted bundle cannot sync and then
        // prevent the UI bundle from loading.
        let mut one_frame = project();
        one_frame.assets.push(animated_asset_with(1));
        assert!(compiler::compile(&one_frame).is_err());

        let mut too_many = project();
        too_many.assets.push(animated_asset_with(121));
        assert!(compiler::compile(&too_many).is_err());

        let mut valid = project();
        valid.assets.push(animated_asset_with(2));
        valid.profiles[0].pages[0].buttons[0].icon_id = Some("anim".into());
        let bundle = compiler::compile(&valid).unwrap();
        let restored = compiler::decompile(&bundle).unwrap();
        assert_eq!(restored.assets[0].animation_fps, 15);
    }

    #[test]
    fn decompile_rejects_animation_frame_count_mismatch() {
        let mut valid = project();
        valid.assets.push(animated_asset_with(2));
        valid.profiles[0].pages[0].buttons[0].icon_id = Some("anim".into());
        let mut bundle = compiler::compile(&valid).unwrap();
        // Asset table starts after the SDB header; the frame_count field is at
        // asset offset 16 within the 20-byte m5_ui_asset_t.
        let assets_offset = u32::from_le_bytes(bundle[32..36].try_into().unwrap()) as usize;
        bundle[16 + assets_offset + 16..16 + assets_offset + 18]
            .copy_from_slice(&3u16.to_le_bytes());
        let payload_crc = compiler::crc32(&bundle[16..]);
        bundle[12..16].copy_from_slice(&payload_crc.to_le_bytes());
        assert!(compiler::decompile(&bundle).is_err());
    }

    #[test]
    fn summarize_reports_oversized_bundle_as_blocking() {
        // E9: validation and compilation share the 16 MiB limit, so Sync is
        // blocked before a too-large project ever reaches the device.
        let mut source = project();
        // 23 MiB of base64 'A' decodes to ~17.25 MiB, above the 16 MiB limit.
        let blob = "A".repeat(23 * 1024 * 1024);
        source.assets.push(model::Asset {
            id: "huge".into(),
            name: "huge.png".into(),
            media_type: "image/png".into(),
            data_url: format!("data:image/png;base64,{blob}"),
            source_name: String::new(),
            source_media_type: String::new(),
            source_data_url: String::new(),
            animation_data_url: String::new(),
            animation_fps: 0,
        });
        let summary = compiler::summarize(&source);
        assert!(summary
            .issues
            .iter()
            .any(|issue| issue.message.contains("16 MiB device limit")));
        assert!(compiler::compile(&source).is_err());
    }

    #[test]
    fn archive_open_rejects_oversized_entries() {
        // E7: a crafted archive whose project.json expands beyond the cap must
        // be rejected with a clear limit error, before any unbounded read.
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("bomb.sdeck");
        {
            let file = std::fs::File::create(&path).unwrap();
            let mut zip = zip::ZipWriter::new(file);
            let options = zip::write::SimpleFileOptions::default()
                .compression_method(zip::CompressionMethod::Stored);
            zip.start_file("project.json", options).unwrap();
            std::io::Write::write_all(&mut zip, &vec![0u8; 9 * 1024 * 1024]).unwrap();
            zip.finish().unwrap();
        }
        let error = archive::open(&path).unwrap_err();
        assert!(matches!(error, archive::ArchiveError::Limit(_)));
    }

    #[test]
    fn structural_gate_accepts_work_in_progress_but_rejects_unsafe_structure() {
        // E2: empty titles and extra profiles are work in progress and must
        // load; zero profiles would crash the editor UI and must be rejected.
        let mut wip = project();
        wip.name = "".into();
        for index in 0..8 {
            wip.profiles.push(model::Profile {
                id: format!("wip-{index}"),
                name: format!("Work {index}"),
                pages: vec![model::Page {
                    id: format!("wip-page-{index}"),
                    name: "Main".into(),
                    buttons: vec![
                        model::Button {
                            action: model::ActionKind::None,
                            icon_id: None,
                            image_fit: None,
                            macro_id: None,
                            radial: None,
                        };
                        32
                    ],
                }],
            });
        }
        assert!(
            structural_gate(&wip).is_ok(),
            "9-profile work in progress must load"
        );
        wip.profiles.clear();
        assert!(
            structural_gate(&wip).is_err(),
            "zero profiles are unsafe structure"
        );
    }

    #[test]
    fn minimal_bundle_layout_matches_smoke_test_fixture() {
        // The device-sync.ps1 -CommitTestBundle fixture (MinimalUiPayload)
        // hardcodes these offsets, and the firmware validator
        // (m5_ui_bundle_valid) accepts the bundle only while they hold. Pin
        // the fixture against the compiler's reference emit.
        let mut minimal = project();
        minimal.macros.clear();
        for button in &mut minimal.profiles[0].pages[0].buttons {
            button.action = model::ActionKind::None;
            button.macro_id = None;
        }
        let bundle = compiler::compile(&minimal).unwrap();
        let payload = &bundle[16..];
        assert_eq!(u16::from_le_bytes(payload[6..8].try_into().unwrap()), 72);
        assert_eq!(u16::from_le_bytes(payload[14..16].try_into().unwrap()), 32);
        assert_eq!(u32::from_le_bytes(payload[24..28].try_into().unwrap()), 72);
        assert_eq!(u32::from_le_bytes(payload[28..32].try_into().unwrap()), 80);
        assert_eq!(u32::from_le_bytes(payload[32..36].try_into().unwrap()), 336);
        assert_eq!(u32::from_le_bytes(payload[36..40].try_into().unwrap()), 336);
        assert_eq!(u32::from_le_bytes(payload[40..44].try_into().unwrap()), 400);
        assert_eq!(u32::from_le_bytes(payload[44..48].try_into().unwrap()), 400);
        assert_eq!(u32::from_le_bytes(payload[48..52].try_into().unwrap()), 400);
        assert_eq!(u32::from_le_bytes(payload[56..60].try_into().unwrap()), 400);
        assert_eq!(u32::from_le_bytes(payload[60..64].try_into().unwrap()), 400);
        assert_eq!(u32::from_le_bytes(payload[64..68].try_into().unwrap()), 0);
    }

    #[test]
    fn legacy_black_empty_buttons_import_as_grey() {
        let source = project();
        let mut bundle = compiler::compile(&source).unwrap();
        let settings = u32::from_le_bytes(bundle[84..88].try_into().unwrap()) | (1 << 10);
        bundle[84..88].copy_from_slice(&settings.to_le_bytes());
        let payload_crc = compiler::crc32(&bundle[16..]);
        bundle[12..16].copy_from_slice(&payload_crc.to_le_bytes());
        assert_eq!(
            compiler::decompile(&bundle).unwrap().empty_button_style,
            "grey"
        );
    }

    #[test]
    fn compiler_serializes_button_macro_references_and_steps() {
        let bundle = compiler::compile(&project()).unwrap();
        let payload = &bundle[16..];
        assert_eq!(u16::from_le_bytes(payload[16..18].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(payload[20..24].try_into().unwrap()), 3);
        let refs = u32::from_le_bytes(payload[36..40].try_into().unwrap()) as usize;
        let macros = u32::from_le_bytes(payload[40..44].try_into().unwrap()) as usize;
        let steps = u32::from_le_bytes(payload[44..48].try_into().unwrap()) as usize;
        assert_eq!(
            u16::from_le_bytes(payload[refs..refs + 2].try_into().unwrap()),
            0
        );
        assert_eq!(
            u16::from_le_bytes(payload[refs + 2..refs + 4].try_into().unwrap()),
            u16::MAX
        );
        assert_eq!(
            u16::from_le_bytes(payload[macros..macros + 2].try_into().unwrap()),
            0
        );
        assert_eq!(
            u16::from_le_bytes(payload[macros + 2..macros + 4].try_into().unwrap()),
            3
        );
        assert_eq!(&payload[steps..steps + 3], &[1, 0x07, 0x68]);
        assert_eq!(payload[steps + 8], 3);
        assert_eq!(
            u32::from_le_bytes(payload[steps + 12..steps + 16].try_into().unwrap()),
            25
        );
        assert_eq!(payload[steps + 16], 2);
    }

    #[test]
    fn device_bundle_decompiles_to_an_editable_project() {
        let source = project();
        let restored = compiler::decompile(&compiler::compile(&source).unwrap()).unwrap();
        assert_eq!(restored.profiles.len(), 1);
        assert_eq!(restored.screensaver_timeout_seconds, 30);
        assert_eq!(restored.profiles[0].pages[0].buttons.len(), 32);
        assert_eq!(restored.macros[0].steps.len(), 3);
        assert_eq!(
            restored.profiles[0].pages[0].buttons[0].macro_id.as_deref(),
            Some("device-macro-0")
        );
        assert!(model::validate(&restored).is_empty());
    }

    #[test]
    fn radial_menu_and_settings_round_trip() {
        let mut source = project();
        source.brightness_percent = 0;
        source.orientation = "landscape_flipped".into();
        source.screensaver_enabled = false;
        source.profiles[0].pages[0].buttons[0].radial = Some(model::RadialMenu {
            size: 4,
            items: vec![
                model::RadialItem {
                    icon_id: None,
                    image_fit: Some("cover".into()),
                    action: model::ActionKind::Macro,
                    macro_id: Some("m1".into()),
                },
                model::RadialItem {
                    icon_id: None,
                    image_fit: Some("contain".into()),
                    action: model::ActionKind::PageNext,
                    macro_id: None,
                },
                model::RadialItem {
                    icon_id: None,
                    image_fit: None,
                    action: model::ActionKind::PagePrevious,
                    macro_id: None,
                },
                model::RadialItem {
                    icon_id: None,
                    image_fit: Some("cover".into()),
                    action: model::ActionKind::ProfileNext,
                    macro_id: None,
                },
            ],
        });
        let restored = compiler::decompile(&compiler::compile(&source).unwrap()).unwrap();
        assert_eq!(restored.brightness_percent, 0);
        assert_eq!(restored.orientation, "landscape_flipped");
        assert!(!restored.screensaver_enabled);
        let items = &restored.profiles[0].pages[0].buttons[0]
            .radial
            .as_ref()
            .unwrap()
            .items;
        assert_eq!(items.len(), 4);
        assert_eq!(items[0].action, model::ActionKind::Macro);
        assert_eq!(items[1].action, model::ActionKind::PageNext);
        assert_eq!(items[2].action, model::ActionKind::PagePrevious);
        assert_eq!(items[3].action, model::ActionKind::ProfileNext);
        assert_eq!(items[0].image_fit.as_deref(), Some("cover"));
        assert_eq!(items[1].image_fit.as_deref(), Some("contain"));
        assert_eq!(items[2].image_fit.as_deref(), Some("contain"));
    }

    #[test]
    fn key_press_round_trips_with_modifiers_and_short_duration() {
        let mut source = project();
        source.macros[0].steps = vec![model::MacroStep {
            kind: model::StepKind::KeyPress,
            key: Some("K".into()),
            duration_ms: Some(25),
            modifiers: vec!["CTRL".into(), "SHIFT".into(), "GUI".into()],
        }];
        let restored = compiler::decompile(&compiler::compile(&source).unwrap()).unwrap();
        let step = &restored.macros[0].steps[0];
        assert_eq!(step.kind, model::StepKind::KeyPress);
        assert_eq!(step.key.as_deref(), Some("K"));
        assert_eq!(step.duration_ms, Some(25));
        assert_eq!(step.modifiers, ["CTRL", "SHIFT", "GUI"]);
    }

    #[test]
    fn blank_macro_round_trips_as_an_editable_sequence() {
        let mut source = project();
        source.macros[0].steps.clear();
        assert!(model::validate(&source).is_empty());
        let restored = compiler::decompile(&compiler::compile(&source).unwrap()).unwrap();
        assert!(restored.macros[0].steps.is_empty());
        assert!(model::validate(&restored).is_empty());
    }

    #[test]
    fn validation_rejects_unbalanced_key_ownership() {
        let mut source = project();
        source.macros[0].steps = vec![
            model::MacroStep {
                kind: model::StepKind::KeyUp,
                key: Some("F14".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
            model::MacroStep {
                kind: model::StepKind::KeyDown,
                key: Some("F13".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
            model::MacroStep {
                kind: model::StepKind::KeyDown,
                key: Some("F13".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
        ];
        let issues = model::validate(&source);
        assert!(issues
            .iter()
            .any(|item| item.message.contains("F14 is not currently held")));
        assert!(issues
            .iter()
            .any(|item| item.message.contains("F13 is already held")));
        assert!(issues
            .iter()
            .any(|item| item.message.contains("F13 must be released")));
    }

    #[test]
    fn validation_accepts_balanced_overlapping_keys() {
        let mut source = project();
        source.macros[0].steps = vec![
            model::MacroStep {
                kind: model::StepKind::KeyDown,
                key: Some("F13".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
            model::MacroStep {
                kind: model::StepKind::KeyDown,
                key: Some("F14".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
            model::MacroStep {
                kind: model::StepKind::KeyUp,
                key: Some("F13".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
            model::MacroStep {
                kind: model::StepKind::KeyUp,
                key: Some("F14".into()),
                duration_ms: None,
                modifiers: Vec::new(),
            },
        ];
        assert!(model::validate(&source).is_empty());
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
    fn failed_archive_save_preserves_the_previous_archive() {
        // E3: a failure at any stage (here: invalid asset base64 during
        // decoding) must leave the previous archive byte-for-byte intact and
        // must not leave a staging file behind.
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("media.sdeck");
        let mut good = project();
        good.assets.push(model::Asset {
            id: "asset-1".into(),
            name: "icon.png".into(),
            media_type: "image/png".into(),
            data_url: "data:image/png;base64,REVWSUNF".into(),
            source_name: String::new(),
            source_media_type: String::new(),
            source_data_url: String::new(),
            animation_data_url: String::new(),
            animation_fps: 0,
        });
        archive::save(&path, &good).unwrap();
        let before = std::fs::read(&path).unwrap();

        let mut broken = good.clone();
        broken.assets[0].data_url = "data:image/png;base64,!!!not-base64!!!".into();
        assert!(archive::save(&path, &broken).is_err());

        assert_eq!(
            std::fs::read(&path).unwrap(),
            before,
            "previous archive changed"
        );
        let leftovers: Vec<_> = std::fs::read_dir(directory.path())
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .filter(|name| name != "media.sdeck")
            .collect();
        assert!(
            leftovers.is_empty(),
            "staging files left behind: {leftovers:?}"
        );
    }

    #[test]
    fn archive_preserves_original_and_device_derivative_separately() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("media.sdeck");
        let mut sample = project();
        sample.assets.push(model::Asset {
            id: "asset-1".into(),
            name: "icon.png".into(),
            media_type: "image/png".into(),
            data_url: "data:image/png;base64,REVWSUNF".into(),
            source_name: "photo.webp".into(),
            source_media_type: "image/webp".into(),
            source_data_url: "data:image/webp;base64,T1JJR0lOQUw=".into(),
            animation_data_url: String::new(),
            animation_fps: 0,
        });
        archive::save(&path, &sample).unwrap();
        let restored = archive::open(&path).unwrap();
        assert_eq!(restored.assets[0].data_url, sample.assets[0].data_url);
        assert_eq!(
            restored.assets[0].source_data_url,
            sample.assets[0].source_data_url
        );
        assert_ne!(
            restored.assets[0].data_url,
            restored.assets[0].source_data_url
        );
    }

    #[test]
    #[ignore = "requires FFmpeg"]
    fn ffmpeg_converts_image_to_device_mjpeg() {
        let media = prepare_screensaver(Path::new("icons/32x32.png")).unwrap();
        assert!(media.len() <= 16 * 1024 * 1024);
        assert_eq!(&media[..2], &[0xff, 0xd8]);
        assert_eq!(&media[media.len() - 2..], &[0xff, 0xd9]);
    }

    #[test]
    #[ignore = "requires FFmpeg"]
    fn ffmpeg_converts_video_to_60fps_device_mjpeg() {
        let media = prepare_screensaver(Path::new("../../../.m5_sample.mp4")).unwrap();
        let frames = media
            .windows(2)
            .filter(|pair| *pair == [0xff, 0xd8])
            .count();
        assert_eq!(
            frames, 12,
            "the 0.2 second sample must contain 12 frames at 60 FPS"
        );
    }

    #[test]
    #[ignore = "requires FFmpeg and a physical Screendeck connected to the USB-OTG port"]
    fn physical_60fps_screensaver_conversion_and_upload() {
        let media = prepare_screensaver(Path::new("../../../.m5_sample.mp4")).unwrap();
        let frames = media
            .windows(2)
            .filter(|pair| *pair == [0xff, 0xd8])
            .count();
        assert_eq!(
            frames, 12,
            "the 0.2 second sample must contain 12 frames at 60 FPS"
        );
        let result = device::upload_screensaver(&media).unwrap();
        assert_eq!(result.bytes_sent + result.resumed_at as usize, media.len());
    }

    #[test]
    #[ignore = "requires a physical Screendeck connected to the USB-OTG port"]
    fn physical_winusb_round_trip_and_sync() {
        let before = device::status();
        assert!(before.connected, "{}", before.detail);
        assert_eq!(before.capabilities & 0x1f, 0x1f);
        assert_ne!(
            before.capabilities & 0x200,
            0,
            "bundle batching not advertised"
        );
        let mut sample = project();
        let icon = std::fs::read("icons/128x128.png").unwrap();
        sample.assets.push(model::Asset {
            id: "physical-icon".into(),
            name: "Physical icon.png".into(),
            media_type: "image/png".into(),
            data_url: format!(
                "data:image/png;base64,{}",
                base64::Engine::encode(&base64::engine::general_purpose::STANDARD, icon)
            ),
            source_name: String::new(),
            source_media_type: String::new(),
            source_data_url: String::new(),
            animation_data_url: String::new(),
            animation_fps: 0,
        });
        sample.profiles[0].pages[0].buttons[0].icon_id = Some("physical-icon".into());
        let bundle = compiler::compile(&sample).unwrap();
        let result = device::sync(&bundle, compiler::fingerprint(&bundle)).unwrap();
        assert!(result.generation > 0);
        std::thread::sleep(std::time::Duration::from_secs(1));
        let mut after = device::status();
        for _ in 0..30 {
            if after.connected {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(500));
            after = device::status();
        }
        assert!(after.connected, "{}", after.detail);
        assert_eq!(after.generation, result.generation);
    }
}
