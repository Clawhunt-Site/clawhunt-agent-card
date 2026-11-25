// main/week.h -- what the agent earned on each of the last seven days, as this card saw it.
//
// The server tells the card one number, the agent's lifetime earnings, about once a minute. The card has no clock,
// but every answer of the server carries the time (the HTTP Date header). From the two the card keeps a week:
// when the total rises between two answers that are close together, the rise is today's.
//
// What the card did not see it does not make up, and where it cannot tell it leaves out rather than adds:
//   - a day on which the card had no answer has no figure, and what was earned while the card was off, out of reach
//     or restarting is in the total but in no day;
//   - only what rises above the highest total the card was ever told counts. A total that fell and came back (a
//     correction on the server's side, one answer with a wrong figure) has earned nothing, and after a correction
//     downwards the days stay empty until the total has passed its earlier height;
//   - nothing is measured across an answer that was dated oddly (before the one before it, or before the kept day).
// A day begins at midnight of CARD_UTC_OFFSET_MIN (card_model.h).
#pragma once

#include "card_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEEK_TEXT_MAX 128   // what week_text() needs, with room

// Seconds since 1970 from an HTTP Date header ("Wed, 25 Nov 2026 00:23:40 GMT", years 2024 to 2099). False when
// value is not one; *epoch_s is then left alone.
bool week_parse_http_date(const char *value, int64_t *epoch_s);

// Take up what week_text() gave before a restart; anything else (NULL, "", text of another kind) starts empty.
void week_restore(const char *text);

// An answer with the agent's lifetime total, and the server's time when it gave it. The caller leaves out an answer
// that does not say the total: no figure is not a total of zero.
// True when what the card keeps changed: the caller stores week_text() again.
bool week_sample(int agent_id, int total_cents, int64_t epoch_s);

// What is kept, as text for the card's store: the agent, the day, seven days' cents and the highest total.
void week_text(char *dst, size_t size);

// The week of this agent for the face: cents earned per day, today last, < 0 where there is no figure; and today's
// day of the week, 0 = Monday. False when the card keeps no week for this agent.
bool week_for(int agent_id, int days[CARD_WEEK_DAYS], int *weekday);

void week_forget(void);
