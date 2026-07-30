# Screendeck Desktop Editor UI/UX Audit

**Audit date:** 30 July 2026  
**Audited version:** M6 editor, app version 0.6.0  
**Primary implementation:** Tauri 2, Svelte 5, Windows  
**Audit target:** `desktop/m4_editor`

## Executive summary

The editor has a coherent dark visual language, a recognisable hardware preview, and a generally sensible three-panel information architecture. Core button configuration, macro editing, radial editing, artwork assignment, and device settings are all present in one workspace. The current implementation is a strong functional prototype.

It is not yet ready for a broad production release without addressing data-safety and interaction-accessibility risks. The most important problems are:

1. Unsaved work can be replaced without warning by **New project** or **Open project**.
2. “Project” and “profile” are used for different concepts, while two different **New project** commands perform materially different actions.
3. There is no undo/redo for a mutation-heavy editor.
4. Dialog focus is not modal or reliably restored.
5. The 32-button canvas and drag-to-move workflow have no practical keyboard equivalent.
6. Several controls look interactive in ways they are not, notably the macro-step drag handles.
7. Project/page and artwork collections become clipped when they grow.

### Recommended release gate

Treat findings `UX-01` through `UX-09` as the pre-release usability and accessibility gate. Address `UX-10` through `UX-16` immediately afterward as the first product-quality pass.

## Scope and method

The audit included:

- Source review of `App.svelte`, all editor CSS, the frontend model/backend bridge, Tauri window configuration, and existing tests.
- Hands-on review of the running Vite representation of the Tauri UI.
- Default and minimum supported desktop window sizes:
  - 1440 × 900 default.
  - 1060 × 680 configured minimum.
  - 1280 × 720 intermediate test viewport.
- Primary button, empty button, macro, radial, sidebar, context-menu, rename-dialog, delete-dialog, disconnected-device, status, drag, and responsive states.
- DOM accessibility semantics, accessible names, focus ownership, target sizes, overflow, and representative colour contrast.

The browser console reported no warnings or errors during the exercised flows.

### Audit limitations

- No physical Screendeck was connected. Connected-device, transfer-progress, device-error, and post-sync states were reviewed from source rather than exercised end to end.
- Native file pickers and Windows system dialogs were not visually audited in the browser representation.
- This was an expert inspection, not moderated usability testing with representative end users.

## What is already working well

- The device preview is visually dominant and preserves the hardware’s 8 × 4 spatial model.
- The sidebar, canvas, inspector, and status bar have clear visual separation.
- Selection, radial assignment, drag source, and drag destination have consistent accent treatment.
- The inspector keeps advanced macro and radial editing near the selected button.
- Button moves preserve artwork, macros, and radial configuration and safely swap occupied positions.
- Project validation and device fingerprints are grounded in the same project model used for sync.
- Context menus support arrow-key movement once opened.
- Rename and delete operations have clearer custom UI than a raw browser prompt.
- Context-menu and dialog animation respects `prefers-reduced-motion`.
- The Tauri window declares a realistic minimum size and the core three-column layout remains structurally intact at that minimum.

## Priority summary

| ID | Priority | Area | Finding | Release impact |
|---|---:|---|---|---|
| UX-01 | P0 | Data safety | New/Open can replace dirty work without confirmation | Potential project loss |
| UX-02 | P0 | Information architecture | “Project” and “profile” labels conflict; two “New project” actions do different things | High risk of destructive misinterpretation |
| UX-03 | P1 | Recovery | No undo/redo for editor mutations | Accidental edits cannot be recovered |
| UX-04 | P1 | Dialogs/accessibility | Modal focus, trapping, and restoration are incomplete | Keyboard and assistive-tech blocker |
| UX-05 | P1 | Canvas/accessibility | Button grid and drag movement lack an efficient keyboard model | Core workflow inaccessible |
| UX-06 | P1 | Semantics | Unnamed/unlabelled controls and visual-only selected states | Screen-reader ambiguity |
| UX-07 | P1 | Macro editor | Drag handles imply step reordering, but no reorder interaction exists | Misleading core control |
| UX-08 | P1 | Scalability | Sidebar tree and icon library clip growing collections | Existing content becomes unreachable |
| UX-09 | P1 | Validation/status | Validation and operation feedback are hidden or non-semantic | Users cannot locate or recover from errors |
| UX-10 | P2 | Inspector context | Current button position is absent and macro names become stale after moves | Editing the wrong slot becomes more likely |
| UX-11 | P2 | Discoverability | Rename/delete are effectively right-click-only; shortcut labelling is misleading | Features are hard to find |
| UX-12 | P2 | Legibility/targets | Small text, undersized targets, and low-contrast secondary copy | Frequent precision and readability friction |
| UX-13 | P2 | Empty states | Empty buttons provide almost no guidance | Poor first-run comprehension |
| UX-14 | P2 | Media import | Import errors, unsupported drops, formats, and progress lack clear feedback | Silent or confusing failure |
| UX-15 | P2 | Device lifecycle | Device connection refresh is manual and inconspicuous | Newly connected hardware appears unavailable |
| UX-16 | P3 | Productivity | No copy/paste/duplicate workflow for repetitive layouts | Slow setup of 32-key pages |

