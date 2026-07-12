<script lang="ts">
  import { open, save } from "@tauri-apps/plugin-dialog";
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
  import RefreshCw from "@lucide/svelte/icons/refresh-cw";
  import Save from "@lucide/svelte/icons/save";
  import Trash2 from "@lucide/svelte/icons/trash-2";
  import Upload from "@lucide/svelte/icons/upload";
  import Usb from "@lucide/svelte/icons/usb";
  import X from "@lucide/svelte/icons/x";
  import { backupBundle, deviceStatus, loadWorkspace, openArchive, prepareIconAnimation, saveArchive, saveWorkspace, syncFromDevice, syncProject, testScreensaver as testScreensaverOnDevice, uploadScreensaver as uploadScreensaverToDevice, validateProject } from "./lib/backend";
  import type { CompileSummary, DeviceStatus } from "./lib/backend";
  import { CONSUMER_KEYS, KEYBOARD_KEYS, starterProject } from "./lib/model";
  import type { Asset, MacroStep, Project } from "./lib/model";

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

  $: profile = project.profiles[profileIndex];
  $: page = profile.pages[pageIndex];
  $: button = page.buttons[selectedButton];
  $: macro = button.macroId ? project.macros.find((item) => item.id === button.macroId) : undefined;
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

  function addPage() {
    profile.pages.push({ id: crypto.randomUUID(), name: `Page ${profile.pages.length + 1}`, buttons: Array.from({ length: 32 }, () => ({ action: "none" })) });
    pageIndex = profile.pages.length - 1; changed("Page added");
  }

  function addProfile() {
    project.profiles.push({ id: crypto.randomUUID(), name: `Profile ${project.profiles.length + 1}`, pages: [{ id: crypto.randomUUID(), name: "Main", buttons: Array.from({ length: 32 }, () => ({ action: "none" })) }] });
    profileIndex = project.profiles.length - 1; pageIndex = 0; changed("Profile added");
  }

  function selectProfile(index: number) { profileIndex = index; pageIndex = 0; selectedButton = 0; }
  function selectPage(index: number) { pageIndex = index; selectedButton = 0; }

  function removeMacroIfUnreferenced(id?: string) {
    if (!id) return;
    const referenced = project.profiles.some((itemProfile) => itemProfile.pages.some((itemPage) =>
      itemPage.buttons.some((itemButton) => itemButton.macroId === id)
    ));
    if (!referenced) project.macros = project.macros.filter((item) => item.id !== id);
  }

  function profileMenu(event: MouseEvent, index: number) {
    event.preventDefault();
    const target = project.profiles[index];
    const action = prompt(`Rename or delete “${target.name}”.\n\nEnter a new name, or type DELETE to remove it:`, target.name);
    if (action === null || action === target.name) return;
    if (action.trim().toUpperCase() === "DELETE") {
      if (project.profiles.length === 1) { notice = "A project must keep at least one profile"; return; }
      if (!confirm(`Delete profile “${target.name}” and all of its pages?`)) return;
      const removedMacroIds = target.pages.flatMap((itemPage) => itemPage.buttons.map((itemButton) => itemButton.macroId).filter((id): id is string => Boolean(id)));
      project.profiles.splice(index, 1);
      for (const id of removedMacroIds) removeMacroIfUnreferenced(id);
      profileIndex = Math.min(profileIndex > index ? profileIndex - 1 : profileIndex, project.profiles.length - 1);
      pageIndex = 0; selectedButton = 0; changed("Profile deleted"); return;
    }
    const name = action.trim();
    if (!name) { notice = "Profile name cannot be empty"; return; }
    target.name = name; changed("Profile renamed");
  }

  function updateButton(field: string, value: string) {
    const buttonTarget = page.buttons[selectedButton];
    const previousMacroId = buttonTarget.macroId;
    const target = buttonTarget as unknown as Record<string, string | undefined>;
    target[field] = value || undefined;
    if (field === "action" && value === "macro" && !buttonTarget.macroId) {
      const id = crypto.randomUUID();
      project.macros.push({ id, name: `${profile.name} · ${page.name} · Key ${selectedButton + 1}`, steps: [{ kind: "key_press", key: "F13", durationMs: 25, modifiers: [] }] });
      buttonTarget.macroId = id;
    } else if (field === "action" && value !== "macro") {
      buttonTarget.macroId = undefined;
      removeMacroIfUnreferenced(previousMacroId);
    }
    changed();
  }

  function updateStep(index: number, field: keyof MacroStep, value: string | number) {
    if (!macro) return;
    const target = macro.steps[index];
    (target as unknown as Record<string, string | number | undefined>)[field] = value;
    if (field === "kind") {
      if (value === "delay") { target.key = undefined; target.durationMs ??= 25; }
      else if (value === "key_press") { target.key = "F13"; target.durationMs = 25; target.modifiers = []; }
      else { target.durationMs = undefined; target.modifiers = []; target.key = value === "consumer" ? "PLAY_PAUSE" : "F13"; }
    }
    changed();
  }

  function addStep() { if (!macro) return; macro.steps.push({ kind: "key_press", key: "F13", durationMs: 25, modifiers: [] }); changed("Key press added"); }
  function removeStep(index: number) { if (!macro) return; macro.steps.splice(index, 1); changed("Macro step removed"); }

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
    }
    project.assets = project.assets.filter((item) => item.id !== id);
    changed("Icon removed from library");
  }

  restoreWorkspace();
  refreshDevice();
