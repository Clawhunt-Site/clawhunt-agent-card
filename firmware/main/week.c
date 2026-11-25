// main/week.c -- see week.h. Plain C with no platform calls, so the host tests run it as it is.
// Nothing here reads text with scanf: what comes from a server or from the card's store is taken apart by hand, the
// same on a computer and on the card, and with next to no stack.
#include "week.h"

#include <stdio.h>
#include <string.h>

#define DAY_S        86400
#define GAP_MAX_S    900          // two answers further apart than this: the card was away, the growth is nobody's day
#define DAY_MAX      47482        // 2100-01-01: week_parse_http_date() takes no later year
#define FIGURE_MAX   999999999    // cents in one day, as kept

static struct {
    bool kept;                   // there is a week
    int agent_id;
    int32_t day;                 // the day of the last slot: days since 1970 at the card's offset
    int earned[CARD_WEEK_DAYS];  // cents; < 0: no figure
    int high;                    // the highest total the card was told for this agent; only what rises above it is earned
    // Only of this run of the card: after a restart the first answer earns nothing, it only says where the total is.
    bool have_last;              // the answer before this one was taken, and last_epoch_s is when it was given
    int64_t last_epoch_s;
    bool earlier;                // the answer before this one was dated before the day the card keeps
    int64_t earlier_epoch_s;
} s_week;

static int32_t day_of(int64_t epoch_s)
{
    const int64_t local = epoch_s + (int64_t)CARD_UTC_OFFSET_MIN * 60;
    return (int32_t)(local >= 0 && local / DAY_S <= DAY_MAX + 1 ? local / DAY_S : -1);
}

// Days from 1970-01-01 to a civil date (proleptic Gregorian; H. Hinnant's days_from_civil).
static int64_t days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int yoe = (int)(year - era * 400);
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int days_in(int year, int month)
{
    static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return month == 2 && leap ? 29 : DAYS[month - 1];
}

// The two digits at s as a number; -1 when they are not two digits.
static int two(const char *s)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return -1;
    return (s[0] - '0') * 10 + (s[1] - '0');
}

bool week_parse_http_date(const char *value, int64_t *epoch_s)
{
    // IMF-fixdate, the one form a server sends today (RFC 9110 5.6.7): "Wed, 25 Nov 2026 00:23:40 GMT". It is 29
    // characters with every field at a fixed place. The name of the weekday is not read.
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (!value) return false;
    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) length--;
    if (length != 29) return false;
    if (value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || memcmp(value + 25, " GMT", 4) != 0) {
        return false;
    }
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (memcmp(value + 8, MONTHS + i * 3, 3) == 0) month = i + 1;
    }
    const int day = two(value + 5), century = two(value + 12), years = two(value + 14);
    const int hour = two(value + 17), minute = two(value + 20), second = two(value + 23);
    if (!month || century < 0 || years < 0) return false;
    const int year = century * 100 + years;
    if (year < 2024 || year > 2099 || day < 1 || day > days_in(year, month)) return false;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return false;   // 60: a leap second
    *epoch_s = days_from_civil(year, month, day) * DAY_S + hour * 3600 + minute * 60 + second;
    return true;
}

void week_forget(void)
{
    memset(&s_week, 0, sizeof(s_week));
}

// The number at *at, which then moves past it: digits, with one minus sign before them at most, and no more than an
// int holds. False when that is not what stands there.
static bool number_at(const char **at, int64_t *value)
{
    const char *s = *at;
    const bool minus = *s == '-';
    if (minus) s++;
    if (*s < '0' || *s > '9') return false;
    int64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if (v > INT32_MAX) return false;
    }
    *value = minus ? -v : v;
    *at = s;
    return true;
}

void week_restore(const char *text)
{
    week_forget();
    if (!text) return;
    // "1,<agent>,<day>,<cents of seven days>,<the highest total>": what week_text() writes, and nothing else
    int64_t n[3 + CARD_WEEK_DAYS + 1];
    const size_t count = sizeof(n) / sizeof(n[0]);
    const char *at = text;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            if (*at != ',') return;
            at++;
        }
        if (!number_at(&at, &n[i])) return;
    }
    if (*at) return;   // something follows: not what week_text() writes
    if (n[0] != 1 || n[1] <= 0 || n[2] <= 0 || n[2] > DAY_MAX || n[count - 1] < 0) return;
    s_week.kept = true;
    s_week.agent_id = (int)n[1];
    s_week.day = (int32_t)n[2];
    for (int i = 0; i < CARD_WEEK_DAYS; i++) {
        const int64_t cents = n[3 + i];
        s_week.earned[i] = cents < 0 ? -1 : cents > FIGURE_MAX ? FIGURE_MAX : (int)cents;
    }
    s_week.high = (int)n[count - 1];
}

