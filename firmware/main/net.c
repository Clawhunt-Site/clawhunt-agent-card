// main/net.c -- Wi-Fi station, the card's saved networks and its setup hotspot.
#include "net.h"

#include "card_app.h"
#include "card_model.h"
#include "cJSON.h"
#include "cloud.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host_link.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "setup.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_NAMESPACE    "clawcard"
#define NET_FAST_RETRIES 5
#define NET_RETRY_US     (30LL * 1000 * 1000)
#define NET_SCAN_MAX     8     // entries sent to the computer
#define NET_PICK_MAX     24    // entries looked at when choosing a saved network
#define NET_PICK_TRIES   5
#define NET_TRY_ATTEMPTS 3
#define NET_TRY_MAX_US   (30LL * 1000 * 1000)
#define NET_PORTAL_URI   "http://192.168.4.1/"

typedef enum {
    NET_OFF = 0,
    NET_CONNECTING,
    NET_ONLINE,
    NET_FAILED,
} net_state_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char psk[65];
} net_slot_t;

static const char *TAG = "net";

static volatile net_state_t s_state;
static bool s_nvs_ready;
static bool s_stack_ready;
static int s_retries;
static int64_t s_failed_at_us;
static char s_ip[16];
static volatile bool s_scan_done;
static atomic_int s_own_scans;      // blocking scans whose SCAN_DONE is not for the computer

// Saved networks, most recently added first. s_lock guards the table.
static SemaphoreHandle_t s_lock;
static bool s_loaded;
static net_slot_t s_nets[NET_SLOTS];
static volatile int s_count;
static char s_cur_ssid[NET_SSID_MAX];   // the network being joined or in use
static volatile bool s_need_pick;
static int s_pick_tries;

// Phone setup.
static volatile bool s_setup;
static esp_netif_t *s_ap_netif;
static volatile int s_ap_clients;
static volatile net_try_t s_try;
static volatile net_fail_t s_try_reason;
static net_slot_t s_candidate;
static int s_try_attempts;
static int64_t s_try_started_us;
static volatile bool s_commit_pending;

static void set_state(net_state_t state)
{
    s_state = state;
    if (state == NET_FAILED) s_failed_at_us = esp_timer_get_time();
    card_event_t event = { .type = CARD_EV_NET, .net_state = (int)state };
    card_app_post(&event);
    setup_poke();
}

// ---- settings store -------------------------------------------------------

