// tests/test_card_week.c -- the week of the agent's earnings (main/week.c), on a computer.
// Built and run by tools/card_host_tests.sh.
#include "week.h"

#include <stdio.h>
#include <string.h>

static int s_failures;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++; \
        } \
    } while (0)

#define AGENT 7
#define T0    1795572000LL   // Wed, 25 Nov 2026 02:00:00 GMT: ten in the morning at the card's offset
#define DAY   86400

static int s_days[CARD_WEEK_DAYS];
static int s_weekday;

static bool week_is(int d0, int d1, int d2, int d3, int d4, int d5, int d6, int weekday)
{
    const int want[CARD_WEEK_DAYS] = { d0, d1, d2, d3, d4, d5, d6 };
    if (!week_for(AGENT, s_days, &s_weekday)) return false;
    return memcmp(want, s_days, sizeof(want)) == 0 && s_weekday == weekday;
}

static void check_the_date(void)
{
    int64_t at = 0;
    CHECK(week_parse_http_date("Mon, 01 Jan 2024 00:00:00 GMT", &at) && at == 1704067200LL);
    CHECK(week_parse_http_date("Wed, 25 Nov 2026 02:00:00 GMT", &at) && at == T0);
    CHECK(week_parse_http_date("Tue, 29 Feb 2028 23:59:59 GMT", &at) && at == 1835481599LL);
    CHECK(week_parse_http_date("Thu, 31 Dec 2099 23:59:59 GMT", &at) && at == 4102444799LL);
    // a leap second is the first second of the next day
    CHECK(week_parse_http_date("Thu, 31 Dec 2026 23:59:60 GMT", &at) && at == 1798761600LL);
    // white space after it is not part of it
    CHECK(week_parse_http_date("Wed, 25 Nov 2026 02:00:00 GMT \t", &at) && at == T0);

    // not a date of the one form a server sends
    static const char *const BAD[] = {
        "",
        "25 Nov 2026 02:00:00 GMT",             // no weekday
        "Wed, 25 Nov 2026 02:00:00 +0800",      // another zone
        "Wed, 25 Nov 2026 02:00:00 UTC",
        "Wed, 25 Nox 2026 02:00:00 GMT",        // no such month
        "Wed, 25 nov 2026 02:00:00 GMT",
        "Wed, 25 Nov 1999 02:00:00 GMT",        // years the card does not take
        "Fri, 01 Jan 2100 00:00:00 GMT",
        "Wed, 25 Nov 2026 24:00:00 GMT",        // no such time
        "Wed, 25 Nov 2026 02:60:00 GMT",
        "Wed, 25 Nov 2026 02:00:61 GMT",
        "Wed, 00 Nov 2026 02:00:00 GMT",        // a day the month does not have
        "Tue, 31 Nov 2026 02:00:00 GMT",
        "Sun, 29 Feb 2026 02:00:00 GMT",
        "Wed,  5 Nov 2026 02:00:00 GMT",        // not two digits
        "Wed, 5 Nov 2026 02:00:00 GMT",
        "Wed, 25 Nov 2026 2:00:00 GMT",
        "Wed, 25 Nov 2026 02-00-00 GMT",
        "Wed, 25 Nov 2026 02:00:00 GMTx",       // something after it
        "Wed, 25 Nov 2026 02:00:00 GMT, Thu",
        "Wednesday, 25-Nov-26 02:00:00 GMT",    // the two older forms
        "Wed Nov 25 02:00:00 2026",
    };
    at = 5;
    CHECK(!week_parse_http_date(NULL, &at));
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        if (week_parse_http_date(BAD[i], &at)) {
            printf("FAILED: taken as a date: \"%s\"\n", BAD[i]);
            s_failures++;
        }
    }
    CHECK(at == 5);   // left alone
}

static void check_a_day(void)
{
    week_forget();
    CHECK(!week_for(AGENT, s_days, &s_weekday));
    // the first answer: a week begins today, with nothing in it, and nothing known of the days before
    CHECK(week_sample(AGENT, 100000, T0));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 0, 2));
    // a minute later the total has grown: that is today's
    CHECK(week_sample(AGENT, 100500, T0 + 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 500, 2));
    // nothing new: nothing to store
    CHECK(!week_sample(AGENT, 100500, T0 + 120));
    // the card was away for longer than a quarter of an hour: what the total grew by is nobody's day (the total's
    // new height is kept, so there is something to store)
    CHECK(week_sample(AGENT, 100800, T0 + 120 + 16 * 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 500, 2));
    // and from there on it counts again
    CHECK(week_sample(AGENT, 101000, T0 + 120 + 17 * 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 700, 2));
    // a quarter of an hour to the second still counts; one second more does not
    CHECK(week_sample(AGENT, 101010, T0 + 120 + 17 * 60 + 900));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 710, 2));
    CHECK(week_sample(AGENT, 101020, T0 + 120 + 17 * 60 + 900 + 901));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 710, 2));
    // another agent's numbers are not this agent's week
    int other[CARD_WEEK_DAYS], weekday;
    CHECK(!week_for(AGENT + 1, other, &weekday));
}