## Detailed findings

### UX-01 — Protect dirty work before New/Open

**Priority:** P0 — critical  
**Area:** Data safety

**Evidence**

- `newProject()` immediately replaces the current project and clears `dirty`.
- `openProject()` replaces the project after file selection without checking `dirty`.
- `importFromDevice()` does check `dirty`, so destructive replacement behaviour is inconsistent.
- Automatic workspace saving does not replace the user’s portable `.sdeck` save and can itself overwrite the previous recovery workspace after replacement.

**User impact**

A user can interpret the top-left New icon as “start another project,” click it, and lose the only in-memory version of an unsaved layout. The terminology collision in `UX-02` increases this risk.

**Action**

Create one shared dirty-project guard used by New, Open, From device, window close, and any future project replacement. Offer:

- **Save**
- **Discard changes**
- **Cancel**

Do not use a native `confirm()` for this workflow; it needs three actions and consistent focus behaviour.

**Acceptance criteria**

- With `dirty === true`, New and Open cannot replace the project without an explicit decision.
- Save completes before the pending action continues.
- Cancelling leaves the complete project, selection, and path unchanged.
- A failed save leaves the replacement action cancelled and presents a persistent error.
- Closing the Tauri window follows the same policy or explicitly documents that the recovery workspace is authoritative.
- Automated tests cover clean, discard, cancel, successful-save, and failed-save paths.

**Implementation pointers**

- `src/App.svelte`: `newProject`, `openProject`, `saveProject`, `importFromDevice`, `dirty`.
- `src-tauri`: add a close-request hook if the chosen policy requires it.
- Reuse or extract the custom dialog implementation in `context-menu.css`.

---

### UX-02 — Separate “project” from “profile”

**Priority:** P0 — critical  
**Area:** Information architecture and destructive-action clarity

**Evidence**

- The document title is a Screendeck project (`My Screendeck`).
- The sidebar is labelled **Projects**, but it iterates `project.profiles`.
- The button action calls the same concept **Next profile**.
- `addProfile()` names items `Project N`.
- The top toolbar **New project** replaces the whole document.
- The page/profile context menu **New project** calls `addProfile()` and adds one profile to the existing document.
- The context menu shows `Ctrl+N` for the add-profile action, while no equivalent global shortcut exists.

**User impact**

The same label describes both a whole file and an item inside that file. Two commands with the same name have different scopes, one of which is destructive.

**Action**

Adopt the model’s existing hierarchy in the UI:

```text
Screendeck project
└── Profiles
    └── Pages
        └── Buttons
```

Rename the sidebar section and its commands to **Profiles**, **New profile**, **Rename profile**, and **Delete profile**. Reserve **New project** for creating a whole new document.

**Acceptance criteria**

- Every visible use of project/profile follows the hierarchy above.
- Top toolbar New creates a document; sidebar/context New profile creates a profile.
- Shortcut labels match globally implemented shortcuts.
- Destructive confirmation copy names the correct scope and affected children.
- README and user-facing device-import copy use the same terms.

**Implementation pointers**

- `src/App.svelte`: sidebar markup, `addProfile`, context menu, delete/rename copy.
- `src/lib/model.ts`: keep `Profile` terminology; no data migration should be needed.
- `README.md`: align user-facing wording.

---

### UX-03 — Add undo and redo

**Priority:** P1 — high  
**Area:** Error recovery

**Evidence**