static esp_err_t storage_init(void)
{
    if (s_nvs_ready) return ESP_OK;
    // Never erase on failure: this partition also holds the factory firmware's settings.
    const esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t net_store_get(const char *key, char *out, size_t size)
{
    out[0] = '\0';
    nvs_handle_t nvs;
    esp_err_t err = storage_init();
    if (err == ESP_OK) err = nvs_open(NET_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t len = size;
    err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK) out[0] = '\0';
    return err;
}

esp_err_t net_store_set(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = storage_init();
    if (err == ESP_OK) err = nvs_open(NET_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (value && value[0]) {
        err = nvs_set_str(nvs, key, value);
    } else {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

// ---- saved networks -------------------------------------------------------

// The first slot keeps the key names of the single-network firmware, so a card
// that was set up before keeps its network.
static void slot_keys(int index, char *ssid_key, char *psk_key, size_t size)
{
    if (index == 0) {
        strlcpy(ssid_key, "ssid", size);
        strlcpy(psk_key, "psk", size);
    } else {
        snprintf(ssid_key, size, "ssid%d", index);
        snprintf(psk_key, size, "psk%d", index);
    }
}

// First called from app_main, before any other task can ask.
static void slots_load(void)
{
    if (s_loaded) return;
    s_lock = xSemaphoreCreateMutex();
    int count = 0;
    for (int i = 0; i < NET_SLOTS; i++) {
        char ssid_key[8], psk_key[8];
        slot_keys(i, ssid_key, psk_key, sizeof(ssid_key));
        net_slot_t slot = { 0 };
        net_store_get(ssid_key, slot.ssid, sizeof(slot.ssid));
        if (!slot.ssid[0]) continue;
        net_store_get(psk_key, slot.psk, sizeof(slot.psk));
        s_nets[count++] = slot;
    }
    s_count = count;
    s_loaded = true;
}

static void slots_lock(void)
{
    slots_load();
    if (s_lock) (void)xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void slots_unlock(void)
{
    if (s_lock) (void)xSemaphoreGive(s_lock);
}

// The helpers below need the lock held.

static esp_err_t slots_save(void)
{
    esp_err_t result = ESP_OK;
    for (int i = 0; i < NET_SLOTS; i++) {
        char ssid_key[8], psk_key[8];
        slot_keys(i, ssid_key, psk_key, sizeof(ssid_key));
        const bool used = i < s_count;
        esp_err_t err = net_store_set(ssid_key, used ? s_nets[i].ssid : NULL);
        if (err == ESP_OK) err = net_store_set(psk_key, used ? s_nets[i].psk : NULL);
        if (err != ESP_OK) result = err;
    }
    return result;
}

static int slot_find(const char *ssid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void slot_remove(int index)
{
    for (int i = index; i + 1 < s_count; i++) s_nets[i] = s_nets[i + 1];
    s_count = s_count - 1;
    memset(&s_nets[s_count], 0, sizeof(s_nets[0]));
}

// Newest first; when the table is full the oldest entry makes room.
static void slot_put_front(const net_slot_t *slot)
{
    const int old = slot_find(slot->ssid);
    if (old >= 0) slot_remove(old);
    if (s_count == NET_SLOTS) slot_remove(NET_SLOTS - 1);
    for (int i = s_count; i > 0; i--) s_nets[i] = s_nets[i - 1];
    s_nets[0] = *slot;
    s_count = s_count + 1;
}

bool net_credentials_ok(const char *ssid, const char *psk)
{
    const size_t ssid_len = ssid ? strlen(ssid) : 0;
    const size_t psk_len = psk ? strlen(psk) : 0;
    // WPA2 passphrases are 8..63 characters; an empty one means an open network.
    return ssid_len > 0 && ssid_len < NET_SSID_MAX && psk_len <= 63 && (psk_len == 0 || psk_len >= 8);
}

// ---- Wi-Fi ----------------------------------------------------------------

static esp_err_t connect_with(const char *ssid, const char *psk)
{
    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, psk, sizeof(config.sta.password));
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    s_retries = 0;
    set_state(NET_CONNECTING);
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        set_state(NET_FAILED);
        return err;
    }
    // If the driver is still starting this fails harmlessly: the STA_START event
    // connects, and net_service() retries a failed link every 30 s.
    (void)esp_wifi_disconnect();
    (void)esp_wifi_connect();
    return ESP_OK;
}

static void connect_slot(int index)
{
    slots_lock();
    const net_slot_t slot = s_nets[index];
    slots_unlock();
    (void)connect_with(slot.ssid, slot.psk);
}

// Join a saved network. With one saved it is joined directly (which also finds a
// hidden network); with several, net_service() scans and takes the strongest.
static void connect_auto(void)
{
    if (s_count == 0) {
        s_cur_ssid[0] = '\0';
        set_state(NET_OFF);
    } else if (s_count == 1) {
        connect_slot(0);
    } else {
        s_pick_tries = 0;
        s_need_pick = true;
        set_state(NET_CONNECTING);
        cloud_wake();
    }
}

static void pick_and_connect(void)
{
    wifi_ap_record_t *records = malloc(NET_PICK_MAX * sizeof(*records));
    uint16_t count = NET_PICK_MAX;
    const esp_err_t err = records ? net_scan_blocking(records, &count) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        free(records);
        if (++s_pick_tries < NET_PICK_TRIES) return;   // the driver may still be starting
        s_need_pick = false;
        set_state(NET_FAILED);
        return;
    }
    s_need_pick = false;
    int best = -1;
    int best_rssi = -128;
    slots_lock();
    for (int i = 0; i < count; i++) {
        const int slot = slot_find((const char *)records[i].ssid);
        if (slot >= 0 && records[i].rssi > best_rssi) {
            best = slot;
            best_rssi = records[i].rssi;
        }
    }
    slots_unlock();
    free(records);
    if (best < 0) {
        set_state(NET_FAILED);   // none in range; net_service() looks again later
        return;
    }
    connect_slot(best);
}

static void send_scan_result(void)
{
    uint16_t count = NET_SCAN_MAX;
    wifi_ap_record_t records[NET_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) count = 0;

    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < count && list; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString((const char *)records[i].ssid));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(records[i].rssi));
        cJSON_AddItemToArray(list, pair);
    }
    char *text = list ? cJSON_PrintUnformatted(list) : NULL;
    // Keep the line inside the link's send buffer; drop the weakest entries if needed.
    while (text && strlen(text) > 320 && cJSON_GetArraySize(list) > 0) {
        cJSON_DeleteItemFromArray(list, cJSON_GetArraySize(list) - 1);
        cJSON_free(text);
        text = cJSON_PrintUnformatted(list);
    }
    host_link_send("{\"t\":\"scan\",\"aps\":%s}", text ? text : "[]");
    cJSON_free(text);
    cJSON_Delete(list);
}

static net_fail_t classify(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return NET_FAIL_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return NET_FAIL_NOT_FOUND;
    default:
        return NET_FAIL_OTHER;
    }
}

