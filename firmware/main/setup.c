// main/setup.c -- phone setup: the card's hotspot, its DNS catch-all and the setup page.
#include "setup.h"

#include "card_app.h"
#include "card_model.h"
#include "cJSON.h"
#include "cloud.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "dns_server.h"   // after esp_netif.h: it uses esp_ip4_addr_t without including it
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_link.h"
#include "lwip/sockets.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETUP_IDLE_MAX_US  (5LL * 60 * 1000 * 1000)   // nobody used the page this long: close
#define SETUP_DONE_SHOW_US (6LL * 1000 * 1000)        // how long "connected" stays up before closing
#define SETUP_BODY_MAX     256
#define SETUP_SCAN_MAX     20
#define SETUP_LIST_MAX     12
#define SETUP_HOST         "192.168.4.1"
#define SETUP_PAGE_URL     "http://" SETUP_HOST "/"
#define SETUP_PASS_LEN     12
#define SETUP_JOIN_DELAY_MS 400   // lets the answer to the phone leave before the radio changes network

extern const char PAGE_START[] asm("_binary_setup_page_html_start");
extern const char PAGE_END[] asm("_binary_setup_page_html_end");

static const char *TAG = "setup";
static const char *const PHASES[] = { "off", "starting", "waiting", "joined", "trying", "done", "failed" };

static volatile bool s_want;
static volatile bool s_want_lan;
static volatile bool s_on;
static bool s_allow_lan;
static bool s_succeeded;
static bool s_start_failed;
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static char s_ap_ssid[20];
static char s_ap_pass[SETUP_PASS_LEN + 1];
static volatile int64_t s_activity_us;
static int64_t s_done_at_us;
static volatile card_setup_phase_t s_phase;
// A network the page asked for, waiting for the cloud task to try it.
static volatile bool s_join_pending;
static char s_join_ssid[NET_SSID_MAX];
static char s_join_psk[65];

// ---- what the screen is told ----------------------------------------------

static void publish(card_setup_phase_t phase, int reason)
{
    s_phase = phase;
    card_event_t event = { .type = CARD_EV_SETUP };
    event.setup.phase = phase;
    event.setup.reason = reason;
    strlcpy(event.setup.ap_ssid, s_ap_ssid, sizeof(event.setup.ap_ssid));
    strlcpy(event.setup.ap_pass, s_ap_pass, sizeof(event.setup.ap_pass));
    // A failed try is no longer the card's network, but the screen still names it.
    net_try_ssid(event.setup.ssid, sizeof(event.setup.ssid));
    if (!event.setup.ssid[0]) net_current_ssid(event.setup.ssid, sizeof(event.setup.ssid));
    card_app_post(&event);
    // The computer learns the phase, never the hotspot's password.
    host_link_send("{\"t\":\"setup\",\"phase\":\"%s\",\"ap\":\"%s\",\"why\":%d}", PHASES[phase], s_ap_ssid, reason);
}

static card_setup_phase_t phase_now(void)
{
    if (s_join_pending) return CARD_SETUP_TRYING;
    switch (net_try_state()) {
    case NET_TRY_TRYING: return CARD_SETUP_TRYING;
    case NET_TRY_OK:     return CARD_SETUP_DONE;
    // A failed try with no phone on the hotspot: the phone has to join again first.
    case NET_TRY_FAILED: return net_ap_clients() > 0 ? CARD_SETUP_FAILED : CARD_SETUP_WAITING;
    default:             return net_ap_clients() > 0 ? CARD_SETUP_JOINED : CARD_SETUP_WAITING;
    }
}

// ---- the page's small API -------------------------------------------------

static void touch(void)
{
    s_activity_us = esp_timer_get_time();
}

// Only a phone on the card's own hotspot may use the page: the connection must
// have arrived on the hotspot's address.
static bool fd_on_hotspot(int fd)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return false;
    uint32_t local = 0;
    if (addr.ss_family == AF_INET) {
        local = ((const struct sockaddr_in *)&addr)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    } else if (addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&addr;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) local = v6->sin6_addr.un.u32_addr[3];
#endif
    }
    esp_netif_ip_info_t info;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    return ap && esp_netif_get_ip_info(ap, &info) == ESP_OK && local != 0 && local == info.ip.addr;
}

// Connections from the network the card has joined are closed as soon as they open.
static esp_err_t on_open(httpd_handle_t server, int fd)
{
    (void)server;
    return s_allow_lan || fd_on_hotspot(fd) ? ESP_OK : ESP_FAIL;
}

