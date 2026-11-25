// main/card_app.c -- applies key presses and host messages to the card.
//
// Every transition runs with the LVGL lock held: either on the app task, which
// takes the lock around each event, or inside an LVGL timer callback.
#include "card_app.h"

#include "bsp_display.h"
#include "card_identity.h"
#include "cloud.h"
#include "host_link.h"
#include "lvgl.h"
#include "net.h"
#include "setup.h"
#include "sfx.h"
#include "ui_card.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>

#define EVENT_QUEUE_DEPTH 8
#define MISS_SHOW_MS      1600
#define PAID_SHOW_MS      2400
#define LEVEL_UP_SHOW_MS  2200
#define LINK_TIMEOUT_MS   6000
#define PAY_GUARD_MS      800    // keys are ignored this long after a pay request appears
#define PAY_DONE_SHOW_MS  2400
#define PAY_NO_SHOW_MS    1500
#define PAY_SEND_MAX_MS   15000  // how long to wait for the server to accept an approval
#define HOST_STATS_WIN_MS 90000  // a computer's stats outrank the card's own this long
#define MENU_IDLE_MS      15000  // the settings menu closes by itself
#define MENU_COUNT        3
#define NOTICE_KEEP_MS    60000  // a new-task sheet that could not be shown at once is still shown this long after
#define HOLD_AFTER_CLOSE_MS 1500 // a key that closed a sheet does not also open the menu if it stays down
#define LATER_PAID_MAX    3      // payouts kept while the screen is taken
#define WORD_SHOW_MS      1600   // a line or bubble the card just put up is left this long before a waiting sheet
#define TOLD_ONCE_LOCK_TRIES 5   // seconds the display is waited for before something the server said once is dropped
#define SECRET_GUARD_MS   400    // keys are ignored this long after the secret sheet appears
// After the last press of the secret, before it is sent: a key that is held (to start again, or not
// to pay) first arrives as a press, and the hold is only known 500 ms later (BSP_BTN_LONG_PRESS_MS).
#define SECRET_SEND_MS    800
#define SECRET_AGAIN_MIN_MS 4000 // a wrong secret may be typed again only while this much of the request's time is left
#define GUARD_SHOW_MS     5000   // "frozen" and "no secret set" stay up longer: there is something to do about them

typedef enum {
    MODE_IDLE = 0,
    MODE_OFFER,
    MODE_WORK,
    MODE_PAID,
    MODE_LEVEL_UP,
    MODE_MISS,
    MODE_PAY,
    MODE_PAY_SENDING,
    MODE_PAY_DONE,
    MODE_MENU,    // settings, opened by holding OK
    MODE_SETUP,   // phone setup owns the screen
    MODE_PAY_SECRET,   // a pay request, OK was held: the holder types the secret
} card_mode_t;

static const char *TAG = "card_app";
static const char *const KEY_NAMES[] = { "up", "down", "ok" };
static const char *const QUIPS[] = { "按住我半秒，打开设置", "来活儿了吗", "钳子磨好了", "今天想赚 $100", "别戳，在摸鱼" };

static QueueHandle_t s_queue;
static card_mode_t s_mode;
static card_offer_t s_offer;
static int s_pending_level;
static card_pay_t s_pay;
static uint32_t s_pay_shown_at;
static uint32_t s_host_stats_at;
static card_pairing_t s_pairing;
static bool s_pairing_shown;
static bool s_host_stats_seen;
static lv_timer_t *s_step_timer;   // the one pending "what happens next" timer
static lv_timer_t *s_link_timer;
static size_t s_quip;
static int s_menu_sel;
static volatile unsigned s_physical_keys;
// What the server says the agent is doing (id 0: nothing). It is shown whenever the card rests.
static card_task_t s_task;
// Sheets from the server that arrived while the screen was taken. The server says each thing
// once, so they wait here for the card to come to rest: the payouts in the order they came (a card
// that was off is told up to three), and the newest new task.
static card_paid_t s_later_paid[LATER_PAID_MAX];
static int s_later_paid_count;
static card_offer_t s_later_offer;
static bool s_has_later_offer;
static uint32_t s_later_offer_at;
static card_paid_t s_paid;         // the payout sheet that is up, or was last
static uint32_t s_offer_at;        // when the offer that is up first arrived
static uint32_t s_key_closed_at;
static bool s_key_closed;
// The holder's secret while it is being typed: 'U', 'D' or 'O' for each press so far. It lives here
// for the seconds between the first press and the moment it is handed to cloud.c (or given up), and
// is wiped then. It is never shown, stored, or sent over USB.
static char s_typed[CARD_SECRET_LEN + 1];
static int s_typed_len;
static uint32_t s_secret_shown_at;
static card_guard_t s_guard;       // how the server guards the card; not known: the face says nothing of it