- No undo/redo state, commands, toolbar actions, or shortcuts exist.
- Dragging/swapping buttons, changing actions, editing macro steps, changing radial size, deleting steps, removing artwork, and changing device settings mutate the project immediately.
- Only page/profile deletion and library-wide asset deletion request confirmation.

**User impact**

A single mis-click can destroy a configured sequence or swap content without a reliable recovery path. Confirmation dialogs cannot scale to every editor mutation.

**Action**

Introduce project-history snapshots or command-based history. Prefer command grouping so continuous range/input changes create one history item rather than dozens.

**Acceptance criteria**

- `Ctrl+Z` undoes and `Ctrl+Y`/`Ctrl+Shift+Z` redoes all meaningful editor mutations.
- Undo/redo toolbar controls expose disabled states and accessible names.
- Button moves/swaps, macro/radial edits, asset assignment/removal, settings, page/profile changes, and renames participate in history.
- Selection follows the restored object where practical.
- A new mutation after undo clears the redo branch.
- History has a documented memory bound.
- Save/autosave does not clear history unexpectedly.

**Implementation pointers**

- Extract mutation/history handling from `changed()` in `src/App.svelte`.
- Consider a dedicated `src/lib/history.ts` with unit tests.

---

### UX-04 — Make dialogs genuinely modal

**Priority:** P1 — high  
**Area:** Keyboard and assistive-technology accessibility

**Evidence**

- Rename is exposed as a named form, not `role="dialog"` with `aria-modal="true"`.
- Delete has `role="alertdialog"` and `aria-modal="true"`, but opens with focus on `<body>`.
- Closing either tested dialog left focus on `<body>` rather than returning it to the invoking page/profile control.
- There is no focus trap or background inerting.
- Escape closes dialogs globally, but focus restoration is not implemented.

**User impact**

Keyboard and screen-reader users can lose their place, navigate behind an open dialog, or receive no reliable announcement when a destructive confirmation appears.

**Action**

Build one reusable modal primitive and use it for rename, delete, dirty-project guarding, and destructive asset actions.

**Acceptance criteria**

- Rename uses `role="dialog"`; destructive confirmation uses `role="alertdialog"`.
- Both use `aria-modal="true"` and labelled title/description relationships.
- Initial focus:
  - Rename: selected name field.
  - Destructive dialog: Cancel by default.
- Tab and Shift+Tab remain inside the modal.
- Background content is inert while open.
- Escape closes when safe.
- Focus returns to the exact invoking control.
- Dialog behaviour has keyboard-focused component/integration tests.

**Implementation pointers**

- `src/App.svelte`: `renameDialog`, `deleteDialog`, `startRename`, `requestDelete`, global key handler.
- `src/context-menu.css`: dialog visuals can remain; extract semantics and focus lifecycle.

---

### UX-05 — Replace the 32-tab-stop canvas with a keyboard grid

**Priority:** P1 — high  
**Area:** Core editor accessibility

**Evidence**

- One tested configured screen exposed **84 focusable elements**.
- All 32 canvas buttons are individual tab stops.
- The grid has no arrow-key navigation or roving `tabindex`.
- Selected button state is visual only.
- Pointer drag is the only direct way to move/swap a configured button.
- Buttons are announced only as `Button 1`, `Button 2`, etc., without action/configuration state.

**User impact**

Keyboard users must tab through the entire 32-key grid to reach later controls and cannot perform the newly added move interaction without a pointer.

**Action**

Implement an accessible grid interaction:

- One tab stop enters the grid.
- Arrow keys move active position.
- Enter/Space selects.
- A documented keyboard command moves, cuts/pastes, or swaps the configured button.

**Acceptance criteria**

- The canvas is one stop in the normal tab order.
- Arrow navigation matches the visible 8 × 4 geometry and respects orientation.
- The active/selected position is announced.
- Accessible names include position and useful state, for example:
  - `Button 10, Run macro, radial menu configured`
  - `Button 11, Empty`
- Keyboard users can move/swap a configured button with the same data-preservation guarantees as pointer drag.
- Visible focus is distinct from selected state.
- Pointer dragging continues to work.

**Implementation pointers**

- `src/App.svelte`: button-grid markup and pointer drag functions.
- Consider a `src/lib/grid-navigation.ts` helper with geometry tests.

---

### UX-06 — Complete control names and selected-state semantics

**Priority:** P1 — high  
**Area:** Accessibility semantics

**Evidence from the tested configured state**

