// main/crab_sprite.h -- the crab's two animation frames at two sizes (see tools/gen_crab.py).
// Each frame has its window baked in: the pale sky above, the mint floor from CRAB_FLOOR_ROW (CRAB_BIG_FLOOR_ROW) down.
#pragma once

#include "lvgl.h"

#define CRAB_W 96
#define CRAB_H 66
#define CRAB_FLOOR_ROW 51
#define CRAB_BIG_W 192
#define CRAB_BIG_H 132
#define CRAB_BIG_FLOOR_ROW 102

extern const lv_image_dsc_t crab_frame0;   // CRAB_W x CRAB_H, in the face's window
extern const lv_image_dsc_t crab_frame1;
extern const lv_image_dsc_t crab_big0;     // CRAB_BIG_W x CRAB_BIG_H, in the level-up poster's window
extern const lv_image_dsc_t crab_big1;
