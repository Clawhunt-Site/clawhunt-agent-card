// main/crab_sprite.h -- the crab's two animation frames at two sizes (see tools/gen_crab.py).
// Each frame has the card's dark ground baked in: it is set straight on that ground.
#pragma once

#include "lvgl.h"

#define CRAB_MARK_W 16
#define CRAB_MARK_H 11
#define CRAB_BIG_W 160
#define CRAB_BIG_H 110

extern const lv_image_dsc_t crab_mark0;    // CRAB_MARK_W x CRAB_MARK_H, in the face's header
extern const lv_image_dsc_t crab_mark1;
extern const lv_image_dsc_t crab_big0;     // CRAB_BIG_W x CRAB_BIG_H, on the level-up poster
extern const lv_image_dsc_t crab_big1;