- Two pager icon buttons had no accessible names.
- The icon-library file input had no accessible name.
- Four macro-step selects were unlabelled:
  - Step kind and key for the button macro.
  - Step kind and key for the radial macro.
- No live regions existed.
- Visual active/selected state was not programmatically exposed for:
  - Current profile.
  - Current page.
  - Current canvas button.
  - Inspector tab.
  - Radial size.
  - Current radial position.
  - Modifier/default-icon toggle controls.

**User impact**

Assistive technology cannot reliably identify navigation controls, understand current selection, or associate fields with a particular macro step.

**Action**

Add names and correct control patterns rather than only ARIA attributes:

- Pager buttons: explicit previous/next page labels.
- Inspector: real tab/tablist semantics only if multiple tabs are planned; otherwise use a heading.
- Radial sizes: radio group or pressed-state segmented control.
- Modifier buttons: toggle buttons with `aria-pressed`.
- Selected profile/page: `aria-current` or an appropriate tree/listbox pattern.
- Step controls: unique visible or visually hidden labels containing the step number.
- File input/add button: `Add icons to library`.

**Acceptance criteria**

- Automated accessibility inspection reports no visible unnamed controls.
- Every macro-step field is labelled with sequence and step context.
- Every visual selected/toggled state is programmatically exposed.
- The profile/page structure uses one coherent semantic pattern.
- Screen-reader output can distinguish button selection from keyboard focus.

**Implementation pointers**

- `src/App.svelte`: pager, tree, grid, inspector tabs, macro steps, radial controls, file input.

---

### UX-07 — Make macro-step reordering real or remove the handles

**Priority:** P1 — high  
**Area:** Control affordance and core macro editing

**Evidence**

- Each macro step displays a `GripVertical` drag handle.
- No macro-step pointer, drag, keyboard reorder, or move-up/down handler exists.
- Users can only remove and recreate steps to change order.

**User impact**

The strongest visual affordance in each step promises drag reordering but does nothing. Sequence order is functionally critical to a macro.

**Action**

Implement reorder for both button macros and radial macros. If reorder is intentionally deferred, remove the grip and add explicit move-up/down controls.

**Acceptance criteria**

- Pointer users can reorder from the handle with a clear insertion target.
- Keyboard users can reorder via labelled move-up/down controls or a documented shortcut.
- Focus follows the moved step.
- First/last boundary controls disable correctly.
- Button and radial macro editors share one implementation.
- Unit tests confirm step objects and all properties remain intact.

**Implementation pointers**

- `src/App.svelte`: both duplicated `.steps` blocks, `updateStep`, `addStep`, `removeStep`.
- Prefer extracting a reusable `MacroSteps.svelte` component before adding behaviour.

---

### UX-08 — Make growing collections scrollable

**Priority:** P1 — high  
**Area:** Scalability and content reachability

**Evidence**

- `.assets-strip` uses `overflow:hidden`; once icons exceed the available width, later icons have no reachable scroll surface.
- The sidebar is `overflow:hidden`.
- `.tree` has no vertical scrolling/flex allocation, so enough profiles/pages push device settings or later tree content outside the visible sidebar.
- At the 1060 × 680 supported minimum, the sidebar already measured `scrollWidth: 196` against `clientWidth: 189`; the brightness percentage was visibly clipped.

**User impact**

The more a user configures the product, the less of their content they can reach. This is a severe failure mode for multi-profile projects and icon-heavy layouts.

**Action**

- Make the profile/page tree the flexible, vertically scrollable sidebar region.
- Keep device settings accessible as a collapsible or fixed lower section.
- Make the icon library horizontally scrollable, wrapping, or open it into a dedicated asset drawer.
- Fix the minimum-width sidebar overflow.

**Acceptance criteria**

- At 1060 × 680, 1440 × 900, and 200% Windows text scaling:
  - No sidebar control is horizontally clipped.
  - All profiles/pages remain reachable.
  - All imported assets remain keyboard and pointer reachable.
- Test with at least:
  - 20 profiles.
  - 20 pages in one profile.
  - 100 assets.
- Scrollbars or alternative navigation are visually discoverable.
- The currently selected item scrolls into view.

**Implementation pointers**

- `src/styles.css`: `.sidebar`, `.tree`, `.assets-strip`.
- `src/artwork.css`: `.asset-item`.
- `src/m6.css`: sidebar device-control sizing.

---

### UX-09 — Surface validation and status as actionable UI

