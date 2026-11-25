// main/ui_card.c -- the card face and its full-screen sheets.
//
// One dark ground, one accent. Mint is for the screen's one number, for today and for what is live; amber says
// the card cannot pay yet, red that it is frozen. Only two sheets leave the dark ground: a payment the server
// took and a payout, the two moments something went through, are set in the ground's colour on a whole screen of
// mint.
// Every screen is set from the left edge. A sheet names itself in a tag at the top, and its one number stands
// where the face's number stands, so the eye finds the amount in the same place on every screen.
// Latin and digits are Cascadia Mono, smoothed; Chinese is the 12 px pixel font, at 12 and 24 px. The Latin
// fonts fall back to the pixel font, so a Chinese name in a Latin line is still drawn.
// The face: header (the mark, the name of the site, what the agent is doing), the number with what it counts,
// the exact figure, three facts, the week's bars with today on the right, and at the foot the level and the
// agent's name.
// Layout is in absolute pixels for the 240x320 panel; text is placed by its baseline. The BSP masks the final
// frame to a 30 px corner radius, so nothing stands in the corners.
#include "ui_card.h"

#include "card_identity.h"
#include "crab_sprite.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_px12);
LV_FONT_DECLARE(font_px24);
LV_FONT_DECLARE(font_mono11);    // labels, set in capitals
LV_FONT_DECLARE(font_mono12);
LV_FONT_DECLARE(font_mono12b);
LV_FONT_DECLARE(font_mono14);
LV_FONT_DECLARE(font_big72);     // the number, and a pairing code
LV_FONT_DECLARE(font_big60);
LV_FONT_DECLARE(font_big48);
LV_FONT_DECLARE(font_big32);     // what follows the number in small: its cents, its K or M
LV_FONT_DECLARE(font_big26);
LV_FONT_DECLARE(font_big21);

// tools/gen_crab.py bakes C_MINT and C_BG into the crab: change them together.
#define C_BG     0x171E1C   // the ground
#define C_PANEL  0x202926   // a row that stands out from the ground
#define C_LINE   0x2F3B37   // hairlines, empty slots, a neutral tag
#define C_MINT   0x92DAAC   // the number, today, live
#define C_LEAF   0x2F6E4A   // the days before today
#define C_TEXT   0xE3ECE7
#define C_SOFT   0x94A39C   // secondary type
#define C_MUTE   0x66756E   // labels
#define C_AMBER  0xF0B95A   // the card cannot pay yet; something did not go through
#define C_RED    0xEF7D6C   // the card is frozen
#define C_DEEP   0x1F4A33   // secondary type on mint
#define C_WHITE  0xFFFFFF   // the plate under a QR code

#define SCREEN_W        240
#define SCREEN_H        320
#define MARGIN          14    // left and right edge of everything
#define FULL_W          212   // SCREEN_W - 2 * MARGIN
// the face, by baseline
#define HEAD_BASE       19
#define LABEL_BASE      50
#define NUMBER_BASE     116
#define EXACT_BASE      138
#define FACTS_BASE      156
#define BARS_TOP        172
#define BARS_BASE       246
#define BAR_W           24
#define BAR_STEP        31    // seven bars: 6 * 31 + 24 = 210
#define DAYS_BASE       262
#define FOOT_LINE_Y     274
#define FOOT_BASE       297
#define TOAST_Y         280
#define TOAST_H         24
// a sheet
#define TAG_Y           12
#define NOTE_BASE       34    // beside the tag, at the right: a number that belongs to the sheet
#define SHEET_LABEL     64
#define SHEET_NUMBER    122
#define LAST_BASE       290   // the sheet's last line: what the keys do
#define COUNTDOWN_Y     262

#define ANIM_TICK_MS    160
#define BUBBLE_MS       1400
#define DOT_COUNT       14
#define SLOT_SIZE       20    // one square per press of the holder's secret
#define SLOT_STEP       27    // eight of them across the sheet: 7 * 27 + 20 = 209

static const char *const DEFAULT_LABELS[CARD_CELL_COUNT] = { "deals", "rating", "streak" };
static const char *const DEFAULT_MONEY_LABEL = "TODAY \xC2\xB7 EARNED";
static const char *const PAY_HINT = "按住 OK 付款，上下键不付";

// The number's sizes: the digits, and what follows them in small.
typedef struct {
    const lv_font_t *digits;
    const lv_font_t *small;
} number_size_t;
static const number_size_t NUMBER_SIZES[] = { { &font_big72, &font_big32 }, { &font_big60, &font_big26 }, { &font_big48, &font_big21 } };
#define NUMBER_SIZE_COUNT ((int)(sizeof(NUMBER_SIZES) / sizeof(NUMBER_SIZES[0])))

// What the status line means for the header: the word that stands there, and how loud it is.
typedef enum {
    FACE_PLAIN = 0,   // the agent's state
    FACE_HINT,        // not a state: a line that tells the holder what to do, kept at the foot
    FACE_WARN,        // the card cannot pay
    FACE_STOP,        // the card is frozen
} face_kind_t;
static const struct {
    const char *line;
    const char *word;
    face_kind_t kind;
} LINES[] = {
    { CARD_LINE_IDLE, "IDLE", FACE_PLAIN },          { CARD_LINE_BID, "BIDDING", FACE_PLAIN },
    { CARD_LINE_WON, "WORKING", FACE_PLAIN },        { CARD_LINE_OWN, "WORKING", FACE_PLAIN },
    { CARD_LINE_WORK, "WORKING", FACE_PLAIN },       { CARD_LINE_DONE, "DELIVERED", FACE_PLAIN },
    { CARD_LINE_HELD, "ON HOLD", FACE_PLAIN },       { CARD_LINE_DISPUTED, "DISPUTE", FACE_PLAIN },
    { CARD_LINE_FROZEN_TRIES, "LOCKED", FACE_STOP }, { CARD_LINE_FROZEN, "LOCKED", FACE_STOP },
    { CARD_LINE_NO_SECRET, "NO SECRET", FACE_WARN }, { CARD_LINE_NO_WIFI, "IDLE", FACE_HINT },
};

