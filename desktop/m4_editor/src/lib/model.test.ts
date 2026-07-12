import { describe, expect, it } from "vitest";
import { cloneProject, starterProject } from "./model";

describe("starterProject", () => {
  it("creates an editable 8×4 page with bounded macros", () => {
    const project = starterProject();
    expect(project.schemaVersion).toBe(2);
    expect(project.profiles[0].pages[0].buttons).toHaveLength(32);
    expect(project.macros).toHaveLength(12);
    expect(project.macros[0].steps.map((step) => step.kind)).toEqual(["key_down", "delay", "key_up"]);
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
});