**Priority:** P1 — high  
**Area:** Error prevention and operation feedback

**Evidence**

- Validation issues are retained in `summary.issues` but are not rendered.
- Users see validation detail only after attempting Sync, compressed into the status bar.
- `.status-message` is not a live region.
- Error colouring is inferred from message substrings (`failed` or `Resolve`), so messages such as `Cannot sync` and `Could not restore workspace` are not reliably styled as errors.
- The status bar is only 34 px tall and uses 10 px text.

**User impact**

Users can edit an invalid project without knowing what is wrong or where to fix it. Important failures are easy to miss and are not announced to assistive technology.

**Action**

Represent notices as structured state (`info`, `success`, `warning`, `error`, `progress`) and add an issue summary that navigates to affected fields.

**Acceptance criteria**

- Validation issues are visible before Sync.
- Each issue has human-readable copy and, where possible, an action/focus target.
- Sync is disabled when blocking errors exist, with a visible explanation.
- Operation status uses `role="status"`/`aria-live="polite"`; critical failures use an appropriate assertive pattern.
- Error styling is based on type, never string matching.
- Long errors open into a readable detail surface instead of being truncated in the status bar.
- Busy operations expose progress or an indeterminate busy state and prevent conflicting actions.

**Implementation pointers**

- `src/App.svelte`: `notice`, `summary`, `queueValidation`, `sync`, status markup.
- `src/lib/backend.ts`: existing `ValidationIssue` is a good basis.

---

### UX-10 — Show current position and avoid stale macro identity

**Priority:** P2 — medium  
**Area:** Inspector context

**Evidence**

- The inspector heading is always **Button**, not the selected position.
- Generated macro names include the original position, for example `Default · Main · Key 1`.
- Moving that configured button to position 10 correctly preserves the macro object, so the inspector can still display `Key 1` while editing button 10.
- Generated macro names are not editable in the current UI.

**User impact**

After moving buttons, the inspector can show a plausible but incorrect location cue. This increases wrong-slot edits.

**Action**

Treat current slot identity separately from persistent macro identity.

**Acceptance criteria**

- Inspector heading shows `Button 10` and optionally `Row 2 · Column 2`.
- The macro section shows a neutral name or an editable macro name that does not masquerade as current location.
- Moving a button updates visible position immediately without altering its sequence.
- Imported/generated macro naming is deterministic but never the sole selection cue.

**Implementation pointers**

- `src/App.svelte`: inspector header, macro title, `updateButton`, move completion.
- `src/lib/model.ts`: no schema change is required unless editable naming rules change.

---

### UX-11 — Make profile/page actions discoverable without right-click

**Priority:** P2 — medium  
**Area:** Discoverability and keyboard access

**Evidence**

- Rename and delete are revealed through right-click titles on profile/page rows.
- No visible overflow/action button exists on those rows.
- The context menu supports F2/Delete only after it is already open.
- The menu displays `Ctrl+N`, but that handler is scoped to an open context menu and is not a normal app shortcut.
- Context-menu close does not restore focus to the invoking row.

**User impact**

Users who do not try right-click may never discover rename/delete. Keyboard-only and touchpad users receive weak affordance.

**Action**

Add an ellipsis action button to each selected/hovered row and support the standard Context Menu key/Shift+F10. Keep right-click as a shortcut.

**Acceptance criteria**

- Rename/delete are discoverable with mouse, keyboard, and screen reader.
- The row action button has a unique accessible name.
- Context Menu key and Shift+F10 open the same menu.
- F2 can directly rename the focused row; Delete can directly request deletion.
- Every shown shortcut works in the documented scope.
- Closing the menu restores focus to its invoking row.

**Implementation pointers**

- `src/App.svelte`: profile/page row markup, `openContextMenu`, `contextMenuKeydown`, `closeContextMenu`.

---

### UX-12 — Raise legibility, contrast, and target sizes

**Priority:** P2 — medium  
**Area:** Visual accessibility

**Evidence**

- The interface uses many 8–10 px labels and helper lines.
- Representative contrast calculations:
  - `#777c89` on `#16171b`: **4.29:1**
  - `#747884` on `#16171b`: **4.06:1**
  - `#606571` on `#17181c`: **3.04:1**
  - `#707581` on `#141519`: **3.95:1**
