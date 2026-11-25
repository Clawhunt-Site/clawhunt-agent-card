// main/cloud.c -- pairing, the card's own profile, pay requests and what its agent is
// doing, over HTTPS.
//
// Everything runs on one task with one HTTP client, so requests never overlap and
// the TLS connection is reused between polls.
//
// Holding the card is not enough to approve a payment: the server asks for the holder's secret
// (the pay request says "auth":"secret"), and checks it. This file carries the secret from the
// screen to the server in the body of one request and wipes every copy of it afterwards. It is
// never written to flash, to the USB link or to a log. All the rules (how many wrong tries, when
// the card is frozen, which amounts need no secret) are the server's; the card only shows them.
#include "cloud.h"

#include "card_app.h"
#include "card_identity.h"
#include "card_model.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host_link.h"
#include "net.h"
#include "setup.h"
#include "week.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLOUD_DEFAULT_BASE "https://clawhunt.store"
// Some CDNs challenge clients that do not look like a browser; the card still says what it is at the end.
#define CLOUD_USER_AGENT   "Mozilla/5.0 (X11; Linux riscv32) AppleWebKit/537.36 (KHTML, like Gecko) " \
                           "Chrome/131.0 Safari/537.36 ClawHuntCard/" CARD_FW_VERSION
#define CLOUD_BODY_MAX     3072
#define CLOUD_BASE_MAX     96
#define CLOUD_TOKEN_MAX    80
#define CLOUD_CODE_MAX     96
#define MS                 1000LL   // esp_timer counts microseconds
#define PROFILE_EVERY_US   (60000 * MS)
#define PAY_POLL_US        (4000 * MS)   // 15 a minute: several cards can share one address
#define RETRY_US           (15000 * MS)
#define PAIR_RETRY_MAX_US  (600000 * MS)   // a server that does not offer pairing is asked every 10 minutes
#define PAIR_INTERVAL_MAX_MS 30000         // the slowest the card asks about its pairing code
#define LOOP_WAIT_MS       1000
#define PAY_TTL_MAX_MS     20000
#define PAY_MARGIN_MS      4000     // kept back from the server's expiry for the round trip
#define DECISION_TRIES     3
#define DECISION_RETRY_MS  700      // between two tries of one decision: the request lives for seconds
#define RESEND_WITHIN_US   (3000 * MS)   // a closed connection fails at once; a request that waited out its time is not sent again
#define CLOUD_PATH_MAX     160      // the longest path: the task route with its cursor
#define CLOUD_CURSOR_MAX   96
#define TASK_POLL_S        15       // until the server says how often it wants to be asked
#define TASK_POLL_MIN_S    5
#define TASK_POLL_MAX_S    300
#define TASK_ABSENT_US     (600000 * MS)   // a server without the task route is asked every 10 minutes
#define TASK_SHOW_MIN_MS   5000
#define TASK_SHOW_MAX_MS   30000
#define PROFILE_AFTER_PAID_US (2000 * MS)  // a payout changes the face's numbers: fetch them soon after

typedef enum {
    CLOUD_UNPAIRED = 0,
    CLOUD_PAIRING,
    CLOUD_PAIRED,
} cloud_state_t;

typedef struct {
    int id;
    bool approve;
    cloud_reason_t reason;
    bool with_secret;   // the holder's secret waits in s_secret; it never travels through the queue
} decision_t;

static const char *TAG = "cloud";
static const char *const REASONS[] = { "approved", "declined", "timeout", "busy" };
static const int LADDER[] = { 2, 5, 10, 20 };   // deliveries needed for levels 2..5
// The server's names for card_job_phase_t, in that enum's order.
static const char *const TASK_PHASES[] = { "bid", "won", "own", "work", "done", "lost", "held", "disputed" };

