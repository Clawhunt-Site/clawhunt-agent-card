// tests/test_card_tasks.c -- the card's state machine and its ClawHunt client, run on a computer.
//
// main/card_app.c and main/cloud.c are compiled as they are, over the stand-in platform of
// tests/card_stubs. Three ways to run it (tools/card_host_tests.sh builds and runs all three):
//
//   test_card_tasks checks            what the card does with what the server tells it about its
//                                     agent's tasks: the resting line, the two sheets, sheets that
//                                     wait their turn, and that a job never blocks the card; and
//                                     the holder's secret: typed after OK was held, sent with the
//                                     approval and nowhere else, typed again when the server says
//                                     it was wrong
//   test_card_tasks usb               plays one fixed script of USB messages and keys and prints all
//                                     the card did. Built once from this tree's card_app.c and once
//                                     from the previous release's, the two prints must be the same:
//                                     the USB messages work as before
//   test_card_tasks replay <file>     starts the card paired and answers its task requests with the
//                                     lines of <file>, which are a real server's answers in the order
//                                     one card applied them. Prints "screen ..." for what the holder
//                                     saw; fails if the card asked with any cursor but the last answer's
//
// The screen itself (main/ui_card.c) is not in this: what a sheet looks like needs the device.
#include "card_stubs/card_host.c"

#ifndef CARD_APP_SOURCE
#define CARD_APP_SOURCE "../main/card_app.c"
#endif
#include CARD_APP_SOURCE

#include "week.h"

// The checks below are not built for the previous release's card_app.c (CARD_APP_OLD): unused there.
#define MAYBE_UNUSED __attribute__((unused))

#ifdef CARD_APP_OLD
// The previous release has no card_app_try_post; this tree's cloud.c, linked into both builds, calls it.
bool card_app_try_post(const card_event_t *event)
{
    card_app_post(event);
    return true;
}
#endif

static int s_failures MAYBE_UNUSED;
static int s_mark;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                       \
            for (int i_ = s_mark; i_ < host_log_count; i_++) printf("     | %s\n", host_log[i_]); \
            s_failures++;                                                        \
        }                                                                        \
    } while (0)

// ---- driving the card ---------------------------------------------------------

// One event, applied as the card's own task applies it: one frame.
static void apply_now(const card_event_t *event)
{
    apply(event);
    host_frame();
}

static void MAYBE_UNUSED pump(void)
{
    card_event_t event;
    while (xQueueReceive(s_queue, &event, 0) == pdTRUE) apply_now(&event);
}

static void MAYBE_UNUSED step(void)
{
    s_mark = host_log_count;
}

static bool MAYBE_UNUSED said(const char *line)
{
    for (int i = s_mark; i < host_log_count; i++) {
        if (strcmp(host_log[i], line) == 0) return true;
    }
    return false;
}

static bool MAYBE_UNUSED said_start(const char *start)
{
    for (int i = s_mark; i < host_log_count; i++) {
        if (strncmp(host_log[i], start, strlen(start)) == 0) return true;
    }
    return false;
}

MAYBE_UNUSED static const char *last_of(const char *start)
{
    for (int i = host_log_count - 1; i >= 0; i--) {
        if (strncmp(host_log[i], start, strlen(start)) == 0) return host_log[i];
    }
    return "";
}

// How often, and where first, something was put out since step(). where() is -1 for never.
static int MAYBE_UNUSED times(const char *start)
{
    int count = 0;
    for (int i = s_mark; i < host_log_count; i++) {
        if (strncmp(host_log[i], start, strlen(start)) == 0) count++;
    }
    return count;
}

static int MAYBE_UNUSED where(const char *start)
{
    for (int i = s_mark; i < host_log_count; i++) {
        if (strncmp(host_log[i], start, strlen(start)) == 0) return i;
    }
    return -1;
}

static void key(card_key_t k)
{
    card_event_t event = { .type = CARD_EV_KEY, .key = k };
    apply_now(&event);
}

static void key_long(card_key_t k)
{
    card_event_t event = { .type = CARD_EV_KEY_LONG, .key = k };
    apply_now(&event);
}

static void send_offer(int id, int cents, bool direct, bool remote, int ttl_ms)
{
    card_event_t event = { .type = CARD_EV_OFFER };
    event.offer.id = id;
    card_utf8_copy(event.offer.title, sizeof(event.offer.title), "把 50 页合同翻成英文");
    event.offer.amount_cents = cents;
    event.offer.direct = direct;
    event.offer.ttl_ms = ttl_ms;
#ifndef CARD_APP_OLD
    event.offer.remote = remote;
#else
    (void)remote;
#endif
    apply_now(&event);
}

static void send_job(card_job_phase_t phase, int progress)
{
    card_event_t event = { .type = CARD_EV_JOB };
    event.job.phase = phase;
    event.job.progress = progress;
    apply_now(&event);
}

static void send_paid(int id, int net, int level, bool remote)
{
    card_event_t event = { .type = CARD_EV_PAID };
    event.paid.id = id;
    card_utf8_copy(event.paid.title, sizeof(event.paid.title), "把 50 页合同翻成英文");
    event.paid.net_cents = net;
    event.paid.new_level = level;
#ifndef CARD_APP_OLD
    event.paid.remote = remote;
#else
    (void)remote;
#endif
    apply_now(&event);
}

static void send_pay(int id, int cents, int ttl_ms, bool remote, bool via_usb)
{
    card_event_t event = { .type = CARD_EV_PAY };
    event.pay.id = id;
    card_utf8_copy(event.pay.payee, sizeof(event.pay.payee), "峰会咖啡");
    card_utf8_copy(event.pay.memo, sizeof(event.pay.memo), "一杯拿铁");
    card_utf8_copy(event.pay.code, sizeof(event.pay.code), "284");
    event.pay.amount_cents = cents;
    event.pay.balance_cents = remote ? -1 : 100000;
    event.pay.ttl_ms = ttl_ms;
    event.pay.remote = remote;
    event.pay.via_usb = via_usb;
    apply_now(&event);
}

// ---- the USB script: one print, the same for this release and the last ---------------

static void run_usb_script(void)
{
    card_event_t event;

    event = (card_event_t){ .type = CARD_EV_STATS };
    card_utf8_copy(event.stats.name, sizeof(event.stats.name), "polyglot-dev");
    event.stats.level = 5;
    event.stats.today_cents = 4250;
    apply_now(&event);
    event = (card_event_t){ .type = CARD_EV_PING };
    apply_now(&event);
    event = (card_event_t){ .type = CARD_EV_SAY };
    card_utf8_copy(event.say, sizeof(event.say), "有人碰了我的卡");
    apply_now(&event);
    key(CARD_KEY_OK);
    key(CARD_KEY_UP);
    host_advance(7000);   // the link dot goes out

    // a task grabbed with OK, worked, delivered, accepted, paid with a new level
    send_offer(1432, 2500, false, false, 6000);
    key(CARD_KEY_UP);
    key(CARD_KEY_OK);
    send_job(CARD_JOB_BID, 0);
    send_job(CARD_JOB_WON, 0);
    send_job(CARD_JOB_WORK, 30);
    send_offer(1433, 900, false, false, 6000);   // refused: the card is working
    send_pay(7, 500, 20000, false, false);       // refused likewise
    send_job(CARD_JOB_WORK, 70);
    send_job(CARD_JOB_DONE, 100);
    send_job(CARD_JOB_HELD, 0);
    send_paid(1432, 1875, 8, false);
    host_advance(2400);
    host_advance(2200);
    host_advance(100);

    // an offer nobody answers; a payout that arrives while it is up is dropped
    send_offer(1434, 4000, true, false, 1500);
    send_paid(1, 100, 0, false);
    host_advance(1500);
    host_advance(1600);

    // handed over by the owner, then lost
    send_job(CARD_JOB_OWN, 0);
    send_job(CARD_JOB_WORK, 10);
    send_job(CARD_JOB_LOST, 0);
    send_job(CARD_JOB_WORK, 50);   // nothing: no job is under way

    // a payment from the card's wallet: a press only says to hold, the hold pays
    send_pay(8, 550, 20000, false, false);
    key(CARD_KEY_OK);              // inside the guard: ignored
    host_advance(900);
    key(CARD_KEY_OK);
    key_long(CARD_KEY_OK);
    host_advance(2400);
    send_pay(9, 550, 20000, false, false);
    host_advance(900);
    key(CARD_KEY_DOWN);
    host_advance(1500);
    send_pay(10, 550, 5000, false, false);
    host_advance(5000);
    host_advance(1500);

    // a request played as the server's, answered by the computer
    send_pay(11, 550, 20000, true, true);
    host_advance(900);
    key_long(CARD_KEY_OK);
    event = (card_event_t){ .type = CARD_EV_PAY_RESULT, .pay_result = { .id = 11, .ok = true, .from_usb = true } };
    apply_now(&event);
    host_advance(2400);
    send_pay(12, 550, 20000, true, true);
    host_advance(900);
    key_long(CARD_KEY_OK);
    host_advance(15000);   // no answer came
    host_advance(2400);

    // pairing code, paired
    event = (card_event_t){ .type = CARD_EV_PAIRING };
    card_utf8_copy(event.pairing.code, sizeof(event.pairing.code), "KTRX-ZPQM");
    apply_now(&event);
    send_offer(1435, 100, false, false, 1000);
    host_advance(1000);
    host_advance(1600);    // the code is back as the resting screen
    event = (card_event_t){ .type = CARD_EV_PAIRED };
    apply_now(&event);

    // the settings menu
    key_long(CARD_KEY_OK);
    key(CARD_KEY_DOWN);
    key(CARD_KEY_OK);      // sound on
    key(CARD_KEY_OK);      // sound off
    key(CARD_KEY_DOWN);
    key(CARD_KEY_OK);      // back
    key_long(CARD_KEY_OK);
    host_advance(15000);   // closes by itself
    key_long(CARD_KEY_UP); // nothing

    // phone setup screens, shown and closed
    event = (card_event_t){ .type = CARD_EV_SETUP, .setup = { .phase = CARD_SETUP_JOINED } };
    apply_now(&event);
    event = (card_event_t){ .type = CARD_EV_SETUP, .setup = { .phase = CARD_SETUP_OFF, .reason = 1 } };
    apply_now(&event);

    event = (card_event_t){ .type = CARD_EV_NET, .net_state = 2 };
    apply_now(&event);
    event = (card_event_t){ .type = CARD_EV_NET_STATS };
    card_utf8_copy(event.stats.name, sizeof(event.stats.name), "lingo-crab");
    apply_now(&event);
}

#ifndef CARD_APP_OLD

// ---- what the server tells the card -------------------------------------------------

static void send_task(int id, card_job_phase_t phase, bool called_off)
{
    card_event_t event = { .type = CARD_EV_TASK, .task = { .id = id, .phase = phase, .called_off = called_off } };
    apply_now(&event);
}

static void leave_menu(void)
{
    key(CARD_KEY_UP);   // from the first item to the last: "back"
    key(CARD_KEY_OK);
}

static void check_resting_line(void)
{
    static const struct {
        card_job_phase_t phase;
        const char *status;
        const char *busy;
    } LINES[] = {
        { CARD_JOB_BID, "status 竞标中…|0", "busy 1" },        { CARD_JOB_WON, "status 中标了，开工|0", "busy 1" },
        { CARD_JOB_OWN, "status 收到，这就开工|0", "busy 1" }, { CARD_JOB_WORK, "status 干活中…|0", "busy 1" },
        { CARD_JOB_DONE, "status 交付了，等验收|100", "busy 1" }, { CARD_JOB_HELD, "status 验收了，等放款|0", "busy 0" },
        { CARD_JOB_DISPUTED, "status 有争议，等裁定|0", "busy 0" },
    };
    for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
        step();
        send_task(7, LINES[i].phase, false);
        CHECK(said(LINES[i].status));
        CHECK(said(LINES[i].busy));
        CHECK(s_mode == MODE_IDLE);   // a job is never a mode of the card
        // a chime only for the news that the task is the agent's
        CHECK(said("sfx BLIP") == (LINES[i].phase == CARD_JOB_WON || LINES[i].phase == CARD_JOB_OWN));
    }
    step();
    send_task(0, CARD_JOB_BID, false);
    CHECK(said("status 摸鱼中，等单…|0") && said("busy 0"));
}

