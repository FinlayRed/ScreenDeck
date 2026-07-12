export type StepKind = "key_press" | "key_down" | "key_up" | "delay" | "consumer";
export type ActionKind = "macro" | "page_next" | "page_previous" | "profile_next" | "none";

export interface MacroStep { kind: StepKind; key?: string; durationMs?: number; modifiers?: string[] }
export interface Macro { id: string; name: string; steps: MacroStep[] }
export interface Asset {
  id: string;
  name: string;
  mediaType: string;
  dataUrl: string;
  sourceName?: string;
  sourceMediaType?: string;
  sourceDataUrl?: string;
  animationDataUrl?: string;
  animationFps?: number;
}
export interface Button { iconId?: string; imageFit?: "cover" | "contain"; action: ActionKind; macroId?: string }
export interface Page { id: string; name: string; buttons: Button[] }
export interface Profile { id: string; name: string; pages: Page[] }
export interface Project {
  schemaVersion: 2;
  name: string;
  screensaverTimeoutSeconds: number;
  profiles: Profile[];
  macros: Macro[];
  assets: Asset[];
}

const emptyButton = (): Button => ({ action: "none" });
const page = (id: string, name: string): Page => ({
  id,
  name,
  buttons: Array.from({ length: 32 }, emptyButton)
});

export function starterProject(): Project {
  const first = page("page-1", "Main");
  return {
    schemaVersion: 2,
    name: "My Screendeck",
    screensaverTimeoutSeconds: 15,
    profiles: [{ id: "profile-1", name: "Default", pages: [first, page("page-2", "Media")] }],
    macros: [],
    assets: []
  };
}

export const KEYBOARD_KEYS = [
  ..."ABCDEFGHIJKLMNOPQRSTUVWXYZ".split(""),
  ...Array.from({ length: 12 }, (_, i) => `F${i + 13}`),
  "ENTER", "ESCAPE", "TAB", "SPACE", "BACKSPACE", "DELETE",
  "LEFT", "RIGHT", "UP", "DOWN", "CTRL", "SHIFT", "ALT", "GUI"
];
export const CONSUMER_KEYS = [
  "VOLUME_UP", "VOLUME_DOWN", "MUTE", "PLAY_PAUSE", "NEXT_TRACK", "PREVIOUS_TRACK"
];
export function cloneProject(project: Project): Project {
  return structuredClone(project);
}