static TaskHandle_t s_task;
static QueueHandle_t s_decisions;
static esp_http_client_handle_t s_client;
static volatile bool s_reload;
static volatile cloud_state_t s_state;
static char s_base[CLOUD_BASE_MAX];
static char s_token[CLOUD_TOKEN_MAX];
static char s_body[CLOUD_BODY_MAX];
static int s_body_len;
static bool s_connected;          // the client opened a connection for the request in hand
static bool s_answered;           // a header of the server's answer to it has come
static char s_device_code[CLOUD_CODE_MAX];
static int s_pair_interval_ms;
static int64_t s_pair_expires_us;
static int64_t s_next_pair_us;
static int64_t s_next_profile_us;
static int64_t s_next_pay_us;
static int s_shown_pay_id;
static int s_pair_failures;
static bool s_code_shown;         // the screen holds a pairing sheet with a code this client was given
static bool s_start_failed;       // the last request for a pairing code brought none
static int64_t s_next_task_us;
static int64_t s_task_poll_us = TASK_POLL_S * 1000 * MS;
// What the server last answered with: it says what this card has already been told, and is sent
// back with the next request. Kept in flash, so a card that was switched off is not told again.
static char s_cursor[CLOUD_CURSOR_MAX];
static card_task_t s_task_told;   // the job last handed to the screen
static bool s_task_told_any;
static volatile bool s_forget_cursor;   // set by another task: the SITE's cursor in flash goes with the next reload
// A token, and the cursor that goes with it, belong to one server. The site's own are kept as
// "token" and "tcur"; those of any other server the card is pointed at (a local backend, for a
// test) as "token2" and "tcur2". So pointing the card elsewhere and back does not cost it its
// pairing with the site: the site's token stays in flash and is never sent to the other server.
// Written by this task only, when it reloads its settings. Another task must not decide by it: a
// command that changed the address a moment ago is not in it yet (see cloud_unpair).
static volatile bool s_other_server;    // the address in use is not the site's
static volatile bool s_forget_other;    // set by another task: the other server's cursor goes with the next reload
// An id for this run of the card, made up at power-on and sent with every request of a paired card.
// The server takes a new one for a restart, after which it wants the holder's secret again.
static char s_boot[9];
// The holder's secret on its way out: put here by the screen's task, taken and wiped by this one.
// One approval is under way at a time, so one slot is enough; s_secret_for says whose it is.
static portMUX_TYPE s_secret_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_secret[CARD_SECRET_LEN + 1];
static char s_attempt[9];         // an id for this typed secret: a try sent twice is judged once
static int s_secret_for;
static card_guard_t s_guard_told; // how the server guards the card, as last handed to the screen
static bool s_guard_told_any;
// The server's time, from the Date header of the answer in hand: 0 when the answer had none. The card has no
// clock of its own; this is what tells it which day the agent's earnings belong to (main/week.c).
static int64_t s_answer_epoch_s;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static int clamp_int(double value, int min, int max)
{
    if (!(value >= min)) return min;   // also catches NaN from a missing field
    if (value > max) return max;
    return (int)value;
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static const char *token_key(void)
{
    return s_other_server ? "token2" : "token";
}

static const char *cursor_key(void)
{
    return s_other_server ? "tcur2" : "tcur";
}

// The week of the agent's earnings is kept per server as well: agent 1 of a test server is not agent 1 of the site.
static const char *week_key(void)
{
    return s_other_server ? "week2" : "week";
}

// Overwrite what held the holder's secret. Through a volatile pointer, so the compiler keeps it.
static void wipe(void *memory, size_t size)
{
    volatile unsigned char *byte = memory;
    while (size--) *byte++ = 0;
}

// ---- HTTP -----------------------------------------------------------------

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    // What the client tells of the request in hand: that it opened a connection for it, and that a
    // header of the answer came (the server has heard the request, whatever becomes of the rest).
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) s_connected = true;
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        s_answered = true;
        if (evt->header_key && evt->header_value && strcasecmp(evt->header_key, "Date") == 0) {
            int64_t epoch_s;
            if (week_parse_http_date(evt->header_value, &epoch_s)) s_answer_epoch_s = epoch_s;
        }
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const int room = CLOUD_BODY_MAX - 1 - s_body_len;
        const int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(s_body + s_body_len, evt->data, (size_t)n);
            s_body_len += n;
        }
    }
    return ESP_OK;
}

static void client_drop(void)
{
    if (!s_client) return;
    esp_http_client_cleanup(s_client);
    s_client = NULL;
}

static bool client_ready(void)
{
    if (s_client) return true;
    char url[CLOUD_BASE_MAX + 4];
    snprintf(url, sizeof(url), "%s/", s_base);
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .user_agent = CLOUD_USER_AGENT,
        .event_handler = on_http_event,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        // Without this the client treats a 401 as a login challenge, fails the request and
        // hides the status: the card would never learn that its token was revoked.
        .max_authorization_retries = -1,
    };
    s_client = esp_http_client_init(&config);
    return s_client != NULL;
}

// One try of one request, on the connection the client has or on a new one. The reply body lands
// in s_body, the client's status code in *status. After a failure that code can be the one of the
// answer BEFORE (the client only forgets it once a request has been written), so whether the
// server said anything to this request is not read off it: that is in s_answered, and whether the
// client had to connect for it in s_connected.
static esp_err_t api_try(esp_http_client_method_t method, const char *path, const char *json, bool auth, int *status)
{
    s_body_len = 0;
    s_body[0] = '\0';
    s_connected = false;
    s_answered = false;
    s_answer_epoch_s = 0;
    *status = -1;
    if (!client_ready()) return ESP_FAIL;

    // Only the cloud task makes requests, so this need not be on its stack next to the TLS handshake.
    static char url[CLOUD_BASE_MAX + CLOUD_PATH_MAX];
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, method);
    esp_http_client_set_header(s_client, "Accept", "application/json");
    if (json) {
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_client, json, (int)strlen(json));
    } else {
        esp_http_client_delete_header(s_client, "Content-Type");
        esp_http_client_set_post_field(s_client, NULL, 0);
    }
    if (auth && s_token[0]) {
        char header[CLOUD_TOKEN_MAX + 8];
        snprintf(header, sizeof(header), "Bearer %s", s_token);
        esp_http_client_set_header(s_client, "Authorization", header);
        esp_http_client_set_header(s_client, "X-Card-Boot", s_boot);
    } else {
        esp_http_client_delete_header(s_client, "Authorization");
        esp_http_client_delete_header(s_client, "X-Card-Boot");
    }

    const esp_err_t err = esp_http_client_perform(s_client);
    s_body[s_body_len] = '\0';
    *status = esp_http_client_get_status_code(s_client);
    return err;
}

// Whether the try that just failed went into a connection that was open before it and brought
// nothing back: the mark of a connection the server had closed while it was idle. Not so when no
// client could be made, when the client opened a connection for this try (it says so) or failed
// to open one, or when a header of an answer came.
static bool lost_on_kept_connection(esp_err_t err)
{
    return s_client && !s_connected && err != ESP_ERR_HTTP_CONNECT && !s_answered;
}

// One request; the reply body lands in s_body. Returns the HTTP status, or -1.
//
// The connection is kept between requests, and a server closes one that has been idle for a while
// (after five seconds, for some). The card only finds out when it next uses it: the request goes
// into a closed connection and is lost. That one case gets a second try at once, on a new
// connection. The server has most likely not seen the request. If it has, nothing is lost by
// asking twice: the server answers a decision it has already taken with what it took, counts a
// wrong secret sent twice under one attempt id once, and a second request for a pairing code only
// leaves one code unused. A request that waited out its time is not sent again, nor one that
// failed on a new connection: the caller asks again in its own time.
static int api(esp_http_client_method_t method, const char *path, const char *json, bool auth)
{
    const int64_t began = now_us();
    int status;
    esp_err_t err = api_try(method, path, json, auth, &status);
    if (err != ESP_OK && lost_on_kept_connection(err) && now_us() - began < RESEND_WITHIN_US) {
        client_drop();
        err = api_try(method, path, json, auth, &status);
    }
    if (err != ESP_OK) {
        client_drop();   // the next request starts on a fresh connection
        // A refusal is still an answer, even when the client reports it as a failure. It is this
        // request's only if a header came with it: without one, the code is left over from the
        // answer before.
        if (!s_answered) return -1;
        return (status == 401 || status == 403) ? status : -1;
    }
    return status;
}