static void check_a_job_does_not_block_the_card(void)
{
    send_task(7, CARD_JOB_WORK, false);

    // a payment can be asked for and refused, and the line comes back
    step();
    send_pay(21, 550, 20000, false, false);
    CHECK(s_mode == MODE_PAY && said_start("overlay pay #21"));
    CHECK(!said_start("usb {\"t\":\"busy\""));
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_IDLE);
    CHECK(strcmp(last_of("status "), "status 干活中…|0") == 0 && strcmp(last_of("busy "), "busy 1") == 0);

    // the settings open and close, and the line comes back
    step();
    host_advance(HOLD_AFTER_CLOSE_MS);
    key_long(CARD_KEY_OK);
    CHECK(s_mode == MODE_MENU);
    leave_menu();
    CHECK(s_mode == MODE_IDLE && strcmp(last_of("status "), "status 干活中…|0") == 0);
    send_task(0, CARD_JOB_BID, false);
}

static void check_the_new_task_sheet(void)
{
    send_task(8, CARD_JOB_BID, false);

    // OK closes it. It takes nothing: no "accept", no work mode; the line says what the agent does.
    step();
    send_offer(8, 2500, false, true, 20000);
    CHECK(said("overlay offer #8 remote=1 direct=0 amt=2500 ttl=20000 把 50 页合同翻成英文") && said("sfx RING"));
    CHECK(s_mode == MODE_OFFER);
    key(CARD_KEY_OK);
    CHECK(said("usb {\"t\":\"seen\",\"id\":8}") && !said_start("usb {\"t\":\"accept\""));
    CHECK(s_mode == MODE_IDLE && strcmp(last_of("status "), "status 竞标中…|0") == 0);
    // the same press, held on: it answered the sheet, it does not open the menu
    key_long(CARD_KEY_OK);
    CHECK(s_mode == MODE_IDLE);
    host_advance(HOLD_AFTER_CLOSE_MS);
    key_long(CARD_KEY_OK);
    CHECK(s_mode == MODE_MENU);
    leave_menu();

    // any key closes it
    step();
    send_offer(8, 2500, true, true, 20000);
    CHECK(said_start("overlay offer #8 remote=1 direct=1"));
    key(CARD_KEY_DOWN);
    CHECK(s_mode == MODE_IDLE && said("usb {\"t\":\"seen\",\"id\":8}"));

    // left alone it closes when its time is up, and nothing was missed
    step();
    send_offer(8, 2500, false, true, 20000);
    host_advance(19999);
    CHECK(s_mode == MODE_OFFER);
    host_advance(1);
    CHECK(s_mode == MODE_IDLE);
    CHECK(!said_start("overlay miss") && !said("sfx LOSE") && !said_start("usb {\"t\":\"miss\""));
    CHECK(strcmp(last_of("status "), "status 竞标中…|0") == 0);

    // something that needs an answer takes its place, and the sheet comes back afterwards
    step();
    send_offer(8, 2500, false, true, 20000);
    host_advance(3000);
    send_pay(22, 550, 20000, false, false);
    CHECK(s_mode == MODE_PAY && said_start("overlay pay #22") && !said_start("usb {\"t\":\"busy\""));
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_OFFER && times("overlay offer #8 remote=1") == 2);
    key(CARD_KEY_OK);
    CHECK(s_mode == MODE_IDLE);

    // a computer's offer takes its place too, is answered as ever, and the sheet comes back
    step();
    send_offer(8, 2500, false, true, 20000);
    send_offer(33, 900, false, false, 6000);
    CHECK(s_mode == MODE_OFFER && said_start("overlay offer #33 remote=0") && !said_start("usb {\"t\":\"busy\""));
    host_advance(6000);
    CHECK(s_mode == MODE_MISS && said("usb {\"t\":\"miss\",\"id\":33}"));
    host_advance(MISS_SHOW_MS);
    CHECK(s_mode == MODE_OFFER && times("overlay offer #8 remote=1") == 2);
    key(CARD_KEY_OK);
    send_task(0, CARD_JOB_BID, false);
}

static void check_the_computers_offer_is_as_before(void)
{
    step();
    send_offer(31, 2500, false, false, 6000);
    key(CARD_KEY_OK);
    CHECK(said("usb {\"t\":\"accept\",\"id\":31}") && s_mode == MODE_WORK && said("status 竞标中…|0"));
    // the server's word arrives while the computer's job runs: kept, shown when the card rests
    step();
    send_task(5, CARD_JOB_DONE, false);
    CHECK(!said_start("status "));
    send_job(CARD_JOB_LOST, 0);
    CHECK(s_mode == MODE_IDLE && said("status 交付了，等验收|100") && said("say 这单没中"));
    send_task(0, CARD_JOB_BID, false);

    step();
    send_offer(32, 2500, false, false, 1500);
    send_paid(32, 100, 0, false);   // from the computer: dropped while its offer is up, as before
    host_advance(1500);
    CHECK(s_mode == MODE_MISS && said("overlay miss #32") && said("usb {\"t\":\"miss\",\"id\":32}"));
    host_advance(MISS_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && !said_start("overlay paid"));
}

static void check_sheets_wait_their_turn(void)
{
    // a payout that arrives while the menu is open is shown when the menu closes
    step();
    host_advance(HOLD_AFTER_CLOSE_MS);
    key_long(CARD_KEY_OK);
    send_paid(41, 1875, 0, true);
    CHECK(s_mode == MODE_MENU && !said_start("overlay paid"));
    leave_menu();
    CHECK(s_mode == MODE_PAID && said("overlay paid #41 net=1875 lvl=0 把 50 页合同翻成英文") && said("sfx COIN"));
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_IDLE);

    // while a payment is being answered: a payout and a new task both wait; the payout goes first,
    // with its level, then the task
    step();
    send_pay(42, 550, 20000, false, false);
    send_paid(43, 2000, 2, true);
    send_offer(44, 1200, false, true, 20000);
    CHECK(s_mode == MODE_PAY && !said_start("overlay paid") && !said_start("overlay offer"));
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_PAID && said_start("overlay paid #43 net=2000 lvl=2"));
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_LEVEL_UP && said("overlay level_up 2"));
    host_advance(LEVEL_UP_SHOW_MS);
    CHECK(s_mode == MODE_OFFER && said_start("overlay offer #44 remote=1"));
    key(CARD_KEY_OK);
    CHECK(s_mode == MODE_IDLE);

    // a new task that waited for more than a minute is no longer news
    step();
    send_pay(45, 550, 90000, false, false);
    send_offer(46, 1200, false, true, 20000);
    host_advance(NOTICE_KEEP_MS + 1000);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && !said_start("overlay offer"));

    // a card that was off is told up to three payouts: all three wait, and are shown in the order they came
    step();
    host_advance(HOLD_AFTER_CLOSE_MS);
    key_long(CARD_KEY_OK);
    send_paid(51, 100, 0, true);
    send_paid(52, 200, 0, true);
    send_paid(53, 300, 0, true);
    leave_menu();
    CHECK(s_mode == MODE_PAID && times("overlay paid #") == 1 && said_start("overlay paid #51 net=100"));
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_PAID && said_start("overlay paid #52 net=200"));
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_PAID && said_start("overlay paid #53 net=300"));
    CHECK(where("overlay paid #51") < where("overlay paid #52") && where("overlay paid #52") < where("overlay paid #53"));
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && times("overlay paid #") == 3);

    // a payout sheet gives way to a pay request (the person at the counter is not told "busy"),
    // and is shown again afterwards, with its level
    step();
    send_paid(61, 1875, 3, true);
    CHECK(s_mode == MODE_PAID);
    send_pay(62, 550, 20000, false, false);
    CHECK(s_mode == MODE_PAY && said_start("overlay pay #62") && !said_start("usb {\"t\":\"busy\""));
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_PAID && times("overlay paid #61 net=1875 lvl=3") == 2);
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_LEVEL_UP && said("overlay level_up 3"));
    send_pay(63, 550, 20000, false, false);   // and so does the level sheet; the payout itself was seen
    CHECK(s_mode == MODE_PAY);
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && times("overlay paid #61") == 2);

    // a sheet that waited does not cover what the card says at that moment: the word first
    step();
    send_offer(34, 2500, false, false, 6000);
    key(CARD_KEY_OK);                 // a computer's job is under way
    send_paid(64, 500, 0, true);      // the server's payout waits behind it
    send_job(CARD_JOB_LOST, 0);
    CHECK(s_mode == MODE_IDLE && said("say 这单没中") && !said_start("overlay paid"));
    host_advance(WORD_SHOW_MS);
    CHECK(s_mode == MODE_PAID && said_start("overlay paid #64"));
    host_advance(PAID_SHOW_MS);
    CHECK(s_mode == MODE_IDLE);

    // when the pairing ends, what still waited goes with it
    step();
    host_advance(HOLD_AFTER_CLOSE_MS);
    key_long(CARD_KEY_OK);
    send_paid(65, 500, 0, true);
    send_offer(66, 1200, false, true, 20000);
    card_event_t unpaired = { .type = CARD_EV_TASK, .task = { .reset = true } };
    apply_now(&unpaired);
    leave_menu();
    CHECK(s_mode == MODE_IDLE && !said_start("overlay paid") && !said_start("overlay offer"));
}

static void check_a_task_that_ends_without_a_payout(void)
{
    step();
    send_task(9, CARD_JOB_BID, false);
    send_task(9, CARD_JOB_LOST, false);
    CHECK(said("say 这单没中") && strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0 && s_task.id == 0);
    step();
    send_task(9, CARD_JOB_WORK, false);
    send_task(9, CARD_JOB_LOST, true);
    CHECK(said("say 这单结束了，没有到账") && strcmp(last_of("busy "), "busy 0") == 0);

    // behind a sheet nothing is said; when the sheet goes the line rests
    step();
    send_task(9, CARD_JOB_WORK, false);
    send_pay(47, 550, 20000, false, false);
    send_task(9, CARD_JOB_LOST, true);
    CHECK(!said_start("say "));
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0);
}

// ---- the client: what it asks, and what it does with the answers ------------------------

static const char PROFILE[] =
    "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
    "\"total_tasks_completed\":0,\"success_rate\":0.0,\"total_earned\":0,\"settled_total\":0},"
    "\"limits\":{\"per_payment_cents\":5000,\"daily_cents\":20000,\"remaining_today_cents\":20000},\"settles_money\":false}";

static const char *s_task_reply = "";   // what the task route answers next
static const char *s_pay_reply = "{\"request\":null}";   // and the pay route
static const char *s_profile_reply = PROFILE;             // and the card's own view
static int s_task_status = 200, s_profile_status = 200, s_pay_status = 200;
static int s_task_asks, s_profile_asks, s_pay_asks, s_pairing_asks;
static char s_last_task_url[512];
static char s_last_auth[160];
static char s_pay_auth[160];   // what the last question about pay requests carried
// Pairing: what the server answers a request for a code, and a question about one. As they stand,
// a server that does not offer pairing.
static int s_pair_start_status = 404, s_pair_poll_status = 404;
static const char *s_pair_start_reply = "{\"detail\":\"Not found\"}";
static const char *s_pair_poll_reply = "{\"detail\":\"Not found\"}";
static int s_pair_starts, s_pair_polls;
// A pay decision: the status each try gets, in turn (200 once the list is used up), and the 200's body.
static int s_decision_statuses[4], s_decision_count, s_decision_asks;
static const char *s_decision_reply = "";
static char s_last_decision[320];
// What the server itself says against an approval (the body of a 400 or a 423), and every body the
// card sent, in order.
static const char *s_decision_refusal = "<html>not the server</html>";
static char s_decision_bodies[8][160];
// How often a request to the local test server carried the token the card holds for the site.
static int s_site_token_elsewhere;

static int scripted(const char *method, const char *url, const char *auth, const char *body, char *reply, size_t size)
{
    (void)method;
    (void)body;
    if (strncmp(url, "http://192.168.", 15) == 0 && strstr(auth, "chd_test-token")) s_site_token_elsewhere++;
    if (strstr(url, "/api/cards/device/tasks/next")) {
        s_task_asks++;
        snprintf(s_last_task_url, sizeof(s_last_task_url), "%s", url);
        snprintf(s_last_auth, sizeof(s_last_auth), "%s", auth);
        snprintf(reply, size, "%s", s_task_reply);
        return s_task_status;
    }
    if (strstr(url, "/api/cards/device/me")) {
        s_profile_asks++;
        snprintf(reply, size, "%s", s_profile_status == 200 ? s_profile_reply : "<html>refused</html>");
        return s_profile_status;
    }
    if (strstr(url, "/pay-requests/next")) {
        s_pay_asks++;
        snprintf(s_pay_auth, sizeof(s_pay_auth), "%s", auth);
        snprintf(reply, size, "%s", s_pay_status == 200 ? s_pay_reply : "<html>refused</html>");
        return s_pay_status;
    }
    if (strstr(url, "/decision")) {
        const int status = s_decision_asks < s_decision_count ? s_decision_statuses[s_decision_asks] : 200;
        if (s_decision_asks < 8) snprintf(s_decision_bodies[s_decision_asks], sizeof(s_decision_bodies[0]), "%s", body ? body : "");
        s_decision_asks++;
        snprintf(s_last_decision, sizeof(s_last_decision), "%s %s", url, body ? body : "");
        snprintf(reply, size, "%s",
                 status == 200 ? s_decision_reply : status == 409 ? "{\"detail\":\"This card has reached today's limit.\"}"
                               : status == 400 || status == 423 ? s_decision_refusal : "<html>not the server</html>");
        return status;
    }
    if (strstr(url, "/api/cards/pairing/token")) {
        s_pairing_asks++;
        s_pair_polls++;
        snprintf(reply, size, "%s", s_pair_poll_reply);
        return s_pair_poll_status;
    }
    if (strstr(url, "/api/cards/pairing")) {
        s_pairing_asks++;
        s_pair_starts++;
        snprintf(reply, size, "%s", s_pair_start_reply);
        return s_pair_start_status;
    }
    snprintf(reply, size, "{}");
    return 404;
}