// During setup the phone decides what is tried: no reconnecting on our own.
static void on_setup_disconnect(uint8_t reason)
{
    if (s_try != NET_TRY_TRYING) {
        set_state(s_count ? NET_FAILED : NET_OFF);
        return;
    }
    // Leaving the previous network is our own doing, not a failed attempt.
    if (reason != WIFI_REASON_ASSOC_LEAVE && ++s_try_attempts >= NET_TRY_ATTEMPTS) {
        s_try_reason = classify(reason);
        s_try = NET_TRY_FAILED;
        s_cur_ssid[0] = '\0';
        set_state(NET_FAILED);
        return;
    }
    (void)esp_wifi_connect();
}

// Runs on the small event-loop stack: anything heavy is left to net_service().
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (atomic_load(&s_own_scans) > 0) {
            atomic_fetch_sub(&s_own_scans, 1);
        } else {
            s_scan_done = true;
            cloud_wake();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_setup && s_count == 1) (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *lost = data;
        s_ip[0] = '\0';
        if (s_setup) {
            on_setup_disconnect(lost->reason);
        } else if (s_count == 0) {
            set_state(NET_OFF);
        } else if (!s_cur_ssid[0]) {
            set_state(NET_FAILED);   // its network was just forgotten
        } else if (s_retries++ < NET_FAST_RETRIES) {
            set_state(NET_CONNECTING);
            (void)esp_wifi_connect();
        } else {
            set_state(NET_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&got->ip_info.ip));
        s_retries = 0;
        if (s_setup && s_try == NET_TRY_TRYING) {
            s_try = NET_TRY_OK;
            s_commit_pending = true;   // saved by net_service(), which has the stack for it
        }
        set_state(NET_ONLINE);
        cloud_wake();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients = s_ap_clients + 1;
        setup_poke();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients = s_ap_clients - 1;
        setup_poke();
    }
}

static esp_err_t stack_start(void)
{
    if (s_stack_ready) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    // RAM only: the driver must not overwrite the network the factory firmware saved.
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_stack_ready = true;
    return ESP_OK;
}

// ---- public ---------------------------------------------------------------

esp_err_t net_start(void)
{
    slots_load();
    if (s_count == 0) return ESP_OK;
    const esp_err_t err = stack_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        set_state(NET_FAILED);
        return err;
    }
    connect_auto();
    return ESP_OK;
}

esp_err_t net_radio_start(void)
{
    slots_load();
    return stack_start();
}

esp_err_t net_set_credentials(const char *ssid, const char *psk)
{
    if (!net_credentials_ok(ssid, psk)) return ESP_ERR_INVALID_ARG;
    net_slot_t slot = { 0 };
    strlcpy(slot.ssid, ssid, sizeof(slot.ssid));
    strlcpy(slot.psk, psk ? psk : "", sizeof(slot.psk));

    slots_lock();
    slot_put_front(&slot);
    esp_err_t err = slots_save();
    slots_unlock();
    if (err != ESP_OK) ESP_LOGW(TAG, "network not saved (%s); it will be lost at power-off", esp_err_to_name(err));
    err = stack_start();
    if (err != ESP_OK) return err;
    s_need_pick = false;
    return connect_with(slot.ssid, slot.psk);
}

esp_err_t net_forget(void)
{
    slots_lock();
    memset(s_nets, 0, sizeof(s_nets));
    s_count = 0;
    const esp_err_t err = slots_save();
    slots_unlock();
    s_need_pick = false;
    s_cur_ssid[0] = '\0';
    if (s_stack_ready) (void)esp_wifi_disconnect();
    set_state(NET_OFF);
    return err;
}