// The page's own address. Every name resolves to the card during setup, so a page
// from some other site would otherwise count as the same origin as this API.
static bool host_is_card(httpd_req_t *req)
{
    if (s_allow_lan) return true;
    char host[24] = "";
    (void)httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    return strcmp(host, SETUP_HOST) == 0 || strcmp(host, SETUP_HOST ":80") == 0;
}

static void answer_text(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    (void)httpd_resp_sendstr(req, text);
}

// Every handler starts here. False means the request was answered and the handler
// must return ESP_FAIL, which closes the connection: otherwise the server would
// keep reading an unread body, however long and however slowly it arrives.
static bool admit(httpd_req_t *req, bool api)
{
    if (!(s_allow_lan || fd_on_hotspot(httpd_req_to_sockfd(req))) || (api && !host_is_card(req))) {
        answer_text(req, "403 Forbidden", "请先连上卡的热点，再打开这个页面。");
        return false;
    }
    if (req->content_len > SETUP_BODY_MAX || (req->method != HTTP_POST && req->content_len > 0)) {
        answer_text(req, "413 Content Too Large", "too large");
        return false;
    }
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    httpd_resp_set_status(req, text ? status : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t send_flag(httpd_req_t *req, const char *status, const char *key)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, key, 1);
    return send_json(req, status, json);
}

// A JSON body from the page. Asking for application/json keeps other sites'
// forms from posting here. Returns NULL after answering the request itself; the
// handler then returns ESP_FAIL so that whatever was not read is not waited for.
static cJSON *read_body(httpd_req_t *req)
{
    char type[40] = "";
    (void)httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
    if (strncmp(type, "application/json", 16) != 0 || req->content_len == 0 || req->content_len > SETUP_BODY_MAX) {
        (void)send_flag(req, "400 Bad Request", "bad");
        return NULL;
    }
    char body[SETUP_BODY_MAX + 1];
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            (void)send_flag(req, "400 Bad Request", "bad");
            return NULL;
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    cJSON *json = cJSON_Parse(body);   // nesting is capped at build time (CJSON_NESTING_LIMIT)
    memset(body, 0, sizeof(body));     // it may hold a Wi-Fi password
    if (!json) (void)send_flag(req, "400 Bad Request", "bad");
    return json;
}