static void cloud_round(uint32_t after_ms)
{
    host_advance(after_ms);
    host_task_round("cloud");
    pump();
}

// As if the card had been paired again: its settings are read anew and find a token in flash.
static void pair_again(void)
{
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b9.d4.p8.j0-none\",\"poll_s\":15}";
    CHECK(cloud_unpair() == ESP_OK);
    host_store_put("token", "chd_test-token");
    cloud_round(100);
    CHECK(strcmp(cloud_state_name(), "paired") == 0);
}

static void check_the_client(void)
{
    host_store_put("token", "chd_test-token");
    host_store_put("tcur", "v1.a1.b3.d4.p5.j0-none");   // kept from before the card was switched off
    host_http_set(scripted);
    CHECK(cloud_start() == ESP_OK);

    // the first request carries the cursor the card kept, and the card's token
    step();
    s_task_reply = "{\"job\":{\"id\":7,\"phase\":\"bid\"},\"event\":{\"kind\":\"new_task\",\"id\":7,\"title\":\"\","
                   "\"private\":true,\"amount_cents\":2500,\"direct\":false,\"ttl_s\":20},"
                   "\"cursor\":\"v1.a1.b9.d4.p5.j7-bid\",\"poll_s\":15}";
    cloud_round(100);
    CHECK(s_task_asks == 1 && strstr(s_last_task_url, "/api/cards/device/tasks/next?cursor=v1.a1.b3.d4.p5.j0-none"));
    CHECK(strcmp(s_last_auth, "Bearer chd_test-token") == 0);
    CHECK(said("status 竞标中…|0"));
    // a private order's title never reached the card: it prints its own words
    CHECK(said("overlay offer #7 remote=1 direct=0 amt=2500 ttl=20000 私密订单") && s_mode == MODE_OFFER);
    CHECK(strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p5.j7-bid") == 0 && host_store_writes("tcur") == 1);
    key(CARD_KEY_OK);

    // nothing new: the same cursor comes back and nothing is written to flash; asked every 15 s
    step();
    s_task_reply = "{\"job\":{\"id\":7,\"phase\":\"bid\"},\"event\":null,\"cursor\":\"v1.a1.b9.d4.p5.j7-bid\",\"poll_s\":15}";
    cloud_round(14000);
    CHECK(s_task_asks == 1);
    cloud_round(1500);
    CHECK(s_task_asks == 2 && strstr(s_last_task_url, "?cursor=v1.a1.b9.d4.p5.j7-bid"));
    CHECK(host_store_writes("tcur") == 1 && !said_start("status ") && !said_start("overlay "));

    // an answer the card cannot use shows nothing and moves nothing: no cursor, a cursor that would
    // need escaping in a URL, a body that was cut off, a server error
    static const char *const USELESS[] = {
        "{\"job\":{\"id\":7,\"phase\":\"work\"},\"event\":null,\"poll_s\":15}",
        "{\"job\":{\"id\":7,\"phase\":\"work\"},\"event\":null,\"cursor\":\"v1 a1&x=1\",\"poll_s\":15}",
        "{\"job\":{\"id\":7,\"phase\":\"work\"},\"event\":{\"kind\":\"paid\",\"id\":7,\"ti",
        "[1,2,3]",
    };
    for (size_t i = 0; i < sizeof(USELESS) / sizeof(USELESS[0]); i++) {
        step();
        s_task_reply = USELESS[i];
        cloud_round(16000);
        CHECK(!said_start("status ") && !said_start("overlay ") && host_store_writes("tcur") == 1);
    }
    step();
    s_task_status = 500;
    cloud_round(16000);
    CHECK(!said_start("status ") && host_store_writes("tcur") == 1);
    s_task_status = 200;

    // a phase or a kind of sheet this firmware has no words for comes from a newer server: nothing
    // is shown for it, and the cursor is still kept (it would be told the same for ever otherwise)
    step();
    s_task_reply = "{\"job\":{\"id\":7,\"phase\":\"audited\"},\"event\":{\"kind\":\"badge\",\"id\":1},"
                   "\"cursor\":\"v1.a1.b9.d4.p5.j7-audited\",\"poll_s\":15}";
    cloud_round(16000);
    CHECK(!said_start("status ") && !said_start("overlay ") && strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p5.j7-audited") == 0);

    // a payout: the sheet, and the face's numbers are fetched again within seconds, not a minute
    step();
    s_task_reply = "{\"job\":null,\"event\":{\"kind\":\"paid\",\"id\":7,\"title\":\"Translate\",\"private\":false,"
                   "\"net_cents\":1875,\"level\":2},\"cursor\":\"v1.a1.b9.d4.p6.j0-none\",\"poll_s\":30}";
    cloud_round(16000);
    CHECK(said("overlay paid #7 net=1875 lvl=2 Translate") && said("status 摸鱼中，等单…|0"));
    const int profiles = s_profile_asks;   // whatever was fetched up to the payout's own round
    cloud_round(1000);
    CHECK(s_profile_asks == profiles);
    cloud_round(1500);
    CHECK(s_profile_asks == profiles + 1);
    host_advance(PAID_SHOW_MS + LEVEL_UP_SHOW_MS);

    // the server said 30 s: that is now the pace
    const int asks = s_task_asks;
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b9.d4.p6.j0-none\",\"poll_s\":30}";
    cloud_round(20000);
    CHECK(s_task_asks == asks);
    cloud_round(12000);
    CHECK(s_task_asks == asks + 1);

    // a server that has no such route is asked again only after ten minutes
    s_task_status = 404;
    cloud_round(31000);
    const int before = s_task_asks;
    for (int i = 0; i < 9; i++) cloud_round(60000);
    CHECK(s_task_asks == before);
    cloud_round(120000);
    CHECK(s_task_asks == before + 1);

    // what the screen could not take is asked for again: the answer's cursor is kept only once the
    // job and the sheet are both in the screen's queue (the server says each thing once)
    step();
    s_task_status = 200;
    s_task_reply = "{\"job\":{\"id\":8,\"phase\":\"work\"},\"event\":{\"kind\":\"paid\",\"id\":9,\"title\":\"Earlier\","
                   "\"private\":false,\"net_cents\":700,\"level\":0},\"cursor\":\"v1.a1.b9.d4.p7.j8-work\",\"poll_s\":15}";
    card_event_t filler = { .type = CARD_EV_NET, .net_state = 2 };
    for (int i = 0; i < EVENT_QUEUE_DEPTH; i++) card_app_post(&filler);   // the queue is full
    host_advance(600000);
    host_task_round("cloud");
    CHECK(strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p6.j0-none") == 0);
    pump();
    CHECK(!said_start("overlay paid") && !said("status 干活中…|0"));
    const int writes = host_store_writes("tcur");
    cloud_round(31000);   // asked again (at the 30 s pace set above) with the old cursor: now taken whole
    CHECK(strstr(s_last_task_url, "?cursor=v1.a1.b9.d4.p6.j0-none") != NULL);
    CHECK(said("status 干活中…|0") && times("overlay paid #9 net=700") == 1);
    CHECK(strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p7.j8-work") == 0 && host_store_writes("tcur") == writes + 1);
    host_advance(PAID_SHOW_MS);
    CHECK(strcmp(last_of("status "), "status 干活中…|0") == 0);
    // with room for the job but not for the sheet: the job is not told twice, the sheet comes next time
    step();
    s_task_reply = "{\"job\":{\"id\":8,\"phase\":\"done\"},\"event\":{\"kind\":\"paid\",\"id\":10,\"title\":\"Later\","
                   "\"private\":false,\"net_cents\":800,\"level\":0},\"cursor\":\"v1.a1.b9.d4.p8.j8-done\",\"poll_s\":15}";
    cloud_round(1000);     // the profile fetch that follows a payout happens here, not in the round below
    host_advance(15000);
    for (int i = 0; i < EVENT_QUEUE_DEPTH - 1; i++) card_app_post(&filler);
    host_task_round("cloud");
    pump();
    CHECK(times("status 交付了，等验收|100") == 1 && !said_start("overlay paid"));
    CHECK(strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p7.j8-work") == 0);
    cloud_round(16000);
    CHECK(times("overlay paid #10 net=800") == 1 && strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p8.j8-done") == 0);
    host_advance(PAID_SHOW_MS);

    // a 403 is not the server's answer to a card (it sends 401): it comes from the site's edge and
    // does not cost the card its pairing, whichever of its three requests meets it
    step();
    s_task_status = s_profile_status = s_pay_status = 403;
    s_task_reply = "<html>challenge</html>";
    const int asked[] = { s_task_asks, s_profile_asks, s_pay_asks };
    cloud_round(61000);   // all three are due
    CHECK(s_task_asks > asked[0] && s_profile_asks > asked[1] && s_pay_asks > asked[2]);
    CHECK(host_store_peek("token")[0] != '\0' && strcmp(cloud_state_name(), "paired") == 0);
    CHECK(strcmp(host_store_peek("tcur"), "v1.a1.b9.d4.p8.j8-done") == 0 && !said_start("status "));
    CHECK(!said_start("usb {\"t\":\"cloud\",\"token\":\"rejected\""));
    s_profile_status = s_pay_status = 200;

    // the token was revoked: the card forgets it and what it was told, and the face rests
    step();
    s_task_status = 200;
    s_task_reply = "{\"job\":{\"id\":8,\"phase\":\"work\"},\"event\":null,\"cursor\":\"v1.a1.b9.d4.p8.j8-work\",\"poll_s\":15}";
    cloud_round(16000);
    CHECK(said("status 干活中…|0"));
    step();
    s_task_status = 401;
    cloud_round(16000);
    CHECK(host_store_peek("token")[0] == '\0' && host_store_peek("tcur")[0] == '\0');
    CHECK(said("status 摸鱼中，等单…|0") && strcmp(cloud_state_name(), "paired") != 0);
    cloud_round(1000);
    CHECK(s_pairing_asks > 0);   // it asks for a new pairing code
    s_task_status = 200;

    // the same 401 from either of the card's other two requests ends the pairing as well
    pair_again();
    step();
    s_profile_status = 401;
    cloud_round(61000);
    CHECK(host_store_peek("token")[0] == '\0' && strcmp(cloud_state_name(), "paired") != 0);
    CHECK(said("usb {\"t\":\"cloud\",\"token\":\"rejected\",\"http\":401}"));
    s_profile_status = 200;

    pair_again();
    step();
    s_pay_status = 401;
    cloud_round(5000);
    CHECK(host_store_peek("token")[0] == '\0' && strcmp(cloud_state_name(), "paired") != 0);
    s_pay_status = 200;
}

// ---- the client at the site's edge: a pairing code and a pay decision ----------------------------
//
// Between the card and the server sits the site's edge. It can answer in the server's place: a 403
// with a challenge page, a rate limit, a gateway error. None of these is the server's word.

static const char CODE_1[] =
    "{\"device_code\":\"device-code-one\",\"user_code\":\"BCDF-GHJK\","
    "\"verification_uri\":\"https://clawhunt.store/card/pair\",\"expires_in\":600,\"interval\":5}";
static const char CODE_2[] =
    "{\"device_code\":\"device-code-two\",\"user_code\":\"LMNP-QRST\","
    "\"verification_uri\":\"https://clawhunt.store/card/pair\",\"expires_in\":600,\"interval\":5}";
static const char PENDING[] = "{\"error\":\"authorization_pending\"}";
static const char NO_SUCH_ROUTE[] = "{\"detail\":\"Not found\"}";
static const char CHALLENGE[] =
    "<!DOCTYPE html><html><head><title>Just a moment...</title></head><body>not the server</body></html>";
static const char QUIET[] = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b9.d4.p8.j0-none\",\"poll_s\":15}";

// Unpaired, and the server hands out this code: the card asks for it at once.
static void fresh_code(const char *code)
{
    s_pair_start_status = 200;
    s_pair_start_reply = code;
    s_pair_poll_status = 400;
    s_pair_poll_reply = PENDING;
    CHECK(cloud_unpair() == ESP_OK);
    cloud_round(100);
    CHECK(strcmp(cloud_state_name(), "pairing") == 0);
}

static void check_a_pairing_code_at_the_edge(void)
{
    s_task_reply = QUIET;
    static const char GONE[] = "usb {\"t\":\"cloud\",\"pairing\":\"gone\"}";

    // up to here the card never held a pairing code (its requests for one were refused), so it
    // never had one to call gone
    int gone = 0;
    for (int i = 0; i < host_log_count; i++) gone += strcmp(host_log[i], GONE) == 0;
    CHECK(gone == 0);

    // the card asks for a code, shows it, and asks about it every five seconds
    step();
    fresh_code(CODE_1);
    CHECK(said("overlay pairing BCDF-GHJK") && s_mode == MODE_IDLE);
    int starts = s_pair_starts, polls = s_pair_polls;
    cloud_round(5000);
    cloud_round(5000);
    CHECK(s_pair_polls == polls + 2 && s_pair_starts == starts);

    // a refusal that is not the server's word on the code (the edge's challenge page, an empty 403,
    // an error of another shape) leaves the code on the screen, and the card goes on asking about it
    static const struct {
        int status;
        const char *body;
    } NOT_ABOUT_THE_CODE[] = {
        { 403, CHALLENGE }, { 403, "" }, { 400, "{\"detail\":\"Bad request\"}" }, { 422, "{\"detail\":[{\"msg\":\"x\"}]}" },
    };
    step();
    for (size_t i = 0; i < sizeof(NOT_ABOUT_THE_CODE) / sizeof(NOT_ABOUT_THE_CODE[0]); i++) {
        s_pair_poll_status = NOT_ABOUT_THE_CODE[i].status;
        s_pair_poll_reply = NOT_ABOUT_THE_CODE[i].body;
        polls = s_pair_polls;
        cloud_round(5000);
        cloud_round(5000);
        CHECK(s_pair_polls == polls + 2 && s_pair_starts == starts && strcmp(cloud_state_name(), "pairing") == 0);
    }
    CHECK(!said_start("overlay ") && !said_start("usb {\"t\":\"cloud\",\"pairing\""));

    // and the owner's approval still reaches the card on that same code
    step();
    s_pair_poll_status = 200;
    s_pair_poll_reply = "{\"device_token\":\"chd_from-the-first-code\",\"public_id\":\"abcdefghjk\"}";
    cloud_round(5000);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(host_store_peek("token"), "chd_from-the-first-code") == 0);
    CHECK(said("overlay close") && said("say 绑定好了") && s_pair_starts == starts);

    // a rate limit slows the card down, five seconds a time and no slower than every thirty, and
    // does not cost it the code
    step();
    fresh_code(CODE_1);
    s_pair_poll_status = 429;
    s_pair_poll_reply = "{\"detail\":\"Too many requests. Please wait a moment.\"}";
    starts = s_pair_starts;
    int asked_at[16], count = 0;
    for (int second = 1; second <= 200; second++) {
        const int before = s_pair_polls;
        cloud_round(1000);
        if (s_pair_polls != before && count < 16) asked_at[count++] = second;
    }
    // 5 s after the code, then 10, 15, 20, 25 and 30 s apart, and every 30 s from there on
    static const int EXPECTED[] = { 5, 15, 30, 50, 75, 105, 135, 165, 195 };
    CHECK(count == 9);
    for (int i = 0; i < 9 && i < count; i++) CHECK(asked_at[i] == EXPECTED[i]);
    CHECK(s_pair_starts == starts && strcmp(cloud_state_name(), "pairing") == 0);
    CHECK(said("overlay pairing BCDF-GHJK") && times("overlay ") == 1);

    // the server says the code has run out: the card asks for a new one, which takes the old one's
    // place on the screen without the sheet going away in between
    step();
    s_pair_poll_status = 400;
    s_pair_poll_reply = "{\"error\":\"expired_token\"}";
    s_pair_start_reply = CODE_2;
    cloud_round(30000);   // the question about the code (every 30 s after the rate limit)
    cloud_round(30000);   // the request for a new one
    CHECK(s_pair_starts == starts + 1 && said("overlay pairing LMNP-QRST") && !said("overlay close"));

    // a server that knows of no pairing at all (the feature was switched off) says 404: the code is
    // dropped, no new one comes, and the dead code leaves the screen
    step();
    s_pair_poll_status = s_pair_start_status = 404;
    s_pair_poll_reply = s_pair_start_reply = NO_SUCH_ROUTE;
    starts = s_pair_starts;
    cloud_round(5000);    // the question
    CHECK(!said("overlay close"));   // a new code is asked for first: it might take the old one's place
    cloud_round(5000);    // the request for a new code, refused
    CHECK(s_pair_starts == starts + 1 && said("overlay close") && said(GONE));
    CHECK(strcmp(cloud_state_name(), "unpaired") == 0 && s_mode == MODE_IDLE);
    cloud_round(16000);   // and refused again: the sheet went once, and is said gone once
    CHECK(s_pair_starts == starts + 2 && times("overlay close") == 1 && times(GONE) == 1);
    // nor does it come back as the resting screen
    step();
    send_pay(48, 550, 20000, false, false);
    host_advance(900);
    key(CARD_KEY_UP);
    host_advance(PAY_NO_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && !said_start("overlay pairing"));

    // until the server hands out codes again
    step();
    s_pair_start_status = 200;
    s_pair_start_reply = CODE_1;
    s_pair_poll_status = 400;
    s_pair_poll_reply = PENDING;
    cloud_round(31000);
    CHECK(said("overlay pairing BCDF-GHJK") && strcmp(cloud_state_name(), "pairing") == 0);

    // a code that runs out while the server hands out no more: the card is not left "pairing"
    step();
    s_pair_start_status = 404;
    s_pair_start_reply = NO_SUCH_ROUTE;
    for (int i = 0; i < 119; i++) cloud_round(5000);
    CHECK(!said("overlay close") && strcmp(cloud_state_name(), "pairing") == 0);   // 9 min 55 s: still good
    cloud_round(5000);
    cloud_round(5000);
    CHECK(said("overlay close") && times(GONE) == 1 && strcmp(cloud_state_name(), "unpaired") == 0);
    step();
    s_pair_start_status = 200;
    s_pair_start_reply = CODE_1;
    cloud_round(16000);
    CHECK(said("overlay pairing BCDF-GHJK"));

    // a code that runs out while the card is offline does not stay on the screen either
    step();
    host_online = false;
    for (int i = 0; i < 9; i++) cloud_round(60000);
    CHECK(!said("overlay close"));   // nine minutes: still good
    cloud_round(61000);
    CHECK(said("overlay close") && said(GONE));
    step();
    host_online = true;
    cloud_round(1000);
    CHECK(said("overlay pairing BCDF-GHJK"));

    // nor one that runs out while Wi-Fi setup owns the card (it asks the server nothing then)
    step();
    host_setup_active = true;
    cloud_round(540000);
    CHECK(!said("overlay close"));
    cloud_round(61000);
    CHECK(said("overlay close") && times(GONE) == 1);
    step();
    host_setup_active = false;
    cloud_round(1000);
    CHECK(said("overlay pairing BCDF-GHJK"));

    // a code the server ended just before the card went offline goes at once, not when its ten
    // minutes would have been up
    step();
    s_pair_poll_reply = "{\"error\":\"expired_token\"}";
    cloud_round(5000);    // the question: the code has ended; a new one is to be asked for
    CHECK(!said("overlay close"));
    host_online = false;
    cloud_round(1000);
    CHECK(said("overlay close") && times(GONE) == 1);
    step();
    host_online = true;
    s_pair_poll_reply = PENDING;
    cloud_round(5000);
    CHECK(said("overlay pairing BCDF-GHJK"));

    // pointed at another server, which hands out no code: the first server's code and address do
    // not stay on the screen
    step();
    s_pair_start_status = -1;
    CHECK(cloud_set_base("http://192.168.0.10:8496") == ESP_OK);
    cloud_round(100);
    CHECK(said("overlay close") && strcmp(cloud_state_name(), "unpaired") == 0);
    step();
    s_pair_start_status = 200;
    CHECK(cloud_set_base("") == ESP_OK);
    cloud_round(100);
    CHECK(said("overlay pairing BCDF-GHJK") && strcmp(cloud_base(), "https://clawhunt.store") == 0);
}

// The server asks the card to pay; the request is on the screen and takes keys.
static void server_asks_to_pay(int id)
{
    static char request[200];
    snprintf(request, sizeof(request),
             "{\"request\":{\"id\":%d,\"payee\":\"Lobby Cafe\",\"memo\":\"latte\",\"match_code\":\"284\","
             "\"amount_cents\":550,\"expires_in\":30}}", id);
    s_pay_reply = request;
    cloud_round(4000);
    s_pay_reply = "{\"request\":null}";
    CHECK(s_mode == MODE_PAY && s_pay.id == id);
    host_advance(900);   // keys are ignored just after a request appears
}

// The holder answers; the tries of that decision get these statuses in turn, then 200.
static void holder_answers(bool approve, const int *statuses, int count)
{
    for (int i = 0; i < count; i++) s_decision_statuses[i] = statuses[i];
    s_decision_count = count;
    s_decision_asks = 0;
    if (approve) {
        key_long(CARD_KEY_OK);
    } else {
        key(CARD_KEY_UP);
    }
    host_task_round("cloud");
    pump();
}

static void check_a_pay_decision_at_the_edge(void)
{
    char line[64];
    // paired (its settings, read anew, find a token): the code it was showing goes
    step();
    pair_again();
    CHECK(said("overlay close") && said("usb {\"t\":\"cloud\",\"pairing\":\"gone\"}"));
    host_advance(3000);
    s_decision_reply = "{\"id\":1,\"status\":\"approved\"}";

    // the edge answers the first try with a challenge page; the second, a moment later, reaches the server
    step();
    server_asks_to_pay(51);
    uint32_t paused = host_paused_ms;
    holder_answers(true, (const int[]){ 403 }, 1);
    CHECK(s_decision_asks == 2 && host_paused_ms - paused == 700);
    CHECK(strstr(s_last_decision, "/api/cards/device/pay-requests/51/decision {\"approve\":true,\"reason\":\"approved\"}"));
    CHECK(said("overlay pay_done #51 paid=1") && said("usb {\"t\":\"cloud\",\"decision\":51,\"approve\":1,\"http\":200}"));
    host_advance(3000);

    // a rate limit, a gateway error and no answer at all are tried again as well. After no answer
    // the next try goes at once: that request already waited out its time, and the screen stops waiting
    static const int NOT_AN_ANSWER[] = { 429, 502, 503, -1 };
    for (int i = 0; i < 4; i++) {
        step();
        server_asks_to_pay(52 + i);
        paused = host_paused_ms;
        holder_answers(true, &NOT_AN_ANSWER[i], 1);
        snprintf(line, sizeof(line), "overlay pay_done #%d paid=1", 52 + i);
        CHECK(s_decision_asks == 2 && said(line));
        CHECK(host_paused_ms - paused == (NOT_AN_ANSWER[i] < 0 ? 0u : 700u));
        host_advance(3000);
    }

    // three tries and no more: the holder is told it did not go through
    step();
    server_asks_to_pay(60);
    paused = host_paused_ms;
    holder_answers(true, (const int[]){ 403, 403, 403 }, 3);
    CHECK(s_decision_asks == 3 && host_paused_ms - paused == 1400);
    CHECK(said("overlay pay_failed 没付成") && !said_start("overlay pay_done"));
    CHECK(said("usb {\"t\":\"cloud\",\"decision\":60,\"approve\":1,\"http\":403}"));
    host_advance(3000);

    // what the server itself says is final, whatever it says: asked once, without a pause
    static const int AN_ANSWER[] = { 409, 404, 401, 400 };
    for (int i = 0; i < 4; i++) {
        step();
        server_asks_to_pay(61 + i);
        paused = host_paused_ms;
        holder_answers(true, &AN_ANSWER[i], 1);
        CHECK(s_decision_asks == 1 && host_paused_ms == paused && said("overlay pay_failed 没付成"));
        host_advance(3000);
    }
    step();
    server_asks_to_pay(65);
    s_decision_reply = "{\"id\":65,\"status\":\"expired\"}";   // it took the decision too late
    holder_answers(true, NULL, 0);
    CHECK(s_decision_asks == 1 && said("overlay pay_failed 没付成"));
    host_advance(3000);

    // a refusal goes the same way (the holder saw the result at once, without waiting for it)
    step();
    server_asks_to_pay(70);
    s_decision_reply = "{\"id\":70,\"status\":\"declined\"}";
    holder_answers(false, (const int[]){ 403 }, 1);
    CHECK(s_decision_asks == 2 && strstr(s_last_decision, "/pay-requests/70/decision {\"approve\":false,"));
    CHECK(said("usb {\"t\":\"cloud\",\"decision\":70,\"approve\":0,\"http\":200}"));
    host_advance(3000);
    // back at rest, on the face: a paired card has no pairing code to rest on
    CHECK(s_mode == MODE_IDLE && strcmp(cloud_state_name(), "paired") == 0 && !said_start("overlay pairing"));
}

// ---- the holder's secret ---------------------------------------------------------------------
//
// The server may want the holder's secret with an approval. The card asks for it after OK was
// held, sends it with the approval and wipes it; the server judges it. What these checks cannot
// see is the sheet itself (main/ui_card.c): that needs the device.

#define THE_SECRET "UDOUDDOU"
static const char PROFILE_GUARDED[] =
    "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
    "\"total_tasks_completed\":0,\"success_rate\":0.0,\"total_earned\":0,\"settled_total\":0},"
    "\"security\":{\"secret_set\":true,\"frozen\":false,\"frozen_reason\":null},\"settles_money\":false}";
static const char PROFILE_FROZEN[] =
    "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
    "\"total_tasks_completed\":0,\"success_rate\":0.0,\"total_earned\":0,\"settled_total\":0},"
    "\"security\":{\"secret_set\":true,\"frozen\":true,\"frozen_reason\":\"attempts\"},\"settles_money\":false}";
static const char PROFILE_FROZEN_BY_OWNER[] =
    "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
    "\"total_tasks_completed\":0,\"success_rate\":0.0,\"total_earned\":0,\"settled_total\":0},"
    "\"security\":{\"secret_set\":true,\"frozen\":true,\"frozen_reason\":\"owner\"},\"settles_money\":false}";
static const char PROFILE_NO_SECRET[] =
    "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
    "\"total_tasks_completed\":0,\"success_rate\":0.0,\"total_earned\":0,\"settled_total\":0},"
    "\"security\":{\"secret_set\":false,\"frozen\":false,\"frozen_reason\":null},\"settles_money\":false}";

// The server asks the card to pay and wants the holder's secret with the approval.
static void server_asks_for_the_secret(int id)
{
    static char request[240];
    snprintf(request, sizeof(request),
             "{\"request\":{\"id\":%d,\"payee\":\"Lobby Cafe\",\"memo\":\"latte\",\"match_code\":\"284\","
             "\"amount_cents\":550,\"expires_in\":30,\"auth\":\"secret\"}}", id);
    s_pay_reply = request;
    cloud_round(4000);
    s_pay_reply = "{\"request\":null}";
    CHECK(s_mode == MODE_PAY && s_pay.id == id && s_pay.need_secret);
    host_advance(900);   // keys are ignored just after a request appears
}

// What the tries of the next decision get, in turn (then 200).
static void server_will_answer(const int *statuses, int count)
{
    for (int i = 0; i < count; i++) s_decision_statuses[i] = statuses[i];
    s_decision_count = count;
    s_decision_asks = 0;
    memset(s_decision_bodies, 0, sizeof(s_decision_bodies));
}

// One press a key: 'U' up, 'D' down, 'O' OK.
static void type_keys(const char *keys)
{
    for (const char *k = keys; *k; k++) key(*k == 'U' ? CARD_KEY_UP : *k == 'D' ? CARD_KEY_DOWN : CARD_KEY_OK);
}

// OK is held on the request, the sheet opens, and these keys are pressed once it takes keys.
static void holder_types(const char *keys)
{
    key_long(CARD_KEY_OK);
    host_advance(SECRET_GUARD_MS);
    type_keys(keys);
}

// The pause after the last press is over: the secret goes out, and the server's answer comes back.
static void secret_goes_out(void)
{
    host_advance(SECRET_SEND_MS);
    host_task_round("cloud");
    pump();
}

static bool typed_is_wiped(void)
{
    for (size_t i = 0; i < sizeof(s_typed); i++) {
        if (s_typed[i]) return false;
    }
    return s_typed_len == 0;
}

// Nothing the card put out since step() (the screen, the USB link, the sounds) names these keys.
static bool nothing_names(const char *keys)
{
    for (int i = s_mark; i < host_log_count; i++) {
        if (strstr(host_log[i], keys)) return false;
    }
    return true;
}

static void attempt_of(const char *body, char *out, size_t size)
{
    const char *found = strstr(body, "\"attempt\":\"");
    out[0] = '\0';
    if (!found) return;
    found += strlen("\"attempt\":\"");
    size_t n = 0;
    while (found[n] && found[n] != '"' && n + 1 < size) n++;
    memcpy(out, found, n);
    out[n] = '\0';
}

static void check_the_holders_secret(void)
{
    char first[16], second[16];
    s_decision_reply = "{\"id\":1,\"status\":\"approved\"}";
    s_decision_refusal = "{\"error\":\"wrong_secret\",\"detail\":\"That is not this card's secret.\",\"attempts_left\":3}";

    // every request of a paired card says which run of the card it is: the id made up at power-on
    step();
    cloud_round(4000);
    CHECK(strcmp(host_last_boot, "0a1b2c3d") == 0);

    // the server wants the secret: holding OK opens the sheet, and nothing is sent yet
    step();
    server_asks_for_the_secret(80);
    server_will_answer(NULL, 0);
    key(CARD_KEY_OK);
    CHECK(said("overlay pay_holding"));
    key_long(CARD_KEY_OK);
    CHECK(s_mode == MODE_PAY_SECRET && said_start("overlay pay_secret #80 filled=0"));
    host_task_round("cloud");
    pump();
    CHECK(s_decision_asks == 0);
    key(CARD_KEY_OK);   // in the sheet's first moment: the OK that opened it, still on its way up
    CHECK(s_typed_len == 0);
    host_advance(SECRET_GUARD_MS);

    // eight presses fill eight squares; the USB link says a key was pressed, never which
    step();
    type_keys(THE_SECRET);
    CHECK(s_typed_len == 8 && times("overlay pay_secret_fill") == 8 && said("overlay pay_secret_fill 8"));
    CHECK(times("usb {\"t\":\"key\",\"k\":\"*\"}") == 8 && times("usb {\"t\":\"key\"") == 8);
    CHECK(s_mode == MODE_PAY_SECRET);   // not sent at once: a held key would first arrive as a press
    key(CARD_KEY_UP);                   // a ninth press is nothing
    CHECK(s_typed_len == 8 && times("overlay pay_secret_fill") == 8);
    host_advance(SECRET_SEND_MS);
    CHECK(s_mode == MODE_PAY_SENDING && said("overlay pay_sending #80") && typed_is_wiped());
    CHECK(cloud_holds_secret());    // handed to the client, which has not sent it yet
    host_task_round("cloud");
    pump();
    CHECK(!cloud_holds_secret());   // sent, and gone from there too
    // it went to the server with the approval, in the body of that one request ...
    CHECK(s_decision_asks == 1);
    CHECK(strstr(s_last_decision, "/api/cards/device/pay-requests/80/decision "
                                  "{\"approve\":true,\"reason\":\"approved\",\"secret\":\"" THE_SECRET "\",\"attempt\":\""));
    CHECK(said("overlay pay_done #80 paid=1") && said("usb {\"t\":\"cloud\",\"decision\":80,\"approve\":1,\"http\":200}"));
    // ... and nowhere else: not to the screen, not to the link, not into flash
    CHECK(nothing_names(THE_SECRET) && !host_store_holds(THE_SECRET));
    host_advance(3000);

    // a wrong one: the server says so and how many tries are left, and the holder types again
    step();
    server_asks_for_the_secret(81);
    server_will_answer((const int[]){ 400 }, 1);
    holder_types("OOOOUUUU");
    secret_goes_out();
    CHECK(s_mode == MODE_PAY_SECRET && strstr(last_of("overlay pay_secret #81"), "filled=0"));
    CHECK(strstr(last_of("overlay pay_secret #81"), "口令不对，还能试 3 次") && said("sfx LOSE"));
    CHECK(!said_start("overlay pay_failed") && !said_start("overlay pay_done") && typed_is_wiped());
    host_advance(SECRET_GUARD_MS);
    type_keys(THE_SECRET);
    secret_goes_out();
    CHECK(s_decision_asks == 2 && said("overlay pay_done #81 paid=1") && !cloud_holds_secret());
    attempt_of(s_decision_bodies[0], first, sizeof(first));
    attempt_of(s_decision_bodies[1], second, sizeof(second));
    CHECK(strlen(first) == 8 && strlen(second) == 8 && strcmp(first, second) != 0);   // two tries, two ids
    host_advance(3000);

    // the answer is lost on the way: the same try goes out again, word for word, so under the same id
    // (the server judges an id once: a try sent twice is not counted against the holder twice)
    step();
    server_asks_for_the_secret(82);
    server_will_answer((const int[]){ -1 }, 1);
    holder_types(THE_SECRET);
    secret_goes_out();
    CHECK(s_decision_asks == 2 && strcmp(s_decision_bodies[0], s_decision_bodies[1]) == 0);
    CHECK(strstr(s_decision_bodies[0], "\"attempt\":\"") && said("overlay pay_done #82 paid=1"));
    host_advance(3000);

    // holding the down key starts again, also when the eighth press was the hold's own ...
    step();
    server_asks_for_the_secret(83);
    server_will_answer(NULL, 0);
    holder_types("UUUUUUU");
    key(CARD_KEY_DOWN);        // the press a hold begins with: the eighth key
    CHECK(s_typed_len == 8);
    key_long(CARD_KEY_DOWN);   // ... and the hold itself, half a second later
    CHECK(s_mode == MODE_PAY_SECRET && typed_is_wiped() && strstr(last_of("overlay pay_secret #83"), "重新输入"));
    CHECK(said("usb {\"t\":\"key\",\"k\":\"*\",\"long\":1}") && !said_start("usb {\"t\":\"key\",\"k\":\"down"));
    host_advance(SECRET_SEND_MS + 100);
    host_task_round("cloud");
    pump();
    CHECK(s_mode == MODE_PAY_SECRET && s_decision_asks == 0);   // what was complete for a moment was not sent
    // ... and holding the up key refuses the payment
    type_keys("UD");
    key(CARD_KEY_UP);
    key_long(CARD_KEY_UP);
    CHECK(s_mode == MODE_PAY_DONE && said("overlay pay_done #83 paid=0") && typed_is_wiped());
    host_task_round("cloud");
    pump();
    CHECK(s_decision_asks == 1 && strstr(s_last_decision, "/pay-requests/83/decision {\"approve\":false,\"reason\":\"declined\"}"));
    host_advance(3000);

    // the request's time runs out on the sheet: refused as a timeout, and what was typed is gone
    step();
    server_asks_for_the_secret(84);
    server_will_answer(NULL, 0);
    holder_types("UDO");
    CHECK(s_typed_len == 3);
    host_advance(20000);
    CHECK(said("overlay pay_done #84 paid=0") && typed_is_wiped());
    host_task_round("cloud");
    pump();
    CHECK(strstr(s_last_decision, "/pay-requests/84/decision {\"approve\":false,\"reason\":\"timeout\"}"));
    host_advance(3000);

    // little time is left when the server says "wrong": no second try, the sheet says what happened
    step();
    server_asks_for_the_secret(85);
    server_will_answer((const int[]){ 400 }, 1);
    host_advance(16000);
    holder_types("OOOOUUUU");
    secret_goes_out();
    CHECK(s_mode == MODE_PAY_DONE && said("overlay pay_failed 口令不对") && times("overlay pay_secret #85") == 1);
    host_advance(3000);

    // five wrong ones froze the card: the sheet says so, and from then on the resting face does
    step();
    server_asks_for_the_secret(86);
    s_decision_refusal = "{\"error\":\"card_frozen\",\"detail\":\"Too many wrong secrets: this card is frozen.\",\"attempts_left\":0}";
    s_profile_reply = PROFILE_FROZEN;
    server_will_answer((const int[]){ 423 }, 1);
    const int profiles = s_profile_asks;
    holder_types("OOOOUUUU");
    secret_goes_out();
    CHECK(s_mode == MODE_PAY_DONE && said("overlay pay_failed 卡已冻结") && times("overlay pay_secret #86") == 1);
    CHECK(s_profile_asks == profiles + 1);   // asked at once who froze it, not at the next minute
    host_advance(GUARD_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && strcmp(last_of("status "), "status 口令输错，卡已冻结|0") == 0);
    step();
    key(CARD_KEY_OK);   // a key on the resting face says where that is put right
    CHECK(said("say 去网站 /card/pair 解冻") && said("usb {\"t\":\"key\",\"k\":\"ok\"}"));

    // frozen again, and this time the card's own view cannot be fetched: the face says so all the same,
    // from the answer to the approval alone
    s_profile_reply = PROFILE_GUARDED;
    cloud_round(60000);
    CHECK(strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0);
    step();
    server_asks_for_the_secret(90);
    s_profile_status = 503;
    server_will_answer((const int[]){ 423 }, 1);
    holder_types("OOOOUUUU");
    secret_goes_out();
    CHECK(said("overlay pay_failed 卡已冻结"));
    host_advance(GUARD_SHOW_MS);
    CHECK(s_mode == MODE_IDLE && strcmp(last_of("status "), "status 卡已冻结|0") == 0);
    // the view comes back and says what it said before the freeze (the owner has unfrozen the card):
    // it is handed to the screen again although it is what was handed over last, and the face follows
    s_profile_status = 200;
    cloud_round(16000);
    CHECK(strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0);

    // the owner froze it; no secret is set yet; and unfrozen again: the face follows the server
    s_profile_reply = PROFILE_FROZEN_BY_OWNER;
    cloud_round(60000);
    CHECK(strcmp(last_of("status "), "status 卡已冻结|0") == 0);
    s_profile_reply = PROFILE_NO_SECRET;
    cloud_round(60000);
    CHECK(strcmp(last_of("status "), "status 没设口令，还不能付款|0") == 0);
    step();
    key(CARD_KEY_UP);
    CHECK(said("say 去网站 /card/pair 设口令"));
    s_profile_reply = PROFILE_GUARDED;
    cloud_round(60000);
    CHECK(strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0);
    step();
    key(CARD_KEY_UP);
    CHECK(said("say 按住 OK 键打开设置"));
    host_advance(2000);

    // the server asked for nothing with the request, but wants the secret when the approval arrives
    // (its count of approvals without one ran out meanwhile): the card asks the holder then
    step();
    server_asks_to_pay(87);
    s_decision_refusal = "{\"error\":\"secret_required\",\"detail\":\"This payment needs the holder's secret.\"}";
    server_will_answer((const int[]){ 400 }, 1);
    key_long(CARD_KEY_OK);
    host_task_round("cloud");
    pump();
    CHECK(strcmp(s_decision_bodies[0], "{\"approve\":true,\"reason\":\"approved\"}") == 0);
    CHECK(s_mode == MODE_PAY_SECRET && strstr(last_of("overlay pay_secret #87"), "filled=0") && !said("sfx LOSE"));
    host_advance(SECRET_GUARD_MS);
    type_keys(THE_SECRET);
    secret_goes_out();
    CHECK(s_decision_asks == 2 && strstr(s_decision_bodies[1], "\"secret\":\"" THE_SECRET "\"") && said("overlay pay_done #87 paid=1"));
    host_advance(3000);

    // the owner has set no secret (the server says so against an approval): where to set one
    step();
    server_asks_to_pay(88);
    s_decision_refusal = "{\"error\":\"secret_not_set\",\"detail\":\"This card has no secret yet.\"}";
    s_profile_reply = PROFILE_NO_SECRET;   // and its view of the card says the same
    server_will_answer((const int[]){ 400 }, 1);
    key_long(CARD_KEY_OK);
    host_task_round("cloud");
    pump();
    CHECK(s_mode == MODE_PAY_DONE && said("overlay pay_failed 没设口令"));
    host_advance(GUARD_SHOW_MS);
    CHECK(strcmp(last_of("status "), "status 没设口令，还不能付款|0") == 0);
    s_profile_reply = PROFILE_GUARDED;
    cloud_round(60000);   // the owner set one: the next profile says so, and the face is as before
    CHECK(strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0);

    // a request played over USB that wants the secret: the computer is told that one was typed,
    // never which keys, and plays the server's verdict
    step();
    card_event_t event = { .type = CARD_EV_PAY };
    event.pay = (card_pay_t){ .id = 89, .amount_cents = 550, .balance_cents = -1, .ttl_ms = 20000,
                              .remote = true, .via_usb = true, .need_secret = true };
    card_utf8_copy(event.pay.payee, sizeof(event.pay.payee), "峰会咖啡");
    apply_now(&event);
    host_advance(900);
    holder_types(THE_SECRET);
    host_advance(SECRET_SEND_MS);
    CHECK(said("usb {\"t\":\"pay_ok\",\"id\":89,\"secret\":1}") && nothing_names(THE_SECRET) && typed_is_wiped());
    event = (card_event_t){ .type = CARD_EV_PAY_RESULT,
                            .pay_result = { .id = 89, .from_usb = true, .why = CARD_PAY_WRONG_SECRET, .left = 4 } };
    apply_now(&event);
    CHECK(s_mode == MODE_PAY_SECRET && strstr(last_of("overlay pay_secret #89"), "口令不对，还能试 4 次"));
    host_advance(SECRET_GUARD_MS);
    type_keys(THE_SECRET);
    host_advance(SECRET_SEND_MS);
    event = (card_event_t){ .type = CARD_EV_PAY_RESULT, .pay_result = { .id = 89, .ok = true, .from_usb = true } };
    apply_now(&event);
    CHECK(said("overlay pay_done #89 paid=1") && nothing_names(THE_SECRET));
    host_advance(3000);
    CHECK(s_mode == MODE_IDLE && !host_store_holds(THE_SECRET) && !host_store_holds("OOOOUUUU"));
}

// The card is paired with the site. Pointed at another server (a local backend, for a test) it
// starts unpaired there; its token for the site is neither sent there nor lost, and pointed back
// it is paired as before. (Until 0.9.0 changing the server erased the token: a test cost the
// card its pairing, and with it its public link.)
static void check_another_server_does_not_cost_the_pairing(void)
{
    step();
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(host_store_peek("token"), "chd_test-token") == 0);
    cloud_round(16000);
    const char *site_cursor = host_store_peek("tcur");
    char kept[96];
    snprintf(kept, sizeof(kept), "%s", site_cursor);
    CHECK(kept[0] != '\0');

    s_pair_start_status = 200;
    s_pair_start_reply = "{\"device_code\":\"dc-of-the-other-server-0123456789\",\"user_code\":\"BCDF-GHJK\","
                         "\"verification_uri\":\"http://192.168.0.10:8496/card/pair\",\"expires_in\":600,\"interval\":5}";
    s_pair_poll_status = 400;
    s_pair_poll_reply = "{\"error\":\"authorization_pending\"}";
    CHECK(cloud_set_base("http://192.168.0.10:8496") == ESP_OK);
    cloud_round(100);
    CHECK(strcmp(cloud_state_name(), "paired") != 0 && said("overlay pairing BCDF-GHJK"));
    CHECK(strcmp(host_store_peek("token"), "chd_test-token") == 0 && strcmp(host_store_peek("tcur"), kept) == 0);
    cloud_round(6000);
    cloud_round(61000);

    // the other server pairs the card: its token and what it tells the card are kept apart
    s_pair_poll_status = 200;
    s_pair_poll_reply = "{\"device_token\":\"chd_of-the-other-server\",\"public_id\":\"zzzzzzzzzz\",\"agent_id\":1}";
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b1.d1.p1.j0-none\",\"poll_s\":15}";
    cloud_round(31000);
    cloud_round(16000);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(host_store_peek("token2"), "chd_of-the-other-server") == 0);
    CHECK(strcmp(host_store_peek("tcur2"), "v1.a1.b1.d1.p1.j0-none") == 0);
    CHECK(strcmp(host_store_peek("token"), "chd_test-token") == 0 && strcmp(host_store_peek("tcur"), kept) == 0);
    CHECK(strcmp(s_last_auth, "Bearer chd_of-the-other-server") == 0 && s_site_token_elsewhere == 0);

    // back at the site: paired as before, with the site's token and cursor, without asking for a code
    step();
    const int pairings = s_pairing_asks;
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b9.d4.p8.j0-none\",\"poll_s\":15}";
    CHECK(cloud_set_base("") == ESP_OK);
    cloud_round(100);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(cloud_base(), "https://clawhunt.store") == 0);
    CHECK(strcmp(s_last_auth, "Bearer chd_test-token") == 0 && strstr(s_last_task_url, kept));
    CHECK(s_pairing_asks == pairings && !said_start("overlay pairing"));
    CHECK(host_store_peek("token2")[0] == '\0' && host_store_peek("tcur2")[0] == '\0' && s_site_token_elsewhere == 0);
}

// Two USB commands that arrive together are both handled before the cloud task's next round (the USB
// reader's task runs ahead of it, and it may be waiting for a server's answer). `unpair` then goes by
// the address flash names, not by the server the cloud task still speaks to: until 0.9.0 it erased the
// wrong server's token or cursor in all three orders below (the first one cost the pairing with the site).
static void check_usb_commands_that_arrive_together(void)
{
    step();
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(cloud_base(), "https://clawhunt.store") == 0);
    cloud_round(16000);
    char kept[96];
    snprintf(kept, sizeof(kept), "%s", host_store_peek("tcur"));
    CHECK(kept[0] != '\0' && strcmp(host_store_peek("token"), "chd_test-token") == 0);

    // at the site: `api <other>` and `unpair`, no round between. The unpair is the other server's.
    s_pair_poll_status = 400;
    s_pair_poll_reply = "{\"error\":\"authorization_pending\"}";
    CHECK(cloud_set_base("http://192.168.0.10:8496") == ESP_OK && cloud_unpair() == ESP_OK);
    CHECK(strcmp(host_store_peek("token"), "chd_test-token") == 0);
    cloud_round(100);
    CHECK(strcmp(cloud_state_name(), "paired") != 0 && strcmp(cloud_base(), "http://192.168.0.10:8496") == 0);
    CHECK(strcmp(host_store_peek("token"), "chd_test-token") == 0 && strcmp(host_store_peek("tcur"), kept) == 0);
    cloud_round(6000);
    cloud_round(61000);

    // the other server pairs the card; then `unpair` and `api ""`, no round between. The unpair is
    // still the other server's: the site's cursor stays, and the first task request back carries it.
    s_pair_poll_status = 200;
    s_pair_poll_reply = "{\"device_token\":\"chd_of-the-other-server\",\"public_id\":\"zzzzzzzzzz\",\"agent_id\":1}";
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b1.d1.p1.j0-none\",\"poll_s\":15}";
    cloud_round(31000);
    cloud_round(16000);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(host_store_peek("token2"), "chd_of-the-other-server") == 0);
    CHECK(strcmp(host_store_peek("tcur2"), "v1.a1.b1.d1.p1.j0-none") == 0);
    step();
    const int pairings = s_pairing_asks;
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b9.d4.p9.j0-none\",\"poll_s\":15}";
    CHECK(cloud_unpair() == ESP_OK && cloud_set_base("") == ESP_OK);
    CHECK(host_store_peek("token2")[0] == '\0' && strcmp(host_store_peek("token"), "chd_test-token") == 0);
    CHECK(strcmp(host_store_peek("tcur"), kept) == 0);
    cloud_round(100);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(cloud_base(), "https://clawhunt.store") == 0);
    CHECK(strcmp(s_last_auth, "Bearer chd_test-token") == 0 && strstr(s_last_task_url, kept));
    CHECK(s_pairing_asks == pairings && host_store_peek("tcur2")[0] == '\0' && s_site_token_elsewhere == 0);

    // at the site: `unpair` and `api <other>`, no round between. The unpair is the site's, as it was
    // typed: its token goes, and its cursor goes with it instead of staying for a pairing that is gone.
    CHECK(host_store_peek("tcur")[0] != '\0');
    s_pair_poll_status = 400;
    s_pair_poll_reply = "{\"error\":\"authorization_pending\"}";
    CHECK(cloud_unpair() == ESP_OK && cloud_set_base("http://192.168.0.10:8496") == ESP_OK);
    CHECK(host_store_peek("token")[0] == '\0');
    cloud_round(100);
    CHECK(host_store_peek("tcur")[0] == '\0' && host_store_peek("token2")[0] == '\0');
    CHECK(strcmp(cloud_state_name(), "paired") != 0 && s_site_token_elsewhere == 0);
}