</script>

<svelte:window on:keydown={(event) => { if (event.ctrlKey && event.key.toLowerCase() === "s") { event.preventDefault(); saveProject(event.shiftKey); } }} />

<div class="app-shell">
  <header class="topbar">
    <div class="brand"><div class="brand-mark"><Layers3 size={17}/></div><span>Screendeck</span><span class="version">M4</span></div>
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
    <div class="section-title"><span>Project</span><button title="Add profile" on:click={addProfile}><Plus size={15}/></button></div>
    <div class="tree">
      {#each project.profiles as item, pi}
        <button class:active={pi === profileIndex} class="tree-profile" title="Right-click to rename or delete" on:click={() => selectProfile(pi)} on:contextmenu={(event) => profileMenu(event, pi)}><Layers3 size={15}/><span>{item.name}</span><span class="count">{item.pages.length}</span></button>
        {#if pi === profileIndex}
          <div class="pages">
            {#each item.pages as itemPage, pgi}
              <button class:active={pgi === pageIndex} on:click={() => selectPage(pgi)}><span class="page-dot"></span>{itemPage.name}</button>
            {/each}
            <button class="add-row" on:click={addPage}><CirclePlus size={14}/> Add page</button>
          </div>
        {/if}
      {/each}
    </div>

    <div class="section-title macro-heading"><span>Screensaver</span></div>
    <div class="screensaver-setting">
      <label for="screensaver-delay">Start after</label>
      <div><input id="screensaver-delay" type="number" min="5" max="3600" step="5" bind:value={project.screensaverTimeoutSeconds} on:change={() => changed("Screensaver delay changed")}/><span>seconds</span></div>
      <p>Choose between 5 seconds and 60 minutes.</p>
    </div>
  </aside>

  <main class="workspace">
    <div class="workspace-head">
      <div><span class="eyebrow">{profile.name}</span><h1>{page.name}</h1></div>
      <div class="pager"><button on:click={() => pageIndex = Math.max(0, pageIndex - 1)} disabled={pageIndex === 0}><ChevronLeft size={16}/></button><span>{pageIndex + 1} / {profile.pages.length}</span><button on:click={() => pageIndex = Math.min(profile.pages.length - 1, pageIndex + 1)} disabled={pageIndex === profile.pages.length - 1}><ChevronRight size={16}/></button></div>
    </div>
    <section class="device-preview" aria-label="1280 by 720 Screendeck preview">
      <div class="grid">
        {#each page.buttons as tile, index}
          {@const icon = iconFor(tile.iconId)}
          <button class:selected={index === selectedButton} class="tile" aria-label={`Button ${index + 1}`} on:click={() => selectedButton = index} on:dragover={(e) => e.preventDefault()} on:drop={dropIcon}>
            {#if icon}<img class:contained={tile.imageFit === "contain"} src={icon} alt="" draggable="false"/>
            {:else if tile.action === "page_previous"}<ChevronLeft size={24}/>
            {:else if tile.action === "page_next"}<ChevronRight size={24}/>
            {:else if tile.action === "profile_next"}<Layers3 size={22}/>
            {:else if tile.action === "macro"}<Keyboard size={21}/>
            {:else}<span class="empty-index">{index + 1}</span>{/if}
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
      {#if button.iconId}
        <div class="artwork-controls">
          <label>Artwork display<select value={button.imageFit ?? "cover"} on:change={(e) => updateButton("imageFit", e.currentTarget.value)}><option value="cover">Fill entire key</option><option value="contain">Fit inside key</option></select></label>
          <p>Fill entire key crops non-square artwork at the edges. Fit inside preserves the complete image.</p>
          <button class="remove-artwork" on:click={clearIcon}><Trash2 size={14}/> Remove artwork</button>
        </div>
      {/if}

      {#if button.action === "macro" && macro}
        <div class="rule"></div>
        <div class="macro-title"><div><span>Macro sequence</span><strong>{macro.name}</strong></div></div>
        <div class="steps">
          {#each macro.steps as step, index}
            <div class="step">
              <GripVertical class="grip" size={14}/>
              <span class="step-number">{index + 1}</span>
              <select value={step.kind} on:change={(e) => updateStep(index, "kind", e.currentTarget.value)}><option value="key_press">Key press</option><option value="key_down">Key down</option><option value="key_up">Key up</option><option value="delay">Delay</option><option value="consumer">Media key</option></select>
              {#if step.kind === "delay"}<div class="duration"><input type="number" min="1" max="60000" value={step.durationMs ?? 25} on:change={(e) => updateStep(index, "durationMs", Number(e.currentTarget.value))}/><span>ms</span></div>
              {:else}<select class="key-picker" value={step.key ?? (step.kind === "consumer" ? "PLAY_PAUSE" : "F13")} on:change={(e) => updateStep(index, "key", e.currentTarget.value)}>{#each step.kind === "consumer" ? CONSUMER_KEYS : KEYBOARD_KEYS as key}<option value={key}>{key.replaceAll("_", " ")}</option>{/each}</select>{/if}
              <button class="step-delete" title="Remove step" on:click={() => removeStep(index)}><X size={13}/></button>
              {#if step.kind === "key_press"}<div class="step-modifiers"><span>Hold</span>{#each [["CTRL","Ctrl"],["SHIFT","Shift"],["ALT","Alt"],["GUI","Win"]] as modifier}<button class:active={step.modifiers?.includes(modifier[0])} on:click={() => toggleModifier(step, modifier[0])}>{modifier[1]}</button>{/each}<label><input type="number" min="1" max="1000" value={step.durationMs ?? 25} on:change={(e) => updateStep(index, "durationMs", Number(e.currentTarget.value))}/> ms</label></div>{/if}
            </div>
          {/each}
          <button class="add-step" on:click={addStep}><Plus size={14}/> Add step</button>
        </div>
      {/if}
    </div>
  </aside>

  <footer class="statusbar">
    <button class:connected={device.connected} class="device-pill" on:click={refreshDevice}><span class="status-dot"></span><Usb size={14}/>{device.connected ? `Screendeck · generation ${device.generation}` : "No device"}<RefreshCw size={12}/></button>
    <div class="status-message" class:error={notice.includes("failed") || notice.includes("Resolve")}><span>{notice}</span></div>
    <div class="build-stats" title={deviceDiff}><span>{summary.bundleBytes.toLocaleString()} bytes</span><span>{deviceDiff}</span><Cpu size={13}/><span>{navigator.userAgent.includes("ARM64") ? "ARM64" : "x64"}</span></div>
  </footer>
</div>
