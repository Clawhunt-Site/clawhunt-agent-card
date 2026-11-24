// main/ground_strips.h -- the colour fields the screens stand on (see tools/gen_grounds.py).
// Each is a strip GROUND_STRIP_W wide and as tall as the screen, dithered for the 16-bit panel; a screen
// repeats it across its width.
#pragma once

#include "lvgl.h"

#define GROUND_STRIP_W 16

extern const lv_image_dsc_t ground_home;   // the card at rest: blue at the top, white at the foot
extern const lv_image_dsc_t ground_ask;   // something wants an answer: a payment request, the secret, a new order
extern const lv_image_dsc_t ground_done;   // it went through: confirmed, paid in, connected
extern const lv_image_dsc_t ground_setup;   // setting the card up: its code, its menu, its hotspot
extern const lv_image_dsc_t ground_none;   // nothing happened: missed, refused, failed