// ---- a connection the server closed ---------------------------------------------------------------
//
// The card keeps one connection between its requests. A server closes it once it has been idle for
// a while (a local test server after five seconds), and the card only finds out when it next uses
// it: that request is lost. Until 0.9.1 the card then waited for its next turn to ask, so a card
// that asks about its pairing code every 5 to 20 s hardly ever got through to such a server (seen
// on the real card, 2026-11-16). Now the lost request goes out again at once, on a new connection.

// Paired with the site and at rest: every question has just been asked and answered, the
// connection is kept, and for the next 12 s the card only asks about pay requests, every 4 s.
static void at_rest_on_a_kept_connection(void)
{
    s_profile_reply = PROFILE;
    s_profile_status = s_pay_status = s_task_status = 200;
    s_pay_reply = "{\"request\":null}";
    pair_again();
}

static void check_a_connection_the_server_closed(void)
{
    step();
    CHECK(cloud_set_base("") == ESP_OK);
    at_rest_on_a_kept_connection();
    s_decision_reply = "{\"id\":1,\"status\":\"approved\"}";
    char boot[20];
    snprintf(boot, sizeof(boot), "%s", host_last_boot);
    CHECK(boot[0] != '\0');

    // the server closes it. The next thing the card asks is whether there is something to pay: the
    // question is lost, goes out again on a new connection, and its answer is on the screen in the
    // same round. The server saw it once, with the card's token and the id of its run. (Here the
    // write already failed, so the client still held the 200 of the answer before.)
    int lost = host_http_lost, connections = host_http_connections, pay = s_pay_asks;
    host_http_closed = HOST_HTTP_CLOSED_AT_WRITE;
    server_asks_to_pay(101);
    CHECK(host_http_lost == lost + 1 && host_http_connections == connections + 1 && s_pay_asks == pay + 1);
    CHECK(strcmp(s_pay_auth, "Bearer chd_test-token") == 0 && strcmp(host_last_boot, boot) == 0);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && !said_start("usb {\"t\":\"cloud\",\"token\""));

    // the holder approves, and the server has closed the connection again by then (the usual way:
    // the request is written and the read meets the end). The approval reaches the server once and
    // is accepted, with no pause on the card
    uint32_t paused = host_paused_ms;
    host_http_closed = HOST_HTTP_CLOSED_FIN;
    holder_answers(true, NULL, 0);
    CHECK(host_http_lost == lost + 2 && s_decision_asks == 1 && host_paused_ms == paused);
    CHECK(strstr(s_last_decision, "/api/cards/device/pay-requests/101/decision {\"approve\":true,\"reason\":\"approved\"}"));
    CHECK(said("overlay pay_done #101 paid=1") && said("usb {\"t\":\"cloud\",\"decision\":101,\"approve\":1,\"http\":200}"));
    host_advance(3000);

    // the same with the holder's secret in it (and a reset, as the real card met it): the server
    // gets the secret once, under its id, and afterwards neither the card nor its client holds it
    step();
    server_asks_for_the_secret(102);
    server_will_answer(NULL, 0);
    holder_types(THE_SECRET);
    host_http_closed = HOST_HTTP_CLOSED_RESET;
    secret_goes_out();
    CHECK(host_http_lost == lost + 3 && s_decision_asks == 1);
    CHECK(strstr(s_decision_bodies[0], "\"secret\":\"" THE_SECRET "\"") && strstr(s_decision_bodies[0], "\"attempt\":\""));
    CHECK(said("overlay pay_done #102 paid=1") && !cloud_holds_secret() && typed_is_wiped() && nothing_names(THE_SECRET));
    CHECK(!host_http_holds(THE_SECRET));
    host_advance(3000);

    // what this was found with: a card that asks about its pairing code every five seconds, and a
    // server that closes a connection sooner than that. The owner approves the code; the next
    // question about it goes into the closed connection, goes out again, and the card is paired
    // in that round instead of some rounds later
    step();
    fresh_code(CODE_1);
    const int polls = s_pair_polls;
    lost = host_http_lost;
    s_pair_poll_status = 200;
    s_pair_poll_reply = "{\"device_token\":\"chd_after-a-closed-connection\",\"public_id\":\"abcdefghjk\"}";
    host_http_closed = HOST_HTTP_CLOSED_RESET;
    cloud_round(5000);
    CHECK(host_http_lost == lost + 1 && s_pair_polls == polls + 1);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(host_store_peek("token"), "chd_after-a-closed-connection") == 0);
}