static void show_paid(const card_paid_t *paid);
static void show_offer(const card_offer_t *offer, uint32_t arrived_at);

// ---- timers ---------------------------------------------------------------

static void step_cancel(void)
{
    if (s_step_timer) {
        lv_timer_delete(s_step_timer);
        s_step_timer = NULL;
    }
}

static void step_after(uint32_t ms, lv_timer_cb_t next)
{
    step_cancel();
    s_step_timer = lv_timer_create(next, ms, NULL);
    lv_timer_set_repeat_count(s_step_timer, 1);
}

// The resting face: what the server says the agent is doing, or else the idle line.
static void show_rest(void)
{
    // A card that knows no network says how to give it one.
    const char *line = net_saved_count() > 0 ? CARD_LINE_IDLE : CARD_LINE_NO_WIFI;
    bool busy = false;
    int progress = 0;
    if (s_task.id > 0) {
        switch (s_task.phase) {
        case CARD_JOB_BID:      line = CARD_LINE_BID; busy = true; break;
        case CARD_JOB_WON:      line = CARD_LINE_WON; busy = true; break;
        case CARD_JOB_OWN:      line = CARD_LINE_OWN; busy = true; break;
        // The site keeps no measure of how far a task has got, so there is no percentage here.
        case CARD_JOB_WORK:     line = CARD_LINE_WORK; busy = true; break;
        case CARD_JOB_DONE:     line = CARD_LINE_DONE; busy = true; progress = 100; break;
        case CARD_JOB_HELD:     line = CARD_LINE_HELD; break;
        case CARD_JOB_DISPUTED: line = CARD_LINE_DISPUTED; break;
        case CARD_JOB_LOST:     break;   // said once in handle_task; nothing stays on the line
        }
    }
    // What stops the card from paying goes before everything else on the line: nobody can ask a
    // frozen card, or one without a secret, to pay, so without this it would just look idle.
    if (s_guard.known && s_guard.frozen) {
        line = s_guard.by_attempts ? CARD_LINE_FROZEN_TRIES : CARD_LINE_FROZEN;
        progress = 0;
    } else if (s_guard.known && !s_guard.secret_set) {
        line = CARD_LINE_NO_SECRET;
        progress = 0;
    }
    ui_card_set_busy(busy);
    ui_card_set_status(line, progress);
}

// Keep a payout from the server for when the screen is free. first: it was already on the screen
// and gave way, so it goes back to the head of the line. When the line is full the oldest goes.
static void later_paid_put(const card_paid_t *paid, bool first)
{
    if (s_later_paid_count == LATER_PAID_MAX) {
        for (int i = 1; i < LATER_PAID_MAX; i++) s_later_paid[i - 1] = s_later_paid[i];
        s_later_paid_count--;
    }
    if (first) {
        for (int i = s_later_paid_count; i > 0; i--) s_later_paid[i] = s_later_paid[i - 1];
        s_later_paid[0] = *paid;
    } else {
        s_later_paid[s_later_paid_count] = *paid;
    }
    s_later_paid_count++;
}

static bool sheets_waiting(void)
{
    return s_later_paid_count > 0 || s_has_later_offer;
}

// The card at rest, and nothing more.
static void rest(void)
{
    step_cancel();
    ui_overlay_close();
    s_mode = MODE_IDLE;
    show_rest();
}

// The screen is free: a sheet from the server that had to wait is shown now, a payout first.
static bool show_waiting(void)
{
    if (s_later_paid_count > 0) {
        const card_paid_t paid = s_later_paid[0];
        for (int i = 1; i < s_later_paid_count; i++) s_later_paid[i - 1] = s_later_paid[i];
        s_later_paid_count--;
        show_paid(&paid);
        return true;
    }
    if (s_has_later_offer) {
        s_has_later_offer = false;
        if (lv_tick_elaps(s_later_offer_at) < NOTICE_KEEP_MS) {
            show_offer(&s_later_offer, s_later_offer_at);
            return true;
        }
    }
    return false;
}

static void go_idle(void)
{
    rest();
    if (show_waiting()) return;
    // Until the card is bound to an owner, the pairing code is its resting screen.
    if (s_pairing_shown) ui_overlay_pairing(&s_pairing);
}

// One-shot LVGL timers delete themselves after firing, so each callback first
// forgets the handle.
static void on_step_idle(lv_timer_t *timer)
{
    (void)timer;
    s_step_timer = NULL;
    go_idle();
}

// Back to rest for a caller that then puts a line or a bubble on the face: a sheet that waited
// holds off until that has been read, instead of covering it at once.
static void go_idle_with_a_word(void)
{
    if (!sheets_waiting()) {
        go_idle();
        return;
    }
    rest();
    step_after(WORD_SHOW_MS, on_step_idle);
}

