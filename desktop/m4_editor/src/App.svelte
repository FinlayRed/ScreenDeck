<script lang="ts">
  import { open, save } from "@tauri-apps/plugin-dialog";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import { onDestroy, tick } from "svelte";
  import { flip } from "svelte/animate";
  import Archive from "@lucide/svelte/icons/archive";
  import Check from "@lucide/svelte/icons/check";
  import ChevronLeft from "@lucide/svelte/icons/chevron-left";
  import ChevronRight from "@lucide/svelte/icons/chevron-right";
  import CirclePlus from "@lucide/svelte/icons/circle-plus";
  import Download from "@lucide/svelte/icons/download";
  import FilePlus2 from "@lucide/svelte/icons/file-plus-2";
  import FolderOpen from "@lucide/svelte/icons/folder-open";
  import ArrowDown from "@lucide/svelte/icons/arrow-down";
  import ArrowUp from "@lucide/svelte/icons/arrow-up";
  import Clipboard from "@lucide/svelte/icons/clipboard";
  import Copy from "@lucide/svelte/icons/copy";
  import MoreHorizontal from "@lucide/svelte/icons/more-horizontal";
  import Redo2 from "@lucide/svelte/icons/redo-2";
  import Undo2 from "@lucide/svelte/icons/undo-2";
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
  import { CONSUMER_KEYS, KEYBOARD_KEYS, moveButton, starterProject } from "./lib/model";
  import type { ActionKind, Asset, Button, Macro, MacroStep, Project, RadialSize } from "./lib/model";
  import { createHistory, recordHistory, redoHistory, undoHistory } from "./lib/history";
  import { radialDirection, radialGridOffset } from "./lib/radial";

  let project: Project = starterProject();
  let profileIndex = 0;
  let pageIndex = 0;
  let selectedButton = 0;
  let projectPath = "";
  let dirty = false;
  let busy = false;
  type NoticeKind = "info" | "success" | "warning" | "error" | "progress";
  type AppNotice = { kind: NoticeKind; message: string; detail?: string };
  let notice: AppNotice = { kind: "info", message: "Ready" };
  let device: DeviceStatus = { connected: false, generation: 0, capabilities: 0, detail: "Checking for Screendeck…" };
  let summary: CompileSummary = { bundleBytes: 0, payloadCrc32: 0, fingerprint: "", issues: [] };
  let lastSyncedFingerprint = "";
  let workspaceReady = false;
  let autosaveTimer: ReturnType<typeof setTimeout> | undefined;
  let lastWorkspaceAssets: Asset[] | undefined;
  let validationTimer: ReturnType<typeof setTimeout> | undefined;
  let projectRevision = 0;
  let validatedRevision = -1;
  let selectedRadialItem = 0;
  type ContextTarget = { kind: "profile"; profileIndex: number } | { kind: "page"; profileIndex: number; pageIndex: number };
  let contextMenu: { x: number; y: number; target: ContextTarget } | null = null;
  let buttonContextMenu: { x: number; y: number; index: number } | null = null;
  let contextMenuElement: HTMLDivElement | null = null;
  let contextMenuInvoker: HTMLElement | null = null;
  let renameDialog: { target: ContextTarget; value: string } | null = null;
  let renameInput: HTMLInputElement | null = null;
  let deleteDialog: { target: ContextTarget; name: string } | null = null;
  let assetDeleteDialog: { id: string; name: string } | null = null;
  let dirtyDialog: { action: string; run: () => Promise<void> | void } | null = null;
  let dialogElement: HTMLElement | null = null;
  let dialogInvoker: HTMLElement | null = null;
  let deleteCancel: HTMLButtonElement | null = null;
  let dirtyCancel: HTMLButtonElement | null = null;
  let history = createHistory(project);
  let historyApplying = false;
  let gridActiveIndex = 0;
  let keyboardMoveSource: number | null = null;
  let buttonClipboard: { button: Button; macros: Macro[] } | null = null;
  let devicePoll: ReturnType<typeof setInterval> | undefined;
  let removeCloseListener: (() => void) | undefined;
  let importing = false;
  let draggedButtonIndex: number | null = null;
  let dragOverButtonIndex: number | null = null;
  let buttonPointerDrag: {
    sourceIndex: number;
    pointerId: number;
    startX: number;
    startY: number;
    sourceElement: HTMLButtonElement;
  } | null = null;
  let suppressButtonClick = false;

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
  $: modalOpen = Boolean(renameDialog || deleteDialog || assetDeleteDialog || dirtyDialog);
  $: blockingIssues = summary.issues.filter((issue) => issue.severity === "error");

  function setNotice(kind: NoticeKind, message: string, detail?: string) {
    notice = { kind, message, detail };
  }

  function changed(message = "Unsaved changes") {
    project = { ...project };
    if (!historyApplying) history = recordHistory(history, project);
    summary = { ...summary, payloadCrc32: 0, fingerprint: "" };
    projectRevision += 1;
    dirty = true;
    setNotice("info", message);
  }

  function resetHistory() { history = createHistory(project); }

  function applyHistory(direction: "undo" | "redo") {
    const next = direction === "undo" ? undoHistory(history) : redoHistory(history);
    if (!next) return;
    history = next;
    historyApplying = true;
    project = structuredClone(next.current);
    profileIndex = Math.min(profileIndex, project.profiles.length - 1);
    pageIndex = Math.min(pageIndex, project.profiles[profileIndex].pages.length - 1);
    selectedButton = Math.min(selectedButton, project.profiles[profileIndex].pages[pageIndex].buttons.length - 1);
    historyApplying = false;
    summary = { ...summary, payloadCrc32: 0, fingerprint: "" };
    projectRevision += 1;
    dirty = true;
    setNotice("info", direction === "undo" ? "Undid last change" : "Redid change");
  }

  function rememberInvoker() {
    dialogInvoker = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  }

  async function closeModal(kind: "rename" | "delete" | "asset" | "dirty") {
    if (kind === "rename") renameDialog = null;
    if (kind === "delete") deleteDialog = null;
    if (kind === "asset") assetDeleteDialog = null;
    if (kind === "dirty") dirtyDialog = null;
    await tick();
    dialogInvoker?.focus();
    dialogInvoker = null;
  }

  function trapDialogFocus(event: KeyboardEvent) {
    if (event.key !== "Tab" || !dialogElement) return;
    const controls = [...dialogElement.querySelectorAll<HTMLElement>("button:not(:disabled), input:not(:disabled), select:not(:disabled), [tabindex]:not([tabindex='-1'])")];
    if (!controls.length) return;
    const first = controls[0];
    const last = controls.at(-1)!;
    if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
    else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
  }

  function guardReplacement(action: string, run: () => Promise<void> | void) {
    if (!dirty) { void run(); return; }
    rememberInvoker();
    dirtyDialog = { action, run };
    tick().then(() => dirtyCancel?.focus());
  }

  async function resolveDirty(choice: "save" | "discard") {
    if (!dirtyDialog) return;
    const pending = dirtyDialog;
    if (choice === "save" && !(await saveProject())) return;
    dirtyDialog = null;
    await pending.run();
  }

  function queueWorkspaceSave(revision: number) {
    clearTimeout(autosaveTimer);
    autosaveTimer = setTimeout(async () => {
      if (revision !== projectRevision) return;
      const preserveAssetData = lastWorkspaceAssets === project.assets;
      const workspaceProject = preserveAssetData
        ? {
            ...project,
            assets: project.assets.map((asset) => ({
              ...asset,
              dataUrl: "",
              sourceDataUrl: "",
              animationDataUrl: "",
            })),
          }
        : project;
      try {
        await saveWorkspace(workspaceProject, preserveAssetData);
        lastWorkspaceAssets = project.assets;
      }
      catch (error) { setNotice("error", "Automatic recovery save failed", String(error)); }
    }, 1500);
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
        resetHistory();
        setNotice("success", "Restored previous workspace");
      }
    } catch (error) { setNotice("error", "Could not restore workspace", String(error)); }
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

  async function replaceWithNewProject() {
    project = starterProject(); projectRevision += 1; profileIndex = 0; pageIndex = 0; selectedButton = 0; projectPath = ""; dirty = false; lastSyncedFingerprint = ""; resetHistory(); setNotice("success", "New project created");
  }

  function newProject() { guardReplacement("create a new project", replaceWithNewProject); }

  async function chooseAndOpenProject() {
    const path = await open({ title: "Open Screendeck project", filters: [{ name: "Screendeck project", extensions: ["sdeck"] }] });
    if (!path || Array.isArray(path)) return;
    busy = true;
    try { project = await openArchive(path); projectRevision += 1; projectPath = path; profileIndex = 0; pageIndex = 0; selectedButton = 0; dirty = false; lastSyncedFingerprint = ""; resetHistory(); setNotice("success", `Opened ${path.split(/[\\/]/).pop()}`); }
    catch (error) { setNotice("error", "Could not open project", String(error)); }
    finally { busy = false; }
  }

  function openProject() { guardReplacement("open another project", chooseAndOpenProject); }

  async function saveProject(saveAs = false): Promise<boolean> {
    let path = projectPath;
    if (!path || saveAs) path = await save({ title: "Save Screendeck project", defaultPath: `${project.name}.sdeck`, filters: [{ name: "Screendeck project", extensions: ["sdeck"] }] }) ?? "";
    if (!path) return false;
    busy = true;
    try { await saveArchive(path, project); projectPath = path; dirty = false; setNotice("success", "Project saved"); return true; }
    catch (error) { setNotice("error", "Save failed", String(error)); return false; }
    finally { busy = false; }
  }

  async function backup() {
    const path = await save({ title: "Export compiled backup", defaultPath: `${project.name}.sdb`, filters: [{ name: "Screendeck bundle", extensions: ["sdb"] }] });
    if (!path) return;
    try { await backupBundle(path, project); setNotice("success", "Compiled backup exported"); }
    catch (error) { setNotice("error", "Backup failed", String(error)); }
  }

  async function sync() {
    const errors = validatedRevision === projectRevision
      ? summary.issues.filter((issue) => issue.severity === "error")
      : [];
    if (errors.length) { setNotice("error", "Cannot sync until validation issues are fixed", errors.map((issue) => `${issue.path} — ${issue.message}`).join("\n")); return; }
    busy = true;
    setNotice("progress", "Syncing project to device…");
    try {
      const result = await syncProject(project);
      lastSyncedFingerprint = result.fingerprint;
      summary = { ...summary, fingerprint: result.fingerprint };
      setNotice("success", `Synced ${result.bytesSent.toLocaleString()} bytes · generation ${result.generation}${result.resumedAt ? ` · resumed at ${result.resumedAt}` : ""}`);
      await refreshDevice();
    } catch (error) { setNotice("error", "Sync failed", String(error)); }
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
    setNotice("progress", `Converting and uploading ${path.split(/[\\/]/).pop()}…`);
    try {
      const result = await uploadScreensaverToDevice(path);
      setNotice("success", `Screensaver uploaded · ${result.bytesSent.toLocaleString()} bytes${result.resumedAt ? ` · resumed at ${result.resumedAt}` : ""}`);
    } catch (error) { setNotice("error", "Screensaver upload failed", String(error)); }
    finally { busy = false; }
  }

  async function replaceFromDevice() {
    busy = true; setNotice("progress", "Importing project from device…");
    try {
      project = await syncFromDevice();
      projectRevision += 1;
      profileIndex = 0; pageIndex = 0; selectedButton = 0;
      projectPath = ""; dirty = true; await refreshValidation(); lastSyncedFingerprint = summary.fingerprint;
      resetHistory();
      setNotice("success", `Imported ${project.profiles.length} profile${project.profiles.length === 1 ? "" : "s"}, ${project.assets.length} icon${project.assets.length === 1 ? "" : "s"}, and ${project.macros.length} macro${project.macros.length === 1 ? "" : "s"} from device`);
    } catch (error) { setNotice("error", "Import from device failed", String(error)); }
    finally { busy = false; }
  }

  function importFromDevice() { guardReplacement("replace this project from the device", replaceFromDevice); }

  async function testScreensaver() {
    busy = true;
    setNotice("progress", "Starting screensaver test…");
    try {
      await testScreensaverOnDevice();
      setNotice("success", "Screensaver test started · touch the device to return");
    } catch (error) { setNotice("error", "Screensaver test failed", String(error)); }
    finally { busy = false; }
  }

  function addPage(targetProfileIndex = profileIndex) {
    const targetProfile = project.profiles[targetProfileIndex];
    targetProfile.pages.push({ id: crypto.randomUUID(), name: `Page ${targetProfile.pages.length + 1}`, buttons: Array.from({ length: 32 }, () => ({ action: "none" })) });
    profileIndex = targetProfileIndex; pageIndex = targetProfile.pages.length - 1; selectedButton = 0; changed("Page added");
  }

  function addProfile() {
    project.profiles.push({ id: crypto.randomUUID(), name: `Profile ${project.profiles.length + 1}`, pages: [{ id: crypto.randomUUID(), name: "Main", buttons: Array.from({ length: 32 }, () => ({ action: "none" })) }] });
    profileIndex = project.profiles.length - 1; pageIndex = 0; changed("Profile added");
  }

  function selectProfile(index: number) { profileIndex = index; pageIndex = 0; selectedButton = 0; }
  function selectPage(index: number) { pageIndex = index; selectedButton = 0; }

  function targetName(target: ContextTarget) {
    return target.kind === "profile"
      ? project.profiles[target.profileIndex]?.name ?? "Profile"
      : project.profiles[target.profileIndex]?.pages[target.pageIndex]?.name ?? "Page";
  }

  function canDeleteTarget(target: ContextTarget) {
    return target.kind === "profile"
      ? project.profiles.length > 1
      : project.profiles[target.profileIndex].pages.length > 1;
  }

  async function openContextMenu(event: MouseEvent, target: ContextTarget) {
    event.preventDefault();
    event.stopPropagation?.();
    buttonContextMenu = null;
    if (event.currentTarget instanceof HTMLElement) contextMenuInvoker = event.currentTarget;
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

  function closeContextMenu(restoreFocus = false) {
    contextMenu = null;
    buttonContextMenu = null;
    if (restoreFocus) tick().then(() => contextMenuInvoker?.focus());
  }

  function contextMenuKeydown(event: KeyboardEvent) {
    if (!contextMenuElement || !contextMenu) return;
    if (event.key === "Escape") { event.preventDefault(); closeContextMenu(true); return; }
    if (event.key === "F2") { event.preventDefault(); startRename(contextMenu.target); return; }
    if (event.key === "Delete") { event.preventDefault(); requestDelete(contextMenu.target); return; }
    if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const items = [...contextMenuElement.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
    const current = items.indexOf(document.activeElement as HTMLButtonElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1 : event.key === "ArrowDown" ? (current + 1) % items.length : (current - 1 + items.length) % items.length;
    items[next]?.focus();
  }

  function createFromMenu(kind: "profile" | "page", target: ContextTarget) {
    closeContextMenu();
    if (kind === "profile") addProfile();
    else addPage(target.profileIndex);
  }

  function rowKeydown(event: KeyboardEvent, target: ContextTarget) {
    if (event.key === "F2") { event.preventDefault(); void startRename(target); }
    else if (event.key === "Delete") { event.preventDefault(); requestDelete(target); }
    else if (event.key === "ContextMenu" || (event.shiftKey && event.key === "F10")) {
      event.preventDefault();
      const rect = (event.currentTarget as HTMLElement).getBoundingClientRect();
      void openContextMenu({ preventDefault() {}, clientX: rect.left + 12, clientY: rect.bottom } as MouseEvent, target);
    }
  }

  function openRowActions(event: MouseEvent, target: ContextTarget) {
    event.stopPropagation();
    contextMenuInvoker = event.currentTarget as HTMLElement;
    const rect = (event.currentTarget as HTMLElement).getBoundingClientRect();
    void openContextMenu({ preventDefault() {}, clientX: rect.right, clientY: rect.bottom } as MouseEvent, target);
  }

  async function openButtonContextMenu(event: MouseEvent, index: number) {
    if (!isButtonConfigured(page.buttons[index])) return;
    event.preventDefault();
    event.stopPropagation();
    contextMenu = null;
    contextMenuInvoker = event.currentTarget as HTMLElement;
    selectedButton = index;
    gridActiveIndex = index;
    buttonContextMenu = {
      x: Math.max(8, Math.min(event.clientX, window.innerWidth - 220)),
      y: Math.max(8, Math.min(event.clientY, window.innerHeight - 132)),
      index,
    };
    await tick();
    contextMenuElement?.querySelector<HTMLButtonElement>("button:not(:disabled)")?.focus();
  }

  function buttonContextMenuKeydown(event: KeyboardEvent) {
    if (!contextMenuElement || !buttonContextMenu) return;
    if (event.key === "Escape") { event.preventDefault(); closeContextMenu(true); return; }
    if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const items = [...contextMenuElement.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
    const current = items.indexOf(document.activeElement as HTMLButtonElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1 : event.key === "ArrowDown" ? (current + 1) % items.length : (current - 1 + items.length) % items.length;
    items[next]?.focus();
  }

  function duplicateMacroFromMenu() {
    if (!buttonContextMenu) return;
    selectedButton = buttonContextMenu.index;
    closeContextMenu();
    duplicateButton();
  }

  function deleteMacroFromMenu() {
    if (!buttonContextMenu) return;
    const index = buttonContextMenu.index;
    selectedButton = index;
    closeContextMenu();
    clearButton();
  }

  async function startRename(target: ContextTarget) {
    dialogInvoker = contextMenuInvoker ?? (document.activeElement instanceof HTMLElement ? document.activeElement : null);
    renameDialog = { target, value: targetName(target) };
    closeContextMenu();
    await tick();
    renameInput?.focus();
    renameInput?.select();
  }

  function saveRename() {
    if (!renameDialog) return;
    const name = renameDialog.value.trim();
    if (!name) { setNotice("error", `${renameDialog.target.kind === "profile" ? "Profile" : "Page"} name cannot be empty`); renameInput?.focus(); return; }
    if (renameDialog.target.kind === "profile") project.profiles[renameDialog.target.profileIndex].name = name;
    else project.profiles[renameDialog.target.profileIndex].pages[renameDialog.target.pageIndex].name = name;
    const kind = renameDialog.target.kind;
    renameDialog = null;
    changed(`${kind === "profile" ? "Profile" : "Page"} renamed`);
    void closeModal("rename");
  }

  function requestDelete(target: ContextTarget) {
    dialogInvoker = contextMenuInvoker ?? (document.activeElement instanceof HTMLElement ? document.activeElement : null);
    closeContextMenu();
    if (!canDeleteTarget(target)) {
      setNotice("warning", target.kind === "profile" ? "At least one profile is required" : "A profile must keep at least one page");
      return;
    }
    deleteDialog = { target, name: targetName(target) };
    tick().then(() => deleteCancel?.focus());
  }

  function macrosUsedByPages(pages: typeof profile.pages) {
    return pages.flatMap((itemPage) => itemPage.buttons.flatMap((itemButton) => [itemButton.macroId, ...(itemButton.radial?.items.map((item) => item.macroId) ?? [])]).filter((id): id is string => Boolean(id)));
  }

  function confirmDelete() {
    if (!deleteDialog) return;
    const target = deleteDialog.target;
    if (target.kind === "profile") {
      const removed = project.profiles[target.profileIndex];
      const removedMacroIds = macrosUsedByPages(removed.pages);
      project.profiles.splice(target.profileIndex, 1);
      for (const id of removedMacroIds) removeMacroIfUnreferenced(id);
      profileIndex = Math.min(target.profileIndex, project.profiles.length - 1);
      pageIndex = 0;
      deleteDialog = null; selectedButton = 0; changed("Profile deleted");
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
  function moveStep(targetMacro: Macro, index: number, direction: -1 | 1) {
    const next = index + direction;
    if (next < 0 || next >= targetMacro.steps.length) return;
    [targetMacro.steps[index], targetMacro.steps[next]] = [targetMacro.steps[next], targetMacro.steps[index]];
    changed(`Macro step moved ${direction < 0 ? "up" : "down"}`);
    tick().then(() => document.querySelector<HTMLButtonElement>(`[data-macro-id="${targetMacro.id}"][data-step-index="${next}"][data-step-move="${direction < 0 ? "up" : "down"}"]`)?.focus());
  }

  function toggleModifier(step: MacroStep, modifier: string) {
    step.modifiers ??= [];
    step.modifiers = step.modifiers.includes(modifier) ? step.modifiers.filter((item) => item !== modifier) : [...step.modifiers, modifier];
    changed("Key modifier changed");
  }


  function iconFor(assetId?: string) { return assetId ? assetsById.get(assetId)?.dataUrl : undefined; }

  async function importFiles(files: FileList | File[], targetButtonIndex = selectedButton) {
    if (importing) return;
    importing = true;
    setNotice("progress", `Importing ${files.length} media file${files.length === 1 ? "" : "s"}…`);
    let imported = 0;
    let skipped = 0;
    const failures: string[] = [];
    for (const file of Array.from(files)) {
      if (!file.type.startsWith("image/") && !file.type.startsWith("video/")) { skipped += 1; continue; }
      try {
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
          id: crypto.randomUUID(), name: file.name.replace(/\.[^.]+$/, "") + ".png",
          mediaType: animated ? "image/jpeg" : "image/png", dataUrl, sourceName: file.name,
          sourceMediaType: file.type || "application/octet-stream", sourceDataUrl,
          animationDataUrl, animationFps
        };
        project.assets = [...project.assets, asset];
        page.buttons[targetButtonIndex].iconId = asset.id;
        page.buttons[targetButtonIndex].imageFit = "cover";
        imported += 1;
      } catch (error) {
        failures.push(`${file.name}: ${error}`);
      }
    }
    importing = false;
    if (imported) {
      selectedButton = targetButtonIndex;
      changed(`${imported} imported${skipped ? `, ${skipped} skipped` : ""}${failures.length ? `, ${failures.length} failed` : ""}`);
      if (failures.length) setNotice("warning", `${imported} imported, ${failures.length} failed`, failures.join("\n"));
    } else if (failures.length) setNotice("error", "No media files could be imported", failures.join("\n"));
    else setNotice("warning", `No supported media found${skipped ? ` · ${skipped} skipped` : ""}`);
  }

  function isButtonConfigured(target: Button) {
    return target.action !== "none" || Boolean(target.iconId || target.macroId || target.radial);
  }

  function startButtonPointerDrag(event: PointerEvent, index: number) {
    if (!event.isPrimary || event.button !== 0 || !isButtonConfigured(page.buttons[index])) return;
    const sourceElement = event.currentTarget as HTMLButtonElement;
    buttonPointerDrag = {
      sourceIndex: index,
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      sourceElement,
    };
    selectedButton = index;
    sourceElement.setPointerCapture(event.pointerId);
  }

  function moveButtonPointerDrag(event: PointerEvent) {
    if (!buttonPointerDrag || event.pointerId !== buttonPointerDrag.pointerId) return;
    const distance = Math.hypot(
      event.clientX - buttonPointerDrag.startX,
      event.clientY - buttonPointerDrag.startY,
    );
    if (draggedButtonIndex === null && distance < 6) return;

    event.preventDefault();
    draggedButtonIndex = buttonPointerDrag.sourceIndex;
    suppressButtonClick = true;
    const target = document.elementFromPoint(event.clientX, event.clientY)?.closest<HTMLElement>("[data-button-index]");
    const targetIndex = Number(target?.dataset.buttonIndex);
    dragOverButtonIndex = Number.isInteger(targetIndex) && targetIndex >= 0 && targetIndex < page.buttons.length
      ? targetIndex
      : null;
  }

  function finishButtonPointerDrag(event: PointerEvent) {
    if (!buttonPointerDrag || event.pointerId !== buttonPointerDrag.pointerId) return;
    const { sourceIndex, sourceElement } = buttonPointerDrag;
    const targetIndex = dragOverButtonIndex;
    const wasDragging = draggedButtonIndex !== null;
    if (sourceElement.hasPointerCapture(event.pointerId)) sourceElement.releasePointerCapture(event.pointerId);
    buttonPointerDrag = null;

    if (wasDragging && targetIndex !== null) {
      const targetWasConfigured = isButtonConfigured(page.buttons[targetIndex]);
      if (moveButton(page.buttons, sourceIndex, targetIndex)) {
        selectedButton = targetIndex;
        selectedRadialItem = 0;
        changed(targetWasConfigured
          ? `Buttons ${sourceIndex + 1} and ${targetIndex + 1} swapped`
          : `Button moved to position ${targetIndex + 1}`);
      }
    }

    draggedButtonIndex = null;
    dragOverButtonIndex = null;
    if (wasDragging) setTimeout(() => suppressButtonClick = false, 0);
  }

  function cancelButtonPointerDrag(event: PointerEvent) {
    if (!buttonPointerDrag || event.pointerId !== buttonPointerDrag.pointerId) return;
    buttonPointerDrag = null;
    draggedButtonIndex = null;
    dragOverButtonIndex = null;
    suppressButtonClick = false;
  }

  function selectButtonFromClick(index: number) {
    if (suppressButtonClick) {
      suppressButtonClick = false;
      return;
    }
    selectedButton = index;
    gridActiveIndex = index;
  }

  function buttonAccessibleName(target: Button, index: number) {
    const action = target.action === "none" ? "Empty" : target.action === "macro" ? "Run macro" : target.action.replaceAll("_", " ");
    return `Button ${index + 1}, ${action}${target.radial ? ", radial menu configured" : ""}`;
  }

  function focusIssue(path: string) {
    const direct: Record<string, string> = {
      name: ".project-title input",
      screensaverTimeoutSeconds: "#screensaver-delay",
      brightnessPercent: "#brightness",
      orientation: "#orientation",
    };
    if (direct[path]) {
      document.querySelector<HTMLElement>(direct[path])?.focus();
      return;
    }
    const location = path.match(/profiles\[(\d+)\](?:\.pages\[(\d+)\])?(?:\.buttons\[(\d+)\])?/);
    if (location) {
      profileIndex = Math.min(Number(location[1]), project.profiles.length - 1);
      pageIndex = Math.min(Number(location[2] ?? 0), project.profiles[profileIndex].pages.length - 1);
      selectedButton = Math.min(Number(location[3] ?? 0), 31);
      gridActiveIndex = selectedButton;
      tick().then(() => document.querySelector<HTMLButtonElement>(`[data-button-index="${selectedButton}"]`)?.focus());
      return;
    }
    document.querySelector<HTMLElement>(path.startsWith("assets") ? ".assets-strip" : ".inspector")?.scrollIntoView({ block: "nearest" });
  }

  function gridKeydown(event: KeyboardEvent, index: number) {
    const columns = 8;
    let next = index;
    if (event.key === "ArrowLeft") next = Math.max(0, index - 1);
    else if (event.key === "ArrowRight") next = Math.min(31, index + 1);
    else if (event.key === "ArrowUp") next = Math.max(0, index - columns);
    else if (event.key === "ArrowDown") next = Math.min(31, index + columns);
    else if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      if (keyboardMoveSource === null) selectedButton = index;
      else {
        const source = keyboardMoveSource;
        const occupied = isButtonConfigured(page.buttons[index]);
        if (moveButton(page.buttons, source, index)) {
          selectedButton = index;
          changed(occupied ? `Buttons ${source + 1} and ${index + 1} swapped` : `Button moved to position ${index + 1}`);
        }
        keyboardMoveSource = null;
      }
      return;
    } else if (event.key.toLowerCase() === "m" && isButtonConfigured(page.buttons[index])) {
      event.preventDefault();
      keyboardMoveSource = keyboardMoveSource === index ? null : index;
      setNotice("info", keyboardMoveSource === null ? "Keyboard move cancelled" : `Moving button ${index + 1}: choose a destination with arrow keys, then press Enter`);
      return;
    } else return;
    event.preventDefault();
    gridActiveIndex = next;
    selectedButton = next;
    tick().then(() => document.querySelector<HTMLButtonElement>(`[data-button-index="${next}"]`)?.focus());
  }

  function copyButton(cut = false) {
    const source = page.buttons[selectedButton];
    const macroIds = [source.macroId, ...(source.radial?.items.map((item) => item.macroId) ?? [])].filter((id): id is string => Boolean(id));
    buttonClipboard = {
      button: structuredClone(source),
      macros: structuredClone(project.macros.filter((item) => macroIds.includes(item.id))),
    };
    if (cut) {
      page.buttons[selectedButton] = { action: "none" };
      for (const id of macroIds) removeMacroIfUnreferenced(id);
      changed(`Button ${selectedButton + 1} cut`);
    } else setNotice("success", `Button ${selectedButton + 1} copied`);
  }

  function pasteButton() {
    if (!buttonClipboard) return;
    const clone = structuredClone(buttonClipboard.button);
    const idMap = new Map<string, string>();
    for (const source of buttonClipboard.macros) {
      const id = crypto.randomUUID();
      idMap.set(source.id, id);
      project.macros.push({ ...structuredClone(source), id, name: `${source.name} copy` });
    }
    if (clone.macroId) clone.macroId = idMap.get(clone.macroId);
    for (const item of clone.radial?.items ?? []) if (item.macroId) item.macroId = idMap.get(item.macroId);
    page.buttons[selectedButton] = clone;
    changed(`Configuration pasted to button ${selectedButton + 1}`);
  }

  function clearButton() {
    const source = page.buttons[selectedButton];
    const macroIds = [source.macroId, ...(source.radial?.items.map((item) => item.macroId) ?? [])];
    page.buttons[selectedButton] = { action: "none" };
    for (const id of macroIds) removeMacroIfUnreferenced(id);
    changed(`Button ${selectedButton + 1} cleared`);
  }

  function duplicateButton() {
    const target = page.buttons.findIndex((item, index) => index !== selectedButton && !isButtonConfigured(item));
    if (target < 0) { setNotice("warning", "No empty button is available for duplication"); return; }
    copyButton();
    selectedButton = target;
    gridActiveIndex = target;
    pasteButton();
  }

  function dragOverButton(event: DragEvent, index: number) {
    const isFile = event.dataTransfer?.types.includes("Files");
    if (!isFile) return;
    event.preventDefault();
    if (event.dataTransfer) event.dataTransfer.dropEffect = "copy";
    dragOverButtonIndex = index;
  }

  async function dropOnButton(event: DragEvent, targetIndex: number) {
    event.preventDefault();
    event.stopPropagation();
    if (event.dataTransfer?.files.length) {
      await importFiles(event.dataTransfer.files, targetIndex);
      draggedButtonIndex = null;
      dragOverButtonIndex = null;
      return;
    }
    dragOverButtonIndex = null;
  }

  function leaveButtonGrid(event: DragEvent) {
    const relatedTarget = event.relatedTarget;
    if (!(relatedTarget instanceof Node) || !(event.currentTarget as HTMLElement).contains(relatedTarget)) {
      dragOverButtonIndex = null;
    }
  }

  function assignAsset(id: string) { page.buttons[selectedButton].iconId = id; page.buttons[selectedButton].imageFit ??= "cover"; changed("Icon assigned"); }
  function clearIcon() { page.buttons[selectedButton].iconId = undefined; page.buttons[selectedButton].imageFit = undefined; changed("Icon removed"); }
  function removeAsset(id: string) {
    const asset = project.assets.find((item) => item.id === id);
    if (!asset) return;
    rememberInvoker();
    assetDeleteDialog = { id, name: asset.name };
    tick().then(() => deleteCancel?.focus());
  }

  function deleteAsset(id: string) {
    for (const itemProfile of project.profiles) for (const itemPage of itemProfile.pages) for (const itemButton of itemPage.buttons) {
      if (itemButton.iconId === id) { itemButton.iconId = undefined; itemButton.imageFit = undefined; }
      for (const radialItem of itemButton.radial?.items ?? []) if (radialItem.iconId === id) radialItem.iconId = undefined;
    }
    project.assets = project.assets.filter((item) => item.id !== id);
    assetDeleteDialog = null;
    changed("Icon removed from library");
  }

  function confirmRemoveAsset() {
    if (!assetDeleteDialog) return;
    deleteAsset(assetDeleteDialog.id);
  }

  function assetContextMenu(event: MouseEvent, id: string) {
    event.preventDefault();
    event.stopPropagation();
    closeContextMenu();
    if (event.shiftKey) deleteAsset(id);
    else removeAsset(id);
  }

  function globalKeydown(event: KeyboardEvent) {
    if (event.key === "Escape") {
      closeContextMenu(true);
      if (renameDialog) void closeModal("rename");
      else if (deleteDialog) void closeModal("delete");
      else if (assetDeleteDialog) void closeModal("asset");
      else if (dirtyDialog) void closeModal("dirty");
      keyboardMoveSource = null;
    }
    if (modalOpen) return;
    const key = event.key.toLowerCase();
    if (event.ctrlKey && key === "s") { event.preventDefault(); void saveProject(event.shiftKey); }
    else if (event.ctrlKey && !event.shiftKey && key === "z") { event.preventDefault(); applyHistory("undo"); }
    else if ((event.ctrlKey && key === "y") || (event.ctrlKey && event.shiftKey && key === "z")) { event.preventDefault(); applyHistory("redo"); }
    else if (event.ctrlKey && key === "c" && document.activeElement?.closest(".grid")) { event.preventDefault(); copyButton(); }
    else if (event.ctrlKey && key === "x" && document.activeElement?.closest(".grid")) { event.preventDefault(); copyButton(true); }
    else if (event.ctrlKey && key === "v" && document.activeElement?.closest(".grid")) { event.preventDefault(); pasteButton(); }
    else if (event.ctrlKey && key === "d" && document.activeElement?.closest(".grid")) { event.preventDefault(); duplicateButton(); }
  }

  restoreWorkspace();
  refreshDevice();
  devicePoll = setInterval(() => { if (!busy) void refreshDevice(); }, 5000);
  if ("__TAURI_INTERNALS__" in window) {
    getCurrentWindow().onCloseRequested((event) => {
      if (!dirty) return;
      event.preventDefault();
      guardReplacement("close Screendeck", () => getCurrentWindow().destroy());
    }).then((unlisten) => removeCloseListener = unlisten);
  }
  onDestroy(() => {
    clearTimeout(autosaveTimer);
    clearTimeout(validationTimer);
    clearInterval(devicePoll);
    removeCloseListener?.();
  });
</script>

<svelte:window on:click={() => closeContextMenu()} on:blur={() => closeContextMenu()} on:contextmenu={(event) => { event.preventDefault(); closeContextMenu(); }} on:pointermove={moveButtonPointerDrag} on:pointerup={finishButtonPointerDrag} on:pointercancel={cancelButtonPointerDrag} on:keydown={globalKeydown} />

<div class="app-shell">
  <header class="topbar" inert={modalOpen}>
    <div class="brand"><div class="brand-mark"><Layers3 size={17}/></div><span>Screendeck</span></div>
    <div class="project-title"><input aria-label="Project name" bind:value={project.name} on:input={() => changed()} /></div>
    <nav class="toolbar" aria-label="Project actions">
      <button class="icon-button" aria-label="New project" title="New project" on:click={newProject}><FilePlus2 size={17}/></button>
      <button class="icon-button" aria-label="Open project" title="Open project" on:click={openProject}><FolderOpen size={17}/></button>
      <button class="icon-button" class:dirty-save={dirty} aria-label={dirty ? "Save unsaved changes" : "Project saved"} title={dirty ? "Save unsaved changes" : "Project saved"} on:click={() => saveProject()}>{#if dirty}<Save size={17}/>{:else}<Check size={17}/>{/if}</button>
      <button class="icon-button" aria-label="Undo" title="Undo (Ctrl+Z)" disabled={!history.undo.length} on:click={() => applyHistory("undo")}><Undo2 size={17}/></button>
      <button class="icon-button" aria-label="Redo" title="Redo (Ctrl+Y)" disabled={!history.redo.length} on:click={() => applyHistory("redo")}><Redo2 size={17}/></button>
      <button class="icon-button" title="Export compiled backup" on:click={backup}><Archive size={17}/></button>
      <div class="divider"></div>
      <button class="icon-button" title="Upload screensaver image or video" disabled={busy || !device.connected} on:click={uploadScreensaver}><MonitorUp size={17}/></button>
      <button class="icon-button" title="Test screensaver on device" disabled={busy || !device.connected} on:click={testScreensaver}><Play size={17}/></button>
      <button class="sync-button sync-from" title="Replace the editor project with the active device profile" disabled={busy || !device.connected} on:click={importFromDevice}><Download size={16}/>From device</button>
      <button class="sync-button" title={blockingIssues.length ? "Resolve validation issues before syncing" : "Sync project to device"} disabled={busy || !device.connected || blockingIssues.length > 0} on:click={sync}><Upload size={16}/>{busy ? "Working…" : "Sync to device"}</button>
    </nav>
  </header>

  <aside class="sidebar" inert={modalOpen}>
    <div class="section-title"><span>Profiles</span><button aria-label="Add profile" title="Add profile" on:click={addProfile}><Plus size={15}/></button></div>
    <div class="tree">
      {#each project.profiles as item, pi}
        <div class="tree-row">
          <button class:active={pi === profileIndex} class="tree-profile" aria-current={pi === profileIndex ? "true" : undefined} on:click={() => selectProfile(pi)} on:keydown={(event) => rowKeydown(event, { kind: "profile", profileIndex: pi })} on:contextmenu={(event) => openContextMenu(event, { kind: "profile", profileIndex: pi })}><Layers3 size={15}/><span>{item.name}</span></button>
          <button class="row-actions" aria-label={`Actions for profile ${item.name}`} on:click={(event) => openRowActions(event, { kind: "profile", profileIndex: pi })}><MoreHorizontal size={15}/></button>
        </div>
        {#if pi === profileIndex}
          <div class="pages">
            {#each item.pages as itemPage, pgi}
              <div class="tree-row">
                <button class:active={pgi === pageIndex} aria-current={pgi === pageIndex ? "page" : undefined} on:click={() => selectPage(pgi)} on:keydown={(event) => rowKeydown(event, { kind: "page", profileIndex: pi, pageIndex: pgi })} on:contextmenu={(event) => openContextMenu(event, { kind: "page", profileIndex: pi, pageIndex: pgi })}><span class="page-dot"></span><span>{itemPage.name}</span></button>
                <button class="row-actions" aria-label={`Actions for page ${itemPage.name}`} on:click={(event) => openRowActions(event, { kind: "page", profileIndex: pi, pageIndex: pgi })}><MoreHorizontal size={15}/></button>
              </div>
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
      aria-label={`${contextMenu.target.kind === "profile" ? "Profile" : "Page"} actions`}
      style={`left:${contextMenu.x}px;top:${contextMenu.y}px`}
      bind:this={contextMenuElement}
      on:click|stopPropagation
      on:contextmenu|preventDefault
      on:keydown={contextMenuKeydown}
    >
      <div class="context-menu-heading"><span>{contextMenu.target.kind}</span><strong>{targetName(contextMenu.target)}</strong></div>
      <div class="context-menu-group">
        <button role="menuitem" on:click={() => createFromMenu("profile", contextMenu!.target)}><span class="context-menu-icon"><Plus size={14}/></span><span>New profile</span></button>
        <button role="menuitem" on:click={() => createFromMenu("page", contextMenu!.target)}><span class="context-menu-icon"><CirclePlus size={14}/></span><span>New page</span></button>
      </div>
      <div class="context-menu-separator"></div>
      <div class="context-menu-group">
        <button role="menuitem" on:click={() => startRename(contextMenu!.target)}><span class="context-menu-icon"><Pencil size={14}/></span><span>Rename</span><kbd>F2</kbd></button>
        <button class="danger" role="menuitem" disabled={!canDeleteTarget(contextMenu.target)} on:click={() => requestDelete(contextMenu!.target)}><span class="context-menu-icon"><Trash2 size={14}/></span><span>Delete</span></button>
      </div>
    </div>
  {/if}

  {#if buttonContextMenu}
    <div
      class="context-menu button-context-menu"
      role="menu"
      tabindex="-1"
      aria-label={`Macro actions for button ${buttonContextMenu.index + 1}`}
      style={`left:${buttonContextMenu.x}px;top:${buttonContextMenu.y}px`}
      bind:this={contextMenuElement}
      on:click|stopPropagation
      on:contextmenu|preventDefault
      on:keydown={buttonContextMenuKeydown}
    >
      <div class="context-menu-heading"><span>Button {buttonContextMenu.index + 1}</span><strong>Macro actions</strong></div>
      <div class="context-menu-group">
        <button role="menuitem" on:click={duplicateMacroFromMenu}><span class="context-menu-icon"><Copy size={14}/></span><span>Duplicate macro</span></button>
        <button class="danger" role="menuitem" on:click={deleteMacroFromMenu}><span class="context-menu-icon"><Trash2 size={14}/></span><span>Delete macro</span></button>
      </div>
    </div>
  {/if}

  {#if renameDialog}
    <div class="dialog-backdrop" role="presentation">
      <div class="app-dialog" role="dialog" aria-modal="true" aria-labelledby="rename-title" aria-describedby="rename-description" tabindex="-1" bind:this={dialogElement} on:keydown={trapDialogFocus}>
        <form class="dialog-form" on:submit|preventDefault={saveRename}>
          <div class="dialog-icon"><Pencil size={17}/></div>
          <div class="dialog-copy"><span>{renameDialog.target.kind}</span><h2 id="rename-title">Rename {renameDialog.target.kind}</h2><p id="rename-description">Choose a clear name you’ll recognise in the sidebar.</p></div>
          <label for="rename-input">Name</label>
          <input id="rename-input" bind:this={renameInput} bind:value={renameDialog.value} maxlength="64" autocomplete="off" />
          <div class="dialog-actions"><button type="button" class="secondary" on:click={() => closeModal("rename")}>Cancel</button><button type="submit" class="primary">Save name</button></div>
        </form>
      </div>
    </div>
  {/if}

  {#if deleteDialog}
    <div class="dialog-backdrop" role="presentation">
      <div class="app-dialog delete-dialog" role="alertdialog" aria-modal="true" aria-labelledby="delete-title" aria-describedby="delete-description" tabindex="-1" bind:this={dialogElement} on:keydown={trapDialogFocus}>
        <div class="dialog-icon danger"><Trash2 size={17}/></div>
        <div class="dialog-copy"><span>{deleteDialog.target.kind}</span><h2 id="delete-title">Delete “{deleteDialog.name}”?</h2><p id="delete-description">{deleteDialog.target.kind === "profile" ? "Its pages and button setup will also be removed." : "Its button setup will be removed from this profile."} You can undo this action.</p></div>
        <div class="dialog-actions"><button bind:this={deleteCancel} type="button" class="secondary" on:click={() => closeModal("delete")}>Cancel</button><button type="button" class="delete-confirm" on:click={confirmDelete}>Delete {deleteDialog.target.kind}</button></div>
      </div>
    </div>
  {/if}

  {#if assetDeleteDialog}
    <div class="dialog-backdrop" role="presentation">
      <div class="app-dialog delete-dialog" role="alertdialog" aria-modal="true" aria-labelledby="asset-delete-title" aria-describedby="asset-delete-description" tabindex="-1" bind:this={dialogElement} on:keydown={trapDialogFocus}>
        <div class="dialog-icon danger"><Trash2 size={17}/></div>
        <div class="dialog-copy"><span>Icon library</span><h2 id="asset-delete-title">Remove “{assetDeleteDialog.name}”?</h2><p id="asset-delete-description">The icon will be removed from every button and radial action using it. You can undo this action.</p></div>
        <div class="dialog-actions"><button bind:this={deleteCancel} type="button" class="secondary" on:click={() => closeModal("asset")}>Cancel</button><button type="button" class="delete-confirm" on:click={confirmRemoveAsset}>Remove icon</button></div>
      </div>
    </div>
  {/if}

  {#if dirtyDialog}
    <div class="dialog-backdrop" role="presentation">
      <div class="app-dialog dirty-dialog" role="alertdialog" aria-modal="true" aria-labelledby="dirty-title" aria-describedby="dirty-description" tabindex="-1" bind:this={dialogElement} on:keydown={trapDialogFocus}>
        <div class="dialog-icon danger"><Save size={17}/></div>
        <div class="dialog-copy"><span>Unsaved project</span><h2 id="dirty-title">Save your changes?</h2><p id="dirty-description">To {dirtyDialog.action}, choose what should happen to the unsaved changes in “{project.name}”.</p></div>
        <div class="dialog-actions three-actions"><button bind:this={dirtyCancel} type="button" class="secondary" on:click={() => closeModal("dirty")}>Cancel</button><button type="button" class="secondary discard" on:click={() => resolveDirty("discard")}>Discard changes</button><button type="button" class="primary" on:click={() => resolveDirty("save")}>Save</button></div>
      </div>
    </div>
  {/if}

  <main class="workspace" inert={modalOpen}>
    <div class="workspace-head">
      <div><span class="eyebrow">{profile.name}</span><h1>{page.name}</h1></div>
      <div class="pager"><button aria-label="Previous page" on:click={() => pageIndex = Math.max(0, pageIndex - 1)} disabled={pageIndex === 0}><ChevronLeft size={16}/></button><span>{pageIndex + 1} / {profile.pages.length}</span><button aria-label="Next page" on:click={() => pageIndex = Math.min(profile.pages.length - 1, pageIndex + 1)} disabled={pageIndex === profile.pages.length - 1}><ChevronRight size={16}/></button></div>
    </div>
    {#if summary.issues.length}
      <section class="validation-summary" aria-labelledby="validation-title">
        <div><strong id="validation-title">{blockingIssues.length ? "Resolve before syncing" : "Project checks"}</strong><span>{summary.issues.length} issue{summary.issues.length === 1 ? "" : "s"}</span></div>
        <ul>{#each summary.issues as issue}<li class:error={issue.severity === "error"}><button on:click={() => focusIssue(issue.path)}><strong>{issue.path}</strong> — {issue.message}<span>Go to issue</span></button></li>{/each}</ul>
      </section>
    {/if}
    <section class="device-preview" aria-label="1280 by 720 Screendeck preview">
      <div class="grid" role="grid" tabindex="-1" aria-label="Button layout. Use arrow keys to navigate, Enter to select, or M then Enter to move or swap." aria-rowcount="4" aria-colcount="8" on:dragleave={leaveButtonGrid}>
        {#each page.buttons as tile, index}
          {@const icon = iconFor(tile.iconId)}
          <button
            class:selected={index === selectedButton}
            class:has-radial={Boolean(tile.radial)}
            class:configured={isButtonConfigured(tile)}
            class:dragging={index === draggedButtonIndex}
            class:drop-target={index === dragOverButtonIndex && index !== draggedButtonIndex}
            class:keyboard-moving={index === keyboardMoveSource}
            class="tile"
            role="gridcell"
            aria-label={buttonAccessibleName(tile, index)}
            aria-selected={index === selectedButton}
            aria-grabbed={index === draggedButtonIndex}
            tabindex={index === gridActiveIndex ? 0 : -1}
            data-button-index={index}
            on:click={() => selectButtonFromClick(index)}
            on:focus={() => gridActiveIndex = index}
            on:keydown={(event) => gridKeydown(event, index)}
            on:contextmenu={(event) => openButtonContextMenu(event, index)}
            on:pointerdown={(event) => startButtonPointerDrag(event, index)}
            on:dragover={(event) => dragOverButton(event, index)}
            on:drop={(event) => dropOnButton(event, index)}
          >
            {#if icon}<img class:contained={tile.imageFit === "contain"} src={icon} alt="" draggable="false"/>
            {:else if tile.action === "page_previous"}<ChevronLeft size={24}/>
            {:else if tile.action === "page_next"}<ChevronRight size={24}/>
            {:else if tile.action === "profile_next"}<Layers3 size={22}/>
            {:else if tile.action === "macro"}<Keyboard size={21}/>
            {:else}<span class="empty-index">{index + 1}</span>
            {/if}
          </button>
        {/each}
      </div>
    </section>

    <section class="assets-strip">
      <div class="assets-copy"><ImagePlus size={17}/><div><strong>Icon library</strong><span>Drop images onto any key</span></div></div>
      <div class="assets-list">
        <label class="asset-add" aria-label="Add icons to library" title="Add icons to library"><input aria-label="Add icons to library" type="file" accept="image/png,image/jpeg,image/webp,image/gif,video/mp4,video/webm,video/quicktime" multiple disabled={importing} on:change={(event) => { const files = event.currentTarget.files; if (files) importFiles(files); }}/><Plus size={18}/></label>
        {#each project.assets as asset}<div class="asset-item"><button class="asset" title={`${asset.name}${asset.animationFps ? " · 15 FPS" : ""}`} on:click={() => assignAsset(asset.id)} on:contextmenu={(event) => assetContextMenu(event, asset.id)}><img src={asset.dataUrl} alt={asset.name}/></button></div>{/each}
        {#if !project.assets.length}<span class="empty-assets">PNG, JPEG, WebP, GIF, MP4, WebM or MOV · originals stay in the project archive</span>{/if}
      </div>
    </section>
  </main>

  <aside class="inspector" inert={modalOpen}>
    <div class="inspector-tabs"><h2>Button {selectedButton + 1}</h2><span>Row {Math.floor(selectedButton / 8) + 1} · Column {(selectedButton % 8) + 1}</span></div>
    <div class="inspector-scroll">
      <label>Action<select value={button.action} on:change={(e) => updateButton("action", e.currentTarget.value)}><option value="none">None</option><option value="macro">Run macro</option><option value="page_next">Next page</option><option value="page_previous">Previous page</option><option value="profile_next">Next profile</option></select></label>

      <div class="button-clipboard" aria-label="Button configuration actions">
        <button on:click={() => copyButton()}><Copy size={14}/>Copy</button>
        <button disabled={!buttonClipboard} on:click={pasteButton}><Clipboard size={14}/>Paste</button>
        <button disabled={!isButtonConfigured(button)} on:click={duplicateButton}>Duplicate</button>
        <button disabled={!isButtonConfigured(button)} on:click={clearButton}>Clear</button>
      </div>

      {#if button.action === "macro" && macro}
        <div class="button-macro-editor">
          <div class="macro-title"><div><span>Macro sequence</span><strong>Button macro</strong></div></div>
          <div class="steps">
            {#each macro.steps as step, index (step)}
              <div class="step" animate:flip={{ duration: 110 }}>
                <span class="step-reorder"><button aria-label={`Move step ${index + 1} up`} disabled={index === 0} data-macro-id={macro.id} data-step-index={index} data-step-move="up" on:click={() => moveStep(macro, index, -1)}><ArrowUp size={12}/></button><button aria-label={`Move step ${index + 1} down`} disabled={index === macro.steps.length - 1} data-macro-id={macro.id} data-step-index={index} data-step-move="down" on:click={() => moveStep(macro, index, 1)}><ArrowDown size={12}/></button></span>
                <span class="step-number">{index + 1}</span>
                <select aria-label={`Step ${index + 1} type`} value={step.kind} on:change={(e) => updateStep(macro, index, "kind", e.currentTarget.value)}><option value="key_press">Key press</option><option value="key_down">Key down</option><option value="key_up">Key up</option><option value="delay">Delay</option><option value="consumer">Media key</option></select>
                {#if step.kind === "delay"}<div class="duration"><input aria-label={`Step ${index + 1} delay in milliseconds`} type="number" min="1" max="60000" value={step.durationMs ?? 25} on:change={(e) => updateStep(macro, index, "durationMs", Number(e.currentTarget.value))}/><span>ms</span></div>
                {:else}<select aria-label={`Step ${index + 1} key`} class="key-picker" value={step.key ?? (step.kind === "consumer" ? "PLAY_PAUSE" : "F13")} on:change={(e) => updateStep(macro, index, "key", e.currentTarget.value)}>{#each step.kind === "consumer" ? CONSUMER_KEYS : KEYBOARD_KEYS as key}<option value={key}>{key.replaceAll("_", " ")}</option>{/each}</select>{/if}
                <button class="step-delete" aria-label={`Remove step ${index + 1}`} title={`Remove step ${index + 1}`} on:click={() => removeStep(macro, index)}><X size={13}/></button>
                {#if step.kind === "key_press"}<div class="step-modifiers"><span>Hold</span>{#each [["CTRL","Ctrl"],["SHIFT","Shift"],["ALT","Alt"],["GUI","Win"]] as modifier}<button aria-pressed={step.modifiers?.includes(modifier[0]) ?? false} class:active={step.modifiers?.includes(modifier[0])} on:click={() => toggleModifier(step, modifier[0])}>{modifier[1]}</button>{/each}<label><input aria-label={`Step ${index + 1} duration in milliseconds`} type="number" min="1" max="1000" value={step.durationMs ?? 25} on:change={(e) => updateStep(macro, index, "durationMs", Number(e.currentTarget.value))}/> ms</label></div>{/if}
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
        <div class="radial-layout-options" role="group" aria-label="Radial layout">
          <button aria-pressed={!button.radial} class:active={!button.radial} on:click={removeRadial}>Off</button>
          {#each [4, 6, 8] as size}<button aria-pressed={button.radial?.size === size} class:active={button.radial?.size === size} on:click={() => setRadialSize(String(size))}>{size}-way</button>{/each}
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
              <button aria-pressed={selectedRadialItem === index} class:active={selectedRadialItem === index} class="radial-map-item" style={`left:${118 + offset.x * 72}px;top:${118 + offset.y * 72}px`} title={radialDirection(index, button.radial.size)} aria-label={`Edit ${radialDirection(index, button.radial.size)} radial action`} on:click={() => selectedRadialItem = index}>
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
                  {#each radialMacro.steps as step, index (step)}
                    <div class="step" animate:flip={{ duration: 110 }}>
                      <span class="step-reorder"><button aria-label={`Move radial step ${index + 1} up`} disabled={index === 0} data-macro-id={radialMacro.id} data-step-index={index} data-step-move="up" on:click={() => moveStep(radialMacro, index, -1)}><ArrowUp size={12}/></button><button aria-label={`Move radial step ${index + 1} down`} disabled={index === radialMacro.steps.length - 1} data-macro-id={radialMacro.id} data-step-index={index} data-step-move="down" on:click={() => moveStep(radialMacro, index, 1)}><ArrowDown size={12}/></button></span>
                      <span class="step-number">{index + 1}</span>
                      <select aria-label={`Radial step ${index + 1} type`} value={step.kind} on:change={(e) => updateStep(radialMacro, index, "kind", e.currentTarget.value)}><option value="key_press">Key press</option><option value="key_down">Key down</option><option value="key_up">Key up</option><option value="delay">Delay</option><option value="consumer">Media key</option></select>
                      {#if step.kind === "delay"}<div class="duration"><input aria-label={`Radial step ${index + 1} delay in milliseconds`} type="number" min="1" max="60000" value={step.durationMs ?? 25} on:change={(e) => updateStep(radialMacro, index, "durationMs", Number(e.currentTarget.value))}/><span>ms</span></div>
                      {:else}<select aria-label={`Radial step ${index + 1} key`} class="key-picker" value={step.key ?? (step.kind === "consumer" ? "PLAY_PAUSE" : "F13")} on:change={(e) => updateStep(radialMacro, index, "key", e.currentTarget.value)}>{#each step.kind === "consumer" ? CONSUMER_KEYS : KEYBOARD_KEYS as key}<option value={key}>{key.replaceAll("_", " ")}</option>{/each}</select>{/if}
                      <button class="step-delete" aria-label={`Remove radial step ${index + 1}`} title={`Remove radial step ${index + 1}`} on:click={() => removeStep(radialMacro, index)}><X size={13}/></button>
                      {#if step.kind === "key_press"}<div class="step-modifiers"><span>Hold</span>{#each [["CTRL","Ctrl"],["SHIFT","Shift"],["ALT","Alt"],["GUI","Win"]] as modifier}<button aria-pressed={step.modifiers?.includes(modifier[0]) ?? false} class:active={step.modifiers?.includes(modifier[0])} on:click={() => toggleModifier(step, modifier[0])}>{modifier[1]}</button>{/each}<label><input aria-label={`Radial step ${index + 1} duration in milliseconds`} type="number" min="1" max="1000" value={step.durationMs ?? 25} on:change={(e) => updateStep(radialMacro, index, "durationMs", Number(e.currentTarget.value))}/> ms</label></div>{/if}
                    </div>
                  {/each}
                  <button class="add-step" on:click={() => addStep(radialMacro)}><Plus size={14}/> Add step</button>
                </div>
              </div>
            {/if}
            <span class="field-label">Icon</span>
            <div class="radial-icon-grid">
              <button aria-pressed={!radialItem?.iconId} class:active={!radialItem?.iconId} title="Use the default action icon" on:click={() => { if (radialItem) radialItem.iconId = undefined; changed("Radial icon changed"); }}><Keyboard size={16}/></button>
              {#each project.assets as asset}<button aria-label={`Use ${asset.name} for radial action`} aria-pressed={radialItem?.iconId === asset.id} class:active={radialItem?.iconId === asset.id} title={asset.name} on:click={() => { if (radialItem) radialItem.iconId = asset.id; changed("Radial icon changed"); }}><img src={asset.dataUrl} alt=""/></button>{/each}
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

  <footer class="statusbar" inert={modalOpen}>
    <button aria-label={`Refresh device connection. ${device.connected ? "Screendeck connected" : "No device connected"}`} class:connected={device.connected} class="device-pill" on:click={refreshDevice}><span class="status-dot"></span><Usb size={14}/>{device.connected ? `Screendeck · generation ${device.generation}` : "No device"}<RefreshCw size={12}/></button>
    <div class="status-message" class:error={notice.kind === "error"} class:warning={notice.kind === "warning"} role={notice.kind === "error" ? "alert" : "status"} aria-live={notice.kind === "error" ? "assertive" : "polite"} title={notice.detail ?? notice.message}><span>{notice.message}</span>{#if notice.detail}<details><summary>Details</summary><pre>{notice.detail}</pre></details>{/if}</div>
    <div class="build-stats"><span>{summary.bundleBytes.toLocaleString()} bytes</span></div>
  </footer>
</div>
