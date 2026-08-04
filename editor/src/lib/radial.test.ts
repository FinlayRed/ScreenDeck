// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from "vitest";
import { placeRadial, radialDirection, radialGridOffset, selectRadial } from "./radial";

describe("M6 radial geometry", () => {
  it("clamps all corner and edge origins on-screen", () => {
    for (const origin of [{x:0,y:0},{x:1280,y:0},{x:0,y:720},{x:1280,y:720},{x:640,y:0}]) {
      const g = placeRadial(origin, { width: 1280, height: 720 }, 8);
      expect(g.center.x - g.radius).toBeGreaterThanOrEqual(0);
      expect(g.center.y - g.radius).toBeGreaterThanOrEqual(0);
      expect(g.center.x + g.radius).toBeLessThanOrEqual(1280);
      expect(g.center.y + g.radius).toBeLessThanOrEqual(720);
    }
  });

  it("cancels centre jitter and commits a fast outward flick", () => {
    const g = placeRadial({x:640,y:360}, {width:1280,height:720}, 8);
    expect(selectRadial({x:649,y:368}, g).index).toBeNull();
    const flick = selectRadial({x:640,y:260}, g);
    expect(flick).toMatchObject({ index: 0, committed: true });
  });

  it("keeps an outer selection sticky across a sector boundary", () => {
    const g = placeRadial({x:640,y:360}, {width:1280,height:720}, 4);
    expect(selectRadial({x:700,y:300}, g, 0).index).toBe(0);
  });

  it("lays full-size keys out on a square grid instead of a circle", () => {
    expect([0, 1, 2, 3].map((index) => radialGridOffset(index, 4))).toEqual([
      {x:0,y:-1},{x:1,y:0},{x:0,y:1},{x:-1,y:0}
    ]);
    expect(radialGridOffset(1, 8)).toEqual({x:1,y:-1});
    expect(radialGridOffset(5, 6)).toEqual({x:-1,y:-0.5});
  });

  it("labels radial positions with simple compass directions", () => {
    expect([0, 1, 2, 3].map((index) => radialDirection(index, 4))).toEqual(["North", "East", "South", "West"]);
    expect(radialDirection(1, 6)).toBe("Northeast");
    expect(radialDirection(7, 8)).toBe("Northwest");
  });
});