- Tested targets included:
  - Step delete: approximately **20 × 13 px**
  - Modifier toggles: approximately **23 px high**
  - Add project: **25 × 25 px**
  - Pager buttons: **27 × 27 px**
  - Radial-size controls: **28 px high**
- Only a subset of control classes has an intentional `:focus-visible` treatment.

**User impact**

Secondary information is difficult to read, particularly on dense displays or at the minimum window size. Precision targets increase accidental actions.

**Action**

Create shared typography, colour, target, and focus tokens and apply them across all editor controls.

**Acceptance criteria**

- Normal text meets at least 4.5:1 contrast.
- Essential non-text UI state meets at least 3:1.
- No functional target is smaller than 24 × 24 CSS px; frequently used/destructive controls should target 32–40 px.
- Body/control text is generally at least 11–12 px at 100% scaling.
- Every interactive element has a visible, consistent focus indicator.
- The interface remains usable with 200% text scaling and Windows high-contrast mode.

**Implementation pointers**

- `src/styles.css`, `src/artwork.css`, `src/context-menu.css`, `src/m6.css`.
- Start with CSS custom properties in `:root`.

---

### UX-13 — Improve empty-button and first-run guidance

**Priority:** P2 — medium  
**Area:** Onboarding

**Evidence**

- Selecting an empty key shows only Action and Radial layout controls.
- Blank canvas keys have no visible position numbers or hover/focus summary.
- The app does not explain the core setup loop: select key → choose action → build macro → add artwork → sync.
- Drag movement is discoverable mainly through cursor change on configured keys.

**User impact**

New users face a 32-cell blank canvas and must infer the product’s interaction model.

**Action**

Add compact contextual guidance rather than a blocking tutorial.

**Acceptance criteria**

- Empty inspector state names the selected position and provides a one-sentence next step.
- First project includes either sample content or dismissible setup guidance.
- Key position becomes visible on hover/focus or in the inspector without adding permanent visual noise.
- Artwork and drag guidance appears contextually and is dismissible.
- Guidance does not obscure expert workflows after first use.

**Implementation pointers**

- `src/App.svelte`: canvas buttons, empty inspector state, assets strip.

---

### UX-14 — Make media import feedback accurate and resilient

**Priority:** P2 — medium  
**Area:** Import reliability

**Evidence**

- The file picker accepts GIF and multiple video formats, while empty-library copy advertises only PNG, JPEG, and WebP.
- Unsupported dropped files are silently skipped.
- If every dropped file is unsupported, the flow still reaches `changed("Icon imported and assigned")`.
- `importFiles()` does not wrap decoding/conversion in user-facing error handling.
- Animated conversion can take time, but there is no per-file progress or busy state.

**User impact**

Users cannot tell whether a format is supported, converting, ignored, or failed.

**Action**

Use an explicit import result model with successes, skips, failures, and progress.

**Acceptance criteria**

- Advertised formats match the actual picker and conversion support.
- Unsupported files produce a non-destructive warning naming the file.
- Failed files do not prevent successful files from importing.
- Animated/video conversion shows progress or an indeterminate busy state.
- Final notice reports counts: imported, skipped, failed.
- The selected button is changed only when at least one asset imports successfully.
- Error cases have unit/integration coverage.

**Implementation pointers**

- `src/App.svelte`: `importFiles`, file input, drop handler, assets copy.
- `src/lib/backend.ts`: `prepareIconAnimation`.

---

### UX-15 — Detect device connection changes automatically

**Priority:** P2 — medium  
**Area:** Device lifecycle

**Evidence**

- `refreshDevice()` runs at startup and when the small status pill is clicked.
- No polling or device-connect event subscription exists.
- Sync/device actions remain disabled until the user discovers and activates the refresh pill.

**User impact**

A device connected after launch can appear unavailable, making the primary Sync task look broken.

**Action**

Prefer native device arrival/removal events. A low-frequency disconnected-state poll is an acceptable fallback.

**Acceptance criteria**

- Connecting/disconnecting hardware updates the UI without a manual refresh.
- Transition state is announced and does not interrupt active editing.
- Manual refresh remains available with a clear accessible label.
- Polling, if used, pauses appropriately and does not overlap active transfers.

**Implementation pointers**

- `src/App.svelte`: `refreshDevice`, startup lifecycle, device pill.
- `src-tauri/src/device.rs` and `src-tauri/src/lib.rs`: consider emitting Tauri events.

---

