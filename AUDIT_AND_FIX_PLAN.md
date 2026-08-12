# Screendeck code audit and remediation plan

Date: 2026-08-12
Scope: editor, firmware, editor/firmware protocol, persistence, build scripts, and tests
Status: audit complete, fixes not implemented

## Purpose

This document is the implementation handoff for the Screendeck audit. It records the confirmed defects, the evidence behind them, and a staged plan for fixing them. A later agent should update this file as work progresses.

The audit found no P0 defects. It found 10 P1 defects, 16 P2 defects, and one P3 defect. No source files were changed during the audit.

## Repository state and safety notes

The workspace already contained user changes in these files when the audit began:

- `editor/src-tauri/capabilities/default.json`
- `editor/src-tauri/tauri.conf.json`
- `editor/src/App.svelte`
- `editor/src/styles.css`

Preserve those changes. Inspect their diffs before editing overlapping sections.

Do not run `tests/device-sync.ps1 -CommitTestBundle` against a device in its current form. The test deliberately commits an invalid inner payload and can replace a working device configuration with an unloadable one.

## Severity definitions

- **P0:** immediate release blocker with broad destructive or security impact.
- **P1:** high risk of memory corruption, device failure, data loss, or unrecoverable user work.
- **P2:** important correctness, reliability, compatibility, or bounded denial-of-service issue.
- **P3:** low-impact defect or misleading diagnostic.

## P1 findings

### E1. WinUSB cancellation can use freed stack memory

**Evidence:** `editor/src-tauri/src/device.rs:700-722`, with stack-owned I/O state and buffers around lines 845-867 and 890-921.

The timeout path calls `CancelIoEx` and returns immediately. Windows cancellation is asynchronous, so the kernel may still access the `OVERLAPPED`, byte-count storage, or transfer buffer after their Rust stack frames have ended. A stalled bulk read or write can therefore crash the editor or corrupt memory.

**Fix plan:**

1. Give each pending operation an owned heap allocation containing its `OVERLAPPED`, result storage, and transfer buffer.
2. After `CancelIoEx`, wait for the operation to reach a terminal state with `GetOverlappedResult`, an event wait, or an equivalent completion loop.
3. Handle the race in which the operation finishes before cancellation.
4. Release the allocation only after Windows reports completion or `ERROR_OPERATION_ABORTED`.
5. Keep one common helper for read and write operations.

**Acceptance tests:**

- A mocked or injectable Win32 layer covers success, timeout followed by cancellation, completion racing cancellation, disconnect, and failed cancellation.
- Repeated forced timeouts under Application Verifier or page heap do not crash or report invalid memory access.
- Normal device sync behavior remains unchanged.

### F1. Screensaver replacement renames an open FatFs file

**Evidence:** `firmware/main/m6_media.c:1165`, `firmware/main/storage.c:940-950`, and `firmware/sdkconfig.defaults:19-20`.

The media task retains `screensaver.mjpg` in `s_media.file`, including when the file is preloaded. The sync task deletes the backup and renames the live file during `MEDIA_COMMIT`. FatFs file locking is disabled. Renaming an open file can fail or corrupt the volume.

**Fix plan:**

1. Make the media task the sole owner of screensaver file handles and media buffers.
2. Add a control queue with `QUIESCE`, `RELOAD`, and `TEST` messages.
3. During commit, request `QUIESCE` and wait for acknowledgement before changing files.
4. Validate the staged file fully before touching the active or backup names.
5. Rename the active file to backup, rename the stage to active, sync the directory if supported, then request `RELOAD`.
6. If activation fails, restore the backup before resuming playback.
7. Consider enabling `CONFIG_FATFS_FS_LOCK` as a secondary guard, not as the primary synchronization mechanism.

**Acceptance tests:**

- Replace a screensaver during active playback and while using the preloaded path.
- Inject failures at both renames and confirm that either the old or new valid file remains active.
- Power-cycle after each injected failure and confirm recovery.
- Run a filesystem check after repeated replacements.

### F2. `TEST_SCREENSAVER` races the background indexer

**Evidence:** `firmware/main/storage.c:993-997` and `firmware/main/m6_media.c:1116-1250,1308,1513-1529`.

The sync task directly resets and frees global media state while the media task may be indexing the same file and buffers. A test request soon after boot can cause two simultaneous index passes, use-after-free, corrupt frame tables, false errors, or a crash.

**Fix plan:** route test requests through the media control queue introduced for F1. The media task should serialize indexing, reload, playback, and test transitions. Remove direct cross-task mutation of `s_media`.

