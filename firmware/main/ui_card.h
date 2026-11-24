// main/ui_card.h -- everything drawn on the 240x320 screen.
//
// Every function here touches LVGL objects: call them from the LVGL task or while
// holding bsp_lvgl_lock().
#pragma once

#include "card_model.h"

#include <stdbool.h>

// Build the card face and load it. Call once after bsp_lvgl_init().
void ui_card_init(void);

void ui_card_set_stats(const card_stats_t *stats);
// One line under the stats; progress (0..100) fills the line from the left.
void ui_card_set_status(const char *text, int progress);
// Busy makes the crab scuttle; idle lets it sway slowly.
void ui_card_set_busy(bool busy);
// Speech bubble above the crab, hidden again after a moment.
void ui_card_say(const char *text);
// Dot in the top bar: lit while the computer is feeding data.
void ui_card_set_link(bool up);
// Short network label left of the dot, e.g. "Wi-Fi". "" hides it.
void ui_card_set_net(const char *text);

// Full-screen overlays. Showing one replaces whatever overlay was up.
void ui_overlay_offer(const card_offer_t *offer);
void ui_overlay_paid(const card_paid_t *paid);
void ui_overlay_level_up(int level);
void ui_overlay_miss(const card_offer_t *offer);
// A request to pay from this card's wallet, then its outcome.
void ui_overlay_pay(const card_pay_t *pay);
// OK went down on a request: its last line says to keep holding (paying takes a held OK).
void ui_overlay_pay_holding(void);
// The holder's secret is typed here: one square per press, filled without saying which key.
// left_ms: what is left of the request's time. note: a line to read first ("" for none), e.g.
// that the last try was wrong.
void ui_overlay_pay_secret(const card_pay_t *pay, int filled, int left_ms, const char *note);
// Fill the squares of the sheet that is up to this many, without drawing it again.
void ui_overlay_pay_secret_fill(int filled);
void ui_overlay_pay_done(const card_pay_t *pay, bool paid);
// Waiting for the server to accept an approval; and the two ways that can fail.
void ui_overlay_pay_sending(const card_pay_t *pay);
void ui_overlay_pay_failed(const char *title, const char *detail);
// The pairing code, big, with where to type it.
void ui_overlay_pairing(const card_pairing_t *pairing);
// The settings menu: one row per item, the selected one filled.
void ui_overlay_menu(const char *const *items, int count, int selected);
// Phone setup, one screen per phase. WAITING shows the code a phone scans to join
// the card's hotspot.
void ui_overlay_setup(const card_setup_t *setup);
void ui_overlay_close(void);