static void on_offer_expired(lv_timer_t *timer)
{
    (void)timer;
    s_step_timer = NULL;
    s_mode = MODE_MISS;
    ui_overlay_miss(&s_offer);
    sfx_play(SFX_LOSE);
    host_link_send("{\"t\":\"miss\",\"id\":%d}", s_offer.id);
    step_after(MISS_SHOW_MS, on_step_idle);
}

static void on_paid_shown(lv_timer_t *timer)
{
    (void)timer;
    s_step_timer = NULL;
    if (s_pending_level <= 0) {
        go_idle();
        return;
    }
    s_mode = MODE_LEVEL_UP;
    ui_overlay_level_up(s_pending_level);
    sfx_play(SFX_LEVEL_UP);
    s_pending_level = 0;
    step_after(LEVEL_UP_SHOW_MS, on_step_idle);
}

static void on_link_lost(lv_timer_t *timer)
{
    (void)timer;
    s_link_timer = NULL;
    ui_card_set_link(false);
}

static void link_seen(void)
{
    ui_card_set_link(true);
    if (s_link_timer) lv_timer_delete(s_link_timer);
    s_link_timer = lv_timer_create(on_link_lost, LINK_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_link_timer, 1);
}

// ---- event handlers -------------------------------------------------------

static void start_work(const char *status)
{
    step_cancel();
    ui_overlay_close();
    ui_card_set_busy(true);
    ui_card_set_status(status, 0);
    s_mode = MODE_WORK;
}

static void show_pay_done(bool paid)
{
    step_cancel();
    s_mode = MODE_PAY_DONE;
    ui_overlay_pay_done(&s_pay, paid);
    sfx_play(paid ? SFX_PAID_OUT : SFX_LOSE);
    step_after(paid ? PAY_DONE_SHOW_MS : PAY_NO_SHOW_MS, on_step_idle);
}

static void on_pay_send_timeout(lv_timer_t *timer)
{
    (void)timer;
    s_step_timer = NULL;
    s_mode = MODE_PAY_DONE;
    ui_overlay_pay_failed("没收到确认", "网络没有回应，请看对方手机上的结果");
    sfx_play(SFX_LOSE);
    step_after(PAY_DONE_SHOW_MS, on_step_idle);
}

// Send the holder's answer to whoever sent the request: the server, or a computer
// on USB that stands in for it.
static void tell_requester(bool paid, const char *why)
{
    if (s_pay.via_usb) {
        if (paid) {
            host_link_send("{\"t\":\"pay_ok\",\"id\":%d}", s_pay.id);
        } else {
            host_link_send("{\"t\":\"pay_no\",\"id\":%d,\"why\":\"%s\"}", s_pay.id, why);
        }
        return;
    }
    cloud_pay_decision(s_pay.id, paid,
                       paid ? CLOUD_REASON_APPROVED : why[0] == 't' ? CLOUD_REASON_TIMEOUT : CLOUD_REASON_DECLINED);
}

// A request from the server is only shown as paid once the server has accepted
// the approval; a refusal needs no round trip and is shown at once.
static void finish_remote_pay(bool paid, const char *why)
{
    if (!paid) {
        tell_requester(false, why);
        show_pay_done(false);
        return;
    }
    step_cancel();
    s_mode = MODE_PAY_SENDING;
    ui_overlay_pay_sending(&s_pay);
    tell_requester(true, why);
    step_after(PAY_SEND_MAX_MS, on_pay_send_timeout);
}

// ---- the holder's secret ---------------------------------------------------
//
// A pay request from the server may want the holder's secret with the approval (cloud.c says
// which). Holding OK then opens this sheet instead of approving: eight presses of the three keys,
// shown as squares that fill. After the eighth the secret goes to the server, which checks it;
// the card neither knows nor keeps the right one. Holding the down key starts again, holding the
// up key refuses the payment, and the request's time keeps running throughout.

static void typed_wipe(void)
{
    volatile char *byte = s_typed;
    for (size_t i = 0; i < sizeof(s_typed); i++) byte[i] = 0;
    s_typed_len = 0;
}

// What is left of the request's time on the card.
static uint32_t pay_time_left(void)
{
    const uint32_t gone = lv_tick_elaps(s_pay_shown_at);
    return gone < (uint32_t)s_pay.ttl_ms ? (uint32_t)s_pay.ttl_ms - gone : 0;
}

static void on_pay_expired(lv_timer_t *timer);

// note: a line to read first, "" for none.
static void secret_open(const char *note)
{
    typed_wipe();
    uint32_t left = pay_time_left();
    if (left == 0) left = 1;
    s_mode = MODE_PAY_SECRET;
    s_secret_shown_at = lv_tick_get();
    ui_overlay_pay_secret(&s_pay, 0, (int)left, note);
    step_after(left, on_pay_expired);
}