### UX-16 — Add duplicate/copy/paste for repetitive layouts

**Priority:** P3 — enhancement  
**Area:** Expert productivity

**Evidence**

- Users can move/swap a complete configured button but cannot duplicate it.
- A 32-key page commonly contains related macros and repeated visual treatment.
- No clipboard or context actions exist for button setup.

**User impact**

Repetitive pages require rebuilding similar macros and artwork assignments.

**Action**

Add copy, cut, paste, duplicate, and clear commands for a complete button configuration. Define whether duplication clones the macro or shares its macro ID; the UI must make that rule explicit.

**Acceptance criteria**

- Commands are available from keyboard and a button context menu.
- Paste preserves all supported properties.
- Macro ownership semantics are documented and tested.
- Duplicate creates independent macro sequences unless the product intentionally exposes shared macros.
- Undo/redo covers every clipboard action.

**Implementation pointers**

- Build after `UX-03` so clipboard mutations use the same history system.
- `src/lib/model.ts`: add explicit clone helpers and tests.

## Cross-cutting implementation recommendations

### 1. Break up `App.svelte`

`App.svelte` currently owns project lifecycle, validation, device operations, dialogs, navigation, drag behaviour, media import, macros, radial editing, and all major markup. Before implementing multiple findings, extract:

- `ProjectSidebar.svelte`
- `DeviceSettings.svelte`
- `ButtonGrid.svelte`
- `ButtonInspector.svelte`
- `MacroSteps.svelte`
- `RadialEditor.svelte`
- `AssetLibrary.svelte`
- `AppDialog.svelte`
- `StatusCenter.svelte`

Keep project mutations behind a small store/command API so history, dirty state, validation, and autosave cannot diverge.

### 2. Replace string status with structured state

Recommended shape:

```ts
type AppNotice = {
  kind: "info" | "success" | "warning" | "error" | "progress";
  message: string;
  detail?: string;
  action?: { label: string; run: () => void };
  persistent?: boolean;
};
```

This removes substring-based error styling and provides a path to accessible live announcements.

### 3. Create one selection model

Profile, page, button, and radial selection currently use parallel numeric indices. A single selection object plus helpers should:

- Keep indices in bounds after deletion.
- Restore focus after modal/context actions.
- Produce current location labels.
- Support keyboard grid navigation.
- Scroll selected content into view.

### 4. Add UI-level automated coverage

Current frontend tests cover model/radial helpers, not interaction flows. Add tests for:

- Dirty-project guard.
- Undo/redo.
- Button grid keyboard navigation.
- Pointer and keyboard move/swap.
- Macro-step reorder.
- Dialog focus trap/restoration.
- Collection overflow/reachability.
- Validation issue navigation.
- Accessible names and selected/toggled states.

## Recommended delivery sequence

### Phase 0 — Data safety and vocabulary

1. `UX-01` dirty-project guard.
2. `UX-02` project/profile terminology.
3. `UX-03` history foundation.

### Phase 1 — Core accessibility and truthfulness

1. `UX-04` modal primitive.
2. `UX-05` keyboard grid and move workflow.
3. `UX-06` semantic names/states.
4. `UX-07` real macro reorder.
5. `UX-09` structured status and validation.

### Phase 2 — Scale and clarity

1. `UX-08` scrollable collections.
2. `UX-10` inspector position context.
3. `UX-11` visible profile/page actions.
4. `UX-12` visual accessibility tokens.
5. `UX-13` empty-state guidance.
6. `UX-14` import feedback.
7. `UX-15` device lifecycle.

### Phase 3 — Expert productivity

1. `UX-16` copy/paste/duplicate.
2. Additional workflow improvements informed by user testing.

## Definition of done for the remediation programme

- No P0 or P1 findings remain open.
- All controls have names, roles, states, and visible focus.
- Core project creation, editing, moving, saving, and syncing are keyboard operable.
- No user content becomes unreachable at the supported minimum window.
- Dirty work cannot be replaced implicitly.
- Undo/redo covers every normal editor mutation.
- Validation is visible before sync and actionable.
- `npm run check`, `npm test`, `npm run build`, and `cargo test` pass.
- A manual Windows test passes at:
  - 1060 × 680.
  - 1440 × 900.
  - 200% text scaling.
  - Keyboard-only operation.
  - Screen reader smoke test.
  - Connected, disconnected, connect-after-launch, sync-success, and sync-failure device states.