// ---- pairing --------------------------------------------------------------

// How the card is guarded belongs to one pairing too: the face stops saying it.
static void guard_forget(void)
{
    if (!s_guard_told_any) return;
    card_event_t event = { .type = CARD_EV_GUARD };   // not known
    card_app_post(&event);
    s_guard_told_any = false;
}

// A cursor belongs to one pairing: without it the server starts this card from now, and the
// face stops showing a job that was the last agent's.
static void task_forget(void)
{
    s_cursor[0] = '\0';
    (void)net_store_set(cursor_key(), NULL);
    s_next_task_us = 0;
    // No job, and sheets that still waited for the screen go too.
    card_event_t event = { .type = CARD_EV_TASK, .task = { .reset = true } };
    card_app_post(&event);
    s_task_told_any = false;
    guard_forget();
}

static void forget_token(void)
{
    s_token[0] = '\0';
    (void)net_store_set(token_key(), NULL);
    s_device_code[0] = '\0';
    s_state = CLOUD_UNPAIRED;
    s_next_pair_us = 0;
    task_forget();
}

// The server answers a card it no longer knows with 401, and only that ends a pairing. A 403 is
// not its answer to a card: it comes from the site's edge (a challenge) and says nothing about
// the token, so the request is tried again later like any other that failed.
static bool token_refused(int status)
{
    return status == 401;
}

static const char *strip_scheme(const char *url)
{
    if (strncmp(url, "https://", 8) == 0) return url + 8;
    if (strncmp(url, "http://", 7) == 0) return url + 7;
    return url;
}

// The code on the screen is of no use any more and there is none to put in its place: the sheet
// goes, so nobody types a dead code into the pairing page. The next code brings it back.
// Looked at once a round, so it is asked of the screen again until the screen's queue takes it.
static void pairing_sheet_service(void)
{
    if (!s_code_shown) return;
    const bool paired = s_state == CLOUD_PAIRED;
    if (!paired && s_device_code[0] && now_us() <= s_pair_expires_us) return;   // the code is good
    // While the card can ask (online, not in setup) a new code is asked for at once and takes the
    // old one's place on the screen: the sheet only goes if that brought none.
    if (!paired && !s_start_failed && !setup_active() && net_online()) return;
    card_event_t event = { .type = CARD_EV_PAIRING };   // no code
    if (!card_app_try_post(&event)) return;
    s_code_shown = false;
    host_link_send("{\"t\":\"cloud\",\"pairing\":\"gone\"}");
}

static void pairing_start(void)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"fw\":\"%s\",\"agent_hint\":%d}", CARD_FW_VERSION, CARD_AGENT_ID);
    const int status = api(HTTP_METHOD_POST, "/api/cards/pairing", body, false);
    cJSON *reply = (status == 200 || status == 201) ? cJSON_Parse(s_body) : NULL;
    const char *device_code = json_str(reply, "device_code");
    const char *user_code = json_str(reply, "user_code");
    if (!device_code[0] || !user_code[0] || strlen(device_code) >= sizeof(s_device_code)) {
        // Whatever code the card held had ended, or a new one would not have been asked for.
        s_device_code[0] = '\0';
        s_state = CLOUD_UNPAIRED;
        s_start_failed = true;
        host_link_send("{\"t\":\"cloud\",\"pairing\":\"failed\",\"http\":%d}", status);
        // Back off: 15 s, 30 s, 60 s ... up to 10 minutes, so a server without this
        // feature is not asked over and over.
        int64_t wait = RETRY_US << (s_pair_failures < 6 ? s_pair_failures : 6);
        if (wait > PAIR_RETRY_MAX_US) wait = PAIR_RETRY_MAX_US;
        s_pair_failures++;
        s_next_pair_us = now_us() + wait;
        cJSON_Delete(reply);
        return;
    }
    strlcpy(s_device_code, device_code, sizeof(s_device_code));
    const int expires_s = clamp_int(json_int(reply, "expires_in", 600), 60, 3600);
    s_pair_interval_ms = clamp_int(json_int(reply, "interval", 5) * 1000.0, 2000, PAIR_INTERVAL_MAX_MS);
    s_pair_expires_us = now_us() + expires_s * 1000 * MS;
    s_next_pair_us = now_us() + s_pair_interval_ms * MS;
    s_state = CLOUD_PAIRING;
    s_pair_failures = 0;

    card_event_t event = { .type = CARD_EV_PAIRING };
    card_utf8_copy(event.pairing.code, sizeof(event.pairing.code), user_code);
    card_utf8_copy(event.pairing.where, sizeof(event.pairing.where),
                   strip_scheme(json_str(reply, "verification_uri")));
    event.pairing.minutes = (expires_s + 59) / 60;
    card_app_post(&event);
    s_code_shown = true;
    s_start_failed = false;
    host_link_send("{\"t\":\"cloud\",\"pairing\":\"started\",\"code\":\"%s\"}", event.pairing.code);
    cJSON_Delete(reply);
}

