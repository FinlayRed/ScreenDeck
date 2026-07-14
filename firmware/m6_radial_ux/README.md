# M6 — radial menus and integrated UX

M6 advances the runtime bundle to schema 3 and adds 4/6/8-way per-key radial
menus anchored to each key centre (with edge items intentionally clipped),
centre cancellation, committed-selection hysteresis, brightness/orientation
settings, configurable idle/screensaver behaviour, and wake-touch suppression.

The touch hot path uses squared radii and maximum dot products against Q10
direction vectors. It performs no allocation, division, square root, or
trigonometry. A bounded overlay is created once on press; only the old and new
selection nodes are restyled while dragging.

Gesture acceptance covers all four corners, edges, centre jitter, sector
boundaries, fast single-sample flicks, and the consumed wake touch. The editor
simulator in `desktop/m4_editor` uses the same clockwise-up sector convention,
thresholds, hysteresis, and placement rules.
