// main/card_model.h -- data shared by the host link, the state machine and the UI.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CARD_FW_VERSION "0.11.0"
#define CARD_TITLE_MAX  64   // bytes, including the terminating NUL
#define CARD_NAME_MAX   24
#define CARD_CELL_COUNT 3
#define CARD_CELL_MAX   16   // bytes per stat label or value, including the NUL
#define CARD_SECRET_LEN 8    // the holder's secret: this many presses of the three keys
#define CARD_WEEK_DAYS  7    // the bars of the face: the last seven days, today last
// Where a day of the week begins: minutes east of UTC. The card has no setting for it yet.
#define CARD_UTC_OFFSET_MIN 480

// The status line, in the words the state machine says them (main/card_app.c). The face shows the line when it
// changes and keeps a one-word state in its header: it knows which word from these.
#define CARD_LINE_IDLE         "摸鱼中，等单…"
#define CARD_LINE_BID          "竞标中…"
#define CARD_LINE_WON          "中标了，开工"
#define CARD_LINE_OWN          "收到，这就开工"
#define CARD_LINE_WORK         "干活中…"
#define CARD_LINE_WORK_PART    "干活中"           // "干活中 40%": how a computer's job says how far it is
#define CARD_LINE_DONE         "交付了，等验收"
#define CARD_LINE_HELD         "验收了，等放款"
#define CARD_LINE_DISPUTED     "有争议，等裁定"
#define CARD_LINE_FROZEN_TRIES "口令输错，卡已冻结"
#define CARD_LINE_FROZEN       "卡已冻结"
#define CARD_LINE_NO_SECRET    "没设口令，还不能付款"
#define CARD_LINE_NO_WIFI      "长按 OK 键连 Wi-Fi"   // a card that knows no network says how to give it one
// What the face is told about the network, by state (card_event_t.net_state).
#define CARD_NET_OFF     ""
#define CARD_NET_JOINING "连网中"
#define CARD_NET_ONLINE  "Wi-Fi"
#define CARD_NET_FAILED  "没连上"

// One of the three facts under the number: the computer decides what each one shows.
typedef struct {
    char label[CARD_CELL_MAX];
    char value[CARD_CELL_MAX];
} card_cell_t;

typedef struct {
    char name[CARD_NAME_MAX];
    int agent_id;      // the agent these numbers belong to; 0 = the one this build was made for
    int level;
    int xp;            // 0..100, progress towards the next level
    int deals;         // accepted deliveries
    int rating10;      // average rating times ten: 49 means 4.9
    int streak;
    int today_cents;   // the face's one number: what money_label says it is
    // Optional overrides. Without them the facts are deals / rating / streak
    // and the number reads "earned today".
    bool has_cells;
    card_cell_t cells[CARD_CELL_COUNT];
    char money_label[CARD_CELL_MAX];
    // The bars: what the agent earned on each of the last seven days, in cents, today last; < 0 where there is
    // no figure. Without has_week the face draws no bars, only where they would stand.
    bool has_week;
    int week[CARD_WEEK_DAYS];
    int weekday;       // today's day of the week, 0 = Monday
} card_stats_t;

typedef struct {
    int id;
    char title[CARD_TITLE_MAX];
    int amount_cents;
    bool direct;       // someone asked for this agent by name
    int ttl_ms;        // how long the offer stays on screen
    bool remote;       // told by the server: the agent acts with its own key, so OK only closes the sheet
} card_offer_t;

typedef struct {
    int id;
    char title[CARD_TITLE_MAX];
    int net_cents;
    int new_level;     // 0 when this payout did not level the agent up
    bool remote;       // told by the server, once: shown when the screen is next free instead of being dropped
} card_paid_t;

// Someone asks to be paid from this card's wallet; the holder confirms on the card.
typedef struct {
    int id;
    char payee[CARD_NAME_MAX];
    char memo[CARD_TITLE_MAX];
    int amount_cents;
    int balance_cents;   // wallet balance before paying; < 0 when the card holds no money
    int ttl_ms;
    bool remote;         // came from the server rather than the computer
    bool via_usb;        // a remote request sent over USB: the computer answers for the server
                         // (tests and screenshots); the card's own server is never told
    char code[8];        // digits the requester's page also shows; "" when there are none
    bool need_secret;    // the server wants the holder's secret with the approval: holding OK opens the
                         // sheet it is typed on. The server decides again when the approval arrives.
} card_pay_t;

// Why the server did not take an approval (CARD_EV_PAY_RESULT that is not ok).
typedef enum {
    CARD_PAY_REFUSED = 0,   // not accepted: the request ran out, or the day's limit is reached
    CARD_PAY_NEED_SECRET,   // it needs the holder's secret, and none was sent
    CARD_PAY_WRONG_SECRET,  // not this card's secret; `left` more tries before the card is frozen
    CARD_PAY_FROZEN,        // the card is frozen until its owner unfreezes it on the site
    CARD_PAY_NO_SECRET,     // the owner has not given the card a secret yet
} card_pay_why_t;