static void pairing_done(const cJSON *reply)
{
    strlcpy(s_token, json_str(reply, "device_token"), sizeof(s_token));
    const esp_err_t err = net_store_set(token_key(), s_token);
    s_device_code[0] = '\0';
    s_code_shown = false;   // the screen closes the sheet itself when it is told of the pairing
    s_state = CLOUD_PAIRED;
    s_next_profile_us = 0;
    s_next_pay_us = 0;
    task_forget();   // a new pairing is told nothing from before it
    card_event_t event = { .type = CARD_EV_PAIRED };
    card_app_post(&event);
    host_link_send("{\"t\":\"cloud\",\"paired\":1,\"saved\":%d}", err == ESP_OK ? 1 : 0);
}

static void pairing_poll(void)
{
    char body[CLOUD_CODE_MAX + 24];
    snprintf(body, sizeof(body), "{\"device_code\":\"%s\"}", s_device_code);
    const int status = api(HTTP_METHOD_POST, "/api/cards/pairing/token", body, false);
    cJSON *reply = cJSON_Parse(s_body);
    const char *token = json_str(reply, "device_token");
    if (status == 200 && token[0] && strlen(token) < sizeof(s_token)) {
        pairing_done(reply);
    } else if (status >= 400 && status < 500) {
        // The server's word on this code is an RFC 8628 error name; a 404 means it knows of no
        // pairing at all. Any other refusal is not about the code: the site's edge answering
        // with a 403 challenge page, or a rate limit. Then the code on the screen is still good:
        // it is kept and asked about again, more slowly after a 429, until it runs out.
        const char *error = json_str(reply, "error");
        if (strcmp(error, "slow_down") == 0) {
            s_pair_interval_ms += 5000;
        } else if (status == 429) {
            if (s_pair_interval_ms < PAIR_INTERVAL_MAX_MS) s_pair_interval_ms += 5000;
        } else if (status == 404 || (error[0] && strcmp(error, "authorization_pending") != 0)) {
            s_device_code[0] = '\0';   // expired, refused or unknown: ask for a new code
            s_state = CLOUD_UNPAIRED;
        }
    }
    s_next_pair_us = now_us() + s_pair_interval_ms * MS;
    cJSON_Delete(reply);
}

static void pairing_step(void)
{
    if (now_us() < s_next_pair_us) return;
    if (!s_device_code[0] || now_us() > s_pair_expires_us) {
        pairing_start();
    } else {
        pairing_poll();
    }
}

// ---- profile --------------------------------------------------------------

static int ladder_progress(int tasks)
{
    int previous = 0;
    for (size_t i = 0; i < sizeof(LADDER) / sizeof(LADDER[0]); i++) {
        if (tasks < LADDER[i]) return (tasks - previous) * 100 / (LADDER[i] - previous);
        previous = LADDER[i];
    }
    return 100;
}

// Fill the card face from an agent object: name, level, deliveries, success rate,
// lifetime earnings. settled is shown when known (< 0 means unknown).
static bool agent_to_stats(const cJSON *agent, int settled, card_stats_t *stats)
{
    const char *name = json_str(agent, "name");
    if (!name[0]) return false;
    const int tasks = clamp_int(cJSON_GetNumberValue(cJSON_GetObjectItem(agent, "total_tasks_completed")), 0, 999999);
    const double rate = cJSON_GetNumberValue(cJSON_GetObjectItem(agent, "success_rate"));

    memset(stats, 0, sizeof(*stats));
    card_utf8_copy(stats->name, sizeof(stats->name), name);
    stats->agent_id = clamp_int(cJSON_GetNumberValue(cJSON_GetObjectItem(agent, "id")), 0, 99999999);
    stats->level = clamp_int(cJSON_GetNumberValue(cJSON_GetObjectItem(agent, "level")), 1, 999);
    stats->xp = ladder_progress(tasks);
    stats->today_cents = clamp_int(cJSON_GetNumberValue(cJSON_GetObjectItem(agent, "total_earned")), 0, 99999999);
    stats->has_cells = true;
    // The face's one number is the agent's lifetime earnings; under it, deliveries, settled deals and the success
    // rate. A fact whose value is "-" is not known, and the face leaves it out.
    card_utf8_copy(stats->money_label, sizeof(stats->money_label), "TOTAL · EARNED");
    card_utf8_copy(stats->cells[0].label, CARD_CELL_MAX, "deals");
    snprintf(stats->cells[0].value, CARD_CELL_MAX, "%d", tasks);
    card_utf8_copy(stats->cells[1].label, CARD_CELL_MAX, "settled");
    if (settled >= 0) {
        snprintf(stats->cells[1].value, CARD_CELL_MAX, "%d", settled);
    } else {
        card_utf8_copy(stats->cells[1].value, CARD_CELL_MAX, "-");
    }
    stats->cells[2].label[0] = '\0';   // a percentage says what it is
    if (tasks > 0) {
        snprintf(stats->cells[2].value, CARD_CELL_MAX, "%d%%", clamp_int(rate * 100 + 0.5, 0, 100));
    } else {
        card_utf8_copy(stats->cells[2].value, CARD_CELL_MAX, "-");   // nothing delivered yet: there is no rate
    }
    return true;
}

// The bars of the face: which day the agent's earnings belong to, from how the total moved since the answer
// before and the server's time of this one (main/week.c). An answer without a time, or one that does not say the
// total (no figure is not a total of zero), leaves the week as it was.
// The week is written to flash only when what is kept of it changes.
static void week_note(const cJSON *agent, card_stats_t *stats, int64_t answered_s)
{
    const bool told = cJSON_IsNumber(cJSON_GetObjectItem(agent, "total_earned"));
    if (told && answered_s > 0 && week_sample(stats->agent_id, stats->today_cents, answered_s)) {
        char text[WEEK_TEXT_MAX];
        week_text(text, sizeof(text));
        (void)net_store_set(week_key(), text);
    }
    stats->has_week = week_for(stats->agent_id, stats->week, &stats->weekday);
}