// The second try is for that one case. In every other the caller asks again in its own time, as before.
static void check_what_is_not_sent_again(void)
{
    // the server is not there at all. What fails on the kept connection gets its one more try, on a
    // new connection, and no third; what then cannot connect is not sent again
    step();
    at_rest_on_a_kept_connection();
    int pay = s_pay_asks, connections = host_http_connections;
    s_pay_status = -1;
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 2 && host_http_connections == connections + 1);
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 3 && host_http_connections == connections + 2);

    // a question that got no answer until the card gave up waiting
    at_rest_on_a_kept_connection();
    pay = s_pay_asks;
    s_pay_status = HOST_HTTP_TIMEOUT;
    uint32_t before = host_now_ms();
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1 && host_now_ms() - before == 4000 + 12000);

    // "at once" is within three seconds: a connection that is reset a little sooner than that gets
    // the second try, one that is reset a little later does not
    at_rest_on_a_kept_connection();
    pay = s_pay_asks;
    s_pay_status = HOST_HTTP_RESET_AFTER(2900);
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 2);
    at_rest_on_a_kept_connection();
    pay = s_pay_asks;
    s_pay_status = HOST_HTTP_RESET_AFTER(3100);
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1);

    // a server that takes a new connection and drops it without a word: the card had only just
    // connected, so this is not a connection that went stale
    at_rest_on_a_kept_connection();
    s_pay_status = -1;
    cloud_round(4000);   // on the kept connection, then failing to connect: the card is left without a client
    pay = s_pay_asks;
    s_pay_status = HOST_HTTP_RESET_AFTER(0);
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1);

    // an answer that ends the connection: the client closes it itself and keeps its place. If the
    // server is gone by the next question, that is a connection that could not be opened
    at_rest_on_a_kept_connection();
    host_http_ends_connection = true;
    cloud_round(4000);
    pay = s_pay_asks;
    connections = host_http_connections;
    s_pay_status = -1;
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1 && host_http_connections == connections + 1 && !host_http_ends_connection);

    // an answer that breaks off in its body is an answer: the server has heard the question
    at_rest_on_a_kept_connection();
    pay = s_pay_asks;
    host_http_cuts_body = true;
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1 && !host_http_cuts_body);

    // no memory for a client: there was no connection that could have gone stale, and the card does
    // not try to make one twice
    at_rest_on_a_kept_connection();
    s_pay_status = -1;
    cloud_round(4000);   // the card is left without a client
    s_pay_status = 200;
    pay = s_pay_asks;
    host_http_no_client = 1;
    cloud_round(4000);
    CHECK(s_pay_asks == pay && host_http_no_client == 0);
    // nor does it stumble when there is none for the second try: the question is lost this once,
    // and asked again in its turn
    at_rest_on_a_kept_connection();
    pay = s_pay_asks;
    int lost = host_http_lost;
    host_http_closed = HOST_HTTP_CLOSED_FIN;
    host_http_no_client = 1;
    cloud_round(4000);
    CHECK(s_pay_asks == pay && host_http_lost == lost + 1 && host_http_no_client == 0);
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1);

    // an approval that gets no answer until the card gives up waiting: its own second try follows
    // at once, without the pause that follows an answer which was not the server's
    step();
    at_rest_on_a_kept_connection();
    s_decision_reply = "{\"id\":1,\"status\":\"approved\"}";
    server_asks_to_pay(103);
    uint32_t paused = host_paused_ms;
    before = host_now_ms();
    holder_answers(true, (const int[]){ HOST_HTTP_TIMEOUT }, 1);
    CHECK(s_decision_asks == 2 && host_paused_ms == paused && host_now_ms() - before == 12000);
    CHECK(said("usb {\"t\":\"cloud\",\"decision\":103,\"approve\":1,\"http\":200}"));
    host_advance(3000);

    // a refusal is this request's only when a header came with it. The edge answers the first try
    // of an approval with its 403, and the server closes the connection behind that. The second
    // try's write stalls and fails, too late for another go: that is no answer, not the 403 once
    // more. So the third try follows without a pause, and is accepted
    step();
    server_asks_to_pay(104);
    paused = host_paused_ms;
    lost = host_http_lost;
    host_http_closing = HOST_HTTP_CLOSED_AT_WRITE;
    host_http_closed_slowly_ms = 3100;
    holder_answers(true, (const int[]){ 403 }, 1);
    CHECK(s_decision_asks == 2 && host_http_lost == lost + 1 && host_paused_ms - paused == 700);
    CHECK(said("overlay pay_done #104 paid=1"));
    host_advance(3000);

    // an answer is an answer, also the one that ends the pairing: asked once, on the kept connection
    step();
    at_rest_on_a_kept_connection();
    pay = s_pay_asks;
    lost = host_http_lost;
    connections = host_http_connections;
    s_pay_status = 401;
    cloud_round(4000);
    CHECK(s_pay_asks == pay + 1 && host_http_lost == lost && host_http_connections == connections);
    CHECK(strcmp(cloud_state_name(), "paired") != 0 && host_store_peek("token")[0] == '\0');
    s_pay_status = 200;
}

