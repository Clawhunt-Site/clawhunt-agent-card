// tests/card_stubs/card_host.h -- what the card's own code needs from the platform, on a computer.
//
// The card's state machine (main/card_app.c) and its ClawHunt client (main/cloud.c) are compiled
// as they are; everything under them is replaced here: time, LVGL's timers and lock, the FreeRTOS
// queue and task calls, the HTTP client, the key-value store, and the screen, which only writes
// down what it was asked to draw.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- time ------------------------------------------------------------------
// One clock for esp_timer (microseconds) and LVGL (milliseconds). host_advance fires the LVGL
// timers that fall due on the way, in order.
void host_advance(uint32_t ms);
uint32_t host_now_ms(void);

// ---- the screen and everything else the card puts out -----------------------
// One line per call the card made: "status ...", "overlay ...", "say ...", "sfx ...", "usb ...".
// "--" ends a frame: the card applies one event, or runs one timer callback, with the display
// locked, so only what stands at the end of it was ever seen.
#define HOST_LOG_MAX 4096
#define HOST_FRAME "--"
extern char host_log[HOST_LOG_MAX][160];
extern int host_log_count;
void host_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void host_frame(void);
void host_log_clear(void);

// ---- the HTTP client ---------------------------------------------------------
// The test answers each request the card makes. Returns the HTTP status; the body goes into reply.
typedef int (*host_http_fn)(const char *method, const char *url, const char *auth, const char *body,
                            char *reply, size_t reply_size);
void host_http_set(host_http_fn fn);
// What a host_http_fn can return in place of a status:
//   -1 (or 0): no answer. On a connection the card has to open: it cannot be opened. On a kept one:
//   the read fails at once.
#define HOST_HTTP_TIMEOUT (-2)                         // nothing came back until the client gave up waiting
#define HOST_HTTP_RESET_AFTER(ms) (-1000 - (int)(ms))  // the connection is there, and is reset after this long with nothing heard
extern int host_http_connections;   // how many times the card set out to open a connection
extern int host_http_lost;          // how many requests went into a connection the server had closed
// Set by a test: the server has closed the connection the card keeps (it was idle too long). The
// next request on it is lost: the server never sees it, and the card's client fails at once, in
// one of the ways the real client does. A new connection is open again.
#define HOST_HTTP_CLOSED_FIN      1   // the read meets the end of the connection (the usual way)
#define HOST_HTTP_CLOSED_RESET    2   // the read is reset by the server (seen on the real card)
#define HOST_HTTP_CLOSED_AT_WRITE 3   // the write already fails: the client keeps the status of the answer before
extern int host_http_closing;            // the server closes the connection, in this way, right after its next complete answer
extern int host_http_closed_slowly_ms;   // the next request lost that way only fails after this long (the write stalls)
extern bool host_http_ends_connection;   // the next complete answer ends the connection: the client closes it and keeps its handle
extern bool host_http_cuts_body;         // the next answer breaks off in the middle of its body
extern int host_http_no_client;          // so many of the card's next attempts to make a client fail (no memory)
bool host_http_holds(const char *text);  // whether the client still holds this text (its address, its headers, its body)
extern int host_http_closed;

// ---- the key-value store (NVS) -------------------------------------------------
const char *host_store_peek(const char *key);   // "" when the key is not set
int host_store_writes(const char *key);        // how often the key was written or erased
void host_store_put(const char *key, const char *value);   // as if it had been stored before power-on
bool host_store_holds(const char *text);       // whether any stored value contains this text

// ---- knobs -------------------------------------------------------------------
extern bool host_online;        // net_online()
extern int host_saved_nets;     // net_saved_count()
extern bool host_setup_active; // setup_active(): Wi-Fi setup owns the card
extern uint32_t host_paused_ms; // all the time tasks spent in vTaskDelay
extern uint32_t host_random;    // what esp_random() returns next; each call adds 0x01010101
extern char host_last_boot[20]; // the X-Card-Boot header of the last request ("" when it had none)
