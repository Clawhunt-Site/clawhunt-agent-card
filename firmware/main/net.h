// main/net.h -- Wi-Fi station, the card's saved networks and its setup hotspot.
//
// The card remembers a few networks (home, a phone's hotspot) in NVS under its
// own namespace and joins whichever one is in range. Nothing else in NVS is
// touched: the Wi-Fi driver keeps its own state in RAM, so the factory
// firmware's saved network survives.
#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_SLOTS    4    // networks the card remembers
#define NET_SSID_MAX 33   // bytes, including the terminating NUL

typedef enum {
    NET_TRY_NONE = 0,
    NET_TRY_TRYING,
    NET_TRY_OK,
    NET_TRY_FAILED,
} net_try_t;

typedef enum {
    NET_FAIL_NONE = 0,
    NET_FAIL_PASSWORD,   // the network refused the password
    NET_FAIL_NOT_FOUND,  // no such network in range (or it is 5 GHz only)
    NET_FAIL_OTHER,
} net_fail_t;

// Bring Wi-Fi up if a network was saved earlier. Does nothing (and uses no
// memory) when the card has never been given one.
esp_err_t net_start(void);

// Start the radio without joining anything (setup needs it for its hotspot).
esp_err_t net_radio_start(void);

// Save a network and connect to it. psk may be empty for an open network.
esp_err_t net_set_credentials(const char *ssid, const char *psk);

// Forget every saved network and go offline.
esp_err_t net_forget(void);
// Forget one saved network.
esp_err_t net_forget_one(const char *ssid);

// List nearby networks; the result is sent to the computer as a "scan" message.
esp_err_t net_scan(void);

// Housekeeping that must run on a task with a real stack (scan results, choosing
// among saved networks, the slow reconnect after repeated failures). The cloud
// task calls it every loop.
void net_service(void);

bool net_online(void);

// "off", "connecting", "online" or "failed".
const char *net_state_name(void);

// Dotted address while online, "" otherwise.
const char *net_ip(void);

// Saved networks, most recently added first. Returns how many names were written.
int net_saved_names(char names[][NET_SSID_MAX], int max);
int net_saved_count(void);
// The network the card is on or trying to join; "" when there is none.
void net_current_ssid(char *out, size_t size);

// ---- phone setup ------------------------------------------------------------
// While setup is on, the card also runs a hotspot and stops joining networks on
// its own: the phone decides which network is tried.

esp_err_t net_setup_begin(const char *ap_ssid, const char *ap_pass);
void net_setup_end(void);
// Phones currently joined to the card's hotspot.
int net_ap_clients(void);

// Whether a name and password could be a Wi-Fi network at all.
bool net_credentials_ok(const char *ssid, const char *psk);

// Try a network the phone chose. It is saved only once the card got an address.
esp_err_t net_try(const char *ssid, const char *psk);
net_try_t net_try_state(void);
// The network of the latest try; "" when none was made in this setup session.
void net_try_ssid(char *out, size_t size);
net_fail_t net_try_reason(void);

// Scan and wait for the result. *count holds the capacity on entry and the
// number of records on return, strongest first.
esp_err_t net_scan_blocking(wifi_ap_record_t *records, uint16_t *count);

// Small persistent strings in the card's own NVS namespace. A NULL or empty value
// erases the key. get leaves out empty when the key is missing.
esp_err_t net_store_get(const char *key, char *out, size_t size);
esp_err_t net_store_set(const char *key, const char *value);
