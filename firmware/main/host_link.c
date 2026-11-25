// main/host_link.c -- line protocol and screenshot service over USB Serial/JTAG.
#include "host_link.h"

#include "bsp_display.h"
#include "card_app.h"
#include "card_identity.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "cloud.h"
#include "net.h"
#include "setup.h"
#include "sfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LINK_LINE_MAX   384
#define LINK_SEND_MAX   400
#define SHOT_COMMAND    "FAP_SCREENSHOT_V1"
#define SHOT_W          240
#define SHOT_H          320
#define SHOT_BYTES      (SHOT_W * SHOT_H * 2)
#define QUIET_LOG_LEVEL ESP_LOG_WARN

static const char *TAG = "host_link";

static host_link_sink_t s_sink;
static char s_line[LINK_LINE_MAX];
static size_t s_line_len;
static bool s_line_overflow;
static char s_window[sizeof(SHOT_COMMAND) - 1];
static size_t s_window_len;
static volatile bool s_binary_window;
// Screenshot state. A shot is streamed band by band as LVGL redraws the screen,
// so no full-frame buffer is needed. -1 means no shot is being taken.
static volatile int s_shot_row = -1;

// ---- JSON field helpers ---------------------------------------------------

static int json_int(const cJSON *msg, const char *key, int fallback, int min, int max)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(msg, key);
    if (!cJSON_IsNumber(item)) return fallback;
    if (item->valueint < min) return min;
    if (item->valueint > max) return max;
    return item->valueint;
}

static const char *json_str(const cJSON *msg, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(msg, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// ---- message parsers: fill the event, return false to drop the message ------

// "cells" must be exactly three [label, value] string pairs; anything else is ignored.
static bool parse_cells(const cJSON *cells, card_cell_t *out)
{
    if (!cJSON_IsArray(cells) || cJSON_GetArraySize(cells) != CARD_CELL_COUNT) return false;
    for (int i = 0; i < CARD_CELL_COUNT; i++) {
        const cJSON *pair = cJSON_GetArrayItem(cells, i);
        if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2) return false;
        const cJSON *label = cJSON_GetArrayItem(pair, 0);
        const cJSON *value = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsString(label) || !cJSON_IsString(value)) return false;
        card_utf8_copy(out[i].label, sizeof(out[i].label), label->valuestring);
        card_utf8_copy(out[i].value, sizeof(out[i].value), value->valuestring);
    }
    return true;
}

static bool parse_stats(const cJSON *msg, card_event_t *event)
{
    card_stats_t *stats = &event->stats;
    card_utf8_copy(stats->name, sizeof(stats->name), json_str(msg, "name"));
    stats->agent_id = json_int(msg, "id", 0, 0, 99999999);
    stats->level = json_int(msg, "lvl", 1, 0, 999);
    stats->xp = json_int(msg, "xp", 0, 0, 100);
    stats->deals = json_int(msg, "deals", 0, 0, 999999);
    stats->rating10 = json_int(msg, "rating", 0, 0, 50);
    stats->streak = json_int(msg, "streak", 0, 0, 9999);
    stats->today_cents = json_int(msg, "today", 0, 0, 99999999);
    card_utf8_copy(stats->money_label, sizeof(stats->money_label), json_str(msg, "money_label"));
    stats->has_cells = parse_cells(cJSON_GetObjectItemCaseSensitive(msg, "cells"), stats->cells);
    // "week": what the agent earned on each of the last seven days, in cents, today last (below zero: no figure),
    // and "wd": today's day of the week, 0 = Monday. Both, or the face draws no bars.
    const cJSON *week = cJSON_GetObjectItemCaseSensitive(msg, "week");
    const cJSON *weekday = cJSON_GetObjectItemCaseSensitive(msg, "wd");
    if (cJSON_IsArray(week) && cJSON_GetArraySize(week) == CARD_WEEK_DAYS && cJSON_IsNumber(weekday)) {
        for (int i = 0; i < CARD_WEEK_DAYS; i++) {
            const cJSON *day = cJSON_GetArrayItem(week, i);
            const double cents = cJSON_IsNumber(day) ? day->valuedouble : -1;
            stats->week[i] = cents >= 0 ? (cents > 99999999 ? 99999999 : (int)cents) : -1;
        }
        stats->weekday = weekday->valuedouble >= 0 && weekday->valuedouble <= 6 ? (int)weekday->valuedouble : 0;
        stats->has_week = true;
    }
    return stats->name[0] != '\0';
}