static void on_pay_send_timeout(lv_timer_t *timer);

static void secret_send(void)
{
    step_cancel();
    s_mode = MODE_PAY_SENDING;
    ui_overlay_pay_sending(&s_pay);
    if (s_pay.via_usb) {
        // A computer plays the server: it is told that a secret was typed, never which keys.
        host_link_send("{\"t\":\"pay_ok\",\"id\":%d,\"secret\":1}", s_pay.id);
    } else {
        cloud_pay_approve(s_pay.id, s_typed);
    }
    typed_wipe();
    step_after(PAY_SEND_MAX_MS, on_pay_send_timeout);
}

static void on_secret_typed(lv_timer_t *timer)
{
    (void)timer;
    s_step_timer = NULL;
    secret_send();
}

static void secret_key(card_key_t key)
{
    static const char KEYS[] = { 'U', 'D', 'O' };   // card_key_t's order
    // The OK that opened the sheet may still be on its way up; and once all eight are in, the
    // secret is already on its way out.
    if (lv_tick_elaps(s_secret_shown_at) < SECRET_GUARD_MS || s_typed_len >= CARD_SECRET_LEN) return;
    s_typed[s_typed_len++] = KEYS[key];
    s_typed[s_typed_len] = '\0';
    ui_overlay_pay_secret_fill(s_typed_len);
    sfx_play(SFX_BLIP);
    if (s_typed_len == CARD_SECRET_LEN) step_after(SECRET_SEND_MS, on_secret_typed);
}

static void finish_pay(bool paid, const char *why);

static void secret_key_long(card_key_t key)
{
    if (lv_tick_elaps(s_secret_shown_at) < SECRET_GUARD_MS) return;
    if (key == CARD_KEY_DOWN) {
        // Start again. The press this hold began with was taken as a key: it goes with the rest,
        // and a secret that was about to be sent is not sent.
        secret_open("重新输入");
    } else if (key == CARD_KEY_UP) {
        finish_pay(false, "declined");
    }
    // OK held is no more than the press it began with.
}

static void handle_pay_result(int id, bool ok, bool from_usb, card_pay_why_t why, int left)
{
    // Only whoever sent the request may say how it ended.
    if (s_mode != MODE_PAY_SENDING || id != s_pay.id || from_usb != s_pay.via_usb) return;
    if (ok) {
        show_pay_done(true);
        return;
    }
    // The server wants the secret, or the one it got was wrong: while the request has time left
    // the holder types it (again). The server has counted a wrong one; the card only shows how
    // many tries it says are left.
    if ((why == CARD_PAY_WRONG_SECRET || why == CARD_PAY_NEED_SECRET) && pay_time_left() >= SECRET_AGAIN_MIN_MS) {
        char note[48] = "";
        if (why == CARD_PAY_WRONG_SECRET) {
            snprintf(note, sizeof(note), "口令不对，还能试 %d 次", left);
            sfx_play(SFX_LOSE);
        }
        s_pay.need_secret = true;
        secret_open(note);
        return;
    }
    step_cancel();
    s_mode = MODE_PAY_DONE;
    uint32_t show_ms = PAY_DONE_SHOW_MS;
    char detail[96];
    switch (why) {
    case CARD_PAY_WRONG_SECRET:
        snprintf(detail, sizeof(detail), "这一笔已作废。还能试 %d 次，再错卡会被冻结", left);
        ui_overlay_pay_failed("口令不对", detail);
        break;
    case CARD_PAY_NEED_SECRET:
        ui_overlay_pay_failed("没付成", "这一笔要输口令，时间不够了");
        break;
    case CARD_PAY_FROZEN:
        // The face says so from now on; the next profile says who froze it.
        s_guard.known = true;
        s_guard.frozen = true;
        ui_overlay_pay_failed("卡已冻结", "主人登录 clawhunt.store/card/pair 才能解冻");
        show_ms = GUARD_SHOW_MS;
        break;
    case CARD_PAY_NO_SECRET:
        s_guard.known = true;
        s_guard.secret_set = false;
        ui_overlay_pay_failed("没设口令", "主人登录 clawhunt.store/card/pair 给这张卡设口令");
        show_ms = GUARD_SHOW_MS;
        break;
    default:
        ui_overlay_pay_failed("没付成", "ClawHunt 没有接受这一笔，可能已经过期");
        break;
    }
    sfx_play(SFX_LOSE);
    step_after(show_ms, on_step_idle);
}

// How the server guards the card. Nothing here changes what the card can do: the server refuses
// what it refuses. The resting face says it, so a card that cannot pay does not just look idle.
static void handle_guard(const card_guard_t *guard)
{
    s_guard = *guard;
    if (s_mode == MODE_IDLE) show_rest();
}

