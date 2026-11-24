// main/host_link.h -- the USB serial link to the computer that feeds the card.
//
// Wire format, one message per line in both directions:
//   @{"t":"stats","name":"polyglot-dev","lvl":7,"xp":70,"deals":39,"rating":49,"streak":6,"today":4250}
//        "id":2101 names the agent the numbers belong to (the plate shows it; without it, the built-in agent's)
//   @{"t":"offer","id":1432,"title":"...","amt":2500,"direct":0,"ttl":6000}
//   @{"t":"job","s":"bid|won|own|work|done|lost|held","p":40}
//        "lost": no payout will come; back to resting. "held": the buyer accepted and the payout is on hold;
//        back to resting, and the status line says what the card waits for
//   @{"t":"paid","id":1432,"title":"...","net":1875,"lvl":8}
//   @{"t":"task","id":1432,"s":"bid|won|own|work|done|held|disputed|lost","why":"called_off"}
//        plays what the server says the agent is doing (cloud.c): the line and the crab of the resting
//        face. The card stays at rest: keys, pay requests and settings work as ever. "lost" is said once
//        ("why":"called_off": the task ended without a payout). "id":0 clears it.
//        "offer" and "paid" take "remote":1 to play the server's sheets: such an offer only informs (the
//        agent bid or claimed with its own key; any key closes it, and the card answers "seen"), and
//        either sheet waits for the screen when it is taken instead of being refused or dropped.
//   @{"t":"pay","id":7,"payee":"Lobby Cafe","memo":"latte","amt":500,"bal":192332,"ttl":20000}
//        "bal":-1 means the card holds no money (the trial note). "remote":1 plays a request
//        from the server: approving shows "sending" until the computer answers with
//   @{"t":"pay_result","id":7,"ok":1}
//        "auth":"secret" (with "remote":1) plays a request that needs the holder's secret: holding OK
//        opens the sheet it is typed on. The card then answers {"t":"pay_ok","id":7,"secret":1}: that
//        a secret was typed, never which keys. While that sheet is up the card's "key" lines say
//        "k":"*". The computer plays the server's verdict with
//   @{"t":"pay_result","id":7,"ok":0,"why":"wrong_secret|need_secret|frozen|no_secret","left":3}
//   @{"t":"guard","secret":1,"frozen":0}  (how the server guards the card: "frozen":1 by its owner,
//        2 by wrong secrets; "known":0 forgets it. The resting face says what stops the card.)
//   @{"t":"pairing","code":"KTRX-4PQM","where":"clawhunt.store/card/pair","min":10}
//   @{"t":"paired"}                      (show and clear a pairing code the server never issued)
//   @{"t":"setup_view","phase":"joined|trying|done|failed|off","ssid":"Home","reason":1}
//        (show a later phone-setup screen; display only, and never the one with the hotspot password)
//   @{"t":"say","text":"..."}            (the crab says it, over the status line)
//   @{"t":"key","k":"up|down|ok"}        (lets the computer press a button)
//   @{"t":"ping"}
// "stats" may also carry "cells":[["label","value"] x3] and "money_label" to relabel
// the three tiles and the money line.
// Money is in cents and rating is times ten, so no floats cross the wire.
// The card answers with @{"t":"hello"|"accept"|"miss"|"busy"|"seen"|"key"|"pay_ok"|"pay_no",...}
// lines. "hello" names the agent this firmware was built for.
//
// The link also serves FAP_SCREENSHOT_V1: send that word and the card replies
// "FAP_SCREENSHOT_V1 240 320 RGB565LE 153600\n" followed by the raw pixels.
#pragma once

#include "card_model.h"
#include "esp_err.h"

typedef void (*host_link_sink_t)(const card_event_t *event);

// Installs the USB serial driver and starts the reader task. Parsed messages are
// handed to sink on the reader task; the sink must only queue them.
esp_err_t host_link_init(host_link_sink_t sink);

// Send one "@{...}" line. fmt is the JSON body without the leading '@'.
// Safe from any task. Drops the line if the computer is not reading.
void host_link_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Announce the firmware version and heap headroom; sent at boot and on every ping.
void host_link_send_hello(void);