static bool parse_offer(const cJSON *msg, card_event_t *event)
{
    card_offer_t *offer = &event->offer;
    offer->id = json_int(msg, "id", 0, 0, 99999999);
    card_utf8_copy(offer->title, sizeof(offer->title), json_str(msg, "title"));
    offer->amount_cents = json_int(msg, "amt", 0, 0, 99999999);
    offer->direct = json_int(msg, "direct", 0, 0, 1) != 0;
    offer->ttl_ms = json_int(msg, "ttl", 6000, 1000, 30000);
    // "remote":1 plays a task the server told of: the sheet only informs, OK closes it.
    offer->remote = json_int(msg, "remote", 0, 0, 1) != 0;
    return offer->title[0] != '\0';
}

// The names of card_job_phase_t, in that enum's order. "disputed" is said by the server only
// (and by a computer playing the server with "task"), never as a "job" message.
static const char *const PHASES[] = { "bid", "won", "own", "work", "done", "lost", "held", "disputed" };
#define JOB_PHASES 7

static bool parse_phase(const char *name, size_t count, card_job_phase_t *out)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, PHASES[i]) != 0) continue;
        *out = (card_job_phase_t)i;
        return true;
    }
    return false;
}

static bool parse_job(const cJSON *msg, card_event_t *event)
{
    if (!parse_phase(json_str(msg, "s"), JOB_PHASES, &event->job.phase)) return false;
    event->job.progress = json_int(msg, "p", 0, 0, 100);
    return true;
}

// What the server would say the agent is doing (tests and screenshots): the line and the crab
// of the resting face, without the card leaving rest. "id":0 clears it.
static bool parse_task(const cJSON *msg, card_event_t *event)
{
    card_task_t *task = &event->task;
    task->id = json_int(msg, "id", 0, 0, 2147483647);
    if (task->id == 0) return true;
    if (!parse_phase(json_str(msg, "s"), sizeof(PHASES) / sizeof(PHASES[0]), &task->phase)) return false;
    task->called_off = strcmp(json_str(msg, "why"), "called_off") == 0;
    return true;
}

static bool parse_paid(const cJSON *msg, card_event_t *event)
{
    card_paid_t *paid = &event->paid;
    paid->id = json_int(msg, "id", 0, 0, 99999999);
    card_utf8_copy(paid->title, sizeof(paid->title), json_str(msg, "title"));
    paid->net_cents = json_int(msg, "net", 0, 0, 99999999);
    paid->new_level = json_int(msg, "lvl", 0, 0, 999);
    // "remote":1 plays a payout the server told of: it waits for the screen instead of being dropped.
    paid->remote = json_int(msg, "remote", 0, 0, 1) != 0;
    return true;
}

static bool parse_key(const cJSON *msg, card_event_t *event)
{
    static const char *const KEYS[] = { "up", "down", "ok" };
    const char *key = json_str(msg, "k");
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (strcmp(key, KEYS[i]) != 0) continue;
        event->key = (card_key_t)i;
        if (json_int(msg, "long", 0, 0, 1)) event->type = CARD_EV_KEY_LONG;
        return true;
    }
    return false;
}

static bool parse_pay(const cJSON *msg, card_event_t *event)
{
    card_pay_t *pay = &event->pay;
    pay->id = json_int(msg, "id", 0, 0, 99999999);
    card_utf8_copy(pay->payee, sizeof(pay->payee), json_str(msg, "payee"));
    card_utf8_copy(pay->memo, sizeof(pay->memo), json_str(msg, "memo"));
    card_utf8_copy(pay->code, sizeof(pay->code), json_str(msg, "code"));
    pay->amount_cents = json_int(msg, "amt", 0, 0, 99999999);
    // -1: the card holds no money (the trial note, as for a request from the server)
    pay->balance_cents = json_int(msg, "bal", 0, -1, 99999999);
    pay->ttl_ms = json_int(msg, "ttl", 20000, 5000, 60000);
    // "remote":1 plays a request from the server, with the computer answering for it.
    pay->remote = json_int(msg, "remote", 0, 0, 1) != 0;
    pay->via_usb = pay->remote;
    // "auth":"secret": the server wants the holder's secret with the approval.
    pay->need_secret = pay->remote && strcmp(json_str(msg, "auth"), "secret") == 0;
    return pay->payee[0] != '\0' && pay->amount_cents > 0;
}