// ---- the week of the agent's earnings: kept from the total and the server's time -------------

static const char *profile_with(int total_cents)
{
    static char reply[400];
    snprintf(reply, sizeof(reply),
             "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
             "\"total_tasks_completed\":3,\"success_rate\":1.0,\"total_earned\":%d,\"settled_total\":2},"
             "\"security\":{\"secret_set\":true,\"frozen\":false}}",
             total_cents);
    return reply;
}

// The card's own view with the agent's total left out: what a server that is half deployed might answer.
static const char PROFILE_WITHOUT_TOTAL[] =
    "{\"card\":{\"public_id\":\"abcdefghjk\"},\"agent\":{\"id\":1,\"name\":\"lingo-crab\",\"level\":1,"
    "\"total_tasks_completed\":3,\"success_rate\":1.0,\"settled_total\":2},"
    "\"security\":{\"secret_set\":true,\"frozen\":false}}";

static void check_the_week_of_earnings(void)
{
    step();
    pair_again();
    host_advance(HOST_STATS_WIN_MS);   // no computer's numbers stand in the way of the card's own

    // the first answer that carries the server's time: a week begins today (a Wednesday at the card's offset),
    // with nothing earned yet and nothing known of the days before
    host_http_date = "Wed, 25 Nov 2026 02:00:00 GMT";
    s_profile_reply = profile_with(1000);
    int count = host_stats_count;
    cloud_round(61000);
    CHECK(host_stats_count > count && host_last_stats.has_week && host_last_stats.weekday == 2);
    CHECK(host_last_stats.week[6] == 0 && host_last_stats.week[5] == -1 && host_last_stats.week[0] == -1);
    // what goes to flash: the agent, the day, seven days' cents, and the highest total the card was told
    CHECK(strcmp(host_store_peek("week"), "1,1,20782,-1,-1,-1,-1,-1,-1,0,1000") == 0);
    // the face names what its numbers are
    CHECK(strcmp(host_last_stats.money_label, "TOTAL \xC2\xB7 EARNED") == 0 && host_last_stats.today_cents == 1000);
    CHECK(strcmp(host_last_stats.cells[0].label, "deals") == 0 && strcmp(host_last_stats.cells[0].value, "3") == 0);
    CHECK(strcmp(host_last_stats.cells[1].label, "settled") == 0 && strcmp(host_last_stats.cells[1].value, "2") == 0);
    CHECK(host_last_stats.cells[2].label[0] == '\0' && strcmp(host_last_stats.cells[2].value, "100%") == 0);

    // a minute later the total has grown: that is today's, and it is written to flash
    host_http_date = "Wed, 25 Nov 2026 02:01:01 GMT";
    s_profile_reply = profile_with(1500);
    int writes = host_store_writes("week");
    cloud_round(61000);
    CHECK(host_last_stats.week[6] == 500 && host_store_writes("week") == writes + 1);
    CHECK(strcmp(host_store_peek("week"), "1,1,20782,-1,-1,-1,-1,-1,-1,500,1500") == 0);
    // nothing new: nothing is written
    host_http_date = "Wed, 25 Nov 2026 02:02:02 GMT";
    cloud_round(61000);
    CHECK(host_last_stats.week[6] == 500 && host_store_writes("week") == writes + 1);
    // an answer without the server's time: the total is shown, the week stays as it is
    host_http_date = NULL;
    s_profile_reply = profile_with(1700);
    cloud_round(61000);
    CHECK(host_last_stats.today_cents == 1700 && host_last_stats.has_week && host_last_stats.week[6] == 500);
    CHECK(host_store_writes("week") == writes + 1);
    // the next answer with a time finds the growth since the last one with a time, two minutes before
    host_http_date = "Wed, 25 Nov 2026 02:04:05 GMT";
    cloud_round(61000);
    CHECK(host_last_stats.week[6] == 700);

    // midnight at the card's offset is 16:00 GMT: a new day, and yesterday keeps its figure
    host_http_date = "Wed, 25 Nov 2026 16:00:30 GMT";
    cloud_round(61000);
    CHECK(host_last_stats.weekday == 3 && host_last_stats.week[5] == 700 && host_last_stats.week[6] == 0);
    CHECK(strcmp(host_store_peek("week"), "1,1,20783,-1,-1,-1,-1,-1,700,0,1700") == 0);

    // the settings are read anew, as after a restart: the week is taken up from flash, and the first answer only
    // says where the total stands. What was earned while the card was not asking is in the total and in no day
    s_profile_reply = profile_with(1900);
    pair_again();
    cloud_round(1000);
    CHECK(host_last_stats.today_cents == 1900 && host_last_stats.has_week && host_last_stats.week[6] == 0);
    CHECK(host_last_stats.week[5] == 700);
    host_http_date = "Wed, 25 Nov 2026 16:01:31 GMT";
    s_profile_reply = profile_with(2000);
    cloud_round(61000);
    CHECK(host_last_stats.week[6] == 100);

    // an answer that leaves the total out is not a total of zero: the week is left as it is, nothing is written,
    // and the total coming back a minute later has earned nothing
    writes = host_store_writes("week");
    host_http_date = "Wed, 25 Nov 2026 16:02:32 GMT";
    s_profile_reply = PROFILE_WITHOUT_TOTAL;
    cloud_round(61000);
    CHECK(host_last_stats.today_cents == 0 && host_last_stats.has_week && host_last_stats.week[6] == 100);
    CHECK(host_store_writes("week") == writes);
    host_http_date = "Wed, 25 Nov 2026 16:03:33 GMT";
    s_profile_reply = profile_with(2000);
    cloud_round(61000);
    CHECK(host_last_stats.today_cents == 2000 && host_last_stats.week[6] == 100 && host_store_writes("week") == writes);
    // nor has it after an answer that said zero outright: only what rises above the highest total counts
    host_http_date = "Wed, 25 Nov 2026 16:04:34 GMT";
    s_profile_reply = profile_with(0);
    cloud_round(61000);
    CHECK(host_last_stats.week[6] == 100 && host_store_writes("week") == writes);
    host_http_date = "Wed, 25 Nov 2026 16:05:35 GMT";
    s_profile_reply = profile_with(2050);
    cloud_round(61000);
    CHECK(host_last_stats.week[6] == 150 && host_store_writes("week") == writes + 1);
    CHECK(strcmp(host_store_peek("week"), "1,1,20783,-1,-1,-1,-1,-1,700,150,2050") == 0);

    // another server's agents are not the site's: while the card is pointed elsewhere the site's week stays in
    // flash as it is. What the other server tells the card is kept apart, and goes when the card leaves it
    char kept[WEEK_TEXT_MAX];
    snprintf(kept, sizeof(kept), "%s", host_store_peek("week"));
    CHECK(kept[0] != '\0');
    s_pair_start_status = 200;
    s_pair_start_reply = "{\"device_code\":\"dc-of-the-other-server-0123456789\",\"user_code\":\"BCDF-GHJK\","
                         "\"verification_uri\":\"http://192.168.0.10:8496/card/pair\",\"expires_in\":600,\"interval\":5}";
    s_pair_poll_status = 400;
    s_pair_poll_reply = "{\"error\":\"authorization_pending\"}";
    host_http_date = "Thu, 26 Nov 2026 03:00:00 GMT";
    s_profile_reply = profile_with(50);
    CHECK(cloud_set_base("http://192.168.0.10:8496") == ESP_OK);
    cloud_round(100);
    CHECK(strcmp(host_store_peek("week"), kept) == 0 && host_store_peek("week2")[0] == '\0');
    cloud_round(6000);
    cloud_round(61000);
    s_pair_poll_status = 200;
    s_pair_poll_reply = "{\"device_token\":\"chd_of-the-other-server\",\"public_id\":\"zzzzzzzzzz\",\"agent_id\":1}";
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b1.d1.p1.j0-none\",\"poll_s\":15}";
    cloud_round(31000);
    cloud_round(16000);
    CHECK(strcmp(cloud_state_name(), "paired") == 0 && strcmp(host_store_peek("token2"), "chd_of-the-other-server") == 0);
    cloud_round(61000);
    CHECK(strcmp(host_store_peek("week2"), "1,1,20783,-1,-1,-1,-1,-1,-1,0,50") == 0);
    host_http_date = "Thu, 26 Nov 2026 03:01:01 GMT";
    s_profile_reply = profile_with(80);
    cloud_round(61000);
    CHECK(strcmp(host_store_peek("week2"), "1,1,20783,-1,-1,-1,-1,-1,-1,30,80") == 0);
    CHECK(strcmp(host_store_peek("week"), kept) == 0);

    // back at the site: its week as it was, and the other server's gone with its token
    host_http_date = NULL;
    s_task_reply = "{\"job\":null,\"event\":null,\"cursor\":\"v1.a1.b9.d4.p8.j0-none\",\"poll_s\":15}";
    CHECK(cloud_set_base("") == ESP_OK);
    cloud_round(100);
    CHECK(strcmp(host_store_peek("week"), kept) == 0 && strcmp(cloud_state_name(), "paired") == 0);
    CHECK(host_store_peek("week2")[0] == '\0' && host_store_peek("token2")[0] == '\0');
    s_profile_reply = PROFILE;
    s_pair_start_status = s_pair_poll_status = 404;
    s_pair_start_reply = s_pair_poll_reply = "{\"detail\":\"Not found\"}";
}

