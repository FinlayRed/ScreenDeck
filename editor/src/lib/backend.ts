// SPDX-License-Identifier: GPL-3.0-or-later

import { invoke } from "@tauri-apps/api/core";
import type { Project } from "./model";

export interface ValidationIssue { path: string; message: string; severity: "error" | "warning" }
export interface CompileSummary { bundleBytes: number; payloadCrc32: number; fingerprint: string; issues: ValidationIssue[] }
export interface DeviceStatus { connected: boolean; generation: number; capabilities: number; detail: string }
export interface SyncResult { generation: number; bytesSent: number; resumedAt: number; fingerprint: string }
export interface ScreensaverResult { bytesSent: number; resumedAt: number }
export interface IconConversion { posterDataUrl: string; animationDataUrl: string; frameCount: number }

const native = () => "__TAURI_INTERNALS__" in window;

export async function validateProject(project: Project): Promise<CompileSummary> {
  if (native()) return invoke("validate_project", { project });
  const issues: ValidationIssue[] = [];
  if (!project.name.trim()) issues.push({ path: "name", message: "Project name is required.", severity: "error" });
  if (project.screensaverTimeoutSeconds < 5 || project.screensaverTimeoutSeconds > 3600) issues.push({ path: "screensaverTimeoutSeconds", message: "Screensaver delay must be between 5 seconds and 60 minutes.", severity: "error" });
  if (!project.profiles.length) issues.push({ path: "profiles", message: "Add at least one profile.", severity: "error" });
  project.profiles.forEach((profile, pi) => profile.pages.forEach((page, pgi) => {
    if (page.buttons.length !== 32) issues.push({ path: `profiles[${pi}].pages[${pgi}]`, message: "A page must contain exactly 32 buttons.", severity: "error" });
  }));
  const encoded = new TextEncoder().encode(JSON.stringify(project));
  return { bundleBytes: encoded.length + 16, payloadCrc32: 0, fingerprint: "preview", issues };
}

export const deviceStatus = (): Promise<DeviceStatus> => native()
  ? invoke("device_status")
  : Promise.resolve({ connected: false, generation: 0, capabilities: 0, detail: "Browser preview — open in the Tauri app to connect." });

export const exitApplication = (): Promise<void> => invoke("exit_application");
export const syncProject = (project: Project): Promise<SyncResult> => invoke("sync_project", { project });
export const syncFromDevice = (): Promise<Project> => invoke("sync_from_device");
export const uploadScreensaver = (path: string): Promise<ScreensaverResult> => invoke("upload_screensaver", { path });
export const testScreensaver = (): Promise<void> => invoke("test_screensaver");
export const prepareIconAnimation = (name: string, dataUrl: string): Promise<IconConversion> => invoke("prepare_icon_animation", { name, dataUrl });
export const saveArchive = (path: string, project: Project): Promise<void> => invoke("save_archive", { path, project });
export const openArchive = (path: string): Promise<Project> => invoke("open_archive", { path });
export const backupBundle = (path: string, project: Project): Promise<void> => invoke("backup_bundle", { path, project });
export const saveWorkspace = (project: Project, preserveAssetData = false): Promise<void> => native()
  ? invoke("save_workspace", { project, preserveAssetData })
  : Promise.resolve((() => {
      if (preserveAssetData) {
        const saved = JSON.parse(localStorage.getItem("screendeck.workspace") ?? "null") as Project | null;
        const assets = new Map(saved?.assets.map((asset) => [asset.id, asset]));
        project.assets = project.assets.map((asset) => ({
          ...asset,
          dataUrl: assets.get(asset.id)?.dataUrl ?? asset.dataUrl,
          sourceDataUrl: assets.get(asset.id)?.sourceDataUrl ?? asset.sourceDataUrl,
          animationDataUrl: assets.get(asset.id)?.animationDataUrl ?? asset.animationDataUrl,
        }));
      }
      localStorage.setItem("screendeck.workspace", JSON.stringify(project));
    })());
export const loadWorkspace = (): Promise<Project | null> => native()
  ? invoke("load_workspace")
  : Promise.resolve(JSON.parse(localStorage.getItem("screendeck.workspace") ?? "null"));
export const clearWorkspace = (): Promise<void> => native()
  ? invoke("clear_workspace")
  : Promise.resolve(localStorage.removeItem("screendeck.workspace"));
