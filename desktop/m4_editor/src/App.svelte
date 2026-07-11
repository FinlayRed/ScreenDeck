<script lang="ts">
  import { open, save } from "@tauri-apps/plugin-dialog";
  import {
    AlertCircle, Archive, ChevronLeft, ChevronRight, CirclePlus, Copy, Cpu,
    Download, FilePlus2, FolderOpen, GripVertical, ImagePlus, Keyboard,
    Layers3, MonitorUp, Plus, RefreshCw, Save, Trash2, Upload, Usb, X
  } from "@lucide/svelte";
  import { backupBundle, deviceStatus, openArchive, saveArchive, syncProject, uploadScreensaver as uploadScreensaverToDevice, validateProject } from "./lib/backend";
  import type { CompileSummary, DeviceStatus } from "./lib/backend";
  import { cloneProject, HID_KEYS, starterProject } from "./lib/model";
  import type { Asset, MacroStep, Project } from "./lib/model";

  let project: Project = starterProject();
  let profileIndex = 0;
  let pageIndex = 0;
  let selectedButton = 0;
  let selectedMacroId = project.macros[0].id;
  let projectPath = "";
  let dirty = false;
  let busy = false;
  let notice = "Ready";
  let device: DeviceStatus = { connected: false, generation: 0, capabilities: 0, detail: "Checking for Screendeck…" };
  let summary: CompileSummary = { bundleBytes: 0, payloadCrc32: 0, fingerprint: "", issues: [] };

  $: profile = project.profiles[profileIndex];
  $: page = profile.pages[pageIndex];
  $: button = page.buttons[selectedButton];
  $: macro = project.macros.find((item) => item.id === (button.macroId ?? selectedMacroId)) ?? project.macros[0];
  $: validationTick = JSON.stringify(project);
  $: if (validationTick) refreshValidation();

  function changed(message = "Unsaved changes") {
    project = cloneProject(project);
    dirty = true;
    notice = message;
  }

  async function refreshValidation() {
    summary = await validateProject(project);
  }

  async function refreshDevice() {
    try { device = await deviceStatus(); }
    catch (error) { device = { connected: false, generation: 0, capabilities: 0, detail: String(error) }; }
  }

  async function newProject() {
    project = starterProject(); profileIndex = 0; pageIndex = 0; selectedButton = 0; projectPath = ""; dirty = false; notice = "New project";
  }

  async function openProject() {
    const path = await open({ title: "Open Screendeck project", filters: [{ name: "Screendeck project", extensions: ["sdeck"] }] });
    if (!path || Array.isArray(path)) return;
    busy = true;
    try { project = await openArchive(path); projectPath = path; profileIndex = 0; pageIndex = 0; selectedButton = 0; dirty = false; notice = `Opened ${path.split(/[\\/]/).pop()}`; }
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
    if (summary.issues.some((issue) => issue.severity === "error")) { notice = "Resolve validation errors before syncing"; return; }
    busy = true;
    notice = "Syncing project to device…";
    try {
      const result = await syncProject(project);
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

  function addPage() {
    profile.pages.push({ id: crypto.randomUUID(), name: `Page ${profile.pages.length + 1}`, buttons: Array.from({ length: 32 }, () => ({ action: "none", accent: "#2a2c33" })) });
    pageIndex = profile.pages.length - 1; changed("Page added");
  }

  function addProfile() {
    project.profiles.push({ id: crypto.randomUUID(), name: `Profile ${project.profiles.length + 1}`, pages: [{ id: crypto.randomUUID(), name: "Main", buttons: Array.from({ length: 32 }, () => ({ action: "none", accent: "#2a2c33" })) }] });
    profileIndex = project.profiles.length - 1; pageIndex = 0; changed("Profile added");
  }

  function selectProfile(index: number) { profileIndex = index; pageIndex = 0; selectedButton = 0; }
  function selectPage(index: number) { pageIndex = index; selectedButton = 0; }

  function updateButton(field: string, value: string) {
    const target = page.buttons[selectedButton] as unknown as Record<string, string | undefined>;
    target[field] = value || undefined;
    if (field === "action" && value !== "macro") target.macroId = undefined;
    changed();
  }

  function updateStep(index: number, field: keyof MacroStep, value: string | number) {
    if (!macro) return;
    (macro.steps[index] as unknown as Record<string, string | number | undefined>)[field] = value;
    changed();
  }

  function addStep() { macro.steps.push({ kind: "delay", durationMs: 25 }); changed("Macro step added"); }
  function removeStep(index: number) { macro.steps.splice(index, 1); changed("Macro step removed"); }

  function addMacro() {
    const next = project.macros.length + 1;
    project.macros.push({ id: crypto.randomUUID(), name: `Macro ${next}`, steps: [{ kind: "key_down", key: "F13" }, { kind: "delay", durationMs: 25 }, { kind: "key_up", key: "F13" }] });
    selectedMacroId = project.macros.at(-1)!.id; changed("Macro added");
  }

  function iconFor(assetId?: string) { return project.assets.find((asset) => asset.id === assetId)?.dataUrl; }

  async function importFiles(files: FileList | File[]) {
    for (const file of Array.from(files)) {
      if (!file.type.startsWith("image/")) continue;
      const dataUrl = await new Promise<string>((resolve, reject) => {
        const reader = new FileReader(); reader.onload = () => resolve(String(reader.result)); reader.onerror = () => reject(reader.error); reader.readAsDataURL(file);
      });
      const asset: Asset = { id: crypto.randomUUID(), name: file.name, mediaType: file.type, dataUrl };
      project.assets.push(asset);
      page.buttons[selectedButton].iconId = asset.id;
      page.buttons[selectedButton].imageFit = "cover";
    }
    changed("Icon imported and assigned");
  }

  function dropIcon(event: DragEvent) { event.preventDefault(); if (event.dataTransfer?.files.length) importFiles(event.dataTransfer.files); }
  function assignAsset(id: string) { page.buttons[selectedButton].iconId = id; page.buttons[selectedButton].imageFit ??= "cover"; changed("Icon assigned"); }
  function clearIcon() { page.buttons[selectedButton].iconId = undefined; page.buttons[selectedButton].imageFit = undefined; changed("Icon removed"); }

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
      <button class="sync-button" disabled={busy || !device.connected} on:click={sync}><Upload size={16}/>{busy ? "Working…" : "Sync to device"}</button>
    </nav>
  </header>

  <aside class="sidebar">
    <div class="section-title"><span>Project</span><button title="Add profile" on:click={addProfile}><Plus size={15}/></button></div>
    <div class="tree">
      {#each project.profiles as item, pi}
        <button class:active={pi === profileIndex} class="tree-profile" on:click={() => selectProfile(pi)}><Layers3 size={15}/><span>{item.name}</span><span class="count">{item.pages.length}</span></button>
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

    <div class="section-title macro-heading"><span>Macros</span><button title="Add macro" on:click={addMacro}><Plus size={15}/></button></div>
    <div class="macro-list">
      {#each project.macros as item}
        <button class:active={item.id === macro?.id} on:click={() => selectedMacroId = item.id}><Keyboard size={14}/><span>{item.name}</span><span class="count">{item.steps.length}</span></button>
      {/each}
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
          <button class:selected={index === selectedButton} class="tile" style:--accent={tile.accent} aria-label={`Button ${index + 1}`} on:click={() => selectedButton = index} on:dragover={(e) => e.preventDefault()} on:drop={dropIcon}>
            {#if iconFor(tile.iconId)}<img class:contained={tile.imageFit === "contain"} src={iconFor(tile.iconId)} alt="" draggable="false"/>
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
      <label class="asset-add"><input type="file" accept="image/png,image/jpeg,image/webp" multiple on:change={(event) => { const files = event.currentTarget.files; if (files) importFiles(files); }}/><Plus size={18}/></label>
      {#each project.assets as asset}<button class="asset" title={asset.name} on:click={() => assignAsset(asset.id)}><img src={asset.dataUrl} alt={asset.name}/></button>{/each}
      {#if !project.assets.length}<span class="empty-assets">PNG, JPEG or WebP · originals stay in the project archive</span>{/if}
    </section>
  </main>

  <aside class="inspector">
    <div class="inspector-tabs"><button class="active">Button</button><button>Page</button></div>
    <div class="inspector-scroll">
      <div class="selection-label"><span>Selected key</span><strong>{selectedButton + 1}</strong></div>
      <label>Action<select value={button.action} on:change={(e) => updateButton("action", e.currentTarget.value)}><option value="none">None</option><option value="macro">Run macro</option><option value="page_next">Next page</option><option value="page_previous">Previous page</option><option value="profile_next">Next profile</option></select></label>
      {#if button.action === "macro"}
        <label>Macro<select value={button.macroId ?? macro.id} on:change={(e) => updateButton("macroId", e.currentTarget.value)}>{#each project.macros as item}<option value={item.id}>{item.name}</option>{/each}</select></label>
      {/if}
      <label>Accent<div class="color-control"><input type="color" value={button.accent} on:input={(e) => updateButton("accent", e.currentTarget.value)}/><input value={button.accent} on:change={(e) => updateButton("accent", e.currentTarget.value)}/></div></label>
      {#if button.iconId}
        <div class="artwork-controls">
          <label>Artwork display<select value={button.imageFit ?? "cover"} on:change={(e) => updateButton("imageFit", e.currentTarget.value)}><option value="cover">Fill entire key</option><option value="contain">Fit inside key</option></select></label>
          <p>Fill entire key crops non-square artwork at the edges. Fit inside preserves the complete image.</p>
          <button class="remove-artwork" on:click={clearIcon}><Trash2 size={14}/> Remove artwork</button>
        </div>
      {/if}

      {#if button.action === "macro" && macro}
        <div class="rule"></div>
        <div class="macro-title"><div><span>Macro sequence</span><strong>{macro.name}</strong></div><button title="Duplicate"><Copy size={14}/></button></div>
        <div class="steps">
          {#each macro.steps as step, index}
            <div class="step">
              <GripVertical class="grip" size={14}/>
              <span class="step-number">{index + 1}</span>
              <select value={step.kind} on:change={(e) => updateStep(index, "kind", e.currentTarget.value)}><option value="key_down">Key down</option><option value="key_up">Key up</option><option value="delay">Delay</option><option value="consumer">Media key</option></select>
              {#if step.kind === "delay"}<div class="duration"><input type="number" min="1" max="60000" value={step.durationMs ?? 25} on:change={(e) => updateStep(index, "durationMs", Number(e.currentTarget.value))}/><span>ms</span></div>
              {:else}<select class="key-picker" value={step.key ?? "F13"} on:change={(e) => updateStep(index, "key", e.currentTarget.value)}>{#each HID_KEYS as key}<option value={key}>{key.replaceAll("_", " ")}</option>{/each}</select>{/if}
              <button class="step-delete" title="Remove step" on:click={() => removeStep(index)}><X size={13}/></button>
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
    <div class="build-stats"><span>{summary.bundleBytes.toLocaleString()} bytes</span><span>{summary.issues.length ? `${summary.issues.length} issue${summary.issues.length === 1 ? "" : "s"}` : "Valid project"}</span><Cpu size={13}/><span>{navigator.userAgent.includes("ARM64") ? "ARM64" : "x64"}</span></div>
  </footer>
</div>