// The computer's answer to a "remote" request it sent: did the server accept the approval, and if
// not, what it said against it ("why") and how many tries of the secret it says are left ("left").
static bool parse_pay_result(const cJSON *msg, card_event_t *event)
{
    static const struct {
        const char *name;
        card_pay_why_t why;
    } WHY[] = {
        { "need_secret", CARD_PAY_NEED_SECRET }, { "wrong_secret", CARD_PAY_WRONG_SECRET },
        { "frozen", CARD_PAY_FROZEN },           { "no_secret", CARD_PAY_NO_SECRET },
    };
    event->pay_result.id = json_int(msg, "id", 0, 0, 99999999);
    event->pay_result.ok = json_int(msg, "ok", 0, 0, 1) != 0;
    event->pay_result.from_usb = true;
    event->pay_result.why = CARD_PAY_REFUSED;
    const char *why = json_str(msg, "why");
    for (size_t i = 0; i < sizeof(WHY) / sizeof(WHY[0]); i++) {
        if (strcmp(why, WHY[i].name) == 0) event->pay_result.why = WHY[i].why;
    }
    event->pay_result.left = json_int(msg, "left", 0, 0, 99);
    return true;
}

// How the server would say the card is guarded (tests and screenshots): the resting face then says
// what stops the card from paying. "frozen":1 by its owner, 2 by wrong secrets; "known":0 forgets it.
static bool parse_guard(const cJSON *msg, card_event_t *event)
{
    card_guard_t *guard = &event->guard;
    const int frozen = json_int(msg, "frozen", 0, 0, 2);
    guard->known = json_int(msg, "known", 1, 0, 1) != 0;
    guard->secret_set = json_int(msg, "secret", 0, 0, 1) != 0;
    guard->frozen = frozen != 0;
    guard->by_attempts = frozen == 2;
    return true;
}

// Show a pairing code as if the server had issued it (tests and screenshots; the
// server knows nothing of it, so typing it on the pairing page finds nothing).
// {"t":"pairing","gone":1} plays the other end: the code ran out and no new one came.
static bool parse_pairing(const cJSON *msg, card_event_t *event)
{
    card_pairing_t *pairing = &event->pairing;
    if (json_int(msg, "gone", 0, 0, 1) == 1) return true;   // the event carries no code
    card_utf8_copy(pairing->code, sizeof(pairing->code), json_str(msg, "code"));
    const char *where = json_str(msg, "where");
    card_utf8_copy(pairing->where, sizeof(pairing->where), where[0] ? where : "clawhunt.store/card/pair");
    pairing->minutes = json_int(msg, "min", 10, 1, 60);
    return pairing->code[0] != '\0';
}

static bool parse_paired(const cJSON *msg, card_event_t *event)
{
    (void)msg;
    (void)event;
    return true;
}

// Show one of the later phone-setup screens without a phone on the hotspot (tests and
// screenshots). Display only: the hotspot and the setup state are not touched, and the
// first screen, which carries the hotspot's password, is not offered. Like the setup
// module's own events it does not count as a computer being attached (the link dot).
static bool parse_setup_view(const cJSON *msg, card_event_t *event)
{
    static const struct {
        const char *name;
        card_setup_phase_t phase;
    } PHASES[] = {
        { "joined", CARD_SETUP_JOINED }, { "trying", CARD_SETUP_TRYING },
        { "done", CARD_SETUP_DONE },     { "failed", CARD_SETUP_FAILED },
        { "off", CARD_SETUP_OFF },   // closes a shown screen; reason 1 says "Wi-Fi 连上了", as after a real setup
    };
    const char *phase = json_str(msg, "phase");
    for (size_t i = 0; i < sizeof(PHASES) / sizeof(PHASES[0]); i++) {
        if (strcmp(phase, PHASES[i].name) != 0) continue;
        event->setup.phase = PHASES[i].phase;
        card_utf8_copy(event->setup.ssid, sizeof(event->setup.ssid), json_str(msg, "ssid"));
        event->setup.reason = json_int(msg, "reason", 0, 0, 3);
        return true;
    }
    return false;
}

static bool parse_say(const cJSON *msg, card_event_t *event)
{
    card_utf8_copy(event->say, sizeof(event->say), json_str(msg, "text"));
    return event->say[0] != '\0';
}

static bool parse_ping(const cJSON *msg, card_event_t *event)
{
    (void)msg;
    (void)event;
    return true;
}

// ---- messages answered here, without going through the state machine ------

static void handle_sound(const cJSON *msg)
{
    sfx_set_enabled(json_int(msg, "on", 0, 0, 1) != 0);
    host_link_send("{\"t\":\"sound\",\"on\":%d}", sfx_enabled() ? 1 : 0);
}