static const char *text_of(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// A network name is up to 32 arbitrary bytes, not always valid UTF-8 (older
// routers use GBK). The page shows "ssid" but sends back "id", the same bytes in
// hex, so such a network can still be chosen.
static void add_name(cJSON *object, const char *ssid)
{
    char id[2 * NET_SSID_MAX];
    size_t at = 0;
    for (const unsigned char *c = (const unsigned char *)ssid; *c && at + 2 < sizeof(id); c++) {
        at += (size_t)snprintf(id + at, sizeof(id) - at, "%02x", *c);
    }
    id[at] = '\0';
    cJSON_AddStringToObject(object, "ssid", ssid);
    cJSON_AddStringToObject(object, "id", id);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// The network a request names: "id" (hex) when present, else "ssid" as typed.
static bool name_from(const cJSON *body, char out[NET_SSID_MAX])
{
    const char *id = text_of(body, "id");
    if (!id[0]) {
        const char *ssid = text_of(body, "ssid");
        strlcpy(out, ssid, NET_SSID_MAX);
        return strlen(ssid) < NET_SSID_MAX;
    }
    const size_t len = strlen(id);
    if (len % 2 != 0 || len / 2 >= NET_SSID_MAX) return false;
    for (size_t i = 0; i < len / 2; i++) {
        const int high = hex_digit(id[2 * i]);
        const int low = hex_digit(id[2 * i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
        out[i] = (char)(high * 16 + low);
    }
    out[len / 2] = '\0';
    return true;
}

static esp_err_t redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", SETUP_PAGE_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // iOS wants a body before it treats the answer as a sign-in page.
    const esp_err_t err = httpd_resp_sendstr(req, "Open " SETUP_PAGE_URL);
    return req->content_len > 0 ? ESP_FAIL : err;   // never wait for a body nobody reads
}

static esp_err_t on_page(httpd_req_t *req)
{
    if (!admit(req, false)) return ESP_FAIL;
    if (!host_is_card(req)) return redirect(req);   // opened under some other name
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE_START, PAGE_END - PAGE_START - 1);
}

static esp_err_t on_state(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_FAIL;
    static const char *const TRY[] = { "idle", "trying", "ok", "failed" };
    static const char *const WHY[] = { "", "password", "notfound", "other" };
    char ssid[NET_SSID_MAX];
    net_try_ssid(ssid, sizeof(ssid));
    if (!ssid[0]) net_current_ssid(ssid, sizeof(ssid));
    char names[NET_SLOTS][NET_SSID_MAX];
    const int saved = net_saved_names(names, NET_SLOTS);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "card", s_ap_ssid);
    cJSON_AddStringToObject(json, "phase", s_join_pending ? "trying" : TRY[net_try_state()]);
    cJSON_AddStringToObject(json, "reason", WHY[net_try_reason()]);
    cJSON_AddStringToObject(json, "ssid", ssid);
    cJSON_AddBoolToObject(json, "online", net_online());
    cJSON *list = cJSON_AddArrayToObject(json, "saved");
    for (int i = 0; i < saved && list; i++) {
        cJSON *item = cJSON_CreateObject();
        add_name(item, names[i]);
        cJSON_AddItemToArray(list, item);
    }
    return send_json(req, "200 OK", json);
}

static esp_err_t on_scan(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_FAIL;
    touch();
    if (s_join_pending || net_try_state() == NET_TRY_TRYING) return send_flag(req, "409 Conflict", "busy");
    wifi_ap_record_t *records = malloc(SETUP_SCAN_MAX * sizeof(*records));
    uint16_t count = SETUP_SCAN_MAX;
    if (!records || net_scan_blocking(records, &count) != ESP_OK) {
        free(records);
        return send_flag(req, "503 Service Unavailable", "busy");
    }
    cJSON *list = cJSON_CreateArray();
    int listed = 0;
    for (int i = 0; i < count && listed < SETUP_LIST_MAX; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid[0]) continue;
        bool seen = false;   // the same name on several access points is listed once, strongest first
        for (int j = 0; j < i && !seen; j++) seen = strcmp((const char *)records[j].ssid, ssid) == 0;
        if (seen) continue;
        cJSON *ap = cJSON_CreateObject();
        add_name(ap, ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(ap, "open", records[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, ap);
        listed++;
    }
    free(records);
    return send_json(req, "200 OK", list);
}

static esp_err_t on_join(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_FAIL;
    touch();
    cJSON *body = read_body(req);
    if (!body) return ESP_FAIL;
    char ssid[NET_SSID_MAX];
    const char *psk = text_of(body, "psk");
    const bool busy = s_join_pending || net_try_state() == NET_TRY_TRYING;
    const bool valid = name_from(body, ssid) && net_credentials_ok(ssid, psk);
    if (!busy && valid) {
        strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
        strlcpy(s_join_psk, psk, sizeof(s_join_psk));
    }
    cJSON_Delete(body);
    if (busy) return send_flag(req, "409 Conflict", "busy");
    if (!valid) return send_flag(req, "400 Bad Request", "bad");
    // Joining drops the link this answer may be travelling on, so answer first
    // and leave the join to the cloud task.
    const esp_err_t err = send_flag(req, "200 OK", "ok");
    s_join_pending = true;
    cloud_wake();
    return err;
}

static esp_err_t on_forget(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_FAIL;
    touch();
    cJSON *body = read_body(req);
    if (!body) return ESP_FAIL;
    char ssid[NET_SSID_MAX];
    const esp_err_t err = name_from(body, ssid) ? net_forget_one(ssid) : ESP_ERR_NOT_FOUND;
    cJSON_Delete(body);
    return send_flag(req, err == ESP_OK ? "200 OK" : "404 Not Found", err == ESP_OK ? "ok" : "bad");
}

// Phones probe well-known addresses to find out whether a network needs a sign-in
// page. Every unknown address is answered with a redirect to the setup page.
static esp_err_t on_anything_else(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    if (!admit(req, false)) return ESP_FAIL;
    return redirect(req);
}

static esp_err_t portal_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.open_fn = on_open;
    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        s_httpd = NULL;
        return err;
    }
    static const httpd_uri_t ROUTES[] = {
        { .uri = "/", .method = HTTP_GET, .handler = on_page },
        { .uri = "/api/state", .method = HTTP_GET, .handler = on_state },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = on_scan },
        { .uri = "/api/join", .method = HTTP_POST, .handler = on_join },
        { .uri = "/api/forget", .method = HTTP_POST, .handler = on_forget },
    };
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]) && err == ESP_OK; i++) {
        err = httpd_register_uri_handler(s_httpd, &ROUTES[i]);
    }
    if (err == ESP_OK) err = httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, on_anything_else);
    if (err != ESP_OK) return err;

    // Every name resolves to the card, so whatever the phone opens lands on the page.
    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns);
    return s_dns ? ESP_OK : ESP_ERR_NO_MEM;
}