// How the server guards the card ("security" of the card's own view), for the face: whether a
// secret is set and whether the card is frozen. Said when it changes. An older server says
// nothing, and the face then says nothing about it either.
static void guard_tell(const cJSON *security)
{
    card_guard_t guard = { 0 };
    if (cJSON_IsObject(security)) {
        guard.known = true;
        guard.secret_set = cJSON_IsTrue(cJSON_GetObjectItem(security, "secret_set"));
        guard.frozen = cJSON_IsTrue(cJSON_GetObjectItem(security, "frozen"));
        guard.by_attempts = guard.frozen && strcmp(json_str(security, "frozen_reason"), "attempts") == 0;
    }
    if (s_guard_told_any && guard.known == s_guard_told.known && guard.secret_set == s_guard_told.secret_set &&
        guard.frozen == s_guard_told.frozen && guard.by_attempts == s_guard_told.by_attempts) {
        return;
    }
    card_event_t event = { .type = CARD_EV_GUARD, .guard = guard };
    if (!card_app_try_post(&event)) return;   // the screen's queue was full: said with the next profile
    s_guard_told = guard;
    s_guard_told_any = true;
}

// Paired: one call returns the card, its agent and the limits.
static void profile_device(void)
{
    const int status = api(HTTP_METHOD_GET, "/api/cards/device/me", NULL, true);
    const int64_t answered_s = s_answer_epoch_s;
    if (token_refused(status)) {
        host_link_send("{\"t\":\"cloud\",\"token\":\"rejected\",\"http\":%d}", status);
        forget_token();
        return;
    }
    cJSON *reply = status == 200 ? cJSON_Parse(s_body) : NULL;
    const cJSON *agent = cJSON_GetObjectItem(reply, "agent");
    card_event_t event = { .type = CARD_EV_NET_STATS };
    const bool ok = agent_to_stats(agent, clamp_int(cJSON_GetNumberValue(cJSON_GetObjectItem(agent, "settled_total")), -1, 999999), &event.stats);
    if (ok) week_note(agent, &event.stats, answered_s);
    if (ok) card_app_post(&event);
    if (ok) guard_tell(cJSON_GetObjectItem(reply, "security"));
    host_link_send("{\"t\":\"net\",\"sync\":\"%s\",\"http\":%d,\"via\":\"device\"}", ok ? "ok" : "failed", status);
    s_next_profile_us = now_us() + (ok ? PROFILE_EVERY_US : RETRY_US);
    cJSON_Delete(reply);
}

// Not paired yet: the built-in agent's public profile.
static void profile_public(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/api/agents/%d", CARD_AGENT_ID);
    int status = api(HTTP_METHOD_GET, path, NULL, false);
    const int64_t answered_s = s_answer_epoch_s;   // of the answer that carries the earnings, not of the next one
    cJSON *agent = status == 200 ? cJSON_Parse(s_body) : NULL;
    int settled = -1;
    if (agent) {
        snprintf(path, sizeof(path), "/api/agents/%d/track-record", CARD_AGENT_ID);
        // settled_total sits at the start of the reply, so a cut-off body still has it.
        const char *found = api(HTTP_METHOD_GET, path, NULL, false) == 200 ? strstr(s_body, "\"settled_total\":") : NULL;
        if (found) settled = clamp_int(atoi(found + strlen("\"settled_total\":")), 0, 999999);
    }
    card_event_t event = { .type = CARD_EV_NET_STATS };
    const bool ok = agent_to_stats(agent, settled, &event.stats);
    if (ok) week_note(agent, &event.stats, answered_s);
    if (ok) card_app_post(&event);
    host_link_send("{\"t\":\"net\",\"sync\":\"%s\",\"http\":%d,\"via\":\"public\"}", ok ? "ok" : "failed", status);
    s_next_profile_us = now_us() + (ok ? PROFILE_EVERY_US : RETRY_US);
    cJSON_Delete(agent);
}

// ---- pay requests ---------------------------------------------------------

static void pay_poll(void)
{
    s_next_pay_us = now_us() + PAY_POLL_US;
    const int status = api(HTTP_METHOD_GET, "/api/cards/device/pay-requests/next", NULL, true);
    if (token_refused(status)) {
        forget_token();
        return;
    }
    cJSON *reply = status == 200 ? cJSON_Parse(s_body) : NULL;
    const cJSON *request = cJSON_GetObjectItem(reply, "request");
    const int id = json_int(request, "id", 0);
    if (cJSON_IsObject(request) && id > 0 && id != s_shown_pay_id) {
        card_event_t event = { .type = CARD_EV_PAY };
        card_pay_t *pay = &event.pay;
        pay->id = id;
        pay->remote = true;
        card_utf8_copy(pay->payee, sizeof(pay->payee), json_str(request, "payee"));
        card_utf8_copy(pay->memo, sizeof(pay->memo), json_str(request, "memo"));
        card_utf8_copy(pay->code, sizeof(pay->code), json_str(request, "match_code"));
        pay->amount_cents = clamp_int(json_int(request, "amount_cents", 0), 0, 99999999);
        pay->balance_cents = -1;   // this version holds no money: nothing to show
        pay->ttl_ms = clamp_int(json_int(request, "expires_in", 30) * 1000.0 - PAY_MARGIN_MS, 5000, PAY_TTL_MAX_MS);
        // What the holder is asked for before the approval is sent. An older server does not say:
        // then nothing is asked, as before.
        pay->need_secret = strcmp(json_str(request, "auth"), "secret") == 0;
        if (pay->amount_cents > 0 && pay->payee[0]) {
            s_shown_pay_id = id;
            card_app_post(&event);
        }
    }
    cJSON_Delete(reply);
}

// Whether the server itself said something about the decision. No answer, the edge's 403 (a
// challenge page: it never reached the server), a rate limit and a gateway error did not.
static bool decision_answered(int status)
{
    return status >= 200 && status < 500 && status != 403 && status != 429;
}