**Acceptance tests:**

- Issue repeated test requests throughout boot and large-file indexing.
- Confirm that only one index pass runs at a time.
- Confirm deterministic status replies for queued, ready, and invalid-media states.

### F3. Firmware activates bundles before validating the M5UI payload

**Evidence:** `firmware/main/storage.c:458-475,571-580,779-821`, `firmware/main/m6_media.c:361-425`, and `tests/device-sync.ps1:127-155`.

Storage validates only the SDB envelope and payload CRC. It marks the generation active before the runtime parser checks M5UI magic, offsets, counts, references, assets, and animation streams. An invalid but CRC-correct bundle reports success, restarts, and replaces the working UI without selecting the previous valid generation.

**Fix plan:**

1. Extract the M5UI structural validator from `m5_load_ui_bundle` into a side-effect-free validator that can operate on a file or bounded buffer.
2. Validate both the SDB envelope and all inner M5UI semantics before writing the active pointer.
3. Keep activation separate from validation.
4. At boot, treat a generation as valid only when both layers pass.
5. If the newest generation fails, select the previous valid generation and remove or quarantine the bad pointer.
6. Change the smoke test to build a minimal valid M5UI bundle. Add a separate negative test that expects rejection and never activates it.

**Acceptance tests:**

- Reject bad magic, schema, counts, offsets, references, assets, animations, and truncated tables before activation.
- Confirm that the prior generation stays active after every rejected commit.
- Corrupt the newest generation on disk and confirm boot fallback to the previous valid generation.

### F4. Unaligned M5UI offsets can cause a boot panic

**Evidence:** `firmware/main/m6_media.c:367-410,925`.

Range checks do not enforce alignment before byte offsets are cast to typed pointers. An odd `button_macro_refs_offset` produces an unaligned `uint16_t` load on ESP32-P4 and can create a restart loop after promotion. Other typed tables need equivalent alignment checks.

**Fix plan:**

1. Add a validator helper that checks range, multiplication overflow, and `_Alignof(type)`.
2. Apply it to every typed table offset, not only macro references.
3. Prefer `memcpy` into aligned local structures when parsing untrusted packed data.
4. Include alignment validation in the pre-activation validator from F3.

**Acceptance tests:** test every table at valid alignment, each invalid alignment, the final valid byte range, one-byte truncation, and overflow-sized counts.

### F5. Completed bundle downloads remain open and may be unlinked

**Evidence:** `firmware/main/storage.c:539-550,673-700,812-819,1084` and the 500 ms restart delay at lines 184-188.

`DOWNLOAD_BEGIN` opens the active generation, but EOF never closes it. After commits N+1 and N+2, generation cleanup can unlink the still-open generation N. With FatFs locking disabled, this is undefined and may corrupt the filesystem.

**Fix plan:**

1. Define an explicit download transaction with begin, chunk, end, abort, and detach cleanup.
2. Close the file when the final byte is sent, on any terminal error, on detach, and before generation cleanup.
3. Add a `DOWNLOAD_END` opcode if EOF inference would make retry behavior ambiguous.
4. Make cleanup skip any generation with an active reference as a secondary guard.

**Acceptance tests:** complete, partial, retried, aborted, detached, and back-to-back downloads followed by two commits. Confirm that cleanup never unlinks an open file.

### E2. The editor saves projects it refuses to reopen

**Evidence:** `editor/src/App.svelte:311-317`, `editor/src-tauri/src/lib.rs:31-48,63-117`.

Save and workspace persistence accept invalid in-progress projects, while both readers reject any validation issue. Clearing the title or adding a ninth profile yields a successful save that the app cannot reopen. Autosave has the same mismatch.

**Fix plan:**

1. Separate document-schema validity from device-deployability.
2. Readers should reject malformed or unsafe structure, but load well-formed work in progress even when it exceeds device constraints.
3. Keep deployability errors in the editor and block Sync, not Open or recovery.
4. If a future schema migration fails, expose a recovery error without deleting the source archive.
5. Add the same read/write contract to `.sdeck` and workspace persistence.

**Acceptance tests:** save and reopen empty titles, nine profiles, oversized projects, and other valid work-in-progress states. Malformed JSON, invalid base64, traversal names, and unsupported schemas must still fail safely.

### E3. `.sdeck` overwrite is not atomic

**Evidence:** `editor/src-tauri/src/archive.rs:38-123`.