// The card adds to a day only what it saw the total rise by, above the highest it was ever told.
static void check_a_total_that_falls_and_comes_back(void)
{
    char text[WEEK_TEXT_MAX];
    week_forget();
    CHECK(week_sample(AGENT, 192332, T0));
    CHECK(week_sample(AGENT, 192400, T0 + 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 68, 2));
    // one answer with a wrong figure (a total of nothing) and the true total again a minute later: all the agent
    // ever earned must not turn up as today's
    CHECK(!week_sample(AGENT, 0, T0 + 120));
    CHECK(!week_sample(AGENT, 192400, T0 + 180));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 68, 2));
    CHECK(week_sample(AGENT, 192500, T0 + 240));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 168, 2));
    // a correction downwards on the server's side is not a negative day, and what is earned after it shows only
    // once the total has passed its earlier height
    CHECK(!week_sample(AGENT, 190000, T0 + 300));
    CHECK(!week_sample(AGENT, 191000, T0 + 360));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 168, 2));
    CHECK(week_sample(AGENT, 192600, T0 + 420));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 268, 2));
    // the height is kept with the week: a restart, a wrong figure in the first answer, the true total after it
    week_text(text, sizeof(text));
    CHECK(strcmp(text, "1,7,20782,-1,-1,-1,-1,-1,-1,268,192600") == 0);
    week_forget();
    week_restore(text);
    CHECK(!week_sample(AGENT, 0, T0 + 480));
    CHECK(!week_sample(AGENT, 192600, T0 + 540));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 268, 2));
    CHECK(week_sample(AGENT, 192700, T0 + 600));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 368, 2));
    // more in one day than the figure holds stops at the most it holds
    week_forget();
    CHECK(week_sample(AGENT, 0, T0));
    CHECK(week_sample(AGENT, 2000000000, T0 + 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 999999999, 2));
}

static void check_midnight_and_days_without_the_card(void)
{
    // half a minute before midnight at the card's offset, and half a minute after
    const int64_t late = T0 + 14 * 3600 - 30;   // 15:59:30 GMT
    week_forget();
    CHECK(week_sample(AGENT, 5000, late - 60));
    CHECK(week_sample(AGENT, 5700, late));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 700, 2));
    CHECK(week_sample(AGENT, 5750, late + 60));
    CHECK(week_is(-1, -1, -1, -1, -1, 700, 50, 3));   // Thursday: what came in over midnight is the new day's

    // three days without the card: they have no figure, and what was earned meanwhile is in no day
    CHECK(week_sample(AGENT, 9000, late + 60 + 3 * DAY));
    CHECK(week_is(-1, -1, 700, 50, -1, -1, 0, 6));
    CHECK(week_sample(AGENT, 9100, late + 120 + 3 * DAY));
    CHECK(week_is(-1, -1, 700, 50, -1, -1, 100, 6));
    // more than a week: nothing of the old week is left
    CHECK(week_sample(AGENT, 9100, late + 120 + 12 * DAY));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 0, 1));

    // up to the end of 2099 the week goes on (a Date header of a later year is not taken)
    int64_t at = 0;
    week_forget();
    CHECK(week_parse_http_date("Thu, 31 Dec 2099 15:00:00 GMT", &at));   // eleven at night at the card's offset
    CHECK(week_sample(AGENT, 100, at) && week_sample(AGENT, 300, at + 60));
    CHECK(week_for(AGENT, s_days, &s_weekday) && s_days[CARD_WEEK_DAYS - 1] == 200);
    CHECK(week_sample(AGENT, 400, at + 3660));                           // the first of January 2100 there
    CHECK(week_for(AGENT, s_days, &s_weekday) && s_days[CARD_WEEK_DAYS - 2] == 200 && s_days[CARD_WEEK_DAYS - 1] == 0);
}