// The Wi-Fi password passes through here once and is never logged or echoed.
static void handle_wifi(const cJSON *msg)
{
    esp_err_t err;
    if (json_int(msg, "forget", 0, 0, 1)) {
        err = net_forget();
    } else if (json_int(msg, "scan", 0, 0, 1)) {
        err = net_scan();
    } else {
        err = net_set_credentials(json_str(msg, "ssid"), json_str(msg, "psk"));
    }
    host_link_send("{\"t\":\"wifi\",\"ok\":%d,\"err\":\"%s\",\"state\":\"%s\"}", err == ESP_OK ? 1 : 0,
                   esp_err_to_name(err), net_state_name());
}

// Point the card at another server (a local backend while testing), or unpair it.
static void handle_cloud(const cJSON *msg, bool unpair)
{
    const esp_err_t err = unpair ? cloud_unpair() : cloud_set_base(json_str(msg, "base"));
    host_link_send("{\"t\":\"api\",\"ok\":%d,\"err\":\"%s\"}", err == ESP_OK ? 1 : 0, esp_err_to_name(err));
}

static void dispatch(const char *json)
{
    static const struct {
        const char *name;
        card_event_type_t type;
        bool (*parse)(const cJSON *msg, card_event_t *event);
    } TABLE[] = {
        { "stats", CARD_EV_STATS, parse_stats }, { "offer", CARD_EV_OFFER, parse_offer },
        { "job", CARD_EV_JOB, parse_job },       { "paid", CARD_EV_PAID, parse_paid },
        { "key", CARD_EV_KEY, parse_key },       { "ping", CARD_EV_PING, parse_ping },
        { "pay", CARD_EV_PAY, parse_pay },       { "say", CARD_EV_SAY, parse_say },
        { "pay_result", CARD_EV_PAY_RESULT, parse_pay_result },
        { "pairing", CARD_EV_PAIRING, parse_pairing }, { "paired", CARD_EV_PAIRED, parse_paired },
        { "setup_view", CARD_EV_SETUP, parse_setup_view }, { "task", CARD_EV_TASK, parse_task },
        { "guard", CARD_EV_GUARD, parse_guard },
    };

    cJSON *msg = cJSON_Parse(json);
    if (!cJSON_IsObject(msg)) {
        ESP_LOGW(TAG, "dropped a line that is not a JSON object");
        cJSON_Delete(msg);
        return;
    }
    const char *type = json_str(msg, "t");
    if (strcmp(type, "sound") == 0) {
        handle_sound(msg);
    } else if (strcmp(type, "wifi") == 0) {
        handle_wifi(msg);
    } else if (strcmp(type, "setup") == 0) {
        // "lan" also serves the setup page on the network the card is already on.
        setup_request(json_int(msg, "on", 0, 0, 1) != 0, json_int(msg, "lan", 0, 0, 1) != 0);
    } else if (strcmp(type, "api") == 0 || strcmp(type, "unpair") == 0) {
        handle_cloud(msg, type[0] == 'u');
    }
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(type, TABLE[i].name) != 0) continue;
        card_event_t event = { .type = TABLE[i].type };
        if (TABLE[i].parse(msg, &event)) {
            s_sink(&event);
        } else {
            ESP_LOGW(TAG, "dropped a malformed '%s' message", type);
        }
        break;
    }
    cJSON_Delete(msg);
}

// ---- screenshot -----------------------------------------------------------

static bool write_all(const void *data, size_t size)
{
    return usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(1000)) == (int)size;
}

static void shot_end(void)
{
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
    esp_log_level_set("*", QUIET_LOG_LEVEL);
    s_binary_window = false;
    s_shot_row = -1;
}

// Runs on the LVGL task just before each rendered band goes to the panel, after the
// BSP has masked the corners: the shot is exactly what the screen shows.
static void shot_flush_event(lv_event_t *event)
{
    if (s_shot_row < 0) return;
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!area || !buf || !buf->data) return;
    // Wait for the full-width band that continues the picture; ignore anything else.
    if (area->x1 != 0 || area->x2 != SHOT_W - 1 || area->y1 != s_shot_row) return;

    if (s_shot_row == 0) {
        // Log lines share this stream; one stray byte would shift the whole image.
        s_binary_window = true;
        esp_log_level_set("*", ESP_LOG_NONE);
        char header[64];
        const int len = snprintf(header, sizeof(header), SHOT_COMMAND " %d %d RGB565LE %d\n",
                                 SHOT_W, SHOT_H, SHOT_BYTES);
        if (!write_all(header, (size_t)len)) {
            shot_end();
            return;
        }
    }
    for (int32_t y = area->y1; y <= area->y2; y++) {
        const uint8_t *row = buf->data + (size_t)(y - area->y1) * buf->header.stride;
        if (!write_all(row, SHOT_W * 2)) {   // the computer went away
            shot_end();
            return;
        }
    }
    s_shot_row = area->y2 + 1;
    if (s_shot_row >= SHOT_H) shot_end();
}

