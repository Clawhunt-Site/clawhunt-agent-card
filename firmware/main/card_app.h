// main/card_app.h -- the card's state machine: idle, offer, working, paid.
#pragma once

#include "card_model.h"
#include "esp_err.h"

// Builds the screen and starts the task that applies events. Requires LVGL.
esp_err_t card_app_start(void);

// Queue an event from any task or callback. Never blocks; drops when full.
void card_app_post(const card_event_t *event);
// The same, for a sender that can try again: false if the event was not queued.
bool card_app_try_post(const card_event_t *event);

// The card's own keys, counted since power-on (presses and holds). Keys pressed
// through the USB link are not counted, so this tells whether the hardware keys work.
void card_app_note_physical_key(void);
unsigned card_app_physical_keys(void);