static int run_checks(void)
{
    CHECK(card_app_start() == ESP_OK);
    CHECK(s_mode == MODE_IDLE && strcmp(last_of("status "), "status 摸鱼中，等单…|0") == 0);
    check_resting_line();
    check_a_job_does_not_block_the_card();
    check_the_new_task_sheet();
    check_the_computers_offer_is_as_before();
    check_sheets_wait_their_turn();
    check_a_task_that_ends_without_a_payout();
    check_the_client();
    check_a_pairing_code_at_the_edge();
    check_a_pay_decision_at_the_edge();
    check_the_holders_secret();
    check_another_server_does_not_cost_the_pairing();
    check_usb_commands_that_arrive_together();
    check_a_connection_the_server_closed();
    check_what_is_not_sent_again();
    check_the_week_of_earnings();
    printf(s_failures ? "card tasks: %d FAILED\n" : "card tasks: PASS\n", s_failures);
    return s_failures ? 1 : 0;
}

// ---- a real server's answers, replayed ------------------------------------------------------

static FILE *s_answers;
static char s_line[8192];
static char s_given_cursor[128];   // the cursor of the answer served last
static int s_served, s_mismatches, s_cursor_changes;

static void cursor_of(const char *json, char *out, size_t size)
{
    out[0] = '\0';
    const char *found = strstr(json, "\"cursor\":\"");
    if (!found) return;
    found += strlen("\"cursor\":\"");
    const char *end = strchr(found, '"');
    if (end) snprintf(out, size, "%.*s", (int)(end - found), found);
}

