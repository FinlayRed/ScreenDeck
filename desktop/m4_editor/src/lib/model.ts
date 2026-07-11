export type StepKind = "key_down" | "key_up" | "delay" | "consumer";
export type ActionKind = "macro" | "page_next" | "page_previous" | "profile_next" | "none";

export interface MacroStep { kind: StepKind; key?: string; durationMs?: number }
export interface Macro { id: string; name: string; steps: MacroStep[] }
export interface Asset { id: string; name: string; mediaType: string; dataUrl: string }
export interface Button { iconId?: string; imageFit?: "cover" | "contain"; action: ActionKind; macroId?: string; accent: string }
export interface Page { id: string; name: string; buttons: Button[] }
export interface Profile { id: string; name: string; pages: Page[] }
export interface Project {
  schemaVersion: 1;
  name: string;
  profiles: Profile[];
  macros: Macro[];
  assets: Asset[];
}

const emptyButton = (): Button => ({ action: "none", accent: "#2a2c33" });
const page = (id: string, name: string): Page => ({
  id,
  name,
  buttons: Array.from({ length: 32 }, emptyButton)
});

export function starterProject(): Project {
  const macros: Macro[] = Array.from({ length: 12 }, (_, i) => ({
    id: `macro-${i + 1}`,
    name: `Function F${i + 13}`,
    steps: [
      { kind: "key_down", key: `F${i + 13}` },
      { kind: "delay", durationMs: 25 },
      { kind: "key_up", key: `F${i + 13}` }
    ]
  }));
  const first = page("page-1", "Main");
  first.buttons = first.buttons.map((button, i) => i < 12
    ? { ...button, action: "macro", macroId: macros[i].id, accent: ["#5b6cff", "#00a884", "#e06c4f"][i % 3] }
    : button);
  first.buttons[30] = { action: "page_previous", accent: "#3b3d46" };
  first.buttons[31] = { action: "page_next", accent: "#3b3d46" };
  return {
    schemaVersion: 1,
    name: "My Screendeck",
    profiles: [{ id: "profile-1", name: "Default", pages: [first, page("page-2", "Media")] }],
    macros,
    assets: []
  };
}

export const HID_KEYS = [
  ..."ABCDEFGHIJKLMNOPQRSTUVWXYZ".split(""),
  ...Array.from({ length: 12 }, (_, i) => `F${i + 13}`),
  "ENTER", "ESCAPE", "TAB", "SPACE", "BACKSPACE", "DELETE",
  "LEFT", "RIGHT", "UP", "DOWN", "CTRL", "SHIFT", "ALT", "GUI",
  "VOLUME_UP", "VOLUME_DOWN", "MUTE", "PLAY_PAUSE", "NEXT_TRACK", "PREVIOUS_TRACK"
];

export function cloneProject(project: Project): Project {
  return structuredClone(project);
}