`File::create` truncates the destination before serialization, asset decoding, ZIP writing, and finalization finish. A disk, decoding, or ZIP error destroys the previous good archive.

**Fix plan:**

1. Build the archive in a uniquely named temporary file in the destination directory.
2. Flush file contents and call `sync_all`.
3. Atomically replace the destination with platform-appropriate replacement semantics.
4. Preserve the old file if any earlier step fails.
5. Clean abandoned temporary files on error.
6. Apply the same pattern to workspace persistence where practical.

**Acceptance tests:** inject failures during serialization, asset decoding, entry writes, ZIP finish, flush, and replacement. The previous archive must remain byte-for-byte intact until replacement succeeds.

### E4. Undo history retains full media copies

**Evidence:** `editor/src/App.svelte:143-146` and `editor/src/lib/history.ts:15-20`.

Every edit creates a structured clone of the full project, including base64 device media and unbounded original source media. Fifty snapshots of one representative 10 MiB asset retained roughly 505 MiB of heap.

**Fix plan:**

1. Move immutable media blobs into a content-addressed asset store keyed by digest.
2. Keep lightweight asset metadata and blob keys in project state and history.
3. Store reversible operations or structural snapshots that share immutable blobs.
4. Coalesce text edits into one history entry per editing session or short debounce window.
5. Enforce explicit source-media and total-history budgets.
6. Garbage-collect blobs only when no project, undo, or redo state references them.

**Acceptance tests:** edit a project with large media through more than 50 history operations; confirm bounded memory, correct undo/redo, and no premature blob deletion.

### E5. Device transfers do not isolate editor state

**Evidence:** `editor/src/App.svelte:328-373` and editable controls below line 1028.

`From device` replaces the project after an asynchronous download while the project remains editable. Those interim edits disappear. Upload takes an initial project snapshot but allows later edits, then reports success for a device state that differs from the visible editor.

**Fix plan:**

1. Capture the starting project revision for every transfer.
2. Disable all project-mutating controls while replacement or compilation is active, or download into a temporary project and require an explicit apply step if the revision changed.
3. For upload, compile a captured snapshot and report its revision or fingerprint.
4. Set `lastSyncedFingerprint` only to the uploaded snapshot's fingerprint.
5. Keep cancellation and failure paths from changing the current project.

**Acceptance tests:** attempt title, page, inspector, import, undo, and shortcut edits during each transfer. No edit may disappear, and the reported fingerprint must identify the exact uploaded state.

## P2 findings

### E6. Discarded edits return after restart

**Evidence:** `editor/src/App.svelte:231-278` and `editor/src-tauri/src/lib.rs:132-134`.

Choosing Discard does not remove or roll back the recovery workspace. A previously autosaved edit can reappear at the next launch.

**Fix plan:** add an explicit `clear_workspace` command. Call it after confirmed discard and after intentional replacement with a saved state. Cancel pending autosave timers before clearing.

**Acceptance test:** edit, wait for autosave, choose Discard, relaunch, and confirm the discarded state does not return.

### E7. Archive loading has no decompression limits

**Evidence:** `editor/src-tauri/src/archive.rs:131,140,153,163`.

The reader fully decompresses ZIP entries and then creates base64 copies. A small compressed archive can consume unbounded memory and block the UI.

**Fix plan:** cap entry count, each decompressed entry, total decompressed bytes, compression ratio, JSON size, asset size, and total encoded media. Stream reads with `take(limit + 1)` and reject excess before base64 encoding. Run archive parsing off the main thread.

**Acceptance tests:** boundary-sized archives pass; oversized JSON, assets, entry counts, totals, and high-ratio archives fail with clear errors and bounded memory.

### E8. Device polling blocks Tauri's main thread

**Evidence:** `editor/src-tauri/src/lib.rs:126-129`, `editor/src-tauri/src/device.rs:176-185,525`, and `editor/src/App.svelte:1007`.

The synchronous command may perform two five-second WinUSB exchanges every five seconds. A connected but stalled device can repeatedly freeze the window.

**Fix plan:** make device commands asynchronous and run blocking WinUSB work on a dedicated worker. Serialize access through one device session owner. Prevent a new poll while the prior poll is active, and use a shorter status timeout after initial discovery.

**Acceptance tests:** a stalled mock device does not block animation, input, window movement, or command dispatch. Polls do not overlap.

### E9. Bundle size is absent from validation

**Evidence:** `editor/src-tauri/src/compiler.rs:431-457` and `editor/src/App.svelte:1042`.

