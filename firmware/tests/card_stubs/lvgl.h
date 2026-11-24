// The part of LVGL that main/card_app.c uses: one-shot timers and the tick. main/ui_card.c is not
// compiled in these tests; its functions only write down what they were asked to draw.
#pragma once

#include <stdint.h>

typedef struct lv_timer lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period_ms, void *user_data);
void lv_timer_delete(lv_timer_t *timer);
void lv_timer_set_repeat_count(lv_timer_t *timer, int32_t count);
uint32_t lv_tick_get(void);
uint32_t lv_tick_elaps(uint32_t since);
