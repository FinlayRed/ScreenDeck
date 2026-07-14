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

const DEVICE_ICON_PIXELS: u16 = 149;
const DEVICE_ICON_RADIUS: u16 = 12;

#[tauri::command]
fn validate_project(project: Project) -> compiler::CompileSummary {
    compiler::summarize(&project)
}

#[tauri::command]
fn save_archive(path: String, project: Project) -> Result<(), String> {
    archive::save(Path::new(&path), &project).map_err(|error| error.to_string())
}

#[tauri::command]
fn open_archive(path: String) -> Result<Project, String> {
    let mut project = archive::open(Path::new(&path)).map_err(|error| error.to_string())?;
    model::migrate(&mut project);
    let issues = model::validate(&project);
    if issues.is_empty() {
        Ok(project)
    } else {
        Err(issues
            .into_iter()
            .map(|item| format!("{}: {}", item.path, item.message))
            .collect::<Vec<_>>()
            .join("; "))
    }
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
fn save_workspace(app: tauri::AppHandle, project: Project) -> Result<(), String> {
    let bytes = serde_json::to_vec(&project)
        .map_err(|error| format!("could not serialize workspace: {error}"))?;
    let path = workspace_path(&app)?;
    let temporary = path.with_extension("tmp");
    fs::write(&temporary, bytes).map_err(|error| format!("could not save workspace: {error}"))?;
    fs::rename(&temporary, &path)
        .or_else(|_| {
            let _ = fs::remove_file(&path);
            fs::rename(&temporary, &path)
        })
        .map_err(|error| format!("could not activate saved workspace: {error}"))
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
    if model::validate(&project).is_empty() {
        Ok(Some(project))
    } else {
        Err("saved workspace failed validation".into())
    }
}

#[tauri::command]
fn backup_bundle(path: String, project: Project) -> Result<(), String> {
    let bundle = compiler::compile(&project).map_err(|error| error.to_string())?;
    fs::write(path, bundle).map_err(|error| format!("could not write backup: {error}"))
}

#[tauri::command]
fn device_status() -> device::DeviceStatus {
    device::status()
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
    for quality in ["10", "20", "31"] {
        let result = Command::new(&ffmpeg)
            .args(["-hide_banner", "-loglevel", "error", "-y", "-i"])
            .arg(path)
            .args([
                "-an",
                "-vf",
                "fps=30,scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black,transpose=clock,format=yuvj420p",
                "-frames:v",
                "900",
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
            backup_bundle,
            device_status,
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
                model::RadialItem { icon_id: None, image_fit: Some("cover".into()), action: model::ActionKind::Macro, macro_id: Some("m1".into()) },
                model::RadialItem { icon_id: None, image_fit: Some("contain".into()), action: model::ActionKind::PageNext, macro_id: None },
                model::RadialItem { icon_id: None, image_fit: None, action: model::ActionKind::PagePrevious, macro_id: None },
                model::RadialItem { icon_id: None, image_fit: Some("cover".into()), action: model::ActionKind::ProfileNext, macro_id: None },
            ],
        });
        let restored = compiler::decompile(&compiler::compile(&source).unwrap()).unwrap();
        assert_eq!(restored.brightness_percent, 0);
        assert_eq!(restored.orientation, "landscape_flipped");
        assert!(!restored.screensaver_enabled);
        let items = &restored.profiles[0].pages[0].buttons[0].radial.as_ref().unwrap().items;
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
    fn portable_archive_round_trips_editable_source() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("sample.sdeck");
        archive::save(&path, &project()).unwrap();
        let restored = archive::open(&path).unwrap();
        assert_eq!(restored.name, "Round trip");
        assert!(model::validate(&restored).is_empty());
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
    #[ignore = "requires a physical Screendeck connected to the USB-OTG port"]
    fn physical_winusb_round_trip_and_sync() {
        let before = device::status();
        assert!(before.connected, "{}", before.detail);
        assert_eq!(before.capabilities & 0x1f, 0x1f);
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
        let summary = compiler::summarize(&sample);
        let bundle = compiler::compile(&sample).unwrap();
        let result = device::sync(&bundle, summary.fingerprint).unwrap();
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