static lv_obj_t *s_screen;
static lv_obj_t *s_crab;
static lv_obj_t *s_state;            // the header's word
static lv_obj_t *s_state_dot;
static lv_obj_t *s_money_label;
static lv_obj_t *s_number;           // holds the number's glyph runs; rebuilt when the number changes
static lv_obj_t *s_exact;
static lv_obj_t *s_facts;            // holds the three facts; rebuilt with the stats
static lv_obj_t *s_guard_mark;       // the line that takes the facts' place when the card cannot pay
static lv_obj_t *s_guard_text;
static lv_obj_t *s_bars[CARD_WEEK_DAYS];
static lv_obj_t *s_days[CARD_WEEK_DAYS];
static lv_obj_t *s_level;
static lv_obj_t *s_xp;
static lv_obj_t *s_name;
static lv_obj_t *s_bubble;           // the foot's one line: what the crab says, or a hint that stays
static lv_obj_t *s_bubble_text;
static int s_agent_id = CARD_AGENT_ID;   // the agent the face shows
static lv_obj_t *s_overlay;
static uint32_t s_ink = C_TEXT;      // type on the sheet that is up
static uint32_t s_soft = C_SOFT;
static lv_obj_t *s_pay_hint;         // the line of a payment request that says how to answer it
static lv_obj_t *s_slots[CARD_SECRET_LEN];   // the squares of the secret sheet, while it is up
static lv_obj_t *s_poster_crab;      // the big crab of the level-up poster, while it is up
static lv_timer_t *s_bubble_timer;
static lv_timer_t *s_hint_timer;
static bool s_busy;
static uint32_t s_tick;
static char s_line[48];              // the status line, as the state machine said it
static int s_progress;
static int s_net;                    // 0 off, 1 joining, 2 online, 3 failed
static bool s_link;                  // a computer is feeding the card
static char s_hint[48];              // the line that stays at the foot, "" when there is none

// ---- small builders -------------------------------------------------------

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// Something to build into and throw away as one: as large as the screen, and drawn as nothing.
static lv_obj_t *holder(lv_obj_t *parent)
{
    lv_obj_t *obj = box(parent, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, s);
    return label;
}

// How far a font's baseline is under the top of its line.
static int ascent(const lv_font_t *font)
{
    return font->line_height - font->base_line;
}

// Text at x, standing on a baseline.
static lv_obj_t *put(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int baseline, const char *s)
{
    lv_obj_t *label = text(parent, font, color, s);
    lv_obj_set_pos(label, x, baseline - ascent(font));
    return label;
}

// Text that ends at the right edge, standing on a baseline. It stays there when its words change.
static lv_obj_t *put_right(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int baseline, const char *s)
{
    lv_obj_t *label = text(parent, font, color, s);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -MARGIN, baseline - ascent(font));
    return label;
}

// A label in capitals: what a number counts, the header's words.
static lv_obj_t *caps(lv_obj_t *parent, uint32_t color, int x, int baseline, const char *s)
{
    lv_obj_t *label = put(parent, &font_mono11, color, x, baseline, s);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    return label;
}

static int width_of(lv_obj_t *label)
{
    lv_obj_update_layout(label);
    return lv_obj_get_width(label);
}

static lv_obj_t *bar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t track, uint32_t fill)
{
    lv_obj_t *obj = lv_bar_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(fill), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(obj, 0, 100);
    return obj;
}

// One line of text cut to a width, ending in an ellipsis if it is longer.
static void one_line(lv_obj_t *label, int w)
{
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
}

// A number set glyph by glyph on a baseline. A point or a comma gets half of the font's cell: in a monospaced
// font a full cell leaves a hole in the number. Returns the x after it.
static int run(lv_obj_t *parent, int x, int baseline, const char *s, const lv_font_t *font, uint32_t color)
{
    char part[24];
    while (*s) {
        size_t n = *s == '.' || *s == ',' ? 1 : strcspn(s, ".,");
        if (n >= sizeof(part)) n = sizeof(part) - 1;
        memcpy(part, s, n);
        part[n] = '\0';
        lv_obj_t *label = text(parent, font, color, part);
        const int w = width_of(label);
        if (*s == '.' || *s == ',') {
            const int cell = w / 2;
            lv_obj_set_pos(label, x - (w - cell) / 2, baseline - ascent(font));
            x += cell;
        } else {
            lv_obj_set_pos(label, x, baseline - ascent(font));
            x += w;
        }
        s += n;
    }
    return x;
}

// A screen's one number: its digits, and what follows them in small, on one baseline at the left edge, in the
// largest size in which both fit. Returns what holds it (delete that to take the number away).
static lv_obj_t *number(lv_obj_t *parent, int baseline, const char *digits, const char *small, uint32_t color, uint32_t small_color)
{
    for (int i = 0;; i++) {
        lv_obj_t *held = holder(parent);
        int x = run(held, MARGIN - 3, baseline, digits, NUMBER_SIZES[i].digits, color);   // the digits' own left bearing
        if (small[0]) x = run(held, x + 2, baseline, small, NUMBER_SIZES[i].small, small_color);
        if (x <= SCREEN_W - MARGIN || i == NUMBER_SIZE_COUNT - 1) return held;
        lv_obj_delete(held);
    }
}

// An amount as the number sets it: dollars large, cents small.
static void split_money(int cents, char *digits, size_t digits_size, char *small, size_t small_size)
{
    snprintf(digits, digits_size, "%d", cents / 100);
    snprintf(small, small_size, ".%02d", cents % 100);
}

// The face's number: thousands and millions are cut to three figures with a K or an M (cut, not rounded up: the
// exact figure stands under it).
static void split_total(int cents, char *digits, size_t digits_size, char *small, size_t small_size)
{
    const int dollars = cents / 100;
    const int unit = dollars >= 1000000 ? 1000000 : dollars >= 1000 ? 1000 : 0;
    if (!unit) {
        split_money(cents, digits, digits_size, small, small_size);
        return;
    }
    const int hundredths = (int)((int64_t)dollars * 100 / unit);
    const int whole = hundredths / 100;
    if (whole >= 100) {
        snprintf(digits, digits_size, "%d", whole);
    } else if (whole >= 10) {
        snprintf(digits, digits_size, "%d.%d", whole, hundredths % 100 / 10);
    } else {
        snprintf(digits, digits_size, "%d.%02d", whole, hundredths % 100);
    }
    snprintf(small, small_size, "%s", unit == 1000 ? "K" : "M");
}

// "$1,923.32"
static void exact_money(int cents, char *out, size_t size)
{
    char plain[16];
    snprintf(plain, sizeof(plain), "%d", cents / 100);
    const int len = (int)strlen(plain);
    size_t used = 0;
    if (used + 1 < size) out[used++] = '$';
    for (int i = 0; i < len && used + 2 < size; i++) {
        out[used++] = plain[i];
        if ((len - 1 - i) % 3 == 0 && i != len - 1) out[used++] = ',';
    }
    out[used] = '\0';
    snprintf(out + used, size - used, ".%02d", cents % 100);
}

// ---- timers ---------------------------------------------------------------