static void check_what_is_kept_over_a_restart(void)
{
    char text[WEEK_TEXT_MAX], again[WEEK_TEXT_MAX];
    week_forget();
    week_text(text, sizeof(text));
    CHECK(text[0] == '\0');
    CHECK(week_sample(AGENT, 100, T0));
    CHECK(week_sample(AGENT, 400, T0 + 60));
    week_text(text, sizeof(text));
    CHECK(strcmp(text, "1,7,20782,-1,-1,-1,-1,-1,-1,300,400") == 0);

    // a restart: the week is there again, and the first answer only says where the total stands (its new height
    // is something to store; no day gets it)
    week_forget();
    week_restore(text);
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 300, 2));
    CHECK(week_sample(AGENT, 900, T0 + 120));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 300, 2));
    CHECK(week_sample(AGENT, 950, T0 + 180));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 350, 2));
    week_text(again, sizeof(again));
    CHECK(strcmp(again, "1,7,20782,-1,-1,-1,-1,-1,-1,350,950") == 0);
    // what is stored reads back as the same text
    week_forget();
    week_restore(again);
    week_text(text, sizeof(text));
    CHECK(strcmp(text, again) == 0);

    // a restart on a later day
    CHECK(week_sample(AGENT, 2000, T0 + 2 * DAY));
    CHECK(week_is(-1, -1, -1, -1, 350, -1, 0, 4));

    // what is not a week as week_text writes it starts empty
    static const char *const BAD[] = {
        "", "x", "1", "1,7", "1,7,20782", "1,7,207",
        "2,7,20782,-1,-1,-1,-1,-1,-1,300,400",     // another version
        "1,7,20782,-1,-1,-1,-1,-1,-1,300",         // without the total's height
        "1,7,20782,-1,-1,-1",                      // cut off
        "1,0,20782,1,2,3,4,5,6,7,8",               // no agent
        "1,-7,20782,1,2,3,4,5,6,7,8",
        "1,7,20782,1,2,3,4,5,6,7,8,9",             // one figure too many
        "1,7,20782,1,2,3,4,5,6,7,8 ",              // something after it
        "1,7,20782,1,2,3,4,5,6,7,8,",
        " 1,7,20782,1,2,3,4,5,6,7,8",              // or before it
        "1,+7,20782,1,2,3,4,5,6,7,8",
        "1,7,-3,1,2,3,4,5,6,7,8",                  // not a day
        "1,7,0,1,2,3,4,5,6,7,8",
        "1,7,47483,1,2,3,4,5,6,7,8",
        "1,7,20782,,2,3,4,5,6,7,8",                // not numbers
        "1,7,20782,--1,2,3,4,5,6,7,8",
        "1,7,20782,1,2,3,4,5,6,7,x",
        "1,7,20782,1,2,3,4,5,6,99999999999,8",     // more than an int holds
        "1,7,20782,1,2,3,4,5,6,7,-8",              // a height below nothing
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        week_restore(BAD[i]);
        if (week_for(AGENT, s_days, &s_weekday)) {
            printf("FAILED: taken as a week: \"%s\"\n", BAD[i]);
            s_failures++;
        }
    }
    week_restore(NULL);
    CHECK(!week_for(AGENT, s_days, &s_weekday));
    // figures the card would not write itself are taken as what they must mean
    week_restore("1,7,20782,-5,0,2,3,4,5,2000000000,2147483647");
    CHECK(week_is(-1, 0, 2, 3, 4, 5, 999999999, 2));
    // the longest text there is fits, and is written back whole
    static const char LONGEST[] = "1,99999999,47482,999999999,999999999,999999999,999999999,999999999,999999999,999999999,2147483647";
    week_restore(LONGEST);
    week_text(text, sizeof(text));
    CHECK(strcmp(text, LONGEST) == 0 && strlen(LONGEST) < WEEK_TEXT_MAX - 1);
    // a buffer that is too small is not written past
    char tiny[8] = "???????";
    week_restore(again);
    week_text(tiny, sizeof(tiny));
    CHECK(tiny[7] == '\0' && strlen(tiny) < sizeof(tiny));
}

