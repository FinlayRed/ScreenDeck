#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { int16_t x, y; } m6_point_t;
typedef struct {
    m6_point_t center;
    uint16_t radius, dead_zone, commit_radius;
    uint8_t size;
} m6_radial_geometry_t;
typedef struct { int8_t index; bool committed; } m6_radial_selection_t;

m6_radial_geometry_t m6_radial_place(m6_point_t origin, uint16_t width, uint16_t height, uint8_t size);
m6_radial_selection_t m6_radial_select(const m6_radial_geometry_t *geometry, m6_point_t point, int8_t previous);
m6_point_t m6_radial_item_position(const m6_radial_geometry_t *geometry, uint8_t index);
