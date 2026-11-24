// main/cloud.h -- the card talking to ClawHunt on its own, over Wi-Fi.
//
// Before the card is paired it shows its built-in agent's public profile and a
// pairing code. The owner enters the code on clawhunt.store; the card then holds
// a device token and fetches its own profile and pay requests with it.
#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef enum {
    CLOUD_REASON_APPROVED = 0,
    CLOUD_REASON_DECLINED,
    CLOUD_REASON_TIMEOUT,
    CLOUD_REASON_BUSY,
} cloud_reason_t;

// Load the saved API address and device token and start the cloud task.
esp_err_t cloud_start(void);

// Something changed (network up, scan finished): run the loop now.
void cloud_wake(void);

// Point the card at another ClawHunt server. Plain http is only accepted for
// private LAN addresses. The card starts unpaired on another server, and nothing of that server
// is kept when the card leaves it; its token for the site itself stays, unused, until the card
// is pointed back ("" or the site's own address).
esp_err_t cloud_set_base(const char *base);
const char *cloud_base(void);

// Forget the device token; the card shows a new pairing code.
esp_err_t cloud_unpair(void);

// Report the holder's answer to a pay request that came from the server.
void cloud_pay_decision(int id, bool approve, cloud_reason_t reason);
// The same for an approval that carries the holder's secret (CARD_SECRET_LEN of 'U', 'D', 'O').
// The secret is copied for the one request it goes out in and wiped after it; it is never
// stored, logged or sent anywhere else.
void cloud_pay_approve(int id, const char *secret);
// Whether a secret is still held here: true from cloud_pay_approve() until the cloud task has put
// it into its request. For the host tests, which cannot look into this file's memory.
bool cloud_holds_secret(void);

// "unpaired", "pairing" or "paired".
const char *cloud_state_name(void);
