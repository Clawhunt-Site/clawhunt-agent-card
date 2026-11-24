// tests/card_stubs/card_host.c -- the platform under the card's code, on a computer (see card_host.h).
//
// Included once, by the test's own source file. main/ui_card.c, main/sfx.c, main/net.c, main/setup.c
// and main/host_link.c are not compiled: their functions are here and only write down what was asked.
#include "card_host.h"
#include "esp_random.h"

#include "bsp_display.h"
#include "card_model.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host_link.h"
#include "lvgl.h"
#include "net.h"
#include "setup.h"
#include "sfx.h"
#include "ui_card.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- what the card put out -----------------------------------------------------

char host_log[HOST_LOG_MAX][160];
int host_log_count;

void host_note(const char *fmt, ...)
{
    if (host_log_count >= HOST_LOG_MAX) {
        fprintf(stderr, "host_log is full\n");
        exit(2);
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(host_log[host_log_count++], sizeof(host_log[0]), fmt, args);
    va_end(args);
}

void host_log_clear(void)
{
    host_log_count = 0;
}

void host_frame(void)
{
    host_note(HOST_FRAME);
}

// ---- time and LVGL's timers --------------------------------------------------------

struct lv_timer {
    lv_timer_cb_t cb;
    uint32_t due;
    uint32_t period;
    int32_t repeat;   // -1: for ever
    bool live;
};

static uint32_t s_now_ms = 1000;
static struct lv_timer s_timers[16];

uint32_t host_now_ms(void)
{
    return s_now_ms;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)s_now_ms * 1000;
}

uint32_t lv_tick_get(void)
{
    return s_now_ms;
}

uint32_t lv_tick_elaps(uint32_t since)
{
    return s_now_ms - since;
}

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period_ms, void *user_data)
{
    (void)user_data;
    for (size_t i = 0; i < sizeof(s_timers) / sizeof(s_timers[0]); i++) {
        if (s_timers[i].live) continue;
        s_timers[i] = (struct lv_timer){ .cb = cb, .due = s_now_ms + period_ms, .period = period_ms, .repeat = -1, .live = true };
        return &s_timers[i];
    }
    fprintf(stderr, "out of timers\n");
    exit(2);
}

void lv_timer_delete(lv_timer_t *timer)
{
    timer->live = false;
}

void lv_timer_set_repeat_count(lv_timer_t *timer, int32_t count)
{
    timer->repeat = count;
}

void host_advance(uint32_t ms)
{
    const uint32_t target = s_now_ms + ms;
    for (;;) {
        struct lv_timer *next = NULL;
        for (size_t i = 0; i < sizeof(s_timers) / sizeof(s_timers[0]); i++) {
            struct lv_timer *timer = &s_timers[i];
            if (timer->live && timer->due <= target && (!next || timer->due < next->due)) next = timer;
        }
        if (!next) break;
        s_now_ms = next->due;
        const bool last = next->repeat > 0 && --next->repeat == 0;
        next->due += next->period;
        next->cb(next);
        host_frame();
        // As in LVGL: a timer that has run its count is deleted after its callback returns.
        if (last) next->live = false;
    }
    s_now_ms = target;
}

bool bsp_lvgl_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void bsp_lvgl_unlock(void)
{
}

// ---- FreeRTOS: queues and tasks ----------------------------------------------------

struct host_queue {
    size_t item;
    size_t cap;
    size_t count;
    unsigned char *data;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct host_queue *queue = calloc(1, sizeof(*queue));
    queue->item = item_size;
    queue->cap = length;
    queue->data = calloc(length, item_size);
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    (void)wait;
    if (queue->count >= queue->cap) return pdFALSE;
    memcpy(queue->data + queue->count * queue->item, item, queue->item);
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)wait;
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->data, queue->item);
    queue->count--;
    memmove(queue->data, queue->data + queue->item, queue->count * queue->item);
    return pdTRUE;
}

struct host_task {
    TaskFunction_t fn;
    void *arg;
    char name[16];
};

static struct host_task s_tasks[4];
static size_t s_task_count;
static jmp_buf s_round;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                       TaskHandle_t *handle)
{
    (void)stack;
    (void)priority;
    struct host_task *task = &s_tasks[s_task_count++];
    task->fn = fn;
    task->arg = arg;
    snprintf(task->name, sizeof(task->name), "%s", name);
    if (handle) *handle = task;
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear;
    (void)wait;
    longjmp(s_round, 1);   // the task's round is over
}

uint32_t host_paused_ms;

void vTaskDelay(TickType_t ticks)
{
    host_paused_ms += (uint32_t)ticks;
    host_advance((uint32_t)ticks);
}

