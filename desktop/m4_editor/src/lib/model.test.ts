import { describe, expect, it } from "vitest";
import { cloneProject, moveButton, starterProject } from "./model";

describe("starterProject", () => {
  it("creates an editable 8×4 page with bounded macros", () => {
    const project = starterProject();
    expect(project.schemaVersion).toBe(3);
    expect(project.brightnessPercent).toBe(80);
    expect(project.profiles[0].pages[0].buttons).toHaveLength(32);
    expect(project.macros).toHaveLength(0);
    for (const profile of project.profiles) {
      for (const page of profile.pages) {
        expect(page.buttons.every((button) => button.action === "none")).toBe(true);
        expect(page.buttons.every((button) => button.macroId === undefined)).toBe(true);
        expect(page.buttons.every((button) => button.iconId === undefined)).toBe(true);
      }
    }
  });

  it("clones without sharing nested editor state", () => {
    const source = starterProject();
    const copy = cloneProject(source);
    copy.profiles[0].pages[0].buttons[0].action = "page_next";
    expect(source.profiles[0].pages[0].buttons[0].action).toBe("none");
  });

  it("moves a configured button as one unit and swaps an occupied target", () => {
    const project = starterProject();
    const page = project.profiles[0].pages[0];
    const configured = {
      action: "macro" as const,
      macroId: "macro-1",
      iconId: "icon-1",
      imageFit: "cover" as const,
      radial: {
        size: 4 as const,
        items: [
          { action: "macro" as const, macroId: "radial-1", iconId: "icon-2" },
          { action: "none" as const },
          { action: "page_next" as const },
          { action: "profile_next" as const },
        ],
      },
    };
    const occupied = { action: "page_previous" as const, iconId: "icon-3" };
    project.macros.push(
      { id: "macro-1", name: "Configured macro", steps: [{ kind: "key_press", key: "F13", modifiers: ["CTRL"] }] },
      { id: "radial-1", name: "Radial macro", steps: [{ kind: "delay", durationMs: 250 }] },
    );
    page.buttons[2] = configured;
    page.buttons[11] = occupied;

    expect(moveButton(page.buttons, 2, 11)).toBe(true);
    expect(page.buttons[11]).toBe(configured);
    expect(page.buttons[2]).toBe(occupied);
    expect(project.macros.find((macro) => macro.id === page.buttons[11].macroId)?.steps).toEqual([
      { kind: "key_press", key: "F13", modifiers: ["CTRL"] },
    ]);
    expect(project.macros.find((macro) => macro.id === page.buttons[11].radial?.items[0].macroId)?.steps).toEqual([
      { kind: "delay", durationMs: 250 },
    ]);
  });

  it("ignores invalid or no-op button moves", () => {
    const buttons = starterProject().profiles[0].pages[0].buttons;
    const original = [...buttons];

    expect(moveButton(buttons, 4, 4)).toBe(false);
    expect(moveButton(buttons, -1, 4)).toBe(false);
    expect(moveButton(buttons, 4, buttons.length)).toBe(false);
    expect(buttons).toEqual(original);
  });
});