Compilation rejects bundles above 16 MiB, but the validation summary shows no blocking issue, so Sync remains enabled until compilation fails.

**Fix plan:** use one shared constant and one size calculation for validation and compilation. Add a blocking issue when the exact or conservative estimate exceeds the limit.

**Acceptance tests:** projects one byte below, at, and above the limit produce matching validation and compiler results.

### E10. Async imports can target the wrong project or page

**Evidence:** `editor/src/App.svelte:684-727`.

Media conversion awaits file reading and FFmpeg, then writes through reactive `project` and `page` references. Switching the page or project during conversion redirects the result.

**Fix plan:** capture project identity, revision, profile ID, page ID, and target button before awaiting. On completion, resolve those IDs in the same project or ask the user to retry if the destination no longer exists. Never use the then-current reactive page implicitly.

**Acceptance tests:** switch pages, profiles, and projects during conversion; confirm the result reaches only its original destination or is rejected cleanly.

### E11. Packaged installations omit FFmpeg

**Evidence:** `editor/src-tauri/src/lib.rs:185-295`, `editor/src-tauri/tauri.conf.json:26-34`, and `README.md`.

Animated icons and screensaver conversion require FFmpeg. Packaging declares no sidecar or resource, and runtime lookup only checks beside the executable or `PATH`.

**Fix plan:** choose one supported distribution model. Prefer a pinned, licensed FFmpeg sidecar with checksum verification and Tauri external-binary packaging. Document its source, version, license, and update process. If bundling is rejected, detect FFmpeg during startup, disable affected actions, and explain installation precisely.

**Acceptance tests:** install in a clean Windows VM with no system FFmpeg and convert each supported media class.

### E12. Failed FFmpeg launch leaks source media into `%TEMP%`

**Evidence:** `editor/src-tauri/src/lib.rs:185-215`.

The code writes source media before spawning FFmpeg. Early `?` returns skip cleanup.

**Fix plan:** use a scoped temporary directory or guard whose `Drop` implementation removes all staged input and output files. Keep cleanup independent of process-start and conversion success.

**Acceptance tests:** missing executable, failed spawn, nonzero exit, timeout, malformed output, and success all remove temporary media.

### I1. `STATUS` returns two incompatible values

**Evidence:** `firmware/main/storage.c:1004-1006`, `editor/src-tauri/src/device.rs:185,231`, and `editor/src/App.svelte:1337`.

Firmware returns upload bytes while an upload is resumable and active generation otherwise. The editor always treats the value as generation, which corrupts display and lost-commit verification.

**Fix plan:** version the response and return an explicit struct containing `active_generation`, `upload_open`, `received_bytes`, `total_bytes`, and upload fingerprint or CRC. Keep a compatibility parser only if older firmware must remain supported.

**Acceptance tests:** idle, partial upload, resumed upload, committed upload, aborted upload, and older-protocol responses all decode unambiguously.

### I2. Lost-COMMIT recovery reuses a restarting USB session

**Evidence:** `editor/src-tauri/src/device.rs:309-322` and `firmware/main/storage.c:184-188,821-825`.

After a timeout, the editor waits 250 ms and sends `STATUS` through the same connection, while firmware restarts 500 ms after commit. Disconnect errors bypass this recovery path. A successful commit can therefore be reported as failed.

**Fix plan:** treat timeout, disconnect, and pipe failure after COMMIT as indeterminate. Close the old handle, wait for device disappearance and re-enumeration, open a new session, and compare the explicit active generation or committed fingerprint. Use the same recovery for media commits.

**Acceptance tests:** drop the acknowledgement before and after durable commit, delay restart, disconnect during response, and restart immediately. Report success only when the new session confirms the exact commit.

### I3. The standalone converter emits incompatible screensavers

**Evidence:** `scripts/convert-media.ps1:29-30` and `firmware/main/m6_media.c:45,47,49,54-55,1230-1232`.

The script produces 1280x720 at 30 frames per input second. Firmware requires 720x1280, presents at 60 FPS, limits streams to 1,800 frames, and limits frame and file sizes.

**Fix plan:** share one documented media contract with the editor converter and script. Produce 720x1280 MJPEG at 60 FPS, preserve intended aspect behavior, and validate dimensions, frame count, maximum frame bytes, total bytes, JPEG framing, and duration before output.

**Acceptance tests:** portrait, landscape, square, short, 30-second, overlong, oversized, and malformed sources. Verify the result with `ffprobe` and the same parser rules used by firmware.

