use serde::{Deserialize, Serialize};
use std::collections::HashSet;

pub const MAX_PROFILES: usize = 8;
pub const MAX_PAGES_PER_PROFILE: usize = 16;
pub const BUTTONS_PER_PAGE: usize = 32;
pub const MAX_MACROS: usize = 128;
pub const MAX_STEPS: usize = 64;
pub const MAX_BUNDLE_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Project {
    pub schema_version: u16,
    pub name: String,
    pub profiles: Vec<Profile>,
    pub macros: Vec<Macro>,
    pub assets: Vec<Asset>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile { pub id: String, pub name: String, pub pages: Vec<Page> }

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Page { pub id: String, pub name: String, pub buttons: Vec<Button> }

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Button {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub image_fit: Option<String>,
    pub action: ActionKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub macro_id: Option<String>,
    pub accent: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum ActionKind { Macro, PageNext, PagePrevious, ProfileNext, None }

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Macro { pub id: String, pub name: String, pub steps: Vec<MacroStep> }

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MacroStep {
    pub kind: StepKind,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub key: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub duration_ms: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum StepKind { KeyDown, KeyUp, Delay, Consumer }

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Asset { pub id: String, pub name: String, pub media_type: String, #[serde(default)] pub data_url: String }

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ValidationIssue { pub path: String, pub message: String, pub severity: &'static str }

fn issue(path: impl Into<String>, message: impl Into<String>) -> ValidationIssue {
    ValidationIssue { path: path.into(), message: message.into(), severity: "error" }
}

pub fn validate(project: &Project) -> Vec<ValidationIssue> {
    let mut issues = Vec::new();
    if project.schema_version != 1 { issues.push(issue("schemaVersion", "Only project schema version 1 is supported.")); }
    if project.name.trim().is_empty() || project.name.len() > 80 { issues.push(issue("name", "Project name must contain 1–80 characters.")); }
    if project.profiles.is_empty() || project.profiles.len() > MAX_PROFILES { issues.push(issue("profiles", format!("A project needs 1–{MAX_PROFILES} profiles."))); }
    if project.macros.len() > MAX_MACROS { issues.push(issue("macros", format!("At most {MAX_MACROS} macros are supported."))); }

    let macro_ids: HashSet<_> = project.macros.iter().map(|item| item.id.as_str()).collect();
    let asset_ids: HashSet<_> = project.assets.iter().map(|item| item.id.as_str()).collect();
    if macro_ids.len() != project.macros.len() { issues.push(issue("macros", "Macro IDs must be unique.")); }
    if asset_ids.len() != project.assets.len() { issues.push(issue("assets", "Asset IDs must be unique.")); }

    for (pi, profile) in project.profiles.iter().enumerate() {
        if profile.pages.is_empty() || profile.pages.len() > MAX_PAGES_PER_PROFILE {
            issues.push(issue(format!("profiles[{pi}].pages"), format!("A profile needs 1–{MAX_PAGES_PER_PROFILE} pages.")));
        }
        for (pgi, page) in profile.pages.iter().enumerate() {
            if page.buttons.len() != BUTTONS_PER_PAGE { issues.push(issue(format!("profiles[{pi}].pages[{pgi}].buttons"), "A page must contain exactly 32 buttons.")); }
            for (bi, button) in page.buttons.iter().enumerate() {
                let path = format!("profiles[{pi}].pages[{pgi}].buttons[{bi}]");
                if button.action == ActionKind::Macro && button.macro_id.as_deref().is_none_or(|id| !macro_ids.contains(id)) { issues.push(issue(path.clone(), "The button references a missing macro.")); }
                if button.icon_id.as_deref().is_some_and(|id| !asset_ids.contains(id)) { issues.push(issue(path, "The button references a missing icon.")); }
                if button.image_fit.as_deref().is_some_and(|fit| fit != "cover" && fit != "contain") { issues.push(issue(format!("profiles[{pi}].pages[{pgi}].buttons[{bi}].imageFit"), "Artwork display must be cover or contain.")); }
            }
        }
    }
    for (mi, item) in project.macros.iter().enumerate() {
        if item.steps.is_empty() || item.steps.len() > MAX_STEPS { issues.push(issue(format!("macros[{mi}].steps"), format!("A macro needs 1–{MAX_STEPS} steps."))); }
        for (si, step) in item.steps.iter().enumerate() {
            let path = format!("macros[{mi}].steps[{si}]");
            if step.kind == StepKind::Delay && !(1..=60_000).contains(&step.duration_ms.unwrap_or(0)) { issues.push(issue(path.clone(), "Delay must be between 1 and 60,000 ms.")); }
            if step.kind != StepKind::Delay && step.key.as_deref().is_none_or(str::is_empty) { issues.push(issue(path, "Choose a HID key.")); }
        }
    }
    issues
}