// How the server guards the card (GET /api/cards/device/me, "security"). The card only shows it:
// every rule is the server's. Not known: an older server, or the card is not paired.
typedef struct {
    bool known;
    bool secret_set;
    bool frozen;
    bool by_attempts;    // frozen by wrong secrets rather than by the owner
} card_guard_t;

// A code the owner types on clawhunt.store to bind this card to their account.
typedef struct {
    char code[16];
    char where[48];      // the page to open, without the scheme
    int minutes;         // how long the code stays valid
} card_pairing_t;

// Phone setup: the card runs a hotspot, the phone picks a Wi-Fi network on its page.
typedef enum {
    CARD_SETUP_OFF = 0,
    CARD_SETUP_STARTING,
    CARD_SETUP_WAITING,   // hotspot is up, nobody joined yet
    CARD_SETUP_JOINED,    // a phone is on the hotspot
    CARD_SETUP_TRYING,    // joining the network the phone chose
    CARD_SETUP_DONE,
    CARD_SETUP_FAILED,
} card_setup_phase_t;

typedef struct {
    card_setup_phase_t phase;
    char ap_ssid[20];    // the card's hotspot
    char ap_pass[16];
    char ssid[33];       // the network being tried
    int reason;          // FAILED, or WAITING after a failed try: 1 wrong password, 2 not found, 3 other.
                         // OFF: 1 after a success, 2 the hotspot could not start
} card_setup_t;

typedef enum {
    CARD_JOB_BID = 0,  // bidding on an open job
    CARD_JOB_WON,
    CARD_JOB_OWN,      // the owner handed the job over directly
    CARD_JOB_WORK,
    CARD_JOB_DONE,     // delivered, waiting for the buyer
    CARD_JOB_LOST,     // the bid was not picked, or the job was called off
    CARD_JOB_HELD,     // the buyer accepted; the payout comes after the acceptance hold
    CARD_JOB_DISPUTED, // a dispute is open on the delivery; a ruling decides it (only the server says this)
} card_job_phase_t;

// What the server says the agent is doing now. It is not a mode of the card: the status line and
// the crab show it whenever the card is at rest, and keys, pay requests and settings work as ever.
typedef struct {
    int id;                  // the task; 0 = nothing in hand
    card_job_phase_t phase;
    bool called_off;         // CARD_JOB_LOST: the task ended without a payout, rather than the bid not being picked
    bool reset;              // the pairing ended: sheets that still wait to be shown go with it
} card_task_t;

typedef enum {
    CARD_KEY_UP = 0,
    CARD_KEY_DOWN,
    CARD_KEY_OK,
} card_key_t;

typedef enum {
    CARD_EV_KEY = 0,
    CARD_EV_STATS,
    CARD_EV_OFFER,
    CARD_EV_JOB,
    CARD_EV_PAID,
    CARD_EV_PING,
    CARD_EV_PAY,
    CARD_EV_SAY,
    CARD_EV_NET,        // Wi-Fi state changed
    CARD_EV_NET_STATS,  // profile fetched by the card itself; a computer's stats win
    CARD_EV_PAIRING,    // show a pairing code; with no code, the one that was up is gone
    CARD_EV_PAIRED,     // the owner approved the code
    CARD_EV_PAY_RESULT, // the server accepted (or not) an approved pay request
    CARD_EV_KEY_LONG,   // a key was held down
    CARD_EV_SETUP,      // phone setup moved to another phase
    CARD_EV_TASK,       // the server said what the agent is doing now
    CARD_EV_GUARD,      // the server said how the card is guarded (a secret set, frozen)
} card_event_type_t;

typedef struct {
    card_event_type_t type;
    union {
        card_key_t key;
        card_stats_t stats;
        card_offer_t offer;
        struct {
            card_job_phase_t phase;
            int progress;  // 0..100, used by CARD_JOB_WORK
        } job;
        card_paid_t paid;
        card_task_t task;
        card_pay_t pay;
        char say[CARD_TITLE_MAX];
        int net_state;     // 0 off, 1 connecting, 2 online, 3 failed
        card_pairing_t pairing;
        struct {
            int id;
            bool ok;
            bool from_usb;   // only answers a request that came over USB
            card_pay_why_t why;   // when not ok
            int left;             // CARD_PAY_WRONG_SECRET: tries left before the card is frozen
        } pay_result;
        card_setup_t setup;
        card_guard_t guard;
    };
} card_event_t;

// Copy UTF-8 text into dst without cutting a multi-byte character in half.
void card_utf8_copy(char *dst, size_t dst_size, const char *src);

// "$18.75" style text for an amount in cents. sign is "" or "+".
void card_format_money(char *dst, size_t dst_size, const char *sign, int cents);
