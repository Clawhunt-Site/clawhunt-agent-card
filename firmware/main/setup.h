// main/setup.h -- phone setup: the card opens its own hotspot and serves one page on it.
//
// The holder joins the hotspot by scanning the code on the card's screen, picks a
// Wi-Fi network on the page and types its password. The hotspot's password is
// random for every session and is only ever shown on the screen, so the home
// network's password does not travel over an open network.
#pragma once

#include <stdbool.h>

// Ask for setup to start or stop. The work happens on the cloud task; the screen
// follows through CARD_EV_SETUP events. allow_lan also serves the page to the
// network the card is already on (for testing from a computer); a phone on the
// hotspot never needs it.
void setup_request(bool on, bool allow_lan);

// True from the request until the hotspot is down again.
bool setup_active(void);

// Something the setup screen depends on changed (a phone joined, a join finished).
void setup_poke(void);

// Called by the cloud task every loop.
void setup_service(void);

// "off", "waiting", "joined", "trying", "done" or "failed".
const char *setup_phase_name(void);