esp_err_t net_forget_one(const char *ssid)
{
    slots_lock();
    const int index = slot_find(ssid ? ssid : "");
    if (index < 0) {
        slots_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    slot_remove(index);
    const esp_err_t err = slots_save();
    slots_unlock();

    if (strcmp(s_cur_ssid, ssid) == 0) {
        s_cur_ssid[0] = '\0';
        if (s_stack_ready) (void)esp_wifi_disconnect();
        if (s_setup) {
            set_state(s_count ? NET_FAILED : NET_OFF);
        } else {
            connect_auto();
        }
    }
    return err;
}

esp_err_t net_scan(void)
{
    const esp_err_t err = stack_start();
    if (err != ESP_OK) return err;
    return esp_wifi_scan_start(NULL, false);
}

esp_err_t net_scan_blocking(wifi_ap_record_t *records, uint16_t *count)
{
    esp_err_t err = stack_start();
    if (err == ESP_OK) {
        const wifi_scan_config_t config = {
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active = { .min = 60, .max = 110 },
        };
        atomic_fetch_add(&s_own_scans, 1);
        err = esp_wifi_scan_start(&config, true);
        if (err != ESP_OK) atomic_fetch_sub(&s_own_scans, 1);
    }
    if (err != ESP_OK) {
        *count = 0;
        return err;
    }
    return esp_wifi_scan_get_ap_records(count, records);
}

void net_service(void)
{
    if (s_scan_done) {
        s_scan_done = false;
        send_scan_result();
    }
    if (s_commit_pending) {
        s_commit_pending = false;
        slots_lock();
        slot_put_front(&s_candidate);
        const esp_err_t err = slots_save();
        slots_unlock();
        if (err != ESP_OK) ESP_LOGW(TAG, "network not saved (%s)", esp_err_to_name(err));
    }
    if (s_try == NET_TRY_TRYING && esp_timer_get_time() - s_try_started_us > NET_TRY_MAX_US) {
        s_try_reason = NET_FAIL_OTHER;
        s_try = NET_TRY_FAILED;
        s_cur_ssid[0] = '\0';
        (void)esp_wifi_disconnect();
        set_state(NET_FAILED);
    }
    if (s_setup) return;
    if (s_need_pick) pick_and_connect();
    if (s_state == NET_FAILED && s_count > 0 && esp_timer_get_time() - s_failed_at_us > NET_RETRY_US) {
        connect_auto();
    }
}

bool net_online(void)
{
    return s_state == NET_ONLINE;
}

const char *net_state_name(void)
{
    static const char *const NAMES[] = { "off", "connecting", "online", "failed" };
    return NAMES[s_state];
}

const char *net_ip(void)
{
    return s_ip;
}

int net_saved_count(void)
{
    slots_load();
    return s_count;
}

int net_saved_names(char names[][NET_SSID_MAX], int max)
{
    slots_lock();
    const int count = s_count < max ? s_count : max;
    for (int i = 0; i < count; i++) strlcpy(names[i], s_nets[i].ssid, NET_SSID_MAX);
    slots_unlock();
    return count;
}

void net_current_ssid(char *out, size_t size)
{
    strlcpy(out, s_cur_ssid, size);
}

// ---- phone setup ------------------------------------------------------------

esp_err_t net_setup_begin(const char *ap_ssid, const char *ap_pass)
{
    esp_err_t err = stack_start();
    if (err != ESP_OK) return err;
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_ERR_NO_MEM;
        // DHCP option 114 tells the phone where the setup page is. The server keeps the pointer.
        static char uri[] = NET_PORTAL_URI;
        (void)esp_netif_dhcps_stop(s_ap_netif);
        (void)esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    }
    s_setup = true;
    s_try = NET_TRY_NONE;
    s_try_reason = NET_FAIL_NONE;
    s_need_pick = false;
    s_ap_clients = 0;
    // A join that is still being attempted would keep the radio off the hotspot's channel.
    if (s_state == NET_CONNECTING) {
        (void)esp_wifi_disconnect();
        set_state(NET_FAILED);
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen((const char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password));
    ap.ap.channel = 6;   // follows the station's channel while the card is on a network
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) net_setup_end();
    return err;
}

void net_setup_end(void)
{
    if (!s_setup) return;
    // A try that is still running must not end up as an unsaved network the card is on.
    const bool mid_try = s_try == NET_TRY_TRYING;
    s_setup = false;
    s_try = NET_TRY_NONE;
    s_commit_pending = false;
    s_ap_clients = 0;
    if (mid_try) {
        s_cur_ssid[0] = '\0';
        (void)esp_wifi_disconnect();
        s_state = NET_FAILED;
    }
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    if (s_state != NET_ONLINE) connect_auto();
}

int net_ap_clients(void)
{
    return s_ap_clients;
}

esp_err_t net_try(const char *ssid, const char *psk)
{
    if (!s_setup || s_try == NET_TRY_TRYING) return ESP_ERR_INVALID_STATE;
    if (!net_credentials_ok(ssid, psk)) return ESP_ERR_INVALID_ARG;
    memset(&s_candidate, 0, sizeof(s_candidate));
    strlcpy(s_candidate.ssid, ssid, sizeof(s_candidate.ssid));
    strlcpy(s_candidate.psk, psk ? psk : "", sizeof(s_candidate.psk));
    s_try_attempts = 0;
    s_try_reason = NET_FAIL_NONE;
    s_try_started_us = esp_timer_get_time();
    s_try = NET_TRY_TRYING;
    const esp_err_t err = connect_with(s_candidate.ssid, s_candidate.psk);
    if (err != ESP_OK) {
        s_try_reason = NET_FAIL_OTHER;
        s_try = NET_TRY_FAILED;
    }
    return err;
}

net_try_t net_try_state(void)
{
    return s_try;
}

void net_try_ssid(char *out, size_t size)
{
    strlcpy(out, s_try == NET_TRY_NONE ? "" : s_candidate.ssid, size);
}

net_fail_t net_try_reason(void)
{
    return s_try_reason;
}
