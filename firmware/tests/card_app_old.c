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

static void go_idle(void)
{
    step_cancel();
    ui_overlay_close();
    ui_card_set_busy(false);
    // A card that knows no network says how to give it one.
    ui_card_set_status(net_saved_count() > 0 ? "摸鱼中，等单…" : "长按 OK 键连 Wi-Fi", 0);
    s_mode = MODE_IDLE;
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

static void handle_pay_result(int id, bool ok, bool from_usb)
{
    // Only whoever sent the request may say how it ended.
    if (s_mode != MODE_PAY_SENDING || id != s_pay.id || from_usb != s_pay.via_usb) return;
    if (ok) {
        show_pay_done(true);
        return;
    }
    step_cancel();
    s_mode = MODE_PAY_DONE;
    ui_overlay_pay_failed("没付成", "ClawHunt 没有接受这一笔，可能已经过期");
    sfx_play(SFX_LOSE);
    step_after(PAY_DONE_SHOW_MS, on_step_idle);
}

static void finish_pay(bool paid, const char *why)
{
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

static void handle_pay(const card_pay_t *pay)
{
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
    host_link_send("{\"t\":\"key\",\"k\":\"%s\",\"long\":1}", KEY_NAMES[key]);
    if (s_mode == MODE_PAY && key == CARD_KEY_OK) {
        // OK held: this is the approval. The same guard as for a press applies.
        if (lv_tick_elaps(s_pay_shown_at) < PAY_GUARD_MS) return;
        finish_pay(true, "approved");
        return;
    }
    if (s_mode != MODE_IDLE || key != CARD_KEY_OK) return;
    s_mode = MODE_MENU;
    s_menu_sel = 0;
    menu_show();
}

static void handle_setup(const card_setup_t *setup)
{
    if (setup->phase == CARD_SETUP_OFF) {
        if (s_mode != MODE_SETUP) return;
        go_idle();
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
    host_link_send("{\"t\":\"key\",\"k\":\"%s\"}", KEY_NAMES[key]);
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
    if (s_mode == MODE_OFFER && key == CARD_KEY_OK) {
        sfx_play(SFX_BLIP);
        start_work("竞标中…");
        host_link_send("{\"t\":\"accept\",\"id\":%d}", s_offer.id);
        return;
    }
    if (s_mode != MODE_IDLE) return;
    sfx_play(SFX_BLIP);
    // Every key answers with something on the screen; the up and down keys, which
    // do nothing on the resting card, say where the settings are.
    if (key == CARD_KEY_OK) {
        ui_card_say(QUIPS[s_quip++ % (sizeof(QUIPS) / sizeof(QUIPS[0]))]);
    } else {
        ui_card_say("按住 OK 键打开设置");
    }
}

static void handle_offer(const card_offer_t *offer)
{
    if (s_mode != MODE_IDLE) {
        host_link_send("{\"t\":\"busy\",\"id\":%d}", offer->id);
        return;
    }
    s_offer = *offer;
    s_mode = MODE_OFFER;
    ui_overlay_offer(&s_offer);
    sfx_play(SFX_RING);
    step_after((uint32_t)s_offer.ttl_ms, on_offer_expired);
}

static void handle_job(card_job_phase_t phase, int progress)
{
    // The owner can hand a job over while the card is idle; everything else only
    // makes sense once a job is under way.
    if (phase == CARD_JOB_OWN && s_mode == MODE_IDLE) {
        sfx_play(SFX_BLIP);
        start_work("收到，这就开工");
        return;
    }
    if (s_mode != MODE_WORK) return;

    char line[40];
    switch (phase) {
    case CARD_JOB_BID:  ui_card_set_status("竞标中…", 0); break;
    case CARD_JOB_WON:  ui_card_set_status("中标了，开工", 0); sfx_play(SFX_BLIP); break;
    case CARD_JOB_OWN:  ui_card_set_status("收到，这就开工", 0); break;
    case CARD_JOB_DONE: ui_card_set_status("交付了，等验收", 100); break;
    case CARD_JOB_LOST:
        // No payout will come for this one. Without this the card would stay on "bidding", refuse new tasks and
        // hide a pairing code until some payout arrived.
        go_idle();
        ui_card_say("这单没中");
        break;
    case CARD_JOB_HELD:
        // The buyer has accepted and the payout is on hold for days. The agent is free again: the card rests and
        // takes the next task, and its status line says what it waits for until the next thing changes it.
        // Without this the card said "delivered, waiting for acceptance" until the payout, days after the acceptance.
        go_idle();
        ui_card_set_status("验收了，等放款", 0);
        break;
    case CARD_JOB_WORK:
        snprintf(line, sizeof(line), "干活中 %d%%", progress);
        ui_card_set_status(line, progress);
        break;
    }
}

static void handle_paid(const card_paid_t *paid)
{
    // An open offer or a pay request owns the screen until it is answered.
    if (s_mode == MODE_OFFER || s_mode == MODE_PAY || s_mode == MODE_PAY_SENDING) return;
    step_cancel();
    ui_card_set_busy(false);
    ui_card_set_status("摸鱼中，等单…", 0);
    s_mode = MODE_PAID;
    s_pending_level = paid->new_level;
    ui_overlay_paid(paid);
    sfx_play(SFX_COIN);
    step_after(PAID_SHOW_MS, on_paid_shown);
}

static void handle_net(int state)
{
    static const char *const LABELS[] = { "", "连网中", "Wi-Fi", "没连上" };
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
static bool from_computer(card_event_type_t type)
{
    switch (type) {
    case CARD_EV_KEY:
    case CARD_EV_NET:
    case CARD_EV_NET_STATS:
    case CARD_EV_PAIRING:
    case CARD_EV_PAIRED:
    case CARD_EV_PAY_RESULT:
    case CARD_EV_KEY_LONG:
    case CARD_EV_SETUP:
        return false;
    case CARD_EV_PAY:
        return false;   // may come from either side; see handle_pay
    default:
        return true;
    }
}

static void apply(const card_event_t *event)
{
    if (from_computer(event->type)) link_seen();
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
        handle_pay_result(event->pay_result.id, event->pay_result.ok, event->pay_result.from_usb);
        break;
    case CARD_EV_OFFER: handle_offer(&event->offer); break;
    case CARD_EV_JOB:   handle_job(event->job.phase, event->job.progress); break;
    case CARD_EV_PAID:  handle_paid(&event->paid); break;
    case CARD_EV_PING:  host_link_send_hello(); break;
    case CARD_EV_PAY:   handle_pay(&event->pay); break;
    case CARD_EV_SAY:   handle_say(event->say); break;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    card_event_t event;
    for (;;) {
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (!bsp_lvgl_lock(1000)) {
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
    // Until the gateway connects, show the identity this firmware was built for.
    const card_stats_t placeholder = {
        .name = CARD_AGENT_NAME,
        .has_cells = true,
        .cells = { { "成交", "-" }, { "成功率", "-" }, { "已结算", "-" } },
        .money_label = "等网关连上",
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

void card_app_post(const card_event_t *event)
{
    if (s_queue) (void)xQueueSend(s_queue, event, 0);
}
