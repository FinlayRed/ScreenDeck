// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { int16_t x, y; } point_t;
typedef struct {
    point_t center;
    uint16_t radius, dead_zone, commit_radius;
    uint8_t size;
} radial_geometry_t;
typedef struct { int8_t index; bool committed; } radial_selection_t;

radial_geometry_t radial_place(point_t origin, uint16_t width, uint16_t height, uint8_t size);
radial_selection_t radial_select(const radial_geometry_t *geometry, point_t point, int8_t previous);
point_t radial_item_position(const radial_geometry_t *geometry, uint8_t index);