// Take the secret that waits for this request out of its slot, and leave the slot empty.
static void secret_take(int id, char *secret, char *attempt)
{
    secret[0] = '\0';
    attempt[0] = '\0';
    portENTER_CRITICAL(&s_secret_lock);
    if (s_secret_for == id) {
        memcpy(secret, s_secret, sizeof(s_secret));
        memcpy(attempt, s_attempt, sizeof(s_attempt));
    }
    wipe(s_secret, sizeof(s_secret));
    wipe(s_attempt, sizeof(s_attempt));
    s_secret_for = 0;
    portEXIT_CRITICAL(&s_secret_lock);
}

// What the server said against an approval, in the screen's terms. Its refusals carry a name in
// "error"; one without a name is the request having run out, or the day's limit.
static card_pay_why_t refusal_of(int status, const cJSON *reply, int *left)
{
    const char *error = json_str(reply, "error");
    *left = clamp_int(json_int(reply, "attempts_left", 0), 0, 99);
    if (strcmp(error, "wrong_secret") == 0) return CARD_PAY_WRONG_SECRET;
    if (strcmp(error, "secret_required") == 0) return CARD_PAY_NEED_SECRET;
    if (strcmp(error, "secret_not_set") == 0) return CARD_PAY_NO_SECRET;
    if (strcmp(error, "card_frozen") == 0 || status == 423) return CARD_PAY_FROZEN;
    return CARD_PAY_REFUSED;
}

// Approvals are only shown as paid once the server has accepted them. A decision the server
// did not answer is sent again: the server takes the same decision twice as one (a typed secret
// carries an id for that: the same try is not counted against the holder twice), and the
// request it is about only lives for seconds.
static void decisions_flush(void)
{
    decision_t d;
    while (xQueueReceive(s_decisions, &d, 0) == pdTRUE) {
        char path[80];
        char body[112];
        snprintf(path, sizeof(path), "/api/cards/device/pay-requests/%d/decision", d.id);
        char secret[CARD_SECRET_LEN + 1] = "";
        char attempt[sizeof(s_attempt)] = "";
        if (d.approve && d.with_secret) secret_take(d.id, secret, attempt);
        if (secret[0]) {
            snprintf(body, sizeof(body), "{\"approve\":true,\"reason\":\"approved\",\"secret\":\"%s\",\"attempt\":\"%s\"}",
                     secret, attempt);
            wipe(secret, sizeof(secret));
        } else {
            snprintf(body, sizeof(body), "{\"approve\":%s,\"reason\":\"%s\"}", d.approve ? "true" : "false",
                     REASONS[d.reason]);
        }
        int status = -1;
        for (int attempt = 0; attempt < DECISION_TRIES; attempt++) {
            // After an answer that was not the server's, a moment's pause. After no answer at all (the
            // request already waited out its time) the next try goes at once: the screen stops waiting.
            if (attempt > 0 && status >= 0) vTaskDelay(pdMS_TO_TICKS(DECISION_RETRY_MS));
            status = api(HTTP_METHOD_POST, path, body, true);
            if (decision_answered(status)) break;
        }
        // The body held the secret: it goes, and the client is not left pointing at it.
        wipe(body, sizeof(body));
        if (s_client) esp_http_client_set_post_field(s_client, NULL, 0);
        cJSON *reply = cJSON_Parse(s_body);
        const bool accepted = status == 200 && strcmp(json_str(reply, "status"), d.approve ? "approved" : "declined") == 0;
        int left = 0;
        const card_pay_why_t why = accepted ? CARD_PAY_REFUSED : refusal_of(status, reply, &left);
        cJSON_Delete(reply);
        if (d.approve) {
            card_event_t event = { .type = CARD_EV_PAY_RESULT,
                                   .pay_result = { .id = d.id, .ok = accepted, .why = why, .left = left } };
            card_app_post(&event);
            // Frozen, or no secret set: the screen now says so by itself, ahead of the profile. The
            // profile is fetched at once, and what it says of the guard is handed over even if it is
            // what was handed over last: the screen is no longer showing that.
            if (why == CARD_PAY_FROZEN || why == CARD_PAY_NO_SECRET) {
                s_next_profile_us = 0;
                s_guard_told_any = false;
            }
        }
        host_link_send("{\"t\":\"cloud\",\"decision\":%d,\"approve\":%d,\"http\":%d}", d.id, d.approve ? 1 : 0, status);
    }
}

// ---- what the agent is doing ------------------------------------------------
//
// GET /api/cards/device/tasks/next?cursor=... answers with the agent's job now ("job"), at
// most one sheet to show ("event": a new task or a payout) and the cursor for the next
// request. The server keeps nothing about the card: the cursor is the card's memory of what
// it has been told. An answer is applied as a whole and only then is its cursor kept, so a
// request that failed, or was answered twice, shows nothing twice.
//
// Nothing here can act for the agent. The card holds no agent key: it is told what the agent
// did with its own.

// The cursor goes into a URL as it is, so only take one that needs no escaping.
static bool cursor_ok(const char *cursor)
{
    const size_t len = strlen(cursor);
    if (len == 0 || len >= CLOUD_CURSOR_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        const char c = cursor[i];
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '.' || c == '-' || c == '_';
        if (!plain) return false;
    }
    return true;
}

// A sheet's title. A private order's title never reaches the card: it prints its own words.
static void sheet_title(char *dst, size_t size, const cJSON *item)
{
    const char *title = json_str(item, "title");
    if (!title[0]) title = cJSON_IsTrue(cJSON_GetObjectItem(item, "private")) ? "私密订单" : "任务";
    card_utf8_copy(dst, size, title);
}