int host_task_round(const char *name)
{
    for (size_t i = 0; i < s_task_count; i++) {
        if (strcmp(s_tasks[i].name, name) != 0) continue;
        if (setjmp(s_round) == 0) s_tasks[i].fn(s_tasks[i].arg);
        return 1;
    }
    return 0;
}

// ---- the HTTP client -----------------------------------------------------------------
//
// It behaves as ESP-IDF 5.5's esp_http_client does wherever the card can tell the difference: the
// events it sends and when, the error each stage fails with, and the status code that stage
// leaves behind.

struct esp_http_client {
    http_event_handle_cb handler;
    char url[512];
    esp_http_client_method_t method;
    char auth[160];
    char boot[20];
    char body[512];
    bool has_body;
    int status;
    int timeout_ms;
    bool open;   // its connection is open
    bool gone;   // it was cleaned up: the card must not use it any more
};

int host_http_connections;
int host_http_lost;
int host_http_closed;
int host_http_closing;
int host_http_closed_slowly_ms;
bool host_http_ends_connection;
bool host_http_cuts_body;
int host_http_no_client;

char host_last_boot[20];
uint32_t host_random = 0x0a1b2c3d;

uint32_t esp_random(void)
{
    const uint32_t value = host_random;
    host_random += 0x01010101;
    return value;
}

static struct esp_http_client s_http;
static host_http_fn s_http_fn;

void host_http_set(host_http_fn fn)
{
    s_http_fn = fn;
}