### I4. Editor and firmware disagree on animated-icon frame counts

**Evidence:** `editor/src-tauri/src/model.rs:305-309`, `editor/src-tauri/src/compiler.rs:292-301`, and `firmware/main/m6_media.c:61,317-337`.

The editor accepts any non-empty complete MJPEG stream; firmware accepts only 2 through 120 frames. A crafted or legacy project can sync successfully and prevent the full UI bundle from loading.

**Fix plan:** define shared protocol constants in generated or mirrored contract modules with conformance tests. Validate 2 through 120 frames before compile and decompile. Keep firmware validation authoritative.

**Acceptance tests:** 0, 1, 2, 120, and 121 frames, including truncated final frames and extra bytes.

### T1. The WinUSB smoke test uses an undersized native structure

**Evidence:** `tests/device-sync.ps1:27-39`.

`Pack=1` makes `WINUSB_PIPE_INFORMATION` eight bytes, but the Windows ABI layout occupies twelve bytes. `WinUsb_QueryPipe` may write beyond the marshalled buffer. Production Rust correctly reserves twelve bytes.

**Fix plan:** remove `Pack=1`, declare fields with ABI-correct types and padding, and assert `Marshal.SizeOf<PipeInfo>() -eq 12` before the native call.

**Acceptance tests:** run the structure-size assertion on supported Windows architectures, then enumerate endpoints repeatedly under page heap.

### E13. Exported `.sdb` backups cannot be restored

**Evidence:** `editor/src/App.svelte:300-325`; `compiler::decompile` exists but is used only for device downloads.

The editor labels `.sdb` as a compiled backup, but Open accepts only `.sdeck` and no command uploads or imports a local bundle.

**Fix plan:** either implement Restore from compiled backup or relabel the action as Export compiled bundle. The useful path is to validate and decompile a local `.sdb`, show what source-only information will be absent, then open it as an unsaved project. Optionally add direct upload after validation.

**Acceptance tests:** restore valid bundles, reject corrupt or incompatible bundles, and explain loss of original source media.

### F6. USB detach can close a stream during a download read

**Evidence:** `firmware/main/storage.c:673-686,1073-1086`.

The TinyUSB callback can close and clear `s_download_file` while the sync task is between its null check and `fseek` or `fread`.

**Fix plan:** TinyUSB callbacks should publish connection events only. Let the sync task own and close the download stream, or protect the transaction with one owner and explicit cancellation acknowledgement.

**Acceptance tests:** force detach at each read phase with a slow or instrumented storage layer. No use-after-close or malformed successful response may occur.

### F7. `MEDIA_COMMIT` accepts non-decodable MJPEG

**Evidence:** `firmware/main/storage.c:829-850,921-959`, `firmware/main/m6_media.c:1116-1245`, and recovery at `firmware/main/storage.c:603-607`.

Commit checks only byte count, CRC, the first SOI marker, and the final EOI marker. The four bytes `FF D8 FF D9` pass but cannot be decoded at 720x1280. Recovery restores the backup only when active media is missing, not when it is invalid.

**Fix plan:** reuse a side-effect-free MJPEG validator before activation. Validate complete frame boundaries, count, individual size, total size, and dimensions of every frame, or at minimum decode headers for every frame. On boot, restore the backup when active validation fails.

**Acceptance tests:** empty, marker-only, truncated, nested-marker, wrong-dimension, oversized-frame, over-count, and valid streams. Failed commits must retain the prior screensaver.

### F8. Flipped orientation does not apply to screensavers

**Evidence:** `firmware/main/m6_media.c:442-446,1281-1289`.

The project setting rotates LVGL by 180 degrees, but screensaver frames bypass LVGL and draw directly to the panel.

**Fix plan:** store the orientation in media state and apply a 180-degree transform when decoding or presenting raw frames. Choose the cheaper verified path for ESP32-P4 throughput. Avoid maintaining two differently transformed media files.

**Acceptance tests:** compare UI and screensaver orientation in both settings while maintaining the 60 FPS target.

## P3 finding

### E14. The screensaver limit diagnostic is stale

**Evidence:** `editor/src-tauri/src/device.rs:84` reports 900 frames; `firmware/main/m6_media.c:47,1143-1153` enforces 1,800.

**Fix plan:** replace the message with 1,800 and derive future messages from the shared media contract where possible.

## Recommended implementation sequence

The phases below minimize overlapping edits and make later fixes build on shared primitives.

