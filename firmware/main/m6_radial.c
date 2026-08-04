// SPDX-License-Identifier: GPL-3.0-or-later

#include "m6_radial.h"
#include <stdlib.h>

#define M6_RADIUS 116
#define M6_EDGE_PAD 12
#define M6_DEAD_ZONE 24
#define M6_COMMIT_RADIUS 68

/* Clockwise from up, Q10 vectors. Selection is maximum dot product: no
 * atan2, division or square root in the touch hot path. */
static const int16_t directions4[4][2] = {{0,-1024},{1024,0},{0,1024},{-1024,0}};
static const int16_t directions6[6][2] = {{0,-1024},{887,-512},{887,512},{0,1024},{-887,512},{-887,-512}};
static const int16_t directions8[8][2] = {{0,-1024},{724,-724},{1024,0},{724,724},{0,1024},{-724,724},{-1024,0},{-724,-724}};

static int16_t clamp16(int32_t value, int16_t low, int16_t high)
{
    return (int16_t) (value < low ? low : value > high ? high : value);
}

m6_radial_geometry_t m6_radial_place(m6_point_t origin, uint16_t width, uint16_t height, uint8_t size)
{
    uint16_t radius = M6_RADIUS;
    if (width < 2 * (radius + M6_EDGE_PAD)) radius = (width - 2 * M6_EDGE_PAD) / 2;
    if (height < 2 * (radius + M6_EDGE_PAD)) radius = (height - 2 * M6_EDGE_PAD) / 2;
    if (radius < 48) radius = 48;
    return (m6_radial_geometry_t) {
        .center = {clamp16(origin.x, radius + M6_EDGE_PAD, width - radius - M6_EDGE_PAD),
                   clamp16(origin.y, radius + M6_EDGE_PAD, height - radius - M6_EDGE_PAD)},
        .radius = radius, .dead_zone = radius < 75 ? radius * 32 / 100 : M6_DEAD_ZONE,
        .commit_radius = radius < 95 ? radius * 72 / 100 : M6_COMMIT_RADIUS, .size = size,
    };
}

m6_radial_selection_t m6_radial_select(const m6_radial_geometry_t *g, m6_point_t point, int8_t previous)
{
    const int32_t dx = point.x - g->center.x, dy = point.y - g->center.y;
    const uint32_t distance2 = dx * dx + dy * dy;
    if (distance2 <= (uint32_t) g->dead_zone * g->dead_zone) return (m6_radial_selection_t) {-1, false};
    const int16_t (*vectors)[2] = g->size == 4 ? directions4 : g->size == 6 ? directions6 : directions8;
    int8_t best = 0;
    int32_t best_score = INT32_MIN;
    for (uint8_t i = 0; i < g->size; ++i) {
        const int32_t score = dx * vectors[i][0] + dy * vectors[i][1];
        if (score > best_score) { best_score = score; best = i; }
    }
    const bool committed = distance2 >= (uint32_t) g->commit_radius * g->commit_radius;
    if (committed && previous >= 0 && previous < g->size && previous != best) {
        const int32_t old_score = dx * vectors[previous][0] + dy * vectors[previous][1];
        const int32_t length_approx = abs(dx) > abs(dy) ? abs(dx) + abs(dy) / 2 : abs(dy) + abs(dx) / 2;
        if (best_score - old_score < length_approx * 140) best = previous;
    }
    return (m6_radial_selection_t) {best, committed};
}

m6_point_t m6_radial_item_position(const m6_radial_geometry_t *g, uint8_t index)
{
    const int16_t (*vectors)[2] = g->size == 4 ? directions4 : g->size == 6 ? directions6 : directions8;
    const int32_t orbit = g->radius * 72 / 100;
    return (m6_point_t) {g->center.x + vectors[index][0] * orbit / 1024,
                         g->center.y + vectors[index][1] * orbit / 1024};
}