static void start_screenshot(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    s_shot_row = 0;
    lv_obj_invalidate(lv_screen_active());   // the next refresh redraws every band
    bsp_lvgl_unlock();
}

// ---- byte stream ----------------------------------------------------------

// Terminals on Windows may swallow the trailing newline, so the screenshot word
// is matched on a sliding window rather than as a complete line.
static bool window_matches(char byte)
{
    if (byte == '\r' || byte == '\n') {
        s_window_len = 0;
        return false;
    }
    if (s_window_len == sizeof(s_window)) {
        memmove(s_window, s_window + 1, sizeof(s_window) - 1);
        s_window_len--;
    }
    s_window[s_window_len++] = byte;
    if (s_window_len < sizeof(s_window)) return false;
    if (memcmp(s_window, SHOT_COMMAND, sizeof(s_window)) != 0) return false;
    s_window_len = 0;
    return true;
}

static void feed(char byte)
{
    if (window_matches(byte)) {
        s_line_len = 0;
        s_line_overflow = false;
        start_screenshot();
        return;
    }
    if (byte == '\r' || byte == '\n') {
        if (s_line_len > 0 && !s_line_overflow && s_line[0] == '@') {
            s_line[s_line_len] = '\0';
            dispatch(s_line + 1);
        }
        s_line_len = 0;
        s_line_overflow = false;
        return;
    }
    if (s_line_len < sizeof(s_line) - 1) {
        s_line[s_line_len++] = byte;
    } else {
        s_line_overflow = true;   // drop the rest of an over-long line
    }
}

static void reader_task(void *arg)
{
    (void)arg;
    char chunk[64];
    for (;;) {
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        const int got = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(got < 0 ? 200 : 10));   // never spin on an idle line
            continue;
        }
        for (int i = 0; i < got; i++) feed(chunk[i]);
    }
}

esp_err_t host_link_init(host_link_sink_t sink)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    s_sink = sink;

    // Firmware without a REPL never installs this driver; reading without it crashes.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = { .tx_buffer_size = 2048, .rx_buffer_size = 1024 };
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), TAG, "driver install");
    }
    usb_serial_jtag_vfs_use_driver();

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    lv_display_add_event_cb(lv_display_get_default(), shot_flush_event, LV_EVENT_FLUSH_START, NULL);
    bsp_lvgl_unlock();

    // Below the LVGL task (priority 4) so a burst of input never starves the screen.
    if (xTaskCreate(reader_task, "host_link", 6144, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_log_level_set("*", QUIET_LOG_LEVEL);
    return ESP_OK;
}

void host_link_send(const char *fmt, ...)
{
    if (s_binary_window || !usb_serial_jtag_is_driver_installed()) return;

    char line[LINK_SEND_MAX];
    line[0] = '@';
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line + 1, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (len < 0) return;
    if (len > (int)sizeof(line) - 3) len = (int)sizeof(line) - 3;
    line[1 + len] = '\n';
    (void)usb_serial_jtag_write_bytes(line, (size_t)len + 2, pdMS_TO_TICKS(20));
}

void host_link_send_hello(void)
{
    host_link_send("{\"t\":\"hello\",\"fw\":\"%s\",\"agent\":%d,\"sound\":%d,\"wifi\":\"%s\",\"ip\":\"%s\","
                   "\"nets\":%d,\"setup\":\"%s\",\"cloud\":\"%s\",\"api\":\"%s\",\"heap\":%u,\"heap_min\":%u,"
                   "\"keys\":%u}",
                   CARD_FW_VERSION, CARD_AGENT_ID, sfx_enabled() ? 1 : 0, net_state_name(), net_ip(),
                   net_saved_count(), setup_phase_name(),
                   cloud_state_name(), cloud_base(), (unsigned)esp_get_free_heap_size(),
                   (unsigned)esp_get_minimum_free_heap_size(), card_app_physical_keys());
}