void week_text(char *dst, size_t size)
{
    if (!s_week.kept) {
        if (size) dst[0] = '\0';
        return;
    }
    int used = snprintf(dst, size, "1,%d,%ld", s_week.agent_id, (long)s_week.day);
    for (int i = 0; i < CARD_WEEK_DAYS && used > 0 && (size_t)used < size; i++) {
        used += snprintf(dst + used, size - (size_t)used, ",%d", s_week.earned[i]);
    }
    if (used > 0 && (size_t)used < size) snprintf(dst + used, size - (size_t)used, ",%d", s_week.high);
}

// The week ends on `day` from here on. Days move towards the front when the day is later and towards the back when
// it is earlier (a kept day that never came); a place nothing moves into has no figure.
static void end_on(int32_t day)
{
    const int64_t steps = (int64_t)day - s_week.day;
    int moved[CARD_WEEK_DAYS];
    for (int i = 0; i < CARD_WEEK_DAYS; i++) {
        const int64_t from = i + steps;
        moved[i] = from >= 0 && from < CARD_WEEK_DAYS ? s_week.earned[from] : -1;
    }
    memcpy(s_week.earned, moved, sizeof(moved));
    s_week.day = day;
}

bool week_sample(int agent_id, int total_cents, int64_t epoch_s)
{
    const int32_t day = day_of(epoch_s);
    if (agent_id <= 0 || total_cents < 0 || day <= 0 || day > DAY_MAX) return false;

    bool changed = false;
    if (!s_week.kept || s_week.agent_id != agent_id) {
        // no week yet, or another agent's: one begins today, with no figure for the days before
        week_forget();
        s_week.kept = true;
        s_week.agent_id = agent_id;
        s_week.day = day;
        for (int i = 0; i < CARD_WEEK_DAYS; i++) s_week.earned[i] = -1;
        changed = true;
    } else if (day < s_week.day) {
        // Dated before the day the card keeps. One such answer is a stale one and says nothing. Two in a row, the
        // second given after the first and close to it, say that the kept day never came (a server whose clock ran
        // ahead once must not stop the week for good): the week is moved back to end on the day they give.
        const bool again = s_week.earlier && epoch_s > s_week.earlier_epoch_s && epoch_s - s_week.earlier_epoch_s <= GAP_MAX_S;
        s_week.earlier = true;
        s_week.earlier_epoch_s = epoch_s;
        s_week.have_last = false;   // whatever is taken next only says where the total stands
        if (!again) return false;
        s_week.earlier = false;
        end_on(day);
        changed = true;
    } else {
        s_week.earlier = false;
        // given before the answer before it (a clock put back within the day): nothing is measured across that
        if (s_week.have_last && epoch_s < s_week.last_epoch_s) s_week.have_last = false;
        if (day > s_week.day) {
            end_on(day);
            changed = true;
        }
    }

    if (s_week.earned[CARD_WEEK_DAYS - 1] < 0) {   // today had no figure yet: from now on the card is watching it
        s_week.earned[CARD_WEEK_DAYS - 1] = 0;
        changed = true;
    }
    // Earned is what the total rose by above the highest the card was told. A total that fell (a correction on the
    // server's side, or one answer that left the figure out) and came back has earned nothing.
    if (total_cents > s_week.high) {
        if (s_week.have_last && epoch_s - s_week.last_epoch_s <= GAP_MAX_S) {
            const int64_t sum = (int64_t)s_week.earned[CARD_WEEK_DAYS - 1] + (total_cents - s_week.high);
            s_week.earned[CARD_WEEK_DAYS - 1] = sum > FIGURE_MAX ? FIGURE_MAX : (int)sum;
        }
        s_week.high = total_cents;
        changed = true;
    }
    s_week.have_last = true;
    s_week.last_epoch_s = epoch_s;
    return changed;
}

bool week_for(int agent_id, int days[CARD_WEEK_DAYS], int *weekday)
{
    if (!s_week.kept || s_week.agent_id != agent_id) return false;
    memcpy(days, s_week.earned, sizeof(s_week.earned));
    *weekday = (int)((s_week.day + 3) % 7);   // day 0 was a Thursday
    return true;
}