static void portal_stop(void)
{
    if (s_dns) stop_dns_server(s_dns);
    s_dns = NULL;
    if (s_httpd) {
        // A client that is slowly sending something must not hold the server open.
        int fds[8];
        size_t count = sizeof(fds) / sizeof(fds[0]);
        if (httpd_get_client_list(s_httpd, &count, fds) == ESP_OK) {
            for (size_t i = 0; i < count; i++) (void)shutdown(fds[i], SHUT_RDWR);
        }
        (void)httpd_stop(s_httpd);
    }
    s_httpd = NULL;
}

// ---- start and stop -------------------------------------------------------

// A fresh password for every session, from letters and digits that are hard to misread.
static void make_names(void)
{
    static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ClawCard-%02X%02X", mac[4], mac[5]);
    for (int i = 0; i < SETUP_PASS_LEN;) {
        uint8_t byte;
        esp_fill_random(&byte, 1);
        byte &= 31;
        if (byte >= sizeof(ALPHABET) - 1) continue;   // no modulo bias
        s_ap_pass[i++] = ALPHABET[byte];
    }
    s_ap_pass[SETUP_PASS_LEN] = '\0';
}

static void stop(void)
{
    portal_stop();
    s_join_pending = false;
    memset(s_join_psk, 0, sizeof(s_join_psk));
    net_setup_end();
    s_on = false;
    s_want = false;
    memset(s_ap_pass, 0, sizeof(s_ap_pass));
    publish(CARD_SETUP_OFF, s_succeeded ? 1 : s_start_failed ? 2 : 0);
}

static void start(void)
{
    s_succeeded = false;
    s_start_failed = false;
    s_done_at_us = 0;
    s_allow_lan = s_want_lan;
    // The radio must be running before the password is drawn: that is what makes
    // the random numbers truly random.
    esp_err_t err = net_radio_start();
    if (err == ESP_OK) {
        make_names();
        err = net_setup_begin(s_ap_ssid, s_ap_pass);
    }
    if (err == ESP_OK) err = portal_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup did not start: %s", esp_err_to_name(err));
        s_start_failed = true;
        stop();
        return;
    }
    s_on = true;
    touch();
    publish(CARD_SETUP_WAITING, 0);
}

// ---- public ---------------------------------------------------------------

void setup_request(bool on, bool allow_lan)
{
    s_want_lan = allow_lan;
    s_want = on;
    cloud_wake();
}

bool setup_active(void)
{
    return s_on || s_want;
}

void setup_poke(void)
{
    if (s_on) cloud_wake();
}

void setup_service(void)
{
    if (s_want && !s_on) start();
    if (!s_want && s_on) stop();
    if (!s_on) return;

    if (s_join_pending) {
        vTaskDelay(pdMS_TO_TICKS(SETUP_JOIN_DELAY_MS));
        (void)net_try(s_join_ssid, s_join_psk);
        memset(s_join_psk, 0, sizeof(s_join_psk));
        s_join_pending = false;
        touch();
    }
    const int64_t now = esp_timer_get_time();
    const card_setup_phase_t phase = phase_now();
    if (phase == CARD_SETUP_DONE) {
        if (!s_done_at_us) s_done_at_us = now;
        if (now - s_done_at_us > SETUP_DONE_SHOW_US) {
            s_succeeded = true;
            stop();
            return;
        }
    } else {
        s_done_at_us = 0;
    }
    if (phase != CARD_SETUP_TRYING && now - s_activity_us > SETUP_IDLE_MAX_US) {
        stop();
        return;
    }
    if (phase != s_phase) {
        if (phase == CARD_SETUP_JOINED || phase == CARD_SETUP_TRYING) touch();
        publish(phase, net_try_state() == NET_TRY_FAILED ? (int)net_try_reason() : 0);
    }
}

const char *setup_phase_name(void)
{
    return PHASES[s_phase];
}