### Phase 0: Protect the baseline

- Inspect and preserve the four pre-existing editor diffs.
- Record current passing checks.
- Add test seams for Win32 I/O and firmware storage/media ownership before changing behavior.
- Disable or rewrite the destructive invalid-bundle smoke test before anyone runs the hardware suite.

### Phase 1: Fix memory and filesystem safety

Implement E1, F1, F2, F5, F6, and T1.

Target architecture:

- One owned WinUSB operation object per pending editor I/O.
- One firmware media task owning all screensaver files and buffers.
- One firmware sync task owning all download files.
- Callbacks and other tasks communicate through queues or notifications instead of closing shared resources.

Exit criteria:

- Cancellation waits for terminal completion.
- No firmware `FILE *` is mutated by more than one task.
- Replacement and cleanup never rename or unlink open files.
- Forced timeout and detach tests pass repeatedly.

### Phase 2: Make activation transactional

Implement F3, F4, F7, E3, and the boot rollback portions of F1.

Use a common transaction shape:

1. Write a staged artifact.
2. Flush it durably.
3. Validate the full artifact without side effects.
4. Atomically activate it.
5. Retain one verified previous artifact.
6. On boot, select the newest fully valid artifact.

Exit criteria:

- No invalid bundle or screensaver can become active.
- A failure at any write, flush, validation, rename, or pointer step leaves a valid recoverable artifact.
- Editor archive overwrite preserves the prior archive on every injected failure.

### Phase 3: Align the protocol and media contract

Implement I1, I2, I3, I4, E9, and E14.

Create one versioned contract document or generated schema containing:

- Protocol version and response fields.
- Maximum SDB bytes.
- Icon FPS and frame-count limits.
- Screensaver width, height, FPS, duration, frame count, frame bytes, and total bytes.
- Commit identity and reconnect verification rules.

Keep firmware validation authoritative, but test editor and script outputs against the same fixtures.

Exit criteria:

- Status fields are unambiguous.
- Commit recovery reconnects and verifies the exact artifact.
- All conversion paths emit media accepted by firmware.
- Boundary conformance tests pass on both sides.

### Phase 4: Make editor persistence and state transitions safe

Implement E2, E5, E6, E7, E8, and E10.

Exit criteria:

- Every well-formed saved or autosaved work-in-progress project reopens.
- Discard permanently discards recovery state.
- Archives have bounded expansion.
- Device work and imports cannot silently overwrite or redirect edits.
- Blocking device and archive work stays off the UI thread.

### Phase 5: Bound media memory and complete packaging

Implement E4, E11, E12, and E13.

Exit criteria:

- Undo memory depends mainly on changed structure, not multiplied media size.
- Clean Windows installs can use every advertised conversion feature.
- Temporary source media is always removed.
- The compiled-backup action has an honest, tested restore workflow or accurate labeling.

### Phase 6: Hardware and recovery qualification

Run a focused device matrix after the earlier phases:

- Fresh device with no active bundle or screensaver.
- Upgrade from a valid previous generation.
- Resume after detach at several upload offsets.
- Lost acknowledgement before and after durable commit.
- Power loss at each activation step.
- Download followed by rapid commits and cleanup.
- Screensaver replacement during indexing and playback.
- Valid and invalid UI bundles, offsets, icon streams, and MJPEG streams.
- Both display orientations.
- Repeated filesystem checks and reboot cycles.

Document firmware version, board revision, SD-card model, test fixture hashes, and exact results.

## Verification already completed

The audit passed these checks:

- Svelte check with zero diagnostics.
- 11 Vitest tests.
- Vite production build.
- 13 Rust tests.
- Gesture matrix.
- Rust formatting check.
- Clippy across all targets with warnings denied.
- PowerShell parser checks.
- Full npm audit with zero advisories.
- Cargo audit with zero vulnerability advisories.
- The hardware-free FFmpeg image conversion test.

## Audit limitations

- ESP-IDF and the vendor submodule worktree were unavailable, so the firmware was not rebuilt locally.
- Physical WinUSB, upload resume, restart, SD-card, and power-loss tests were not run.
- Five hardware or FFmpeg tests remain ignored.
- The ignored video test lacks `.m5_sample.mp4`.
- Destructive malformed-archive, disk-full, and invalid-device-commit reproductions were assessed from code paths rather than executed against user data or hardware.

## Progress log