static void finish_pay(bool paid, const char *why)
{
    typed_wipe();   // whichever way the request ends, nothing typed outlives it
    if (s_pay.remote) {
        finish_remote_pay(paid, why);
        return;
    }
    step_cancel();
    s_mode = MODE_PAY_DONE;
    ui_overlay_pay_done(&s_pay, paid);
    sfx_play(paid ? SFX_COIN : SFX_LOSE);
    if (paid) {
        host_link_send("{\"t\":\"pay_ok\",\"id\":%d}", s_pay.id);
    } else {
        host_link_send("{\"t\":\"pay_no\",\"id\":%d,\"why\":\"%s\"}", s_pay.id, why);
    }
    step_after(paid ? PAY_DONE_SHOW_MS : PAY_NO_SHOW_MS, on_step_idle);
}

static void on_pay_expired(lv_timer_t *timer)
{
    (void)timer;
    s_step_timer = NULL;
    finish_pay(false, "timeout");
}

// A sheet from the server only informs: it gives way to something that needs an answer (a pay
// request) and to what a computer sends. It is not lost: it waits, and comes back when the screen
// is free.
static void yield_notice(void)
{
    if (s_mode == MODE_OFFER && s_offer.remote) {
        s_later_offer = s_offer;
        s_has_later_offer = true;
        s_later_offer_at = s_offer_at;   // still only shown within a minute of its arrival
    } else if (s_mode == MODE_PAID && s_paid.remote) {
        later_paid_put(&s_paid, true);
        s_pending_level = 0;
    } else if (s_mode == MODE_LEVEL_UP && s_paid.remote) {
        // The payout itself was seen; the new level is on the face.
    } else {
        return;
    }
    rest();
}

static void handle_pay(const card_pay_t *pay)
{
    yield_notice();
    if (s_mode != MODE_IDLE) {
        if (pay->remote && !pay->via_usb) {
            cloud_pay_decision(pay->id, false, CLOUD_REASON_BUSY);
        } else {
            host_link_send("{\"t\":\"busy\",\"id\":%d}", pay->id);
        }
        return;
    }
    s_pay = *pay;
    s_mode = MODE_PAY;
    s_pay_shown_at = lv_tick_get();
    ui_overlay_pay(&s_pay);
    sfx_play(SFX_RING);
    step_after((uint32_t)s_pay.ttl_ms, on_pay_expired);
}

static void handle_say(const char *text)
{
    if (s_mode != MODE_IDLE) return;
    ui_card_say(text);
    sfx_play(SFX_BLIP);
}

// ---- settings menu and phone setup ----------------------------------------

static void menu_show(void)
{
    const char *const items[MENU_COUNT] = { "连 Wi-Fi", sfx_enabled() ? "声音\t开" : "声音\t关", "返回" };
    ui_overlay_menu(items, MENU_COUNT, s_menu_sel);
    step_after(MENU_IDLE_MS, on_step_idle);
}

static void setup_open(void)
{
    step_cancel();
    s_mode = MODE_SETUP;
    const card_setup_t starting = { .phase = CARD_SETUP_STARTING };
    ui_overlay_setup(&starting);
    setup_request(true, false);
}

