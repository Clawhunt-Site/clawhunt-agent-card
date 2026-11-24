// main/sfx.h -- short bell-like chimes played on a worker task.
#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef enum {
    SFX_BLIP = 0,   // key press
    SFX_RING,       // something needs an answer: a new offer or a pay request
    SFX_COIN,       // money came in
    SFX_LEVEL_UP,
    SFX_LOSE,       // offer missed, payment declined
    SFX_PAID_OUT,   // a payment went through
    SFX_COUNT,
} sfx_id_t;

// Starts the audio codec and the worker task. The card still works without sound,
// so callers may log a failure and carry on.
esp_err_t sfx_init(void);

// The card boots silent. Sound is switched on from outside (the gateway decides,
// so quiet hours survive a reboot of the card).
void sfx_set_enabled(bool enabled);
bool sfx_enabled(void);

// Queue a chime. Never blocks; drops the request when sound is off, the queue is
// full or audio is unavailable.
void sfx_play(sfx_id_t id);