// Both return false when the screen's queue was full: the answer is then asked for again.
static bool task_apply_job(const cJSON *job)
{
    card_task_t task = { 0 };   // no job: the face rests
    if (cJSON_IsObject(job)) {
        const char *phase = json_str(job, "phase");
        int found = -1;
        for (size_t i = 0; i < sizeof(TASK_PHASES) / sizeof(TASK_PHASES[0]); i++) {
            if (strcmp(phase, TASK_PHASES[i]) == 0) found = (int)i;
        }
        // A phase this firmware has no words for comes from a newer server: the line stays as it is.
        if (found < 0) return true;
        task.id = clamp_int(json_int(job, "id", 0), 0, 2147483647);
        task.phase = (card_job_phase_t)found;
        task.called_off = strcmp(json_str(job, "why"), "called_off") == 0;
    }
    if (s_task_told_any && task.id == s_task_told.id && task.phase == s_task_told.phase) return true;
    card_event_t event = { .type = CARD_EV_TASK, .task = task };
    if (!card_app_try_post(&event)) return false;
    s_task_told = task;
    s_task_told_any = true;
    return true;
}

static bool task_apply_event(const cJSON *item)
{
    if (!cJSON_IsObject(item)) return true;
    const char *kind = json_str(item, "kind");
    if (strcmp(kind, "new_task") == 0) {
        card_event_t event = { .type = CARD_EV_OFFER };
        card_offer_t *offer = &event.offer;
        offer->id = clamp_int(json_int(item, "id", 0), 0, 99999999);
        sheet_title(offer->title, sizeof(offer->title), item);
        offer->amount_cents = clamp_int(json_int(item, "amount_cents", 0), 0, 99999999);
        offer->direct = cJSON_IsTrue(cJSON_GetObjectItem(item, "direct"));
        offer->ttl_ms = clamp_int(json_int(item, "ttl_s", 20) * 1000.0, TASK_SHOW_MIN_MS, TASK_SHOW_MAX_MS);
        offer->remote = true;
        return card_app_try_post(&event);
    }
    if (strcmp(kind, "paid") == 0) {
        card_event_t event = { .type = CARD_EV_PAID };
        card_paid_t *paid = &event.paid;
        paid->id = clamp_int(json_int(item, "id", 0), 0, 99999999);
        sheet_title(paid->title, sizeof(paid->title), item);
        paid->net_cents = clamp_int(json_int(item, "net_cents", 0), 0, 99999999);
        paid->new_level = clamp_int(json_int(item, "level", 0), 0, 999);
        paid->remote = true;
        if (!card_app_try_post(&event)) return false;
        // The agent's totals changed with it: do not leave the old ones up for a minute.
        if (s_next_profile_us > now_us() + PROFILE_AFTER_PAID_US) s_next_profile_us = now_us() + PROFILE_AFTER_PAID_US;
    }
    // Any other kind comes from a newer server and has no screen here.
    return true;
}

static void task_poll(void)
{
    s_next_task_us = now_us() + s_task_poll_us;
    static char path[CLOUD_PATH_MAX];
    snprintf(path, sizeof(path), "/api/cards/device/tasks/next%s%s", s_cursor[0] ? "?cursor=" : "", s_cursor);
    const int status = api(HTTP_METHOD_GET, path, NULL, true);
    if (token_refused(status)) {
        forget_token();
        return;
    }
    if (status == 404) {
        s_next_task_us = now_us() + TASK_ABSENT_US;   // an older server: it has no such route
        return;
    }
    cJSON *reply = status == 200 ? cJSON_Parse(s_body) : NULL;
    const char *cursor = json_str(reply, "cursor");
    // Without a cursor to keep, nothing is shown: the same things would be shown again next time.
    // And the cursor is kept only once the screen has taken everything: the server says each thing
    // once, so an answer the screen could not take is asked for again with the old cursor.
    if (cJSON_IsObject(reply) && cursor_ok(cursor) && task_apply_job(cJSON_GetObjectItem(reply, "job")) &&
        task_apply_event(cJSON_GetObjectItem(reply, "event"))) {
        if (strcmp(cursor, s_cursor) != 0) {
            // Only when something happened: a quiet card writes nothing to flash.
            strlcpy(s_cursor, cursor, sizeof(s_cursor));
            (void)net_store_set(cursor_key(), s_cursor);
        }
        s_task_poll_us = (int64_t)clamp_int(json_int(reply, "poll_s", TASK_POLL_S), TASK_POLL_MIN_S, TASK_POLL_MAX_S) *
                         1000 * MS;
        s_next_task_us = now_us() + s_task_poll_us;
    }
    cJSON_Delete(reply);
}

// ---- task -----------------------------------------------------------------

static void reload_settings(void)
{
    s_reload = false;
    client_drop();
    net_store_get("api", s_base, sizeof(s_base));
    s_other_server = s_base[0] != '\0';
    if (!s_base[0]) strlcpy(s_base, CLOUD_DEFAULT_BASE, sizeof(s_base));
    net_store_get(token_key(), s_token, sizeof(s_token));
    s_state = s_token[0] ? CLOUD_PAIRED : CLOUD_UNPAIRED;
    s_device_code[0] = '\0';
    s_next_pair_us = 0;
    s_next_profile_us = 0;
    s_next_pay_us = 0;
    if (s_forget_cursor) {
        // The pairing with the site was ended from another task: the site's cursor in flash is the
        // old pairing's. It is erased here, so only this task ever writes it, and by its name: the
        // address may have changed since the pairing was ended, and cursor_key() follows the address.
        s_forget_cursor = false;
        (void)net_store_set("tcur", NULL);
    }
    if (s_forget_other) {
        // The card was pointed at another server, or back at the site: what the other server told
        // it goes. What the site told it stays.
        s_forget_other = false;
        (void)net_store_set("tcur2", NULL);
        (void)net_store_set("week2", NULL);
    }
    net_store_get(cursor_key(), s_cursor, sizeof(s_cursor));
    if (!s_token[0] || !cursor_ok(s_cursor)) s_cursor[0] = '\0';
    // The week of earnings kept for the server the card now speaks to. The first answer after this only says
    // where the total stands: what was earned while the card was not asking belongs to no day.
    char week[WEEK_TEXT_MAX];
    net_store_get(week_key(), week, sizeof(week));
    week_restore(week);
    s_next_task_us = 0;
    // Whatever job the face shows is no longer known to be true.
    if (s_task_told_any) {
        card_event_t event = { .type = CARD_EV_TASK, .task = { .reset = true } };
        card_app_post(&event);
    }
    s_task_told_any = false;
    guard_forget();   // nor how the card is guarded
}