bool host_http_holds(const char *text)
{
    return strstr(s_http.url, text) || strstr(s_http.auth, text) || strstr(s_http.body, text);
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

// The client the card passes: one that it made and has not cleaned up. Anything else ends the test,
// as it would end in a crash on the card.
static struct esp_http_client *in_use(esp_http_client_handle_t client)
{
    if (client && !client->gone) return client;
    fprintf(stderr, "the card used an HTTP client that it had cleaned up, or none\n");
    exit(2);
}

static void tell(struct esp_http_client *client, esp_http_client_event_id_t id)
{
    esp_http_client_event_t event = { .event_id = id };
    client->handler(&event);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    if (host_http_no_client > 0) {
        host_http_no_client--;
        return NULL;   // no memory for one
    }
    memset(&s_http, 0, sizeof(s_http));   // no connection yet, and no status
    s_http.handler = config->event_handler;
    s_http.timeout_ms = config->timeout_ms;
    return &s_http;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    in_use(client);
    // The real client frees what it held. This one is emptied, so that a test can ask whether
    // anything is still held, and marked: using it again ends the test.
    memset(client, 0, sizeof(*client));
    client->gone = true;
    return ESP_OK;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url)
{
    snprintf(in_use(client)->url, sizeof(client->url), "%s", url);
    return ESP_OK;
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method)
{
    in_use(client)->method = method;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    in_use(client);
    if (strcmp(key, "Authorization") == 0) snprintf(client->auth, sizeof(client->auth), "%s", value);
    if (strcmp(key, "X-Card-Boot") == 0) snprintf(client->boot, sizeof(client->boot), "%s", value);
    return ESP_OK;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key)
{
    in_use(client);
    if (strcmp(key, "Authorization") == 0) memset(client->auth, 0, sizeof(client->auth));
    if (strcmp(key, "X-Card-Boot") == 0) memset(client->boot, 0, sizeof(client->boot));
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len)
{
    in_use(client)->has_body = data != NULL;
    // The real client only keeps the pointer it is given: once it is given another, nothing of the
    // body before is with it.
    memset(client->body, 0, sizeof(client->body));
    if (data) snprintf(client->body, sizeof(client->body), "%.*s", len, data);
    return ESP_OK;
}

// A request failed: the client says so, and what its connection is worth after that is not known.
static esp_err_t failed(struct esp_http_client *client, esp_err_t err)
{
    client->open = false;
    tell(client, HTTP_EVENT_ERROR);
    return err;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    static char reply[8192];
    reply[0] = '\0';
    snprintf(host_last_boot, sizeof(host_last_boot), "%s", in_use(client)->boot);
    const bool kept = client->open;
    if (kept && host_http_closed) {
        // The server closed this connection while it was idle, and the card finds out now: its
        // request goes into a closed connection, and the server never sees it. Usually the request
        // is written and the read fails (the status is then -1); sometimes the write itself fails
        // (the status is then still the one of the answer before). It fails at once, unless a test
        // has the write stall first.
        host_http_lost++;
        host_advance((uint32_t)host_http_closed_slowly_ms);
        host_http_closed_slowly_ms = 0;
        if (host_http_closed == HOST_HTTP_CLOSED_AT_WRITE) return failed(client, ESP_ERR_HTTP_WRITE_DATA);
        tell(client, HTTP_EVENT_HEADERS_SENT);
        client->status = -1;
        return failed(client, host_http_closed == HOST_HTTP_CLOSED_FIN ? ESP_ERR_HTTP_CONNECTION_CLOSED : ESP_ERR_HTTP_FETCH_HEADER);
    }
    // What becomes of the request is the test's to say.
    const int answer = s_http_fn ? s_http_fn(client->method == HTTP_METHOD_POST ? "POST" : "GET", client->url, client->auth,
                                             client->has_body ? client->body : NULL, reply, sizeof(reply))
                                 : -1;
    if (!kept) {
        // The client has no open connection: it opens one, or fails to (its status stays as it was).
        host_http_connections++;
        if (answer == -1 || answer == 0) return failed(client, ESP_ERR_HTTP_CONNECT);
        client->open = true;
        host_http_closed = 0;   // a new connection: the server has not closed this one
        tell(client, HTTP_EVENT_ON_CONNECTED);
    }
    tell(client, HTTP_EVENT_HEADERS_SENT);   // the request is written,
    client->status = -1;                     // and from here on the status is this request's
    if (answer == HOST_HTTP_TIMEOUT) {
        host_advance((uint32_t)client->timeout_ms);   // nothing came back until the client gave up waiting
        return failed(client, ESP_ERR_HTTP_EAGAIN);
    }
    if (answer <= -1000) {
        host_advance((uint32_t)(-1000 - answer));     // the connection was reset after this long, with nothing heard
        return failed(client, ESP_ERR_HTTP_FETCH_HEADER);
    }
    if (answer <= 0) return failed(client, ESP_ERR_HTTP_FETCH_HEADER);   // on a kept connection: the read failed
    client->status = answer;
    tell(client, HTTP_EVENT_ON_HEADER);   // what the server sends back begins with its headers
    // As ESP-IDF 5.5 does with max_authorization_retries = -1, which the card sets: a 401 ends the
    // request as a failure once the headers are in, before any of the body. The status stays readable.
    if (client->status == 401) return failed(client, ESP_FAIL);
    const int total = (int)strlen(reply);
    if (host_http_cuts_body) {
        // The connection broke in the middle of the body: half of it came.
        host_http_cuts_body = false;
        esp_http_client_event_t half = { .event_id = HTTP_EVENT_ON_DATA, .data = reply, .data_len = total / 2 };
        if (half.data_len > 0) client->handler(&half);
        return failed(client, ESP_ERR_HTTP_INCOMPLETE_DATA);
    }
    // The body arrives in pieces, as it does from the network.
    for (int sent = 0; sent < total; sent += 97) {
        esp_http_client_event_t event = { .event_id = HTTP_EVENT_ON_DATA, .data = reply + sent,
                                          .data_len = total - sent < 97 ? total - sent : 97 };
        client->handler(&event);
    }
    tell(client, HTTP_EVENT_ON_FINISH);
    if (host_http_ends_connection) {
        // The answer ended the connection: the client closes it, and keeps its handle.
        host_http_ends_connection = false;
        client->open = false;
        tell(client, HTTP_EVENT_DISCONNECTED);
    }
    if (host_http_closing) {
        // The server closes the connection behind its answer; the card does not know.
        host_http_closed = host_http_closing;
        host_http_closing = 0;
    }
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return in_use(client)->status;
}

// ---- the key-value store ---------------------------------------------------------------

static struct {
    char key[16];
    char value[160];
    bool set;
    int writes;
} s_store[16];

static int store_slot(const char *key)
{
    for (int i = 0; i < 16; i++) {
        if (s_store[i].key[0] && strcmp(s_store[i].key, key) == 0) return i;
    }
    for (int i = 0; i < 16; i++) {
        if (s_store[i].key[0]) continue;
        snprintf(s_store[i].key, sizeof(s_store[i].key), "%s", key);
        return i;
    }
    fprintf(stderr, "out of store slots\n");
    exit(2);
}

esp_err_t net_store_get(const char *key, char *out, size_t size)
{
    const int i = store_slot(key);
    snprintf(out, size, "%s", s_store[i].set ? s_store[i].value : "");
    return s_store[i].set ? ESP_OK : ESP_FAIL;
}

esp_err_t net_store_set(const char *key, const char *value)
{
    const int i = store_slot(key);
    s_store[i].set = value && value[0];
    snprintf(s_store[i].value, sizeof(s_store[i].value), "%s", s_store[i].set ? value : "");
    s_store[i].writes++;
    return ESP_OK;
}

const char *host_store_peek(const char *key)
{
    return s_store[store_slot(key)].value;
}

int host_store_writes(const char *key)
{
    return s_store[store_slot(key)].writes;
}

void host_store_put(const char *key, const char *value)
{
    const int i = store_slot(key);
    s_store[i].set = true;
    snprintf(s_store[i].value, sizeof(s_store[i].value), "%s", value);
}

bool host_store_holds(const char *text)
{
    for (int i = 0; i < 16; i++) {
        if (s_store[i].key[0] && strstr(s_store[i].value, text)) return true;
    }
    return false;
}

// ---- network, setup, sound, the USB link: knobs and notes ----------------------------------

bool host_online = true;
int host_saved_nets = 1;
static bool s_sound;

int net_saved_count(void)
{
    return host_saved_nets;
}

bool net_online(void)
{
    return host_online;
}

void net_service(void)
{
}

void setup_request(bool on, bool allow_lan)
{
    host_note("setup request on=%d lan=%d", on, allow_lan);
}

bool host_setup_active;

bool setup_active(void)
{
    return host_setup_active;
}

void setup_service(void)
{
}

void sfx_set_enabled(bool enabled)
{
    s_sound = enabled;
}

bool sfx_enabled(void)
{
    return s_sound;
}

void sfx_play(sfx_id_t id)
{
    static const char *const NAMES[] = { "BLIP", "RING", "COIN", "LEVEL_UP", "LOSE", "PAID_OUT" };
    host_note("sfx %s", NAMES[id]);
}

void host_link_send(const char *fmt, ...)
{
    char line[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    host_note("usb %.150s", line);
}

void host_link_send_hello(void)
{
    host_note("usb hello");
}

// ---- the screen: what was asked for, in words ------------------------------------------------

void ui_card_init(void)
{
}

void ui_card_set_stats(const card_stats_t *stats)
{
    host_note("stats %s lv%d money=%d", stats->name, stats->level, stats->today_cents);
}

void ui_card_set_status(const char *text, int progress)
{
    host_note("status %s|%d", text, progress);
}

void ui_card_set_busy(bool busy)
{
    host_note("busy %d", busy);
}

void ui_card_say(const char *text)
{
    host_note("say %s", text);
}

void ui_card_set_link(bool up)
{
    host_note("link %d", up);
}

void ui_card_set_net(const char *text)
{
    host_note("net %s", text);
}

void ui_overlay_offer(const card_offer_t *offer)
{
    host_note("overlay offer #%d remote=%d direct=%d amt=%d ttl=%d %s", offer->id, offer->remote, offer->direct,
              offer->amount_cents, offer->ttl_ms, offer->title);
}

void ui_overlay_paid(const card_paid_t *paid)
{
    host_note("overlay paid #%d net=%d lvl=%d %s", paid->id, paid->net_cents, paid->new_level, paid->title);
}

void ui_overlay_level_up(int level)
{
    host_note("overlay level_up %d", level);
}

void ui_overlay_miss(const card_offer_t *offer)
{
    host_note("overlay miss #%d", offer->id);
}

void ui_overlay_pay(const card_pay_t *pay)
{
    host_note("overlay pay #%d amt=%d %s", pay->id, pay->amount_cents, pay->payee);
}

void ui_overlay_pay_holding(void)
{
    host_note("overlay pay_holding");
}

// The squares only: the screen is never told which keys were pressed.
void ui_overlay_pay_secret(const card_pay_t *pay, int filled, int left_ms, const char *note)
{
    host_note("overlay pay_secret #%d filled=%d left=%d %s", pay->id, filled, left_ms, note);
}

void ui_overlay_pay_secret_fill(int filled)
{
    host_note("overlay pay_secret_fill %d", filled);
}

void ui_overlay_pay_done(const card_pay_t *pay, bool paid)
{
    host_note("overlay pay_done #%d paid=%d", pay->id, paid);
}

void ui_overlay_pay_sending(const card_pay_t *pay)
{
    host_note("overlay pay_sending #%d", pay->id);
}

void ui_overlay_pay_failed(const char *title, const char *detail)
{
    (void)detail;
    host_note("overlay pay_failed %s", title);
}

void ui_overlay_pairing(const card_pairing_t *pairing)
{
    host_note("overlay pairing %s", pairing->code);
}

void ui_overlay_menu(const char *const *items, int count, int selected)
{
    (void)items;
    host_note("overlay menu %d/%d", selected, count);
}

void ui_overlay_setup(const card_setup_t *setup)
{
    host_note("overlay setup %d", setup->phase);
}

void ui_overlay_close(void)
{
    host_note("overlay close");
}