static int recorded(const char *method, const char *url, const char *auth, const char *body, char *reply, size_t size)
{
    if (!strstr(url, "/api/cards/device/tasks/next")) return scripted(method, url, auth, body, reply, size);
    const char *asked = strstr(url, "?cursor=");
    asked = asked ? asked + strlen("?cursor=") : "";
    if (strcmp(asked, s_given_cursor) != 0) {
        printf("FAIL request %d asked with cursor '%s', the answer before gave '%s'\n", s_served + 1, asked, s_given_cursor);
        s_mismatches++;
    }
    if (fgets(s_line, sizeof(s_line), s_answers)) {
        s_served++;
        char cursor[128];
        cursor_of(s_line, cursor, sizeof(cursor));
        if (strcmp(cursor, s_given_cursor) != 0) s_cursor_changes++;
        snprintf(s_given_cursor, sizeof(s_given_cursor), "%s", cursor);
    }
    // past the end of the record the last answer stands: nothing new
    snprintf(reply, size, "%s", s_line);
    return 200;
}

// What the holder saw, in the words the backend's local walk uses for the same answers: a line
// when the words of the status line change, a sheet when it comes up, a bubble for a lost task.
// Read frame by frame: a line set under a sheet, or set and covered in the same frame, was not seen.
static void print_screen(void)
{
    static const struct {
        const char *text;
        const char *phase;
    } LINES[] = {
        { "竞标中…", "bid" },       { "中标了，开工", "won" },   { "收到，这就开工", "own" },   { "干活中…", "work" },
        { "交付了，等验收", "done" }, { "验收了，等放款", "held" }, { "有争议，等裁定", "disputed" }, { "摸鱼中，等单…", "rest" },
    };
    static char shown[HOST_LOG_MAX][96];
    int count = 0;
    const char *seen = "rest";    // the words the holder last saw on the line (a card starts at rest)
    const char *under = "rest";   // the words on the line now, covered or not
    bool covered = false;         // a sheet is over the face
    char sheet[96] = "", bubble[96] = "";
    for (int i = 0; i <= host_log_count; i++) {
        const char *note = i < host_log_count ? host_log[i] : HOST_FRAME;
        int id = 0, a = 0, b = 0, c = 0;
        if (strcmp(note, HOST_FRAME) == 0) {
            if (bubble[0] && !covered) {
                snprintf(shown[count++], sizeof(shown[0]), "%s", bubble);
                seen = under;   // the bubble is over a line that went to rest in the same frame
            }
            if (sheet[0] && covered) snprintf(shown[count++], sizeof(shown[0]), "%s", sheet);
            if (!covered && strcmp(under, seen) != 0) {
                seen = under;
                snprintf(shown[count++], sizeof(shown[0]), "line:%s", seen);
            }
            sheet[0] = bubble[0] = '\0';
        } else if (strncmp(note, "status ", 7) == 0) {
            for (size_t n = 0; n < sizeof(LINES) / sizeof(LINES[0]); n++) {
                if (strncmp(note + 7, LINES[n].text, strlen(LINES[n].text)) == 0) under = LINES[n].phase;
            }
        } else if (strcmp(note, "say 这单没中") == 0 || strcmp(note, "say 这单结束了，没有到账") == 0) {
            snprintf(bubble, sizeof(bubble), "line:lost:%s", strstr(note, "没中") ? "not_selected" : "called_off");
        } else if (strcmp(note, "overlay close") == 0) {
            covered = false;
        } else if (strncmp(note, "overlay ", 8) == 0) {
            covered = true;
            if (sscanf(note, "overlay offer #%d remote=%d direct=%d amt=%d", &id, &a, &b, &c) == 4 && a == 1) {
                snprintf(sheet, sizeof(sheet), "sheet:new_task:%d:%d:%s", id, c, b ? "direct" : "bid");
            } else if (sscanf(note, "overlay paid #%d net=%d lvl=%d", &id, &a, &b) == 3) {
                snprintf(sheet, sizeof(sheet), "sheet:paid:%d:%d:lv%d", id, a, b);
            }
        }
    }
    for (int i = 0; i < count; i++) printf("screen %s\n", shown[i]);
}

static int run_replay(const char *path)
{
    s_answers = fopen(path, "r");
    if (!s_answers) {
        printf("cannot open %s\n", path);
        return 2;
    }
    host_store_put("token", "chd_test-token");
    host_http_set(recorded);
    if (card_app_start() != ESP_OK || cloud_start() != ESP_OK) return 2;
    host_log_clear();
    // The card asks every 15 s; a sheet stays up to 20 s. Two rounds past the end: nothing more comes.
    int rounds = 0;
    while (!feof(s_answers) || rounds < 2) {
        if (feof(s_answers)) rounds++;
        cloud_round(15500);
        if (s_served > 5000) break;
    }
    host_advance(30000);
    print_screen();
    const bool kept = strcmp(host_store_peek("tcur"), s_given_cursor) == 0;
    const bool thrifty = host_store_writes("tcur") == s_cursor_changes;
    printf("replayed %d answers; cursor kept in flash: %s; written %d times for %d changes: %s; requests with a wrong cursor: %d\n",
           s_served, kept ? "yes" : "NO", host_store_writes("tcur"), s_cursor_changes, thrifty ? "ok" : "NO", s_mismatches);
    return kept && thrifty && s_mismatches == 0 ? 0 : 1;
}

#endif   // CARD_APP_OLD

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "checks";
    if (strcmp(mode, "usb") == 0) {
        if (card_app_start() != ESP_OK) return 2;
        run_usb_script();
        for (int i = 0; i < host_log_count; i++) printf("%s\n", host_log[i]);
        return 0;
    }
#ifndef CARD_APP_OLD
    if (strcmp(mode, "replay") == 0 && argc > 2) return run_replay(argv[2]);
    if (strcmp(mode, "checks") == 0) return run_checks();
#endif
    printf("usage: test_card_tasks checks | usb | replay <answers.jsonl>\n");
    return 2;
}
