<script lang="ts">
  import { open, save } from "@tauri-apps/plugin-dialog";
  import { tick } from "svelte";
  import Archive from "@lucide/svelte/icons/archive";
  import ChevronLeft from "@lucide/svelte/icons/chevron-left";
  import ChevronRight from "@lucide/svelte/icons/chevron-right";
  import CirclePlus from "@lucide/svelte/icons/circle-plus";
  import Cpu from "@lucide/svelte/icons/cpu";
  import Download from "@lucide/svelte/icons/download";
  import FilePlus2 from "@lucide/svelte/icons/file-plus-2";
  import FolderOpen from "@lucide/svelte/icons/folder-open";
  import GripVertical from "@lucide/svelte/icons/grip-vertical";
  import ImagePlus from "@lucide/svelte/icons/image-plus";
  import Keyboard from "@lucide/svelte/icons/keyboard";
  import Layers3 from "@lucide/svelte/icons/layers-3";
  import MonitorUp from "@lucide/svelte/icons/monitor-up";
  import Play from "@lucide/svelte/icons/play";
  import Plus from "@lucide/svelte/icons/plus";
  import Pencil from "@lucide/svelte/icons/pencil";
  import RefreshCw from "@lucide/svelte/icons/refresh-cw";
  import Save from "@lucide/svelte/icons/save";
  import Trash2 from "@lucide/svelte/icons/trash-2";
  import Upload from "@lucide/svelte/icons/upload";
  import Usb from "@lucide/svelte/icons/usb";
  import X from "@lucide/svelte/icons/x";
  import { backupBundle, deviceStatus, loadWorkspace, openArchive, prepareIconAnimation, saveArchive, saveWorkspace, syncFromDevice, syncProject, testScreensaver as testScreensaverOnDevice, uploadScreensaver as uploadScreensaverToDevice, validateProject } from "./lib/backend";
  import type { CompileSummary, DeviceStatus } from "./lib/backend";
  import { CONSUMER_KEYS, KEYBOARD_KEYS, starterProject } from "./lib/model";
  import type { ActionKind, Asset, Macro, MacroStep, Project, RadialSize } from "./lib/model";
  import { radialDirection, radialGridOffset } from "./lib/radial";

  let project: Project = starterProject();
  let profileIndex = 0;
  let pageIndex = 0;
  let selectedButton = 0;
  let projectPath = "";
  let dirty = false;
  let busy = false;
  let notice = "Ready";
  let device: DeviceStatus = { connected: false, generation: 0, capabilities: 0, detail: "Checking for Screendeck…" };
  let summary: CompileSummary = { bundleBytes: 0, payloadCrc32: 0, fingerprint: "", issues: [] };
  let lastSyncedFingerprint = "";
  let workspaceReady = false;
  let autosaveTimer: ReturnType<typeof setTimeout> | undefined;
  let validationTimer: ReturnType<typeof setTimeout> | undefined;
  let projectRevision = 0;
  let validatedRevision = -1;
  let selectedRadialItem = 0;
  type ContextTarget = { kind: "project"; profileIndex: number } | { kind: "page"; profileIndex: number; pageIndex: number };
  let contextMenu: { x: number; y: number; target: ContextTarget } | null = null;
  let contextMenuElement: HTMLDivElement | null = null;
  let renameDialog: { target: ContextTarget; value: string } | null = null;
  let renameInput: HTMLInputElement | null = null;
  let deleteDialog: { target: ContextTarget; name: string } | null = null;

  $: profile = project.profiles[profileIndex];
  $: page = profile.pages[pageIndex];
  $: button = page.buttons[selectedButton];
  $: macro = button.macroId ? project.macros.find((item) => item.id === button.macroId) : undefined;
  $: radialItem = button.radial?.items[selectedRadialItem];
  $: radialMacro = radialItem?.macroId ? project.macros.find((item) => item.id === radialItem.macroId) : undefined;
  $: if (button.radial && selectedRadialItem >= button.radial.items.length) selectedRadialItem = 0;
  $: assetsById = new Map(project.assets.map((asset) => [asset.id, asset]));
  $: queueValidation(projectRevision);
  $: if (workspaceReady) queueWorkspaceSave(projectRevision);
  $: deviceDiff = !device.connected
    ? "Device unavailable"
    : summary.issues.length
      ? "Local project invalid"
      : lastSyncedFingerprint && lastSyncedFingerprint === summary.fingerprint
        ? `Device matches ${summary.fingerprint}`
        : `Pending/unknown · local ${summary.fingerprint || "unbuilt"} · device generation ${device.generation}`;

  function changed(message = "Unsaved changes") {
    project = { ...project };
    projectRevision += 1;
    dirty = true;
    notice = message;
  }

  function queueWorkspaceSave(revision: number) {
    clearTimeout(autosaveTimer);
    autosaveTimer = setTimeout(async () => {
      if (revision !== projectRevision) return;
      try { await saveWorkspace(project); }
      catch (error) { notice = `Automatic save failed: ${error}`; }
    }, 500);
  }

  function queueValidation(revision: number) {
    clearTimeout(validationTimer);
    validationTimer = setTimeout(() => refreshValidation(revision), 150);
  }

  async function restoreWorkspace() {
    try {
      const restored = await loadWorkspace();
      if (restored) {
        project = restored;
        projectRevision += 1;
        notice = "Restored previous workspace";
      }
    } catch (error) { notice = `Could not restore workspace: ${error}`; }
    finally { workspaceReady = true; }
  }

  async function refreshValidation(revision = projectRevision) {
    const result = await validateProject(project);
    if (revision === projectRevision) {
      summary = result;
      validatedRevision = revision;
    }
  }

  async function refreshDevice() {
    try { device = await deviceStatus(); }
    catch (error) { device = { connected: false, generation: 0, capabilities: 0, detail: String(error) }; }
  }

  async function newProject() {
    project = starterProject(); projectRevision += 1; profileIndex = 0; pageIndex = 0; selectedButton = 0; projectPath = ""; dirty = false; lastSyncedFingerprint = ""; notice = "New project";
  }

  async function openProject() {
    const path = await open({ title: "Open Screendeck project", filters: [{ name: "Screendeck project", extensions: ["sdeck"] }] });
    if (!path || Array.isArray(path)) return;
    busy = true;
    try { project = await openArchive(path); projectRevision += 1; projectPath = path; profileIndex = 0; pageIndex = 0; selectedButton = 0; dirty = false; lastSyncedFingerprint = ""; notice = `Opened ${path.split(/[\\/]/).pop()}`; }
    catch (error) { notice = `Could not open project: ${error}`; }
    finally { busy = false; }
  }

  async function saveProject(saveAs = false) {
    let path = projectPath;
    if (!path || saveAs) path = await save({ title: "Save Screendeck project", defaultPath: `${project.name}.sdeck`, filters: [{ name: "Screendeck project", extensions: ["sdeck"] }] }) ?? "";
    if (!path) return;
    busy = true;
    try { await saveArchive(path, project); projectPath = path; dirty = false; notice = "Project saved"; }
    catch (error) { notice = `Save failed: ${error}`; }
    finally { busy = false; }
  }

  async function backup() {
    const path = await save({ title: "Export compiled backup", defaultPath: `${project.name}.sdb`, filters: [{ name: "Screendeck bundle", extensions: ["sdb"] }] });
    if (!path) return;
    try { await backupBundle(path, project); notice = "Compiled backup exported"; }
    catch (error) { notice = `Backup failed: ${error}`; }
  }

  async function sync() {
    const errors = validatedRevision === projectRevision
      ? summary.issues.filter((issue) => issue.severity === "error")
      : [];
    if (errors.length) { notice = `Cannot sync: ${errors.map((issue) => `${issue.path} — ${issue.message}`).join(" · ")}`; return; }
    busy = true;
    notice = "Syncing project to device…";
    try {
      const result = await syncProject(project);
      lastSyncedFingerprint = result.fingerprint;
      notice = `Synced ${result.bytesSent.toLocaleString()} bytes · generation ${result.generation}${result.resumedAt ? ` · resumed at ${result.resumedAt}` : ""}`;
      await refreshDevice();
    } catch (error) { notice = `Sync failed: ${error}`; }
    finally { busy = false; }
  }

  async function uploadScreensaver() {
    const path = await open({
      title: "Upload screensaver",
      filters: [
        { name: "Images and videos", extensions: ["mp4", "mov", "mkv", "avi", "webm", "gif", "png", "jpg", "jpeg", "webp", "bmp", "mjpg", "mjpeg"] },
        { name: "Raw MJPEG screensaver", extensions: ["mjpg", "mjpeg"] }
      ]
    });
    if (!path || Array.isArray(path)) return;
    busy = true;
    notice = `Converting and uploading ${path.split(/[\\/]/).pop()}…`;
    try {
      const result = await uploadScreensaverToDevice(path);
      notice = `Screensaver uploaded · ${result.bytesSent.toLocaleString()} bytes${result.resumedAt ? ` · resumed at ${result.resumedAt}` : ""}`;
    } catch (error) { notice = `Screensaver upload failed: ${error}`; }
    finally { busy = false; }
  }

  async function importFromDevice() {
    if (dirty && !confirm("Replace the current unsaved project with the profile stored on the Screendeck?")) return;
    busy = true; notice = "Syncing profile from device…";
    try {
      project = await syncFromDevice();
      projectRevision += 1;
      profileIndex = 0; pageIndex = 0; selectedButton = 0;
      projectPath = ""; dirty = true; await refreshValidation(); lastSyncedFingerprint = summary.fingerprint;
      notice = `Imported ${project.profiles.length} profile${project.profiles.length === 1 ? "" : "s"}, ${project.assets.length} icon${project.assets.length === 1 ? "" : "s"}, and ${project.macros.length} macro${project.macros.length === 1 ? "" : "s"} from device`;
    } catch (error) { notice = `Sync from device failed: ${error}`; }
    finally { busy = false; }
  }

  async function testScreensaver() {
    busy = true;
    notice = "Starting screensaver test…";
    try {
      await testScreensaverOnDevice();
      notice = "Screensaver test started · touch the device to return";
    } catch (error) { notice = `Screensaver test failed: ${error}`; }
    finally { busy = false; }
  }

  function addPage(targetProfileIndex = profileIndex) {
    const targetProfile = project.profiles[targetProfileIndex];
    targetProfile.pages.push({ id: crypto.randomUUID(), name: `Page ${targetProfile.pages.length + 1}`, buttons: Array.from({ length: 32 }, () => ({ action: "none" })) });
    profileIndex = targetProfileIndex; pageIndex = targetProfile.pages.length - 1; selectedButton = 0; changed("Page added");
  }

  function addProfile() {
    project.profiles.push({ id: crypto.randomUUID(), name: `Project ${project.profiles.length + 1}`, pages: [{ id: crypto.randomUUID(), name: "Main", buttons: Array.from({ length: 32 }, () => ({ action: "none" })) }] });
    profileIndex = project.profiles.length - 1; pageIndex = 0; changed("Project added");
  }

  function selectProfile(index: number) { profileIndex = index; pageIndex = 0; selectedButton = 0; }
  function selectPage(index: number) { pageIndex = index; selectedButton = 0; }

  function targetName(target: ContextTarget) {
    return target.kind === "project"
      ? project.profiles[target.profileIndex]?.name ?? "Project"
      : project.profiles[target.profileIndex]?.pages[target.pageIndex]?.name ?? "Page";
  }

  function canDeleteTarget(target: ContextTarget) {
    return target.kind === "project"
      ? project.profiles.length > 1
      : project.profiles[target.profileIndex].pages.length > 1;
  }

  async function openContextMenu(event: MouseEvent, target: ContextTarget) {
    event.preventDefault();
    if (target.kind === "project") selectProfile(target.profileIndex);
    else { profileIndex = target.profileIndex; pageIndex = target.pageIndex; selectedButton = 0; }
    const width = 224;
    const height = 210;
    contextMenu = {
      x: Math.max(8, Math.min(event.clientX, window.innerWidth - width - 8)),
      y: Math.max(8, Math.min(event.clientY, window.innerHeight - height - 8)),
      target
    };
    await tick();
    contextMenuElement?.querySelector<HTMLButtonElement>("button:not(:disabled)")?.focus();
  }

  function closeContextMenu() { contextMenu = null; }

  function contextMenuKeydown(event: KeyboardEvent) {
    if (!contextMenuElement || !contextMenu) return;
    if (event.key === "Escape") { event.preventDefault(); closeContextMenu(); return; }
    if (event.key === "F2") { event.preventDefault(); startRename(contextMenu.target); return; }
    if (event.key === "Delete") { event.preventDefault(); requestDelete(contextMenu.target); return; }
    if (event.ctrlKey && event.key.toLowerCase() === "n") { event.preventDefault(); createFromMenu("project", contextMenu.target); return; }
    if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const items = [...contextMenuElement.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
    const current = items.indexOf(document.activeElement as HTMLButtonElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1 : event.key === "ArrowDown" ? (current + 1) % items.length : (current - 1 + items.length) % items.length;
    items[next]?.focus();
  }

  function createFromMenu(kind: "project" | "page", target: ContextTarget) {
    closeContextMenu();
    if (kind === "project") addProfile();
    else addPage(target.profileIndex);
  }

  async function startRename(target: ContextTarget) {
    renameDialog = { target, value: targetName(target) };
    closeContextMenu();
    await tick();
    renameInput?.focus();
    renameInput?.select();
  }

  function saveRename() {
    if (!renameDialog) return;
    const name = renameDialog.value.trim();
    if (!name) { notice = `${renameDialog.target.kind === "project" ? "Project" : "Page"} name cannot be empty`; renameInput?.focus(); return; }
    if (renameDialog.target.kind === "project") project.profiles[renameDialog.target.profileIndex].name = name;
    else project.profiles[renameDialog.target.profileIndex].pages[renameDialog.target.pageIndex].name = name;
    const kind = renameDialog.target.kind;
    renameDialog = null;
    changed(`${kind === "project" ? "Project" : "Page"} renamed`);
  }

  function requestDelete(target: ContextTarget) {
    closeContextMenu();
    if (!canDeleteTarget(target)) {
      notice = target.kind === "project" ? "At least one project is required" : "A project must keep at least one page";
      return;
    }
    deleteDialog = { target, name: targetName(target) };
  }

  function macrosUsedByPages(pages: typeof profile.pages) {
    return pages.flatMap((itemPage) => itemPage.buttons.flatMap((itemButton) => [itemButton.macroId, ...(itemButton.radial?.items.map((item) => item.macroId) ?? [])]).filter((id): id is string => Boolean(id)));
  }

  function confirmDelete() {
    if (!deleteDialog) return;
    const target = deleteDialog.target;
    if (target.kind === "project") {
      const removed = project.profiles[target.profileIndex];
      const removedMacroIds = macrosUsedByPages(removed.pages);
      project.profiles.splice(target.profileIndex, 1);
      for (const id of removedMacroIds) removeMacroIfUnreferenced(id);
      profileIndex = Math.min(target.profileIndex, project.profiles.length - 1);
      pageIndex = 0;
      deleteDialog = null; selectedButton = 0; changed("Project deleted");
      return;
    }
    const targetProfile = project.profiles[target.profileIndex];
    const removedMacroIds = macrosUsedByPages([targetProfile.pages[target.pageIndex]]);
    targetProfile.pages.splice(target.pageIndex, 1);
    for (const id of removedMacroIds) removeMacroIfUnreferenced(id);
    profileIndex = target.profileIndex;
    pageIndex = Math.min(target.pageIndex, targetProfile.pages.length - 1);
    deleteDialog = null; selectedButton = 0; changed("Page deleted");
  }

  function removeMacroIfUnreferenced(id?: string) {
    if (!id) return;
    const referenced = project.profiles.some((itemProfile) => itemProfile.pages.some((itemPage) =>
      itemPage.buttons.some((itemButton) => itemButton.macroId === id || itemButton.radial?.items.some((item) => item.macroId === id))
    ));
    if (!referenced) project.macros = project.macros.filter((item) => item.id !== id);
  }

  function makeRadialMacro(slot: number, size: RadialSize) {
    const id = crypto.randomUUID();
    project.macros.push({ id, name: radialDirection(slot, size), steps: [] });
    return id;
  }

  function setRadialSize(value: string) {
    const size = Number(value) as RadialSize;
    if (button.radial?.size === size) return;
    const oldRadial = button.radial;
    const items = Array.from({ length: size }, (_, index) => {
      if (!oldRadial) return { action: "macro" as ActionKind, macroId: makeRadialMacro(index, size), imageFit: "cover" as const };
      const exact = oldRadial.items.find((_, oldIndex) => {
        const delta = Math.abs(oldIndex / oldRadial.size - index / size);
        return Math.min(delta, 1 - delta) < .001;
      });
      return exact ?? { action: "macro" as ActionKind, macroId: makeRadialMacro(index, size), imageFit: "cover" as const };
    });
    const discarded = oldRadial?.items.filter((item) => !items.includes(item)).map((item) => item.macroId) ?? [];
    button.radial = { size, items };
    for (const id of discarded) removeMacroIfUnreferenced(id);
    selectedRadialItem = Math.min(selectedRadialItem, size - 1);
    changed(`Configured ${size}-way radial menu`);
  }

  function removeRadial() {
    if (!button.radial) return;
    const ids = button.radial?.items.map((item) => item.macroId) ?? [];
    button.radial = undefined;
    selectedRadialItem = 0;
    for (const id of ids) removeMacroIfUnreferenced(id);
    changed("Radial menu removed");
  }

  function updateButton(field: string, value: string) {
    const buttonTarget = page.buttons[selectedButton];
    const previousMacroId = buttonTarget.macroId;
    const target = buttonTarget as unknown as Record<string, string | undefined>;
    target[field] = value || undefined;
    if (field === "action" && value === "macro" && !buttonTarget.macroId) {
      const id = crypto.randomUUID();
      project.macros.push({ id, name: `${profile.name} · ${page.name} · Key ${selectedButton + 1}`, steps: [] });
      buttonTarget.macroId = id;
    } else if (field === "action" && value !== "macro") {
      buttonTarget.macroId = undefined;
      removeMacroIfUnreferenced(previousMacroId);
    }
    changed();
  }

  function updateRadialAction(value: string) {
    if (!radialItem || !button.radial) return;
    const previousMacroId = radialItem.macroId;
    radialItem.action = value as ActionKind;
    if (radialItem.action === "macro" && !radialItem.macroId) {
      radialItem.macroId = makeRadialMacro(selectedRadialItem, button.radial.size);
    } else if (radialItem.action !== "macro") {
      radialItem.macroId = undefined;
      removeMacroIfUnreferenced(previousMacroId);
    }
    changed("Radial action changed");
  }

  function updateStep(targetMacro: Macro, index: number, field: keyof MacroStep, value: string | number) {
    const target = targetMacro.steps[index];
    (target as unknown as Record<string, string | number | undefined>)[field] = value;
    if (field === "kind") {
      if (value === "delay") { target.key = undefined; target.durationMs ??= 25; }
      else if (value === "key_press") { target.key = "F13"; target.durationMs = 25; target.modifiers = []; }
      else { target.durationMs = undefined; target.modifiers = []; target.key = value === "consumer" ? "PLAY_PAUSE" : "F13"; }
    }
    changed();
  }

  function addStep(targetMacro: Macro) { targetMacro.steps.push({ kind: "key_press", key: "F13", durationMs: 25, modifiers: [] }); changed("Key press added"); }
  function removeStep(targetMacro: Macro, index: number) { targetMacro.steps.splice(index, 1); changed("Macro step removed"); }

  function toggleModifier(step: MacroStep, modifier: string) {
    step.modifiers ??= [];
    step.modifiers = step.modifiers.includes(modifier) ? step.modifiers.filter((item) => item !== modifier) : [...step.modifiers, modifier];
    changed("Key modifier changed");
  }


  function iconFor(assetId?: string) { return assetId ? assetsById.get(assetId)?.dataUrl : undefined; }

  async function importFiles(files: FileList | File[]) {
    for (const file of Array.from(files)) {
      if (!file.type.startsWith("image/") && !file.type.startsWith("video/")) continue;
      const sourceDataUrl = await new Promise<string>((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(String(reader.result));
        reader.onerror = () => reject(reader.error);
        reader.readAsDataURL(file);
      });
      const animated = file.type === "image/gif" || file.type === "image/webp" || file.type.startsWith("video/");
      let dataUrl: string;
      let animationDataUrl = "";
      let animationFps = 0;
      if (animated) {
        const converted = await prepareIconAnimation(file.name, sourceDataUrl);
        dataUrl = converted.posterDataUrl;
        animationDataUrl = converted.animationDataUrl;
        animationFps = converted.frameCount > 1 ? 15 : 0;
      } else {
        const bitmap = await createImageBitmap(file);
        const scale = Math.min(1, 256 / Math.max(bitmap.width, bitmap.height));
        const canvas = document.createElement("canvas");
        canvas.width = Math.max(1, Math.round(bitmap.width * scale));
        canvas.height = Math.max(1, Math.round(bitmap.height * scale));
        canvas.getContext("2d")!.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
        bitmap.close();
        dataUrl = canvas.toDataURL("image/png");
      }
      const asset: Asset = {
        id: crypto.randomUUID(),
        name: file.name.replace(/\.[^.]+$/, "") + ".png",
        mediaType: animated ? "image/jpeg" : "image/png",
        dataUrl,
        sourceName: file.name,
        sourceMediaType: file.type || "application/octet-stream",
        sourceDataUrl,
        animationDataUrl,
        animationFps
      };
      project.assets.push(asset);
      page.buttons[selectedButton].iconId = asset.id;
      page.buttons[selectedButton].imageFit = "cover";
    }
    changed("Icon imported and assigned");
  }

  function dropIcon(event: DragEvent) { event.preventDefault(); if (event.dataTransfer?.files.length) importFiles(event.dataTransfer.files); }
  function assignAsset(id: string) { page.buttons[selectedButton].iconId = id; page.buttons[selectedButton].imageFit ??= "cover"; changed("Icon assigned"); }
  function clearIcon() { page.buttons[selectedButton].iconId = undefined; page.buttons[selectedButton].imageFit = undefined; changed("Icon removed"); }
  function removeAsset(id: string) {
    const asset = project.assets.find((item) => item.id === id);
    if (!asset || !confirm(`Remove “${asset.name}” from the icon library and every slot using it?`)) return;
    for (const itemProfile of project.profiles) for (const itemPage of itemProfile.pages) for (const itemButton of itemPage.buttons) {
      if (itemButton.iconId === id) { itemButton.iconId = undefined; itemButton.imageFit = undefined; }
      for (const radialItem of itemButton.radial?.items ?? []) if (radialItem.iconId === id) radialItem.iconId = undefined;
    }
    project.assets = project.assets.filter((item) => item.id !== id);
    changed("Icon removed from library");
  }

  restoreWorkspace();
  refreshDevice();
</script>

<svelte:window on:click={closeContextMenu} on:blur={closeContextMenu} on:keydown={(event) => {
  if (event.key === "Escape") { closeContextMenu(); renameDialog = null; deleteDialog = null; }
  if (event.ctrlKey && event.key.toLowerCase() === "s") { event.preventDefault(); saveProject(event.shiftKey); }
}} />

<div class="app-shell">
  <header class="topbar">
    <div class="brand"><div class="brand-mark"><Layers3 size={17}/></div><span>Screendeck</span><span class="version">M6</span></div>
    <div class="project-title"><input aria-label="Project name" bind:value={project.name} on:input={() => changed()} /><span class:visible={dirty}>Unsaved</span></div>
    <nav class="toolbar" aria-label="Project actions">
      <button class="icon-button" title="New project" on:click={newProject}><FilePlus2 size={17}/></button>
      <button class="icon-button" title="Open project" on:click={openProject}><FolderOpen size={17}/></button>
      <button class="icon-button" title="Save project" on:click={() => saveProject()}><Save size={17}/></button>
      <button class="icon-button" title="Export compiled backup" on:click={backup}><Archive size={17}/></button>
      <div class="divider"></div>
      <button class="icon-button" title="Upload screensaver image or video" disabled={busy || !device.connected} on:click={uploadScreensaver}><MonitorUp size={17}/></button>
      <button class="icon-button" title="Test screensaver on device" disabled={busy || !device.connected} on:click={testScreensaver}><Play size={17}/></button>
      <button class="sync-button sync-from" title="Replace the editor project with the active device profile" disabled={busy || !device.connected} on:click={importFromDevice}><Download size={16}/>From device</button>
      <button class="sync-button" disabled={busy || !device.connected} on:click={sync}><Upload size={16}/>{busy ? "Working…" : "Sync to device"}</button>
    </nav>
  </header>

  <aside class="sidebar">
    <div class="section-title"><span>Projects</span><button title="Add project" on:click={addProfile}><Plus size={15}/></button></div>
    <div class="tree">
      {#each project.profiles as item, pi}
        <button class:active={pi === profileIndex} class="tree-profile" title="Right-click for project actions" on:click={() => selectProfile(pi)} on:contextmenu={(event) => openContextMenu(event, { kind: "project", profileIndex: pi })}><Layers3 size={15}/><span>{item.name}</span><span class="count">{item.pages.length}</span></button>
        {#if pi === profileIndex}
          <div class="pages">
            {#each item.pages as itemPage, pgi}
              <button class:active={pgi === pageIndex} title="Right-click for page actions" on:click={() => selectPage(pgi)} on:contextmenu={(event) => openContextMenu(event, { kind: "page", profileIndex: pi, pageIndex: pgi })}><span class="page-dot"></span><span>{itemPage.name}</span></button>
            {/each}
            <button class="add-row" on:click={() => addPage()}><CirclePlus size={14}/> Add page</button>
          </div>
        {/if}
      {/each}
    </div>

    <div class="section-title macro-heading"><span>Device</span></div>
    <div class="screensaver-setting">
      <label for="brightness">Brightness</label>
      <div class="brightness-control"><input id="brightness" type="range" min="0" max="100" step="5" style={`--range-progress:${project.brightnessPercent}%`} bind:value={project.brightnessPercent} on:change={() => changed("Brightness changed")}/><output for="brightness">{project.brightnessPercent}%</output></div>
      <label for="orientation">Orientation</label>
      <select id="orientation" bind:value={project.orientation} on:change={() => changed("Orientation changed")}><option value="landscape">Landscape</option><option value="landscape_flipped">Landscape · flipped</option></select>
      <label class="check-setting"><input type="checkbox" bind:checked={project.screensaverEnabled} on:change={() => changed("Screensaver setting changed")}/> Screensaver enabled</label>
      <label for="screensaver-delay">Idle timeout</label>
      <div><input id="screensaver-delay" type="number" min="5" max="3600" step="5" bind:value={project.screensaverTimeoutSeconds} on:change={() => changed("Screensaver delay changed")}/><span>seconds</span></div>
    </div>
  </aside>

  {#if contextMenu}
    <div
      class="context-menu"
      role="menu"
      tabindex="-1"
      aria-label={`${contextMenu.target.kind === "project" ? "Project" : "Page"} actions`}
      style={`left:${contextMenu.x}px;top:${contextMenu.y}px`}
      bind:this={contextMenuElement}
      on:click|stopPropagation
      on:contextmenu|preventDefault
      on:keydown={contextMenuKeydown}
    >
      <div class="context-menu-heading"><span>{contextMenu.target.kind}</span><strong>{targetName(contextMenu.target)}</strong></div>
      <div class="context-menu-group">
        <button role="menuitem" on:click={() => createFromMenu("project", contextMenu!.target)}><span class="context-menu-icon"><Plus size={14}/></span><span>New project</span><kbd>Ctrl+N</kbd></button>
        <button role="menuitem" on:click={() => createFromMenu("page", contextMenu!.target)}><span class="context-menu-icon"><CirclePlus size={14}/></span><span>New page</span></button>
      </div>
      <div class="context-menu-separator"></div>
      <div class="context-menu-group">
        <button role="menuitem" on:click={() => startRename(contextMenu!.target)}><span class="context-menu-icon"><Pencil size={14}/></span><span>Rename</span><kbd>F2</kbd></button>
        <button class="danger" role="menuitem" disabled={!canDeleteTarget(contextMenu.target)} on:click={() => requestDelete(contextMenu!.target)}><span class="context-menu-icon"><Trash2 size={14}/></span><span>Delete</span></button>
      </div>
    </div>
  {/if}

  {#if renameDialog}
    <div class="dialog-backdrop" role="presentation" on:mousedown={(event) => { if (event.currentTarget === event.target) renameDialog = null; }}>
      <form class="app-dialog" aria-labelledby="rename-title" on:submit|preventDefault={saveRename}>
        <div class="dialog-icon"><Pencil size={17}/></div>
        <div class="dialog-copy"><span>{renameDialog.target.kind}</span><h2 id="rename-title">Rename {renameDialog.target.kind}</h2><p>Choose a clear name you’ll recognise in the sidebar.</p></div>
        <label for="rename-input">Name</label>
        <input id="rename-input" bind:this={renameInput} bind:value={renameDialog.value} maxlength="64" autocomplete="off" />
        <div class="dialog-actions"><button type="button" class="secondary" on:click={() => renameDialog = null}>Cancel</button><button type="submit" class="primary">Save name</button></div>
      </form>
    </div>
  {/if}

  {#if deleteDialog}
    <div class="dialog-backdrop" role="presentation" on:mousedown={(event) => { if (event.currentTarget === event.target) deleteDialog = null; }}>
      <div class="app-dialog delete-dialog" role="alertdialog" aria-modal="true" aria-labelledby="delete-title" tabindex="-1">
        <div class="dialog-icon danger"><Trash2 size={17}/></div>
        <div class="dialog-copy"><span>{deleteDialog.target.kind}</span><h2 id="delete-title">Delete “{deleteDialog.name}”?</h2><p>{deleteDialog.target.kind === "project" ? "Its pages and button setup will also be removed." : "Its button setup will be removed from this project."} This can’t be undone.</p></div>
        <div class="dialog-actions"><button type="button" class="secondary" on:click={() => deleteDialog = null}>Cancel</button><button type="button" class="delete-confirm" on:click={confirmDelete}>Delete {deleteDialog.target.kind}</button></div>
      </div>
    </div>
  {/if}

  <main class="workspace">
    <div class="workspace-head">
      <div><span class="eyebrow">{profile.name}</span><h1>{page.name}</h1></div>
      <div class="pager"><button on:click={() => pageIndex = Math.max(0, pageIndex - 1)} disabled={pageIndex === 0}><ChevronLeft size={16}/></button><span>{pageIndex + 1} / {profile.pages.length}</span><button on:click={() => pageIndex = Math.min(profile.pages.length - 1, pageIndex + 1)} disabled={pageIndex === profile.pages.length - 1}><ChevronRight size={16}/></button></div>
    </div>
    <section class="device-preview" aria-label="1280 by 720 Screendeck preview">
      <div class="grid">
        {#each page.buttons as tile, index}
          {@const icon = iconFor(tile.iconId)}
          <button class:selected={index === selectedButton} class:has-radial={Boolean(tile.radial)} class="tile" aria-label={`Button ${index + 1}`} on:click={() => selectedButton = index} on:dragover={(e) => e.preventDefault()} on:drop={dropIcon}>
            {#if icon}<img class:contained={tile.imageFit === "contain"} src={icon} alt="" draggable="false"/>
            {:else if tile.action === "page_previous"}<ChevronLeft size={24}/>
            {:else if tile.action === "page_next"}<ChevronRight size={24}/>
            {:else if tile.action === "profile_next"}<Layers3 size={22}/>
            {:else if tile.action === "macro"}<Keyboard size={21}/>
            {/if}
          </button>
        {/each}
      </div>
    </section>

    <section class="assets-strip">
      <div class="assets-copy"><ImagePlus size={17}/><div><strong>Icon library</strong><span>Drop images onto any key</span></div></div>
      <label class="asset-add"><input type="file" accept="image/png,image/jpeg,image/webp,image/gif,video/mp4,video/webm,video/quicktime" multiple on:change={(event) => { const files = event.currentTarget.files; if (files) importFiles(files); }}/><Plus size={18}/></label>
      {#each project.assets as asset}<div class="asset-item"><button class="asset" title={`${asset.name}${asset.animationFps ? " · 15 FPS" : ""}`} on:click={() => assignAsset(asset.id)}><img src={asset.dataUrl} alt={asset.name}/></button><button class="asset-delete" title={`Remove ${asset.name} from library`} aria-label={`Remove ${asset.name} from library`} on:click={() => removeAsset(asset.id)}><X size={10}/></button></div>{/each}
      {#if !project.assets.length}<span class="empty-assets">PNG, JPEG or WebP · originals stay in the project archive</span>{/if}
    </section>
  </main>

  <aside class="inspector">
    <div class="inspector-tabs"><button class="active">Button</button></div>
    <div class="inspector-scroll">
      <label>Action<select value={button.action} on:change={(e) => updateButton("action", e.currentTarget.value)}><option value="none">None</option><option value="macro">Run macro</option><option value="page_next">Next page</option><option value="page_previous">Previous page</option><option value="profile_next">Next profile</option></select></label>

      {#if button.action === "macro" && macro}
        <div class="button-macro-editor">
          <div class="macro-title"><div><span>Macro sequence</span><strong>{macro.name}</strong></div></div>
          <div class="steps">
            {#each macro.steps as step, index}
              <div class="step">
                <GripVertical class="grip" size={14}/>
                <span class="step-number">{index + 1}</span>
                <select value={step.kind} on:change={(e) => updateStep(macro, index, "kind", e.currentTarget.value)}><option value="key_press">Key press</option><option value="key_down">Key down</option><option value="key_up">Key up</option><option value="delay">Delay</option><option value="consumer">Media key</option></select>
                {#if step.kind === "delay"}<div class="duration"><input type="number" min="1" max="60000" value={step.durationMs ?? 25} on:change={(e) => updateStep(macro, index, "durationMs", Number(e.currentTarget.value))}/><span>ms</span></div>
                {:else}<select class="key-picker" value={step.key ?? (step.kind === "consumer" ? "PLAY_PAUSE" : "F13")} on:change={(e) => updateStep(macro, index, "key", e.currentTarget.value)}>{#each step.kind === "consumer" ? CONSUMER_KEYS : KEYBOARD_KEYS as key}<option value={key}>{key.replaceAll("_", " ")}</option>{/each}</select>{/if}
                <button class="step-delete" title="Remove step" on:click={() => removeStep(macro, index)}><X size={13}/></button>
                {#if step.kind === "key_press"}<div class="step-modifiers"><span>Hold</span>{#each [["CTRL","Ctrl"],["SHIFT","Shift"],["ALT","Alt"],["GUI","Win"]] as modifier}<button class:active={step.modifiers?.includes(modifier[0])} on:click={() => toggleModifier(step, modifier[0])}>{modifier[1]}</button>{/each}<label><input type="number" min="1" max="1000" value={step.durationMs ?? 25} on:change={(e) => updateStep(macro, index, "durationMs", Number(e.currentTarget.value))}/> ms</label></div>{/if}
              </div>
            {/each}
            <button class="add-step" on:click={() => addStep(macro)}><Plus size={14}/> Add step</button>
          </div>
        </div>
      {/if}

      {#if button.iconId}
        <div class="artwork-controls">
          <label>Artwork display<select value={button.imageFit ?? "cover"} on:change={(e) => updateButton("imageFit", e.currentTarget.value)}><option value="cover">Fill entire key</option><option value="contain">Fit inside key</option></select></label>
          <p>Fill entire key crops non-square artwork at the edges. Fit inside preserves the complete image.</p>
          <button class="remove-artwork" on:click={clearIcon}><Trash2 size={14}/> Remove artwork</button>
        </div>
      {/if}

      <div class="rule"></div>
      <div class="radial-editor">
        <div class="macro-title"><div><span>Flick gesture</span><strong>Radial menu</strong></div></div>
        <span class="field-label">Layout</span>
        <div class="radial-layout-options" aria-label="Radial layout">
          <button class:active={!button.radial} on:click={removeRadial}>Off</button>
          {#each [4, 6, 8] as size}<button class:active={button.radial?.size === size} on:click={() => setRadialSize(String(size))}>{size}-way</button>{/each}
        </div>
        {#if button.radial}
          <p class="helper">Choose a full-size key position to configure its device action.</p>
          <div class="radial-map" aria-label={`${button.radial.size}-way radial positions`}>
            <div class="radial-map-centre">
              {#if iconFor(button.iconId)}<img class:contained={button.imageFit === "contain"} src={iconFor(button.iconId)} alt=""/>
              {:else}<span>Source<br/>key</span>{/if}
            </div>
            {#each button.radial.items as item, index}
              {@const offset = radialGridOffset(index, button.radial.size)}
              <button class:active={selectedRadialItem === index} class="radial-map-item" style={`left:${118 + offset.x * 72}px;top:${118 + offset.y * 72}px`} title={radialDirection(index, button.radial.size)} aria-label={`Edit ${radialDirection(index, button.radial.size)} radial action`} on:click={() => selectedRadialItem = index}>
                {#if iconFor(item.iconId)}<img class:contained={item.imageFit !== "cover"} src={iconFor(item.iconId)} alt=""/>{:else}<Keyboard size={22}/>{/if}
              </button>
            {/each}
          </div>
          <div class="radial-item-editor">
            <div class="radial-item-heading"><span>Position {selectedRadialItem + 1}</span><strong>{radialDirection(selectedRadialItem, button.radial.size)}</strong></div>
            <label>Action<select value={radialItem?.action ?? "macro"} on:change={(e) => updateRadialAction(e.currentTarget.value)}><option value="none">None</option><option value="macro">Run macro</option><option value="page_next">Next page</option><option value="page_previous">Previous page</option><option value="profile_next">Next profile</option></select></label>
            {#if radialItem?.action === "macro" && radialMacro}
              <div class="radial-macro-editor">
                <div class="macro-title"><div><span>Macro sequence</span><strong>{radialDirection(selectedRadialItem, button.radial.size)}</strong></div></div>
                <div class="steps">
                  {#each radialMacro.steps as step, index}
                    <div class="step">
                      <GripVertical class="grip" size={14}/>
                      <span class="step-number">{index + 1}</span>
                      <select value={step.kind} on:change={(e) => updateStep(radialMacro, index, "kind", e.currentTarget.value)}><option value="key_press">Key press</option><option value="key_down">Key down</option><option value="key_up">Key up</option><option value="delay">Delay</option><option value="consumer">Media key</option></select>
                      {#if step.kind === "delay"}<div class="duration"><input type="number" min="1" max="60000" value={step.durationMs ?? 25} on:change={(e) => updateStep(radialMacro, index, "durationMs", Number(e.currentTarget.value))}/><span>ms</span></div>
                      {:else}<select class="key-picker" value={step.key ?? (step.kind === "consumer" ? "PLAY_PAUSE" : "F13")} on:change={(e) => updateStep(radialMacro, index, "key", e.currentTarget.value)}>{#each step.kind === "consumer" ? CONSUMER_KEYS : KEYBOARD_KEYS as key}<option value={key}>{key.replaceAll("_", " ")}</option>{/each}</select>{/if}
                      <button class="step-delete" title="Remove step" on:click={() => removeStep(radialMacro, index)}><X size={13}/></button>
                      {#if step.kind === "key_press"}<div class="step-modifiers"><span>Hold</span>{#each [["CTRL","Ctrl"],["SHIFT","Shift"],["ALT","Alt"],["GUI","Win"]] as modifier}<button class:active={step.modifiers?.includes(modifier[0])} on:click={() => toggleModifier(step, modifier[0])}>{modifier[1]}</button>{/each}<label><input type="number" min="1" max="1000" value={step.durationMs ?? 25} on:change={(e) => updateStep(radialMacro, index, "durationMs", Number(e.currentTarget.value))}/> ms</label></div>{/if}
                    </div>
                  {/each}
                  <button class="add-step" on:click={() => addStep(radialMacro)}><Plus size={14}/> Add step</button>
                </div>
              </div>
            {/if}
            <span class="field-label">Icon</span>
            <div class="radial-icon-grid">
              <button class:active={!radialItem?.iconId} title="Use the default action icon" on:click={() => { if (radialItem) radialItem.iconId = undefined; changed("Radial icon changed"); }}><Keyboard size={16}/></button>
              {#each project.assets as asset}<button class:active={radialItem?.iconId === asset.id} title={asset.name} on:click={() => { if (radialItem) radialItem.iconId = asset.id; changed("Radial icon changed"); }}><img src={asset.dataUrl} alt=""/></button>{/each}
            </div>
            {#if radialItem?.iconId}
              <div class="artwork-controls radial-artwork-controls">
                <label>Artwork display<select value={radialItem.imageFit ?? "contain"} on:change={(e) => { radialItem.imageFit = e.currentTarget.value as "cover" | "contain"; changed("Radial artwork display changed"); }}><option value="cover">Fill entire key</option><option value="contain">Fit inside key</option></select></label>
                <p>Fill entire key crops non-square artwork at the edges. Fit inside preserves the complete image.</p>
              </div>
            {/if}
          </div>
        {/if}
      </div>
    </div>
  </aside>

  <footer class="statusbar">
    <button class:connected={device.connected} class="device-pill" on:click={refreshDevice}><span class="status-dot"></span><Usb size={14}/>{device.connected ? `Screendeck · generation ${device.generation}` : "No device"}<RefreshCw size={12}/></button>
    <div class="status-message" class:error={notice.includes("failed") || notice.includes("Resolve")}><span>{notice}</span></div>
    <div class="build-stats" title={deviceDiff}><span>{summary.bundleBytes.toLocaleString()} bytes</span><span>{deviceDiff}</span><Cpu size={13}/><span>{navigator.userAgent.includes("ARM64") ? "ARM64" : "x64"}</span></div>
  </footer>
</div>