- **Phase 0 (2026-08-12):** Baseline protected on `phase/0-baseline`. The four pre-existing editor diffs and this document are committed. The destructive `-CommitTestBundle` smoke test now refuses to run without `-AllowDestructiveBundle` (F3 will replace it with a valid-bundle test). Baseline checks re-recorded: 13 Rust tests (5 ignored), 11 Vitest, svelte-check clean, Vite build clean, clippy clean across targets, rustfmt clean, firmware builds under ESP-IDF v5.5.5 (previously unavailable).
- **Phase 1 (2026-08-12):** Memory and filesystem safety fixes landed on `phase/1-memory-filesystem-safety`.
  - E1: WinUSB transfers now run through an owned heap `IoOperation` (OVERLAPPED, buffer, byte count). `run_transfer` cancels on timeout and waits for the kernel to reach a terminal state (`ERROR_OPERATION_ABORTED` or a real byte count) before the allocation is freed; the completion-racing-cancellation case is handled via `ERROR_NOT_FOUND`; a genuinely stuck operation is leaked rather than freed. The Win32 layer is injectable (`Win32Io`); seven mock tests cover success, pending completion, timeout+cancel, racing completion, failed cancel, disconnect, and immediate failure.
  - F1: media task owns all screensaver handles and buffers. New control queue (`M5_MEDIA_CTRL_QUIESCE/RELOAD/TEST`) serializes cross-task requests. `MEDIA_COMMIT` quiesces the media task (with acknowledgement) before renaming, then reloads after activation.
  - F2: `TEST_SCREENSAVER` now routes through the control queue; indexing, reload, playback, and test transitions are serialized in the media task.
  - F5: explicit download transaction. `DOWNLOAD_END` opcode (15) added; firmware closes the stream on end, terminal error, detach, and abort; the editor sends `DOWNLOAD_END` after the final chunk; generation cleanup skips the open download generation.
  - F6: the TinyUSB detach callback only publishes the connection change; the sync task closes the download stream on its next bounded wait.
  - T1: smoke-test `WINUSB_PIPE_INFORMATION` no longer uses `Pack=1` (8 bytes); it now marshals to the ABI-correct 12 bytes and the script asserts that size before any native call.
  - Secondary guard: `CONFIG_FATFS_FS_LOCK=8` enabled.
  - Verification: 20 Rust tests (7 new), clippy clean, rustfmt clean, firmware builds, PowerShell parser checks pass, T1 assertion verified at runtime.

- **Phase 2 (2026-08-12):** Transactional activation landed on `phase/2-transactional-activation`.
  - F3: `m5_ui_bundle_valid` extracted as a side-effect-free, file-based M5UI validator (magic, schema, counts, offsets, references, assets, animation streams). Both `COMMIT` and boot pointer selection now require the full SDB + M5UI validation (`m3_bundle_fully_valid`), so a CRC-correct but invalid bundle can never be marked active. The smoke test now commits a minimal valid bundle and gains `-RejectInvalidBundle`, which expects rejection and confirms the generation does not advance.
  - F4: every typed-table offset is alignment-checked (`_Alignof`) in the validator, the in-memory loader additionally rejects odd `button_macro_refs_offset`, and table parsing copies into aligned locals instead of casting file bytes.
  - F7: `m5_mjpeg_file_valid` validates complete frame boundaries, count (<=1800), per-frame size (<=2 MiB), and every frame decoding to 720x1280 before activation and at boot; boot now restores the backup when the active file is present but undecodable.
  - E3: archive saves build in a uniquely named temp file, `sync_all`, then atomically replace via `MoveFileExW` (Windows) so no earlier failure can destroy the previous archive; the same pattern applies to workspace persistence. A regression test injects a mid-save failure and asserts the previous archive stays byte-identical with no staging leftovers.
  - Verification: 22 Rust tests (2 new), clippy clean, rustfmt clean, firmware builds, PowerShell parser checks pass, T1 assertion verified.