static void cloud_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_service();
        // Setup needs the memory the TLS client holds, and the station may be hopping
        // between networks: drop the client before the hotspot and its servers start.
        if (setup_active()) client_drop();
        setup_service();
        if (s_reload) reload_settings();
        if (setup_active()) {
            // Nothing is fetched until setup is over.
        } else if (net_online()) {
            decisions_flush();
            if (now_us() >= s_next_profile_us) {
                if (s_state == CLOUD_PAIRED) {
                    profile_device();
                } else {
                    profile_public();
                }
            }
            if (s_state == CLOUD_PAIRED) {
                if (now_us() >= s_next_pay_us) pay_poll();
                // pay_poll may have found the token revoked
                if (s_state == CLOUD_PAIRED && now_us() >= s_next_task_us) task_poll();
            } else {
                pairing_step();
            }
        }
        pairing_sheet_service();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOOP_WAIT_MS));
    }
}

esp_err_t cloud_start(void)
{
    s_decisions = xQueueCreate(4, sizeof(decision_t));
    if (!s_decisions) return ESP_ERR_NO_MEM;
    snprintf(s_boot, sizeof(s_boot), "%08x", (unsigned)esp_random());
    reload_settings();
    if (xTaskCreate(cloud_task, "cloud", 8192, NULL, 2, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void cloud_wake(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

// Plain http would send the device token in the clear: only allow it on a LAN.
static bool base_allowed(const char *base)
{
    const size_t len = strlen(base);
    if (len == 0 || len >= CLOUD_BASE_MAX || strchr(base, ' ') || base[len - 1] == '/') return false;
    if (strncmp(base, "https://", 8) == 0) return true;
    static const char *const LAN[] = { "http://192.168.", "http://10.", "http://127.", "http://172.16." };
    for (size_t i = 0; i < sizeof(LAN) / sizeof(LAN[0]); i++) {
        if (strncmp(base, LAN[i], strlen(LAN[i])) == 0) return true;
    }
    return false;
}

esp_err_t cloud_set_base(const char *base)
{
    if (base && base[0] && !base_allowed(base)) return ESP_ERR_INVALID_ARG;
    const bool is_default = !base || !base[0] || strcmp(base, CLOUD_DEFAULT_BASE) == 0;
    esp_err_t err = net_store_set("api", is_default ? NULL : base);
    // Another server always starts unpaired, and nothing of it is kept once the card leaves it:
    // its token and what it told the card go either way. The site's own token and cursor are not
    // touched: back at the site, the card is as paired as it was.
    if (err == ESP_OK) err = net_store_set("token2", NULL);
    s_forget_other = true;
    s_reload = true;
    cloud_wake();
    return err;
}

const char *cloud_base(void)
{
    return s_base;
}

// Whether flash names another server than the site: what the last `api` command wrote. This is
// what a command from another task goes by. s_other_server says which server THIS task speaks to,
// and follows only at its next reload: `api` and `unpair` can both arrive before that (the USB
// reader's task runs ahead of this one, and this one may be waiting for a server's answer).
static bool flash_names_other_server(void)
{
    char base[CLOUD_BASE_MAX];
    (void)net_store_get("api", base, sizeof(base));
    return base[0] != '\0';
}

// End the pairing with the server the card is pointed at: the one flash names now. So `api <other>`
// followed by `unpair` ends the pairing with the other server (there is none yet) and leaves the
// site's token alone, and `unpair` followed by `api ""` leaves the site's cursor alone.
esp_err_t cloud_unpair(void)
{
    const bool other = flash_names_other_server();
    const esp_err_t err = net_store_set(other ? "token2" : "token", NULL);
    if (other) {
        s_forget_other = true;
    } else {
        s_forget_cursor = true;
    }
    s_reload = true;
    cloud_wake();
    return err;
}

bool cloud_holds_secret(void)
{
    bool held = s_secret_for != 0;
    for (size_t i = 0; i < sizeof(s_secret); i++) held = held || s_secret[i] != 0;
    for (size_t i = 0; i < sizeof(s_attempt); i++) held = held || s_attempt[i] != 0;
    return held;
}

static void decision_put(const decision_t *d)
{
    if (s_decisions && xQueueSend(s_decisions, d, 0) == pdTRUE) {
        cloud_wake();
    } else {
        ESP_LOGW(TAG, "decision for %d dropped", d->id);
    }
}

void cloud_pay_decision(int id, bool approve, cloud_reason_t reason)
{
    const decision_t d = { .id = id, .approve = approve, .reason = reason };
    decision_put(&d);
}

void cloud_pay_approve(int id, const char *secret)
{
    // An id for this typed secret. It only has to differ from the one of the try before.
    char attempt[sizeof(s_attempt)];
    snprintf(attempt, sizeof(attempt), "%08x", (unsigned)esp_random());
    portENTER_CRITICAL(&s_secret_lock);
    strlcpy(s_secret, secret, sizeof(s_secret));
    memcpy(s_attempt, attempt, sizeof(s_attempt));
    s_secret_for = id;
    portEXIT_CRITICAL(&s_secret_lock);
    const decision_t d = { .id = id, .approve = true, .reason = CLOUD_REASON_APPROVED, .with_secret = true };
    decision_put(&d);
}

const char *cloud_state_name(void)
{
    static const char *const NAMES[] = { "unpaired", "pairing", "paired" };
    return NAMES[s_state];
}