static void menu_key(card_key_t key)
{
    if (key == CARD_KEY_UP) {
        s_menu_sel = (s_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
    } else if (key == CARD_KEY_DOWN) {
        s_menu_sel = (s_menu_sel + 1) % MENU_COUNT;
    } else if (s_menu_sel == 0) {
        setup_open();
        return;
    } else if (s_menu_sel == 1) {
        // Chosen on the card itself, so it is remembered across power-offs.
        const bool on = !sfx_enabled();
        sfx_set_enabled(on);
        (void)net_store_set("snd", on ? "1" : NULL);
        if (on) sfx_play(SFX_BLIP);
    } else {
        go_idle();
        return;
    }
    menu_show();
}

static void handle_key_long(card_key_t key)
{
    // While the secret is typed the link does not say which key: it would spell the secret out.
    host_link_send("{\"t\":\"key\",\"k\":\"%s\",\"long\":1}", s_mode == MODE_PAY_SECRET ? "*" : KEY_NAMES[key]);
    if (s_mode == MODE_PAY_SECRET) {
        secret_key_long(key);
        return;
    }
    if (s_mode == MODE_PAY && key == CARD_KEY_OK) {
        // OK held: this is the approval. The same guard as for a press applies.
        if (lv_tick_elaps(s_pay_shown_at) < PAY_GUARD_MS) return;
        if (s_pay.remote && s_pay.need_secret) {
            secret_open("");   // the server wants the holder's secret with it: typed first
            return;
        }
        finish_pay(true, "approved");
        return;
    }
    if (s_mode != MODE_IDLE || key != CARD_KEY_OK) return;
    // The press that closed a sheet is still down: it was an answer to the sheet, not a call for the menu.
    if (s_key_closed && lv_tick_elaps(s_key_closed_at) < HOLD_AFTER_CLOSE_MS) return;
    s_mode = MODE_MENU;
    s_menu_sel = 0;
    menu_show();
}

static void handle_setup(const card_setup_t *setup)
{
    if (setup->phase == CARD_SETUP_OFF) {
        if (s_mode != MODE_SETUP) return;
        go_idle_with_a_word();
        if (setup->reason == 1) {
            ui_card_say("Wi-Fi 连上了");
            sfx_play(SFX_COIN);
        } else if (setup->reason == 2) {
            ui_card_say("热点没开起来");
        }
        return;
    }
    // A computer can start setup too, but only while the card is resting.
    if (s_mode != MODE_SETUP && s_mode != MODE_IDLE && s_mode != MODE_MENU) return;
    step_cancel();
    s_mode = MODE_SETUP;
    ui_overlay_setup(setup);
    if (setup->phase == CARD_SETUP_JOINED || setup->phase == CARD_SETUP_DONE) {
        sfx_play(SFX_BLIP);
    } else if (setup->phase == CARD_SETUP_FAILED) {
        sfx_play(SFX_LOSE);
    }
}

static void handle_key(card_key_t key)
{
    host_link_send("{\"t\":\"key\",\"k\":\"%s\"}", s_mode == MODE_PAY_SECRET ? "*" : KEY_NAMES[key]);
    if (s_mode == MODE_PAY_SECRET) {
        secret_key(key);
        return;
    }
    if (s_mode == MODE_MENU) {
        menu_key(key);
        return;
    }
    if (s_mode == MODE_SETUP) {
        // Any key leaves setup; the screen follows the OFF event. If that event was
        // lost and setup is already over, leave the screen here.
        if (setup_active()) {
            setup_request(false, false);
        } else {
            go_idle();
        }
        return;
    }
    if (s_mode == MODE_PAY) {
        // A key that was already on its way down when the request appeared must
        // not move money: presses in the first moment are ignored.
        if (lv_tick_elaps(s_pay_shown_at) < PAY_GUARD_MS) return;
        if (key == CARD_KEY_OK) {
            // Paying takes a held key (handle_key_long): a brush against the button
            // must not pay. The press itself only says to keep holding.
            ui_overlay_pay_holding();
            return;
        }
        finish_pay(false, "declined");
        return;
    }
    if (s_mode == MODE_OFFER && s_offer.remote) {
        // A task the server told of: the agent bids or claims with its own key, the card cannot.
        // Any key only closes the sheet; the status line then says what the agent is doing.
        sfx_play(SFX_BLIP);
        host_link_send("{\"t\":\"seen\",\"id\":%d}", s_offer.id);
        s_key_closed = true;
        s_key_closed_at = lv_tick_get();
        go_idle();
        return;
    }
    if (s_mode == MODE_OFFER && key == CARD_KEY_OK) {
        sfx_play(SFX_BLIP);
        start_work(CARD_LINE_BID);
        host_link_send("{\"t\":\"accept\",\"id\":%d}", s_offer.id);
        return;
    }
    if (s_mode != MODE_IDLE) return;
    sfx_play(SFX_BLIP);
    // A card that cannot pay says where that is put right.
    if (s_guard.known && (s_guard.frozen || !s_guard.secret_set)) {
        ui_card_say(s_guard.frozen ? "去网站 /card/pair 解冻" : "去网站 /card/pair 设口令");
        return;
    }
    // Every key answers with something on the screen; the up and down keys, which
    // do nothing on the resting card, say where the settings are.
    if (key == CARD_KEY_OK) {
        ui_card_say(QUIPS[s_quip++ % (sizeof(QUIPS) / sizeof(QUIPS[0]))]);
    } else {
        ui_card_say("按住 OK 键打开设置");
    }
}

// The screen is free: put the new-task sheet up.
static void show_offer(const card_offer_t *offer, uint32_t arrived_at)
{
    s_offer = *offer;
    s_offer_at = arrived_at;
    s_mode = MODE_OFFER;
    ui_overlay_offer(&s_offer);
    sfx_play(SFX_RING);
    // A sheet from the server only informs: when its time is up it closes, and nothing was missed.
    step_after((uint32_t)s_offer.ttl_ms, s_offer.remote ? on_step_idle : on_offer_expired);
}

static void handle_offer(const card_offer_t *offer)
{
    if (!offer->remote) yield_notice();   // a computer's offer goes before what the server only tells
    if (s_mode != MODE_IDLE) {
        if (offer->remote) {
            s_later_offer = *offer;
            s_has_later_offer = true;
            s_later_offer_at = lv_tick_get();
        } else {
            host_link_send("{\"t\":\"busy\",\"id\":%d}", offer->id);
        }
        return;
    }
    show_offer(offer, lv_tick_get());
}

// The server said what the agent is doing now. Nothing here changes the card's mode: the line
// and the crab show it when the card is at rest.
static void handle_task(const card_task_t *task)
{
    if (task->reset) {
        s_later_paid_count = 0;
        s_has_later_offer = false;
    }
    s_task = *task;
    if (s_task.id > 0 && s_task.phase == CARD_JOB_LOST) {
        // No payout will come for this one. It is said once and nothing stays on the line.
        const bool called_off = s_task.called_off;
        s_task.id = 0;
        if (s_mode != MODE_IDLE) return;
        show_rest();
        ui_card_say(called_off ? "这单结束了，没有到账" : "这单没中");
        return;
    }
    if (s_mode != MODE_IDLE) return;
    show_rest();
    if (s_task.id > 0 && (s_task.phase == CARD_JOB_WON || s_task.phase == CARD_JOB_OWN)) sfx_play(SFX_BLIP);
}

static void handle_job(card_job_phase_t phase, int progress)
{
    // The owner can hand a job over while the card is idle; everything else only
    // makes sense once a job is under way.
    if (phase == CARD_JOB_OWN) yield_notice();   // a sheet the server only tells does not stand in the way
    if (phase == CARD_JOB_OWN && s_mode == MODE_IDLE) {
        sfx_play(SFX_BLIP);
        start_work(CARD_LINE_OWN);
        return;
    }
    if (s_mode != MODE_WORK) return;

    char line[40];
    switch (phase) {
    case CARD_JOB_BID:  ui_card_set_status(CARD_LINE_BID, 0); break;
    case CARD_JOB_WON:  ui_card_set_status(CARD_LINE_WON, 0); sfx_play(SFX_BLIP); break;
    case CARD_JOB_OWN:  ui_card_set_status(CARD_LINE_OWN, 0); break;
    case CARD_JOB_DONE: ui_card_set_status(CARD_LINE_DONE, 100); break;
    case CARD_JOB_LOST:
        // No payout will come for this one. Without this the card would stay on "bidding", refuse new tasks and
        // hide a pairing code until some payout arrived.
        go_idle_with_a_word();
        ui_card_say("这单没中");
        break;
    case CARD_JOB_HELD:
        // The buyer has accepted and the payout is on hold for days. The agent is free again: the card rests and
        // takes the next task, and its status line says what it waits for until the next thing changes it.
        // Without this the card said "delivered, waiting for acceptance" until the payout, days after the acceptance.
        go_idle_with_a_word();
        ui_card_set_status(CARD_LINE_HELD, 0);
        break;
    case CARD_JOB_WORK:
        snprintf(line, sizeof(line), CARD_LINE_WORK_PART " %d%%", progress);
        ui_card_set_status(line, progress);
        break;
    case CARD_JOB_DISPUTED:
        break;   // only the server says this (handle_task)
    }
}

static void handle_paid(const card_paid_t *paid)
{
    if (paid->remote) {
        // The server says a payout once: it waits for the screen instead of being dropped.
        if (s_mode != MODE_IDLE) {
            later_paid_put(paid, false);
            return;
        }
        show_paid(paid);
        return;
    }
    yield_notice();   // a computer's payout goes before what the server only tells
    // An open offer or a pay request owns the screen until it is answered.
    if (s_mode == MODE_OFFER || s_mode == MODE_PAY || s_mode == MODE_PAY_SECRET || s_mode == MODE_PAY_SENDING) return;
    show_paid(paid);
}

static void show_paid(const card_paid_t *paid)
{
    step_cancel();
    ui_card_set_busy(false);
    ui_card_set_status(CARD_LINE_IDLE, 0);
    s_paid = *paid;
    s_mode = MODE_PAID;
    s_pending_level = paid->new_level;
    ui_overlay_paid(paid);
    sfx_play(SFX_COIN);
    step_after(PAID_SHOW_MS, on_paid_shown);
}

static void handle_net(int state)
{
    static const char *const LABELS[] = { CARD_NET_OFF, CARD_NET_JOINING, CARD_NET_ONLINE, CARD_NET_FAILED };
    ui_card_set_net(LABELS[state >= 0 && state < 4 ? state : 0]);
}

// What the card fetched by itself is shown unless a computer is feeding it.
static void handle_net_stats(const card_stats_t *stats)
{
    if (s_host_stats_seen && lv_tick_elaps(s_host_stats_at) < HOST_STATS_WIN_MS) return;
    ui_card_set_stats(stats);
}

static void handle_pairing(const card_pairing_t *pairing)
{
    if (!pairing->code[0]) {
        // The code ran out or was refused and no new one came: a dead code is not left up.
        // At rest the only sheet is the pairing code, so that is the one being closed.
        const bool was_shown = s_pairing_shown;
        s_pairing_shown = false;
        if (was_shown && s_mode == MODE_IDLE) ui_overlay_close();
        return;
    }
    s_pairing = *pairing;
    s_pairing_shown = true;
    if (s_mode == MODE_IDLE) ui_overlay_pairing(&s_pairing);
}

static void handle_paired(void)
{
    s_pairing_shown = false;
    if (s_mode != MODE_IDLE) return;
    ui_overlay_close();
    ui_card_say("绑定好了");
    sfx_play(SFX_COIN);
}

// Events that prove a computer is attached and feeding the card.
static bool from_computer(const card_event_t *event)
{
    switch (event->type) {
    case CARD_EV_KEY:
    case CARD_EV_NET:
    case CARD_EV_NET_STATS:
    case CARD_EV_PAIRING:
    case CARD_EV_PAIRED:
    case CARD_EV_PAY_RESULT:
    case CARD_EV_KEY_LONG:
    case CARD_EV_SETUP:
    case CARD_EV_TASK:
    case CARD_EV_GUARD:
        return false;
    case CARD_EV_PAY:
        return false;   // may come from either side; see handle_pay
    case CARD_EV_OFFER:
        return !event->offer.remote;   // the server's, or a computer playing the server
    case CARD_EV_PAID:
        return !event->paid.remote;
    default:
        return true;
    }
}

static void apply(const card_event_t *event)
{
    if (from_computer(event)) link_seen();
    switch (event->type) {
    case CARD_EV_KEY:   handle_key(event->key); break;
    case CARD_EV_KEY_LONG: handle_key_long(event->key); break;
    case CARD_EV_SETUP: handle_setup(&event->setup); break;
    case CARD_EV_STATS:
        s_host_stats_seen = true;
        s_host_stats_at = lv_tick_get();
        ui_card_set_stats(&event->stats);
        break;
    case CARD_EV_NET:       handle_net(event->net_state); break;
    case CARD_EV_NET_STATS: handle_net_stats(&event->stats); break;
    case CARD_EV_PAIRING:   handle_pairing(&event->pairing); break;
    case CARD_EV_PAIRED:    handle_paired(); break;
    case CARD_EV_PAY_RESULT:
        handle_pay_result(event->pay_result.id, event->pay_result.ok, event->pay_result.from_usb,
                          event->pay_result.why, event->pay_result.left);
        break;
    case CARD_EV_GUARD: handle_guard(&event->guard); break;
    case CARD_EV_OFFER: handle_offer(&event->offer); break;
    case CARD_EV_JOB:   handle_job(event->job.phase, event->job.progress); break;
    case CARD_EV_PAID:  handle_paid(&event->paid); break;
    case CARD_EV_TASK:  handle_task(&event->task); break;
    case CARD_EV_PING:  host_link_send_hello(); break;
    case CARD_EV_PAY:   handle_pay(&event->pay); break;
    case CARD_EV_SAY:   handle_say(event->say); break;
    }
}

static bool told_once(const card_event_t *event)
{
    return event->type == CARD_EV_TASK || (event->type == CARD_EV_OFFER && event->offer.remote) ||
           (event->type == CARD_EV_PAID && event->paid.remote);
}

static void app_task(void *arg)
{
    (void)arg;
    card_event_t event;
    for (;;) {
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        // What the server says, it says once: the display is waited for longer before that is given up.
        int tries = told_once(&event) ? TOLD_ONCE_LOCK_TRIES : 1;
        bool locked = false;
        while (!locked && tries-- > 0) locked = bsp_lvgl_lock(1000);
        if (!locked) {
            ESP_LOGW(TAG, "dropped an event: display busy");
            continue;
        }
        apply(&event);
        bsp_lvgl_unlock();
    }
}

esp_err_t card_app_start(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(card_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    ui_card_init();
    // Until something tells the card its agent's numbers, show the identity this firmware was built for and no
    // numbers: without a level the face sets "--" where the number stands, and a fact of "-" is left out.
    const card_stats_t placeholder = {
        .name = CARD_AGENT_NAME,
        .has_cells = true,
        .cells = { { "deals", "-" }, { "settled", "-" }, { "", "-" } },
        .money_label = "NO DATA YET",
    };
    ui_card_set_stats(&placeholder);
    go_idle();
    bsp_lvgl_unlock();

    if (xTaskCreate(app_task, "card_app", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void card_app_note_physical_key(void)
{
    s_physical_keys = s_physical_keys + 1;
}

unsigned card_app_physical_keys(void)
{
    return s_physical_keys;
}

bool card_app_try_post(const card_event_t *event)
{
    return s_queue && xQueueSend(s_queue, event, 0) == pdTRUE;
}

void card_app_post(const card_event_t *event)
{
    (void)card_app_try_post(event);
}