static void anim_tick(lv_timer_t *timer)
{
    (void)timer;
    s_tick++;
    const uint32_t before = s_busy ? ((s_tick - 1) & 1) : (((s_tick - 1) >> 2) & 1);
    const uint32_t now = s_busy ? (s_tick & 1) : ((s_tick >> 2) & 1);
    if (now != before) lv_image_set_src(s_crab, now ? &crab_mark1 : &crab_mark0);
    // The poster crab waves its claws, twice as fast as the resting crab.
    if (s_poster_crab && (((s_tick - 1) >> 1) & 1) != ((s_tick >> 1) & 1)) {
        lv_image_set_src(s_poster_crab, ((s_tick >> 1) & 1) ? &crab_big1 : &crab_big0);
    }
}

// The foot's line goes back to the hint that stays, or goes away.
static void bubble_rest(void)
{
    if (s_hint[0]) {
        lv_label_set_text(s_bubble_text, s_hint);
        lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

static void bubble_hide(lv_timer_t *timer)
{
    (void)timer;
    s_bubble_timer = NULL;   // one-shot timers delete themselves after firing
    bubble_rest();
}

// ---- card face ------------------------------------------------------------

// The agent is at work: the server's line, or a computer's job, which says how far it is ("干活中 40%").
static bool is_work(const char *line)
{
    return strncmp(line, CARD_LINE_WORK_PART, strlen(CARD_LINE_WORK_PART)) == 0;
}

static const char *word_of(const char *line, face_kind_t *kind)
{
    *kind = FACE_PLAIN;
    for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
        if (strcmp(line, LINES[i].line) != 0) continue;
        *kind = LINES[i].kind;
        return LINES[i].word;
    }
    if (is_work(line)) return "WORKING";
    return s_busy ? "ACTIVE" : "IDLE";   // a line this file does not know
}

// The header's word and its dot, and the line that takes the facts' place when the card cannot pay.
// What the agent is doing is only known while something tells the card: the network, or a computer.
static void header_refresh(void)
{
    face_kind_t kind;
    const char *word = word_of(s_line, &kind);
    const bool told = s_net == 2 || s_link;
    char shown[24];
    uint32_t color = C_MINT;
    if (!told) {
        snprintf(shown, sizeof(shown), "%s", s_net == 1 ? "CONNECTING" : "OFFLINE");
        color = C_MUTE;
    } else if (kind == FACE_PLAIN && s_progress > 0 && s_progress < 100 && strcmp(word, "WORKING") == 0) {
        snprintf(shown, sizeof(shown), "WORKING %d%%", s_progress);
    } else {
        snprintf(shown, sizeof(shown), "%s", word);
        if (kind == FACE_WARN) color = C_AMBER;
        if (kind == FACE_STOP) color = C_RED;
    }
    lv_label_set_text(s_state, shown);
    lv_obj_set_style_text_color(s_state, lv_color_hex(color), 0);
    lv_obj_set_pos(s_state_dot, SCREEN_W - MARGIN - width_of(s_state) - 11, HEAD_BASE - 8);
    lv_obj_set_style_bg_opa(s_state_dot, told ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_state_dot, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_state_dot, lv_color_hex(color), 0);

    const bool guarded = kind == FACE_WARN || kind == FACE_STOP;
    if (guarded) {
        const uint32_t loud = kind == FACE_STOP ? C_RED : C_AMBER;
        lv_label_set_text(s_guard_text, s_line);
        lv_obj_set_style_text_color(s_guard_text, lv_color_hex(loud), 0);
        lv_obj_set_style_bg_color(s_guard_mark, lv_color_hex(loud), 0);
    }
    if (guarded) {
        lv_obj_remove_flag(s_guard_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_guard_text, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_facts, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_guard_mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_guard_text, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_facts, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_header(void)
{
    s_crab = lv_image_create(s_screen);
    lv_image_set_src(s_crab, &crab_mark0);
    lv_obj_set_pos(s_crab, MARGIN, HEAD_BASE - 10);
    lv_obj_t *site = put(s_screen, &font_mono12b, C_TEXT, MARGIN + 22, HEAD_BASE, "CLAWHUNT");
    lv_obj_set_style_text_letter_space(site, 1, 0);

    s_state = put_right(s_screen, &font_mono11, C_MUTE, HEAD_BASE, "OFFLINE");
    lv_obj_set_style_text_letter_space(s_state, 1, 0);
    s_state_dot = box(s_screen, 0, HEAD_BASE - 8, 7, 7, C_MUTE);
    lv_obj_set_style_radius(s_state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_state_dot, 1, 0);
}

// The week: a bar per day, today on the right, and under each the day's letter.
static void build_week(void)
{
    for (int i = 0; i < CARD_WEEK_DAYS; i++) {
        s_bars[i] = box(s_screen, MARGIN + i * BAR_STEP, BARS_BASE - 1, BAR_W, 2, C_LINE);
        s_days[i] = text(s_screen, &font_mono11, C_MUTE, "");
        lv_obj_set_width(s_days[i], BAR_W);
        lv_obj_set_style_text_align(s_days[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_days[i], MARGIN + i * BAR_STEP, DAYS_BASE - ascent(&font_mono11));
    }
}

static void build_foot(void)
{
    box(s_screen, MARGIN, FOOT_LINE_Y, FULL_W, 1, C_LINE);
    lv_obj_t *lv = put(s_screen, &font_mono12, C_MUTE, MARGIN, FOOT_BASE, "LV");
    lv_obj_set_style_text_letter_space(lv, 1, 0);
    s_level = put(s_screen, &font_mono14, C_TEXT, MARGIN + 22, FOOT_BASE, "-");
    s_xp = bar(s_screen, MARGIN + 44, FOOT_BASE - 6, 40, 4, C_LINE, C_MINT);
    s_name = text(s_screen, &font_mono12b, C_TEXT, "...");
    one_line(s_name, 118);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_name, LV_ALIGN_TOP_RIGHT, -MARGIN, FOOT_BASE - ascent(&font_mono12b));

    // The foot's one line, over the level and the name: what the crab says, or a hint that stays.
    s_bubble = box(s_screen, MARGIN, TOAST_Y, FULL_W, TOAST_H, C_PANEL);
    box(s_bubble, 0, 0, 3, TOAST_H, C_MINT);
    s_bubble_text = put(s_bubble, &font_px12, C_TEXT, 10, 17, "");
    one_line(s_bubble_text, FULL_W - 16);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

void ui_card_init(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    build_header();
    s_money_label = caps(s_screen, C_MUTE, MARGIN, LABEL_BASE, "");
    lv_obj_set_style_text_letter_space(put_right(s_screen, &font_mono11, C_MUTE, LABEL_BASE, "USD"), 1, 0);
    s_number = number(s_screen, NUMBER_BASE, "--", "", C_MUTE, C_TEXT);
    s_exact = put(s_screen, &font_mono14, C_TEXT, MARGIN, EXACT_BASE, "");
    s_facts = holder(s_screen);
    s_guard_mark = box(s_screen, MARGIN, FACTS_BASE - 12, 3, 15, C_AMBER);
    s_guard_text = put(s_screen, &font_px12, C_AMBER, MARGIN + 8, FACTS_BASE, "");
    lv_obj_add_flag(s_guard_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_guard_text, LV_OBJ_FLAG_HIDDEN);
    build_week();
    build_foot();
    strlcpy(s_line, CARD_LINE_IDLE, sizeof(s_line));
    header_refresh();

    lv_timer_create(anim_tick, ANIM_TICK_MS, NULL);
    lv_screen_load(s_screen);
}

// The three facts under the number, each a figure and what it counts, with points between them. One whose value
// is "-" or empty is not known and is left out; one that no longer fits the line is left out with what follows.
static void set_facts(const card_stats_t *stats)
{
    lv_obj_clean(s_facts);
    int x = MARGIN;
    bool any = false;
    for (int i = 0; i < CARD_CELL_COUNT; i++) {
        char value[CARD_CELL_MAX];
        const char *label = stats->has_cells ? stats->cells[i].label : DEFAULT_LABELS[i];
        if (stats->has_cells) {
            strlcpy(value, stats->cells[i].value, sizeof(value));
        } else if (i == 0) {
            snprintf(value, sizeof(value), "%d", stats->deals);
        } else if (i == 1) {
            snprintf(value, sizeof(value), "%d.%d", stats->rating10 / 10, stats->rating10 % 10);
        } else {
            snprintf(value, sizeof(value), "%d", stats->streak);
        }
        if (!value[0] || strcmp(value, "-") == 0) continue;

        lv_obj_t *made[3] = { NULL, NULL, NULL };
        int at = x;
        if (any) {
            made[0] = put(s_facts, &font_mono12, C_MUTE, at, FACTS_BASE, " \xC2\xB7 ");
            at += width_of(made[0]);
        }
        made[1] = put(s_facts, &font_mono12b, C_TEXT, at, FACTS_BASE, value);
        at += width_of(made[1]);
        if (label[0]) {
            made[2] = put(s_facts, &font_mono12, C_SOFT, at + 7, FACTS_BASE, label);
            at += 7 + width_of(made[2]);
        }
        if (at > SCREEN_W - MARGIN) {
            for (int j = 0; j < 3; j++) {
                if (made[j]) lv_obj_delete(made[j]);
            }
            break;
        }
        x = at;
        any = true;
    }
}

static void set_week(const card_stats_t *stats)
{
    static const char LETTERS[] = "MTWTFSS";
    int peak = 1;
    for (int i = 0; stats->has_week && i < CARD_WEEK_DAYS; i++) {
        if (stats->week[i] > peak) peak = stats->week[i];
    }
    for (int i = 0; i < CARD_WEEK_DAYS; i++) {
        const bool today = stats->has_week && i == CARD_WEEK_DAYS - 1;
        const int earned = stats->has_week ? stats->week[i] : -1;
        // A day without a figure, or with nothing earned, keeps its place as a mark on the baseline.
        int h = earned > 0 ? (int)((int64_t)(BARS_BASE - BARS_TOP) * earned / peak) : 2;
        if (h < 2) h = 2;
        lv_obj_set_pos(s_bars[i], MARGIN + i * BAR_STEP, BARS_BASE - h);
        lv_obj_set_height(s_bars[i], h);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(today ? C_MINT : earned > 0 ? C_LEAF : C_LINE), 0);

        char letter[2] = { '\0', '\0' };
        if (stats->has_week) letter[0] = LETTERS[((stats->weekday - (CARD_WEEK_DAYS - 1) + i) % 7 + 7) % 7];
        lv_label_set_text(s_days[i], letter);
        lv_obj_set_style_text_color(s_days[i], lv_color_hex(today ? C_MINT : C_MUTE), 0);
    }
}

void ui_card_set_stats(const card_stats_t *stats)
{
    lv_label_set_text(s_name, stats->name);
    // The numbers say whose they are (the paired agent, or the one a computer feeds); otherwise the built-in agent's.
    s_agent_id = stats->agent_id > 0 ? stats->agent_id : CARD_AGENT_ID;
    if (stats->level > 0) {
        lv_label_set_text_fmt(s_level, "%d", stats->level);
    } else {
        lv_label_set_text(s_level, "-");   // not known yet
    }
    lv_bar_set_value(s_xp, stats->xp, LV_ANIM_OFF);
    lv_obj_set_x(s_xp, MARGIN + 22 + width_of(s_level) + 8);

    lv_label_set_text(s_money_label, stats->money_label[0] ? stats->money_label : DEFAULT_MONEY_LABEL);
    // Before anything has told the card its agent's numbers (the level is not known either), the number is not
    // a zero: it is not there.
    const bool known = stats->level > 0;
    char digits[16] = "--", small[8] = "", exact[24] = "";
    if (known) {
        split_total(stats->today_cents, digits, sizeof(digits), small, sizeof(small));
        exact_money(stats->today_cents, exact, sizeof(exact));
    }
    lv_obj_delete(s_number);
    s_number = number(s_screen, NUMBER_BASE, digits, small, known ? C_MINT : C_MUTE, C_TEXT);
    lv_obj_move_to_index(s_number, 0);   // under everything else, as when the face was built
    lv_label_set_text(s_exact, exact);

    set_facts(stats);
    set_week(stats);
}

void ui_card_set_status(const char *text_utf8, int progress)
{
    face_kind_t now;
    const bool changed = strcmp(s_line, text_utf8) != 0;
    // A computer's job moves its figure with every step: the same state, which the header shows with the figure.
    const bool step = is_work(s_line) && is_work(text_utf8);
    strlcpy(s_line, text_utf8, sizeof(s_line));
    s_progress = progress;
    (void)word_of(s_line, &now);
    // A line that tells the holder what to do stays at the foot. A state the agent moved into is said once, in
    // its own words, and then the header's word stands for it; resting is not said.
    strlcpy(s_hint, now == FACE_HINT ? s_line : "", sizeof(s_hint));
    if (changed && !step && now == FACE_PLAIN && strcmp(s_line, CARD_LINE_IDLE) != 0) {
        ui_card_say(s_line);
    } else if (!s_bubble_timer) {
        bubble_rest();
    }
    header_refresh();
}

void ui_card_set_busy(bool busy)
{
    s_busy = busy;
}

void ui_card_say(const char *text_utf8)
{
    lv_label_set_text(s_bubble_text, text_utf8);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    if (s_bubble_timer) lv_timer_delete(s_bubble_timer);
    s_bubble_timer = lv_timer_create(bubble_hide, BUBBLE_MS, NULL);
    lv_timer_set_repeat_count(s_bubble_timer, 1);
}

void ui_card_set_link(bool up)
{
    s_link = up;
    header_refresh();
}

void ui_card_set_net(const char *text_utf8)
{
    s_net = strcmp(text_utf8, CARD_NET_ONLINE) == 0 ? 2 : strcmp(text_utf8, CARD_NET_JOINING) == 0 ? 1 :
            strcmp(text_utf8, CARD_NET_FAILED) == 0 ? 3 : 0;
    header_refresh();
}

// ---- sheets ---------------------------------------------------------------

// A sheet on the dark ground, or (mint) the one that says something went through.
static void sheet_open(bool mint)
{
    ui_overlay_close();
    s_overlay = box(s_screen, 0, 0, SCREEN_W, SCREEN_H, mint ? C_MINT : C_BG);
    s_ink = mint ? C_BG : C_TEXT;
    s_soft = mint ? C_DEEP : C_SOFT;
}

// The tag that names the sheet.
static lv_obj_t *tag(const char *s, uint32_t bg, uint32_t fg)
{
    lv_obj_t *label = text(s_overlay, &font_px24, fg, s);
    lv_obj_set_style_bg_color(label, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(label, 6, 0);
    lv_obj_set_style_pad_right(label, 5, 0);
    lv_obj_set_style_pad_top(label, 3, 0);
    lv_obj_set_style_pad_bottom(label, 2, 0);
    lv_obj_set_pos(label, MARGIN, TAG_Y);
    return label;
}

// A line of the sheet, from the left edge, on a baseline.
static lv_obj_t *line(const lv_font_t *font, uint32_t color, int baseline, const char *s)
{
    lv_obj_t *label = put(s_overlay, font, color, MARGIN, baseline, s);
    one_line(label, FULL_W);
    return label;
}

// A block of text across the sheet that wraps, cut after `rows` lines. Returns the y under it.
static int block(const lv_font_t *font, uint32_t color, int top, const char *s, int rows)
{
    const int space = font == &font_px12 ? 6 : 4;
    lv_obj_t *label = text(s_overlay, font, color, s);
    lv_obj_set_width(label, FULL_W);
    lv_obj_set_style_text_line_space(label, space, 0);
    lv_obj_set_pos(label, MARGIN, top);
    const int most = rows * font->line_height + (rows - 1) * space;
    lv_obj_update_layout(label);
    int h = lv_obj_get_height(label);
    if (h > most) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(label, most);
        h = most;
    }
    return top + h;
}

// A line reversed out of a chip: the line to read first. The chip hangs out to the left, so its words begin at
// the edge every other line begins at.
static lv_obj_t *loud(int baseline, const char *s, uint32_t bg)
{
    lv_obj_t *label = put(s_overlay, &font_px12, C_BG, MARGIN - 6, baseline - 3, s);
    lv_obj_set_style_bg_color(label, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_obj_set_style_pad_ver(label, 3, 0);
    return label;
}

static void countdown_step(void *bar_obj, int32_t value)
{
    lv_bar_set_value(bar_obj, value, LV_ANIM_OFF);
    lv_obj_t *seconds = lv_obj_get_user_data(bar_obj);
    if (!seconds) return;
    const intptr_t left = (value + 999) / 1000;
    if ((intptr_t)lv_obj_get_user_data(seconds) == left) return;
    lv_obj_set_user_data(seconds, (void *)left);
    lv_label_set_text_fmt(seconds, "%ds", (int)left);
}

// A line that drains over ttl_ms, so the holder can see how long is left to answer; beside the tag, the seconds.
static void countdown(int y, int ttl_ms)
{
    if (ttl_ms < 1) ttl_ms = 1;
    lv_obj_t *left = bar(s_overlay, MARGIN, y, FULL_W, 3, C_LINE, C_MINT);
    lv_bar_set_range(left, 0, ttl_ms);
    lv_bar_set_value(left, ttl_ms, LV_ANIM_OFF);
    lv_obj_t *seconds = put_right(s_overlay, &font_mono12b, C_SOFT, NOTE_BASE, "");
    lv_obj_set_user_data(seconds, (void *)(intptr_t)-1);
    lv_obj_set_user_data(left, seconds);
    countdown_step(left, ttl_ms);
    lv_anim_t drain;
    lv_anim_init(&drain);
    lv_anim_set_var(&drain, left);
    lv_anim_set_exec_cb(&drain, countdown_step);
    lv_anim_set_values(&drain, ttl_ms, 0);
    lv_anim_set_duration(&drain, (uint32_t)ttl_ms);
    lv_anim_start(&drain);
}

static void dot_move(void *dot, int32_t y)
{
    lv_obj_set_y(dot, y);
}

static void dot_done(lv_anim_t *anim)
{
    lv_obj_delete(anim->var);
}

// Dots fall in for money earned and fly up and away for money spent.
static void scatter_dots(bool upward)
{
    static const uint32_t COLORS[3] = { C_BG, C_LEAF, C_WHITE };
    const int32_t from = upward ? SCREEN_H + 4 : -14;
    const int32_t to = upward ? -14 : SCREEN_H + 4;
    for (int i = 0; i < DOT_COUNT; i++) {
        lv_obj_t *dot = box(s_overlay, (int)lv_rand(10, SCREEN_W - 24), from, 12, 12, COLORS[i % 3]);
        lv_obj_set_style_radius(dot, 6, 0);
        lv_anim_t move;
        lv_anim_init(&move);
        lv_anim_set_var(&move, dot);
        lv_anim_set_exec_cb(&move, dot_move);
        lv_anim_set_values(&move, from, to);
        lv_anim_set_duration(&move, lv_rand(1000, 1400));
        lv_anim_set_delay(&move, lv_rand(0, 600));
        lv_anim_set_path_cb(&move, lv_anim_path_ease_in);
        lv_anim_set_completed_cb(&move, dot_done);
        lv_anim_start(&move);
    }
}

// The sheet's number: an amount, dollars large and cents small.
static void amount(int cents, uint32_t color, uint32_t small_color)
{
    char digits[16], small[8];
    split_money(cents, digits, sizeof(digits), small, sizeof(small));
    number(s_overlay, SHEET_NUMBER, digits, small, color, small_color);
}

void ui_overlay_offer(const card_offer_t *offer)
{
    sheet_open(false);
    tag("新单", C_MINT, C_BG);
    // A task the server told of is one the agent has already bid on, or that a buyer handed to it:
    // the sheet says which, and that OK only puts the sheet away. Nothing can be taken from here.
    const bool told = offer->remote;
    caps(s_overlay, C_MUTE, MARGIN, SHEET_LABEL, told && !offer->direct ? "BID \xC2\xB7 USD" : "REWARD \xC2\xB7 USD");
    if (told && offer->amount_cents <= 0) {
        number(s_overlay, SHEET_NUMBER, "--", "", C_MINT, C_TEXT);   // no price is known: better than "$0.00"
    } else {
        amount(offer->amount_cents, C_MINT, C_TEXT);
    }
    // Two lines at most, so a long title never runs into what is below it.
    const int under = block(&font_px24, C_TEXT, 130, offer->title, 2);
    char note[40];
    snprintf(note, sizeof(note), "#%d  %s", offer->id, offer->direct ? "有人点名要你" : told ? "你的 agent 出价了" : "限时抢单");
    line(&font_px12, C_SOFT, under + 18, note);
    countdown(COUNTDOWN_Y, offer->ttl_ms);
    line(&font_px12, C_TEXT, LAST_BASE, told ? "按 OK 知道了" : "按 OK 抢单");
}

void ui_overlay_paid(const card_paid_t *paid)
{
    sheet_open(true);
    tag("到账", C_BG, C_MINT);
    caps(s_overlay, C_LEAF, MARGIN, SHEET_LABEL, "NET \xC2\xB7 USD");
    amount(paid->net_cents, C_BG, C_BG);
    line(&font_px24, s_ink, 152, "买家验收了");
    char note[CARD_TITLE_MAX + 16];
    snprintf(note, sizeof(note), "#%d %s", paid->id, paid->title);
    line(&font_px12, s_soft, 180, note);
    scatter_dots(false);
}

// A poster: the crab, big, waving its claws over the new level.
void ui_overlay_level_up(int level)
{
    sheet_open(false);
    tag("升级了", C_MINT, C_BG);
    s_poster_crab = lv_image_create(s_overlay);
    lv_image_set_src(s_poster_crab, &crab_big0);
    lv_obj_set_pos(s_poster_crab, MARGIN, 58);
    caps(s_overlay, C_MUTE, MARGIN, 196, "NEW \xC2\xB7 LEVEL");
    char digits[8];
    snprintf(digits, sizeof(digits), "%d", level);
    lv_obj_t *lv = put(s_overlay, &font_big32, C_TEXT, MARGIN, 262, "LV");
    put(s_overlay, &font_big72, C_MINT, MARGIN + width_of(lv) + 4, 262, digits);
    line(&font_px12, C_SOFT, 296, "接单更靠前");
}

void ui_overlay_miss(const card_offer_t *offer)
{
    sheet_open(false);
    tag("手慢了", C_LINE, C_TEXT);
    caps(s_overlay, C_MUTE, MARGIN, SHEET_LABEL, "MISSED \xC2\xB7 USD");
    amount(offer->amount_cents, C_SOFT, C_SOFT);
    line(&font_px12, C_SOFT, 150, "单被别人抢走，连胜断了");
    line(&font_px12, C_SOFT, 170, offer->title);
}

// Who the money goes to, on one line.
static void payee(int baseline, const card_pay_t *pay)
{
    lv_obj_t *to = put(s_overlay, &font_px12, s_soft, MARGIN, baseline - 2, "付给");
    const int x = MARGIN + width_of(to) + 6;
    one_line(put(s_overlay, &font_px24, s_ink, x, baseline, pay->payee), SCREEN_W - MARGIN - x);
}

// What tells the holder whose request this is (the requester's page shows the same digits), or else its memo.
static void pay_mark(const card_pay_t *pay)
{
    if (pay->code[0]) {
        lv_obj_t *frame = box(s_overlay, MARGIN, 166, 10, 25, C_BG);
        lv_obj_set_style_border_width(frame, 1, 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(C_LINE), 0);
        lv_obj_t *name = put(frame, &font_px12, C_SOFT, 8, 17, "核对码");
        lv_obj_t *code = put(frame, &font_mono14, C_TEXT, 8 + width_of(name) + 6, 18, pay->code);
        lv_obj_set_style_text_letter_space(code, 1, 0);
        lv_obj_set_width(frame, 8 + width_of(name) + 6 + width_of(code) + 9);
    } else if (pay->memo[0]) {
        line(&font_px12, C_SOFT, 184, pay->memo);
    }
}

// The request sheet. `sending` is the same sheet once the hold has counted: the tag says so, the countdown is gone
// and the last line says the card is telling ClawHunt. Nothing else changes under the holder's thumb.
static void pay_sheet(const card_pay_t *pay, bool sending)
{
    sheet_open(false);
    tag(sending ? "确认中" : "付款请求", C_MINT, C_BG);
    caps(s_overlay, C_MUTE, MARGIN, SHEET_LABEL, "PAY \xC2\xB7 USD");
    amount(pay->amount_cents, C_MINT, C_TEXT);
    payee(150, pay);
    pay_mark(pay);

    char note[CARD_TITLE_MAX + 24];
    if (pay->balance_cents >= 0) {
        char money[20];
        card_format_money(money, sizeof(money), "", pay->balance_cents);
        snprintf(note, sizeof(note), "卡里还有 %s", money);
        line(&font_px12, C_SOFT, 214, note);
    } else {
        line(&font_px12, C_SOFT, 214, "试运行：只记录，不扣款");
    }
    if (sending) {
        // reversed out, as "keep holding" is: the line that changed is the line to read
        loud(LAST_BASE, "正在告诉 ClawHunt…", C_MINT);
        return;
    }
    countdown(COUNTDOWN_Y, pay->ttl_ms);
    // a press turns this line into "keep holding": a chip without its ground until then
    s_pay_hint = loud(LAST_BASE, PAY_HINT, C_MINT);
    lv_obj_set_style_text_color(s_pay_hint, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_opa(s_pay_hint, LV_OPA_TRANSP, 0);
}

void ui_overlay_pay(const card_pay_t *pay)
{
    pay_sheet(pay, false);
}

void ui_overlay_pay_sending(const card_pay_t *pay)
{
    pay_sheet(pay, true);
}

// One square of the secret sheet: an outline until its key is pressed, then solid. It never says
// which key it was: the screen can be watched, and can be read over USB.
static void slot_set(lv_obj_t *slot, bool pressed)
{
    lv_obj_set_style_bg_opa(slot, pressed ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(slot, lv_color_hex(pressed ? C_MINT : C_LINE), 0);
}

// The request once OK was held and the server wants the holder's secret: who is asking and how much, as on the
// request, and the eight squares.
void ui_overlay_pay_secret(const card_pay_t *pay, int filled, int left_ms, const char *note)
{
    sheet_open(false);
    tag("输入口令", C_MINT, C_BG);
    caps(s_overlay, C_MUTE, MARGIN, SHEET_LABEL, "PAY \xC2\xB7 USD");
    amount(pay->amount_cents, C_MINT, C_TEXT);
    payee(150, pay);

    for (int i = 0; i < CARD_SECRET_LEN; i++) {
        lv_obj_t *slot = box(s_overlay, MARGIN + 1 + i * SLOT_STEP, 170, SLOT_SIZE, SLOT_SIZE, C_MINT);
        lv_obj_set_style_border_width(slot, 2, 0);
        slot_set(slot, i < filled);
        s_slots[i] = slot;
    }
    if (note && note[0]) {
        loud(212, note, C_AMBER);   // e.g. that the last try was wrong
    } else {
        line(&font_px12, C_TEXT, 212, "用上、下、OK 三个键按 8 下");
    }
    countdown(228, left_ms);
    line(&font_px12, C_SOFT, 262, "按住下键：重新输入");
    line(&font_px12, C_SOFT, 280, "按住上键：不付了");
}

void ui_overlay_pay_secret_fill(int filled)
{
    for (int i = 0; i < CARD_SECRET_LEN; i++) {
        if (s_slots[i]) slot_set(s_slots[i], i < filled);
    }
}

static void pay_hint_restore(lv_timer_t *timer)
{
    (void)timer;
    s_hint_timer = NULL;   // one-shot timers delete themselves after firing
    if (!s_pay_hint) return;
    lv_label_set_text(s_pay_hint, PAY_HINT);
    lv_obj_set_style_text_color(s_pay_hint, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_opa(s_pay_hint, LV_OPA_TRANSP, 0);
}

// OK went down on a payment request: the line that says how to answer turns into "keep holding", reversed out.
// If the key is let go before the hold counts, the line goes back to the instruction.
void ui_overlay_pay_holding(void)
{
    if (!s_pay_hint) return;
    lv_label_set_text(s_pay_hint, "继续按住…");
    lv_obj_set_style_text_color(s_pay_hint, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_pay_hint, LV_OPA_COVER, 0);
    if (s_hint_timer) lv_timer_delete(s_hint_timer);
    s_hint_timer = lv_timer_create(pay_hint_restore, 900, NULL);
    lv_timer_set_repeat_count(s_hint_timer, 1);
}

void ui_overlay_pay_done(const card_pay_t *pay, bool paid)
{
    if (!paid) {
        sheet_open(false);
        tag("没付", C_LINE, C_TEXT);
        caps(s_overlay, C_MUTE, MARGIN, SHEET_LABEL, "AMOUNT \xC2\xB7 USD");
        amount(pay->amount_cents, C_SOFT, C_SOFT);
        line(&font_px12, C_SOFT, 150, pay->remote ? "这一笔已作废" : "这笔钱还在你卡里");
        return;
    }
    sheet_open(true);
    tag(pay->remote ? "已确认" : "已付款", C_BG, C_MINT);
    caps(s_overlay, C_LEAF, MARGIN, SHEET_LABEL, "PAID \xC2\xB7 USD");
    amount(pay->amount_cents, C_BG, C_BG);
    payee(152, pay);

    char note[CARD_TITLE_MAX + 24];
    if (pay->balance_cents >= 0) {
        char money[20];
        card_format_money(money, sizeof(money), "", pay->balance_cents - pay->amount_cents);
        snprintf(note, sizeof(note), "卡里还有 %s", money);
        line(&font_px12, s_soft, 180, note);
    } else {
        line(&font_px12, s_soft, 180, "试运行，没有扣款");
    }
    scatter_dots(true);
}

// Something did not go through. The tag is amber; red is kept for the one thing that stops the card: frozen.
void ui_overlay_pay_failed(const char *title, const char *detail)
{
    sheet_open(false);
    tag(title, strcmp(title, CARD_LINE_FROZEN) == 0 ? C_RED : C_AMBER, C_BG);
    block(&font_px12, C_TEXT, 62, detail, 6);
}

// The pairing code, the thing to type, as big as it fits. The backend's codes look like KTRX-4PQM; such a code is
// set on two lines, the dash left out.
static void big_code(const char *code)
{
    // The big sizes hold digits, capitals and the dash. A code with anything else in it (not one as the site gives
    // them) is set in the pixel font, which has it: smaller, but what is to be typed can be read.
    for (const char *c = code; *c; c++) {
        if ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || *c == '-') continue;
        one_line(put(s_overlay, &font_px24, C_MINT, MARGIN, 200, code), FULL_W);
        return;
    }
    const char *dash = strchr(code, '-');
    if (!dash || dash == code || !dash[1]) {
        // one line: the 72 px or the 48 px size, whichever takes it
        lv_obj_t *label = put(s_overlay, &font_big72, C_MINT, MARGIN - 3, 220, code);
        if (width_of(label) > FULL_W + 3) {
            lv_obj_set_style_text_font(label, &font_big48, 0);
            lv_obj_set_y(label, 220 - ascent(&font_big48));
        }
        return;
    }
    char first[16];
    snprintf(first, sizeof(first), "%.*s", (int)(dash - code), code);
    const char *halves[2] = { first, dash + 1 };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *label = put(s_overlay, &font_big72, C_MINT, MARGIN - 3, 186 + i * 64, halves[i]);
        lv_obj_set_style_text_letter_space(label, 5, 0);
        if (width_of(label) > FULL_W + 3) {
            lv_obj_set_style_text_letter_space(label, 0, 0);
            lv_obj_set_style_text_font(label, &font_big48, 0);
            lv_obj_set_y(label, 186 + i * 64 - ascent(&font_big48));
        }
    }
}

void ui_overlay_pairing(const card_pairing_t *pairing)
{
    sheet_open(false);
    tag("绑定这张卡", C_MINT, C_BG);
    line(&font_px12, C_SOFT, 64, "用手机打开");
    lv_obj_t *where = box(s_overlay, MARGIN, 72, FULL_W, 25, C_PANEL);
    one_line(put(where, &font_mono12b, C_TEXT, 8, 17, pairing->where), FULL_W - 16);
    line(&font_px12, C_SOFT, 118, "登录，输入这串码");
    big_code(pairing->code);
    box(s_overlay, MARGIN, 271, FULL_W, 1, C_LINE);
    char note[32];
    snprintf(note, sizeof(note), "%d 分钟内有效", pairing->minutes);
    line(&font_px12, C_SOFT, 296, note);
}

// An item is "name" or "name\tvalue"; the value is set at the right of its row.
void ui_overlay_menu(const char *const *items, int count, int selected)
{
    sheet_open(false);
    tag("设置", C_MINT, C_BG);
    for (int i = 0; i < count; i++) {
        const bool on = i == selected;
        const char *tab = strchr(items[i], '\t');
        char name[32];
        snprintf(name, sizeof(name), "%.*s", tab ? (int)(tab - items[i]) : (int)strlen(items[i]), items[i]);
        lv_obj_t *row = box(s_overlay, MARGIN, 60 + i * 44, FULL_W, 37, on ? C_MINT : C_PANEL);
        lv_obj_align(text(row, &font_px24, on ? C_BG : C_TEXT, name), LV_ALIGN_LEFT_MID, 10, 0);
        if (tab) {
            lv_obj_align(text(row, &font_px24, on ? C_BG : C_SOFT, tab + 1), LV_ALIGN_RIGHT_MID, -10, 0);
        } else if (on) {
            lv_obj_align(text(row, &font_px24, C_BG, ">"), LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }
    line(&font_px12, C_SOFT, 60 + count * 44 + 18, "上下键选择，OK 确认");
    // What this card is, at the foot of the sheet.
    box(s_overlay, MARGIN, FOOT_LINE_Y, FULL_W, 1, C_LINE);
    char note[32];
    snprintf(note, sizeof(note), "AGENT %d", s_agent_id);
    lv_obj_set_style_text_letter_space(put(s_overlay, &font_mono12b, C_SOFT, MARGIN, FOOT_BASE, note), 1, 0);
    lv_obj_set_style_text_letter_space(put_right(s_overlay, &font_mono12b, C_SOFT, FOOT_BASE, "FW " CARD_FW_VERSION), 1, 0);
}

// The code a phone camera reads as "join this Wi-Fi network", on a white plate wide enough to be its quiet zone.
static void setup_code(const card_setup_t *setup)
{
    sheet_open(false);
    tag("手机配网", C_MINT, C_BG);
    char note[96];
    snprintf(note, sizeof(note), "WIFI:T:WPA;S:%s;P:%s;;", setup->ap_ssid, setup->ap_pass);
    lv_obj_t *plate = box(s_overlay, MARGIN, 50, 160, 160, C_WHITE);
    lv_obj_t *code = lv_qrcode_create(plate);
    lv_qrcode_set_size(code, 140);
    lv_qrcode_set_dark_color(code, lv_color_hex(C_BG));
    lv_qrcode_set_light_color(code, lv_color_hex(C_WHITE));
    lv_qrcode_update(code, note, (uint32_t)strlen(note));
    lv_obj_center(code);
    // After a failed try that also cost the phone its place on the hotspot.
    static const char *const AGAIN[] = { "用相机扫码，连上卡的热点", "密码不对，重新扫码再试", "找不到网络，重新扫码再试",
                                         "没连上，重新扫码再试" };
    const char *say = AGAIN[setup->reason >= 0 && setup->reason < 4 ? setup->reason : 0];
    if (setup->reason) {
        loud(232, say, C_AMBER);
    } else {
        line(&font_px12, C_TEXT, 232, say);
    }
    line(&font_mono14, C_MINT, 256, setup->ap_ssid);
    lv_obj_t *name = put(s_overlay, &font_px12, C_SOFT, MARGIN, 276, "密码");
    put(s_overlay, &font_mono12b, C_TEXT, MARGIN + width_of(name) + 6, 277, setup->ap_pass);
    line(&font_px12, C_SOFT, 298, "按任意键退出");
}

// One of the plainer setup sheets: a tag, the network's name, two lines.
static void setup_note(const char *title, uint32_t tag_bg, const char *name, const char *line_1, const char *line_2, const char *hint)
{
    sheet_open(false);
    tag(title, tag_bg, tag_bg == C_LINE ? C_TEXT : C_BG);
    int base = 66;
    if (name[0]) {
        line(&font_px24, C_TEXT, 78, name);
        base = 108;
    }
    line(&font_px12, C_TEXT, base, line_1);
    line(&font_px12, C_SOFT, base + 20, line_2);
    if (hint[0]) line(&font_px12, C_SOFT, 298, hint);
}

void ui_overlay_setup(const card_setup_t *setup)
{
    static const char *const WHY[] = { "网络没有回应", "密码不对", "找不到这个网络（只认 2.4G）", "网络没有回应" };
    switch (setup->phase) {
    case CARD_SETUP_WAITING:
        setup_code(setup);
        break;
    case CARD_SETUP_JOINED:
        sheet_open(false);
        tag("手机连上了", C_MINT, C_BG);
        line(&font_px12, C_TEXT, 66, "在手机弹出的页面里");
        line(&font_px12, C_TEXT, 86, "选 Wi-Fi，输密码");
        line(&font_px12, C_SOFT, 126, "页面没弹出？用浏览器打开");
        run(s_overlay, MARGIN - 1, 162, "192.168.4.1", &font_big26, C_MINT);
        line(&font_px12, C_SOFT, 298, "按任意键退出");
        break;
    case CARD_SETUP_TRYING:
        setup_note("正在连接", C_MINT, setup->ssid, "大约 10 秒", "手机可能会短暂断开热点", "");
        break;
    case CARD_SETUP_DONE:
        setup_note("连上了", C_MINT, setup->ssid, "卡记住了这个网络", "热点马上关闭", "");
        break;
    case CARD_SETUP_FAILED:
        setup_note("没连上", C_AMBER, setup->ssid, WHY[setup->reason >= 0 && setup->reason < 4 ? setup->reason : 0],
                   "在手机页面上再试一次", "按任意键退出");
        break;
    default:
        setup_note("手机配网", C_MINT, "", "正在开热点…", "", "");
        break;
    }
}

void ui_overlay_close(void)
{
    if (!s_overlay) return;
    if (s_hint_timer) {
        lv_timer_delete(s_hint_timer);
        s_hint_timer = NULL;
    }
    s_pay_hint = NULL;
    s_poster_crab = NULL;
    memset(s_slots, 0, sizeof(s_slots));
    lv_obj_delete(s_overlay);   // also drops the dots, the countdown and their animations
    s_overlay = NULL;
}
