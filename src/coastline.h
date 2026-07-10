#pragma once
// World coastline background for the radar scope.
// Project the embedded Natural Earth coastline (coastline_data.h) into screen
// polylines for the current scope, then draw them under the aircraft. Projection
// is done once per home/range change (cheap bbox cull + great-circle), never per
// frame; drawing happens inside the static chrome layer's DRAW_MAIN callback.
#include <lvgl.h>

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx);

void coastline_draw(lv_layer_t *layer, lv_color_t color, lv_opa_t opa, int32_t width);
