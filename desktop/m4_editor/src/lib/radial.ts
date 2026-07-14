import type { RadialSize } from "./model";

export interface Point { x: number; y: number }
export interface Bounds { width: number; height: number }
export interface RadialGeometry {
  center: Point;
  radius: number;
  deadZone: number;
  commitRadius: number;
  size: RadialSize;
}

export interface RadialSelection {
  index: number | null;
  committed: boolean;
  distance: number;
}

export const DEFAULT_RADIAL_RADIUS = 116;
export const DEFAULT_DEAD_ZONE = 24;
export const DEFAULT_COMMIT_RADIUS = 68;
const DIRECTIONS: Record<RadialSize, readonly (readonly [number, number])[]> = {
  4: [[0,-1024],[1024,0],[0,1024],[-1024,0]],
  6: [[0,-1024],[887,-512],[887,512],[0,1024],[-887,512],[-887,-512]],
  8: [[0,-1024],[724,-724],[1024,0],[724,724],[0,1024],[-724,724],[-1024,0],[-724,-724]]
};
const DIRECTION_NAMES: Record<RadialSize, readonly string[]> = {
  4: ["North", "East", "South", "West"],
  6: ["North", "Northeast", "Southeast", "South", "Southwest", "Northwest"],
  8: ["North", "Northeast", "East", "Southeast", "South", "Southwest", "West", "Northwest"]
};

export function radialDirection(index: number, size: RadialSize): string {
  return DIRECTION_NAMES[size][index];
}

export function placeRadial(origin: Point, bounds: Bounds, size: RadialSize, radius = DEFAULT_RADIAL_RADIUS): RadialGeometry {
  const pad = 12;
  const usableRadius = Math.max(48, Math.min(radius, (bounds.width - pad * 2) / 2, (bounds.height - pad * 2) / 2));
  return {
    center: {
      x: Math.min(bounds.width - usableRadius - pad, Math.max(usableRadius + pad, origin.x)),
      y: Math.min(bounds.height - usableRadius - pad, Math.max(usableRadius + pad, origin.y))
    },
    radius: usableRadius,
    deadZone: Math.min(DEFAULT_DEAD_ZONE, usableRadius * 0.32),
    commitRadius: Math.min(DEFAULT_COMMIT_RADIUS, usableRadius * 0.72),
    size
  };
}

export function selectRadial(point: Point, geometry: RadialGeometry, previous: number | null = null): RadialSelection {
  const dx = point.x - geometry.center.x;
  const dy = point.y - geometry.center.y;
  const distanceSquared = dx * dx + dy * dy;
  const distance = Math.sqrt(distanceSquared);
  if (distanceSquared <= geometry.deadZone * geometry.deadZone) return { index: null, committed: false, distance };

  const vectors = DIRECTIONS[geometry.size];
  let index = 0;
  let bestScore = Number.NEGATIVE_INFINITY;
  for (let i = 0; i < vectors.length; i++) {
    const score = dx * vectors[i][0] + dy * vectors[i][1];
    if (score > bestScore) { bestScore = score; index = i; }
  }
  if (previous !== null && distanceSquared >= geometry.commitRadius * geometry.commitRadius) {
    const previousScore = dx * vectors[previous][0] + dy * vectors[previous][1];
    const lengthApprox = Math.max(Math.abs(dx), Math.abs(dy)) + Math.min(Math.abs(dx), Math.abs(dy)) / 2;
    if (bestScore - previousScore < lengthApprox * 140) index = previous;
  }
  return { index, committed: distanceSquared >= geometry.commitRadius * geometry.commitRadius, distance };
}

export function radialItemPosition(index: number, geometry: RadialGeometry): Point {
  const orbit = geometry.radius * 0.72;
  const vector = DIRECTIONS[geometry.size][index];
  return { x: geometry.center.x + vector[0] * orbit / 1024, y: geometry.center.y + vector[1] * orbit / 1024 };
}

/** Positions full-size radial keys on the same square/hex grid used by the device UI. */
export function radialGridOffset(index: number, size: RadialSize): Point {
  const layouts: Record<RadialSize, readonly Point[]> = {
    4: [{x:0,y:-1},{x:1,y:0},{x:0,y:1},{x:-1,y:0}],
    6: [{x:0,y:-1},{x:1,y:-0.5},{x:1,y:0.5},{x:0,y:1},{x:-1,y:0.5},{x:-1,y:-0.5}],
    8: [{x:0,y:-1},{x:1,y:-1},{x:1,y:0},{x:1,y:1},{x:0,y:1},{x:-1,y:1},{x:-1,y:0},{x:-1,y:-1}]
  };
  return layouts[size][index];
}