- **Phase 3 (2026-08-12):** Protocol and media contract alignment landed on `phase/3-protocol-media-contract`.
  - I1: firmware STATUS now returns an explicit fixed struct (version, upload-open flag, active generation, received/total bytes, upload CRC, active media bytes); the editor parses v2 and keeps a legacy 8-byte fallback. New parse tests cover v2, legacy, and malformed payloads.
  - I2: lost COMMIT acknowledgements (timeout, disconnect, pipe failure) are treated as indeterminate; the editor closes the session, waits for re-enumeration, reconnects, and verifies the active generation advanced (bundle) or the media file size matches (screensaver).
  - I3: the standalone converter now produces 720x1280 MJPEG at 60 FPS (max 1800 frames, 16 MiB total, 2 MiB per frame), matching the editor converter and firmware parser, with full output validation (frame scan, per-frame size, ffprobe dimension check). Verified end-to-end: 2 s source -> 120 frames at 720x1280.
  - I4: shared icon contract constants (15 FPS, 2..=120 frames) in model.rs; compilation and decompilation both reject out-of-range or count-mismatched animation streams, with tests for 1, 2, and 121 frames and a patched frame-count mismatch.
  - E9: `summarize` now reports a blocking issue when the estimated bundle size exceeds the shared 16 MiB limit, so Sync is disabled before compilation fails; regression test with a 17.25 MiB asset.
  - E14: the screensaver diagnostic now reports 1800 frames.
  - Verification: 29 Rust tests (7 new), clippy clean, rustfmt clean, firmware builds, PowerShell parser checks pass, T1 assertion verified, converter validated with ffprobe.

- **Phase 4 (2026-08-12):** Editor persistence and state transitions landed on `phase/4-persistence-state`.
  - E2: `open_archive` and `load_workspace` no longer reject projects on deployability issues; a structural gate rejects only unsafe shapes (no profiles or an empty profile). Empty titles, extra profiles, and oversized media load; Sync stays blocked by the validation summary.
  - E5: transfers capture the starting revision. Upload reports its fingerprint only when the revision is unchanged; `From device` refuses to apply when the project changed mid-download; project-mutating regions are `inert` while busy.
  - E6: new `clear_workspace` command; Discard, New, Open, and From-device replacements clear the recovery workspace (after cancelling pending autosaves) so discarded edits never resurrect.
  - E7: archive reads are bounded (entry count 2048, project.json 8 MiB, per-entry 64 MiB, total media 192 MiB, compression ratio 100x) with a `take(limit+1)` reader, and archive open/save/backup run off the main thread. Regression test rejects an oversized-entry archive.
  - E8: `device_status` (plus `open_archive`, `save_archive`, `backup_bundle`) moved to async worker commands; the status poll uses a 2 s timeout after discovery and the frontend never overlaps polls.
  - E10: media imports capture profile/page identity before awaiting and resolve the destination in the current project afterwards; a missing destination fails cleanly instead of redirecting the result.
  - Verification: 31 Rust tests (2 new), clippy clean, rustfmt clean, svelte-check clean, 11 Vitest, Vite build clean.

## Completion checklist

Update this list as fixes land:

- [x] E1 WinUSB cancellation lifetime
- [x] F1 Safe screensaver replacement
- [x] F2 Serialized screensaver testing/indexing
- [x] F3 Full pre-activation M5UI validation
- [x] F4 Typed-table alignment validation
- [x] F5 Download transaction cleanup
- [x] E2 Symmetric project persistence
- [x] E3 Atomic archive saving
- [ ] E4 Bounded undo history
- [x] E5 Transfer state isolation
- [x] E6 True discard behavior
- [x] E7 Bounded archive expansion
- [x] E8 Nonblocking device polling
- [x] E9 Bundle-size validation
- [x] E10 Stable async import destination
- [ ] E11 FFmpeg distribution
- [ ] E12 Temporary-file cleanup
- [x] I1 Explicit status response
- [x] I2 Reconnect-based commit verification
- [x] I3 Compatible standalone conversion
- [x] I4 Shared animation limits
- [x] T1 Correct WinUSB smoke-test ABI
- [ ] E13 Compiled-backup restore or relabeling
- [x] F6 Detach-safe download ownership
- [x] F7 Full MJPEG validation and fallback
- [ ] F8 Screensaver orientation
- [x] E14 Correct frame-limit diagnostic
- [ ] Full editor verification passes
- [ ] Fresh firmware build passes
- [ ] Hardware and recovery matrix passes

## References

- Microsoft `CancelIoEx`: <https://learn.microsoft.com/en-us/windows/win32/fileio/cancelioex-func>
- Microsoft `WINUSB_PIPE_INFORMATION`: <https://learn.microsoft.com/en-us/windows/win32/api/winusbio/ns-winusbio-winusb_pipe_information>
- Tauri command execution: <https://v2.tauri.app/develop/calling-rust/>
- FatFs rename contract: <https://elm-chan.org/fsw/ff/doc/rename.html>
- ESP-IDF FatFs behavior: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/storage/fatfs.html>
- ESP32-P4 alignment exceptions: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/fatal-errors.html>