static void check_another_agent_and_what_is_no_answer(void)
{
    week_forget();
    CHECK(week_sample(AGENT, 100, T0));
    CHECK(week_sample(AGENT, 300, T0 + 60));
    // the card is paired with another agent: its week begins, the old one is gone, and so is the old total's height
    CHECK(week_sample(AGENT + 1, 50, T0 + 120));
    CHECK(!week_for(AGENT, s_days, &s_weekday));
    int days[CARD_WEEK_DAYS], weekday;
    CHECK(week_for(AGENT + 1, days, &weekday) && days[CARD_WEEK_DAYS - 1] == 0 && days[0] == -1);
    CHECK(week_sample(AGENT + 1, 80, T0 + 180));
    CHECK(week_for(AGENT + 1, days, &weekday) && days[CARD_WEEK_DAYS - 1] == 30);
    // nothing to keep a week of: no agent, no total, no time, a time past what the card takes
    CHECK(!week_sample(0, 100, T0) && !week_sample(AGENT + 1, -1, T0 + 240) && !week_sample(AGENT + 1, 100, 0));
    CHECK(!week_sample(AGENT + 1, 100, -5) && !week_sample(AGENT + 1, 100, 4102444800LL + DAY));
    CHECK(!week_sample(AGENT + 1, 100, INT64_MAX / 2));
    CHECK(week_for(AGENT + 1, days, &weekday) && days[CARD_WEEK_DAYS - 1] == 30 && weekday == 2);   // and none of it touched the week
    CHECK(week_sample(AGENT + 1, 90, T0 + 240));
    CHECK(week_for(AGENT + 1, days, &weekday) && days[CARD_WEEK_DAYS - 1] == 40);
}

// A server's clock that is wrong once, and answers that come out of order: the week loses little and adds nothing.
static void check_answers_dated_oddly(void)
{
    // one answer dated later in the day than it was given, and the true time again after it: nothing is measured
    // across either of them
    week_forget();
    CHECK(week_sample(AGENT, 1000, T0));
    CHECK(week_sample(AGENT, 1100, T0 + 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 100, 2));
    CHECK(week_sample(AGENT, 1150, T0 + 10 * 3600));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 100, 2));
    CHECK(week_sample(AGENT, 1200, T0 + 120));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 100, 2));
    CHECK(week_sample(AGENT, 1300, T0 + 180));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 200, 2));

    // one answer dated a day ahead moves the week on. The first true answer after it says nothing; the second moves
    // the week back, with what it held
    CHECK(week_sample(AGENT, 1300, T0 + DAY + 240));
    CHECK(week_is(-1, -1, -1, -1, -1, 200, 0, 3));
    CHECK(!week_sample(AGENT, 1350, T0 + 300));
    CHECK(week_is(-1, -1, -1, -1, -1, 200, 0, 3));
    CHECK(week_sample(AGENT, 1400, T0 + 360));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 200, 2));
    CHECK(week_sample(AGENT, 1500, T0 + 420));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 300, 2));

    // a single stale answer from days back changes nothing, and nothing is measured across it
    CHECK(!week_sample(AGENT, 1500, T0 - 3 * DAY));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 300, 2));
    CHECK(week_sample(AGENT, 1600, T0 + 480));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 300, 2));
    CHECK(week_sample(AGENT, 1700, T0 + 540));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 400, 2));
    // nor do two stale answers that were not given close together, or of which the second was given first
    CHECK(!week_sample(AGENT, 1700, T0 - 3 * DAY));
    CHECK(!week_sample(AGENT, 1700, T0 - 3 * DAY + 3600));
    CHECK(!week_sample(AGENT, 1700, T0 - 2 * DAY));
    CHECK(!week_sample(AGENT, 1700, T0 - 2 * DAY - 60));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 400, 2));

    // a week kept from days that are still to come (a clock that ran far ahead) does not stay for good: two true
    // answers in a row and it begins again from their day, with the total's height as it was
    CHECK(week_sample(AGENT, 1700, T0 + 30 * DAY));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 0, 4));
    CHECK(!week_sample(AGENT, 1750, T0 + 600));
    CHECK(week_sample(AGENT, 1800, T0 + 660));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 0, 2));
    CHECK(week_sample(AGENT, 1900, T0 + 720));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 100, 2));
    // the same over a restart: what was stored names a day to come
    char text[WEEK_TEXT_MAX];
    CHECK(week_sample(AGENT, 1900, T0 + 30 * DAY + 60));
    week_text(text, sizeof(text));
    week_forget();
    week_restore(text);
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 0, 4));
    CHECK(!week_sample(AGENT, 1900, T0 + 780));
    CHECK(week_sample(AGENT, 1900, T0 + 840));
    CHECK(week_is(-1, -1, -1, -1, -1, -1, 0, 2));
}

int main(void)
{
    check_the_date();
    check_a_day();
    check_a_total_that_falls_and_comes_back();
    check_midnight_and_days_without_the_card();
    check_what_is_kept_over_a_restart();
    check_another_agent_and_what_is_no_answer();
    check_answers_dated_oddly();
    printf(s_failures ? "week: %d FAILED\n" : "week: PASS\n", s_failures);
    return s_failures ? 1 : 0;
}
