// main/ui_card.c -- the card face and its full-screen sheets.
//
// The look is the brand's (Clawhunt Brand Guidelines): blue for action, teal and mint for what went through and
// what is live, ink for type. Nothing stands on plain white or plain black: every screen stands on a colour field
// that runs down the screen from one colour to another (main/ground_strips.c), and the field says what kind of
// screen it is:
//   home  -- the card at rest: blue at the top, white at the foot
//   ask   -- something wants an answer (blue): a payment request, the holder's secret, a new order
//   done  -- it went through (teal to mint): confirmed, paid in, connected
//   setup -- setting the card up (ink): its code, its menu, its hotspot
//   none  -- nothing happened (slate): missed, refused, failed
// A sheet is centred: a tag that names it, the facts under it, and its one number in the largest size that fits.
// Numbers carry no sign: the tag and the field say which way the money went.
// Layout is in absolute pixels for the 240x320 panel. The BSP masks the final frame to a 30 px corner radius, so
// nothing important sits in the corners.
#include "ui_card.h"

#include "card_identity.h"
#include "crab_sprite.h"
#include "ground_strips.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_px12);
LV_FONT_DECLARE(font_px24);
LV_FONT_DECLARE(font_px36);   // ASCII and the hanzi of the titles (tools/gen_fonts.py, TITLES)
LV_FONT_DECLARE(font_px48);   // ASCII
LV_FONT_DECLARE(font_px72);   // ASCII

#define C_WHITE     0xFFFFFF
#define C_SKY       0xE5F8FB   // the crab's window; secondary type on blue and ink (tools/gen_crab.py bakes it in)
#define C_MINT      0xC6FDF1   // the window's floor; the number on blue and ink
#define C_BLUE      0x3F7FF5   // action
#define C_BLUE_DEEP 0x2496DF
#define C_TEAL      0x10B6A6   // live, done
#define C_INK       0x0E1C2C   // type
#define C_SLATE     0x4F6477   // secondary type on light
#define C_LINE      0xCFE6E8   // secondary type on slate

#define SCREEN_W        240
#define SCREEN_H        320
#define MARGIN          14    // left and right edge of everything
#define FULL_W          212   // SCREEN_W - 2 * MARGIN
#define WINDOW_Y        28    // the crab's window on the face
#define WINDOW_H        92
#define CRAB_Y          13    // the crab inside its window
#define STATUS_Y        220   // the face's status line, which the crab also speaks on
#define STATUS_H        22
#define ANIM_TICK_MS    160
#define BUBBLE_MS       1400
#define DOT_COUNT       14
#define SLOT_SIZE       20    // one square per press of the holder's secret
#define SLOT_STEP       27    // eight of them across the sheet: 7 * 27 + 20 = 209
#define SLOT_X          15    // (SCREEN_W - 209) / 2
#define COUNTDOWN_W     160

static const char *const DEFAULT_LABELS[CARD_CELL_COUNT] = { "成交", "好评", "连胜" };
static const char *const DEFAULT_MONEY_LABEL = "今天赚了";
static const char *const PAY_HINT = "按住 OK 付款，上下键不付";
static const lv_font_t *const SIZES_72[] = { &font_px72, &font_px48, &font_px36, &font_px24, NULL };
static const lv_font_t *const SIZES_48[] = { &font_px48, &font_px36, &font_px24, NULL };
static const lv_font_t *const SIZES_36[] = { &font_px36, &font_px24, NULL };

// A colour field and what stands on it.
typedef struct {
    const lv_image_dsc_t *strip;
    uint32_t base;     // under the strip, should it ever fail to draw
    uint32_t ink;      // type
    uint32_t soft;     // secondary type
    uint32_t tag_bg;   // the tag that names the sheet
    uint32_t tag_fg;
    uint32_t number;   // the sheet's one number
} ground_t;

static const ground_t G_ASK = { &ground_ask, C_BLUE, C_WHITE, C_SKY, C_INK, C_WHITE, C_MINT };
static const ground_t G_DONE = { &ground_done, C_TEAL, C_INK, C_INK, C_INK, C_WHITE, C_INK };
static const ground_t G_SETUP = { &ground_setup, C_INK, C_WHITE, C_SKY, C_TEAL, C_WHITE, C_MINT };
static const ground_t G_NONE = { &ground_none, C_SLATE, C_WHITE, C_LINE, C_INK, C_WHITE, C_LINE };

static lv_obj_t *s_screen;
static lv_obj_t *s_crab;
static lv_obj_t *s_bubble;           // the crab's voice: an ink bar over the status line
static lv_obj_t *s_bubble_text;
static lv_obj_t *s_name;
static lv_obj_t *s_plate_id;         // the agent's number in the window
static int s_agent_id = CARD_AGENT_ID;   // the agent the face shows
static lv_obj_t *s_level;
static lv_obj_t *s_xp;
static lv_obj_t *s_stat_labels[CARD_CELL_COUNT];
static lv_obj_t *s_stat_values[CARD_CELL_COUNT];
static lv_obj_t *s_status_bar;
static lv_obj_t *s_status_text;
static lv_obj_t *s_money_label;
static lv_obj_t *s_today;
static lv_obj_t *s_link;
static lv_obj_t *s_net;
static lv_obj_t *s_overlay;
static lv_obj_t *s_pay_hint;         // the line of a payment request that says how to answer it
static lv_obj_t *s_slots[CARD_SECRET_LEN];   // the squares of the secret sheet, while it is up
static lv_obj_t *s_poster_crab;      // the big crab of the level-up poster, while it is up
static lv_timer_t *s_bubble_timer;
static lv_timer_t *s_hint_timer;
static bool s_busy;
static uint32_t s_tick;
static const ground_t *s_ground = &G_ASK;   // the field of the sheet that is up

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

// A colour field: its strip repeated across the object, which must be as tall as the screen.
static void lay_ground(lv_obj_t *obj, const lv_image_dsc_t *strip)
{
    lv_obj_set_style_bg_image_src(obj, strip, 0);
    lv_obj_set_style_bg_image_tiled(obj, true, 0);
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, s);
    return label;
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

// A label on a filled chip, e.g. the level badge or the tag of a sheet.
static lv_obj_t *chip(lv_obj_t *parent, const lv_font_t *font, uint32_t bg, uint32_t fg, const char *s)
{
    lv_obj_t *label = text(parent, font, fg, s);
    lv_obj_set_style_bg_color(label, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_obj_set_style_pad_ver(label, 1, 0);
    return label;
}

// One line of text cut to a width, ending in an ellipsis if it is longer.
static void one_line(lv_obj_t *label, int w)
{
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
}

// Set a label in the largest of the sizes in which it fits the width.
static void fit(lv_obj_t *label, const lv_font_t *const *sizes, int max_w)
{
    for (int i = 0; sizes[i]; i++) {
        lv_obj_set_style_text_font(label, sizes[i], 0);
        lv_obj_update_layout(label);
        if (lv_obj_get_width(label) <= max_w) return;
    }
}

// ---- timers ---------------------------------------------------------------

static void anim_tick(lv_timer_t *timer)
{
    (void)timer;
    s_tick++;
    const uint32_t before = s_busy ? ((s_tick - 1) & 1) : (((s_tick - 1) >> 2) & 1);
    const uint32_t now = s_busy ? (s_tick & 1) : ((s_tick >> 2) & 1);
    if (now != before) lv_image_set_src(s_crab, now ? &crab_frame1 : &crab_frame0);
    // The poster crab waves its claws, twice as fast as the resting crab.
    if (s_poster_crab && (((s_tick - 1) >> 1) & 1) != ((s_tick >> 1) & 1)) {
        lv_image_set_src(s_poster_crab, ((s_tick >> 1) & 1) ? &crab_big1 : &crab_big0);
    }
}

static void bubble_hide(lv_timer_t *timer)
{
    (void)timer;
    s_bubble_timer = NULL;   // one-shot timers delete themselves after firing
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

// ---- card face ------------------------------------------------------------
//
// Top to bottom, on the blue-to-white field: the label row (9), the crab in its window (28-120), name and level
// (128), experience (156), three tiles (168-214), the status line (220-242), and what the agent has earned (250-300).

static void build_window(void)
{
    lv_obj_t *win = box(s_screen, MARGIN, WINDOW_Y, FULL_W, WINDOW_H, C_SKY);
    box(win, 0, CRAB_Y + CRAB_FLOOR_ROW, FULL_W, WINDOW_H - CRAB_Y - CRAB_FLOOR_ROW, C_MINT);
    s_crab = lv_image_create(win);
    lv_image_set_src(s_crab, &crab_frame0);
    lv_obj_set_pos(s_crab, (FULL_W - CRAB_W) / 2, CRAB_Y);

    s_plate_id = text(win, &font_px12, C_SLATE, "");
    lv_label_set_text_fmt(s_plate_id, "No.%d", s_agent_id);
    lv_obj_set_pos(s_plate_id, 6, WINDOW_H - 16);
}

static void build_tiles(void)
{
    for (int i = 0; i < CARD_CELL_COUNT; i++) {
        lv_obj_t *cell = box(s_screen, MARGIN + i * 72, 168, 68, 46, C_WHITE);
        lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
        s_stat_labels[i] = text(cell, &font_px12, C_SLATE, DEFAULT_LABELS[i]);
        lv_obj_align(s_stat_labels[i], LV_ALIGN_TOP_MID, 0, 4);
        s_stat_values[i] = text(cell, &font_px24, C_INK, "-");
        lv_obj_align(s_stat_values[i], LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

// The status line, and the crab's voice over it: what the crab says replaces the line for a moment, white on ink.
static void build_status(void)
{
    s_status_bar = bar(s_screen, MARGIN, STATUS_Y, FULL_W, STATUS_H, C_WHITE, C_TEAL);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_40, LV_PART_INDICATOR);
    box(s_screen, MARGIN + 7, STATUS_Y + 8, 6, 6, C_TEAL);
    s_status_text = text(s_screen, &font_px12, C_INK, "");
    lv_obj_set_pos(s_status_text, MARGIN + 19, STATUS_Y + 5);

    s_bubble = box(s_screen, MARGIN, STATUS_Y, FULL_W, STATUS_H, C_INK);
    s_bubble_text = text(s_bubble, &font_px12, C_WHITE, "");
    lv_obj_set_pos(s_bubble_text, 7, 5);
    one_line(s_bubble_text, FULL_W - 14);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

void ui_card_init(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BLUE), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lay_ground(s_screen, &ground_home);

    lv_obj_set_pos(text(s_screen, &font_px12, C_WHITE, "ClawHunt"), 24, 9);
    s_net = text(s_screen, &font_px12, C_WHITE, "");
    lv_obj_align(s_net, LV_ALIGN_TOP_RIGHT, -38, 9);
    s_link = box(s_screen, 208, 12, 6, 6, C_WHITE);
    lv_obj_set_style_bg_opa(s_link, LV_OPA_40, 0);

    build_window();

    s_name = text(s_screen, &font_px24, C_INK, "...");
    lv_obj_set_pos(s_name, MARGIN, 128);
    s_level = chip(s_screen, &font_px24, C_INK, C_WHITE, "Lv.-");
    lv_obj_align(s_level, LV_ALIGN_TOP_RIGHT, -MARGIN, 127);
    s_xp = bar(s_screen, MARGIN, 156, FULL_W, 6, C_WHITE, C_TEAL);
    lv_obj_set_style_bg_opa(s_xp, LV_OPA_70, LV_PART_MAIN);

    build_tiles();
    build_status();

    s_money_label = text(s_screen, &font_px12, C_SLATE, DEFAULT_MONEY_LABEL);
    lv_obj_set_pos(s_money_label, MARGIN + 2, 250);
    lv_obj_align(text(s_screen, &font_px12, C_SLATE, "USD"), LV_ALIGN_TOP_RIGHT, -MARGIN - 2, 250);
    s_today = text(s_screen, &font_px36, C_BLUE, "$0.00");
    lv_obj_set_pos(s_today, MARGIN, 264);

    lv_timer_create(anim_tick, ANIM_TICK_MS, NULL);
    lv_screen_load(s_screen);
}

// The computer may relabel the three tiles; otherwise they show the built-in deals / rating / streak.
static void set_tiles(const card_stats_t *stats)
{
    if (stats->has_cells) {
        for (int i = 0; i < CARD_CELL_COUNT; i++) {
            lv_label_set_text(s_stat_labels[i], stats->cells[i].label);
            lv_label_set_text(s_stat_values[i], stats->cells[i].value);
        }
        return;
    }
    for (int i = 0; i < CARD_CELL_COUNT; i++) {
        lv_label_set_text(s_stat_labels[i], DEFAULT_LABELS[i]);
    }
    lv_label_set_text_fmt(s_stat_values[0], "%d", stats->deals);
    lv_label_set_text_fmt(s_stat_values[1], "%d.%d", stats->rating10 / 10, stats->rating10 % 10);
    lv_label_set_text_fmt(s_stat_values[2], "%d", stats->streak);
}

void ui_card_set_stats(const card_stats_t *stats)
{
    lv_label_set_text(s_name, stats->name);
    // The numbers say whose they are (the paired agent, or the one a computer feeds); otherwise the built-in agent's.
    s_agent_id = stats->agent_id > 0 ? stats->agent_id : CARD_AGENT_ID;
    lv_label_set_text_fmt(s_plate_id, "No.%d", s_agent_id);
    if (stats->level > 0) {
        lv_label_set_text_fmt(s_level, "Lv.%d", stats->level);
    } else {
        lv_label_set_text(s_level, "Lv.-");   // not known yet
    }
    lv_bar_set_value(s_xp, stats->xp, LV_ANIM_OFF);
    set_tiles(stats);
    lv_label_set_text(s_money_label,
                      stats->money_label[0] ? stats->money_label : DEFAULT_MONEY_LABEL);

    char money[20];
    card_format_money(money, sizeof(money), "", stats->today_cents);
    lv_label_set_text(s_today, money);
    fit(s_today, SIZES_36, FULL_W);
}

void ui_card_set_status(const char *text_utf8, int progress)
{
    lv_label_set_text(s_status_text, text_utf8);
    lv_bar_set_value(s_status_bar, progress, LV_ANIM_OFF);
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
    lv_obj_set_style_bg_color(s_link, lv_color_hex(up ? C_MINT : C_WHITE), 0);
    lv_obj_set_style_bg_opa(s_link, up ? LV_OPA_COVER : LV_OPA_40, 0);
}

void ui_card_set_net(const char *text_utf8)
{
    lv_label_set_text(s_net, text_utf8);
}

// ---- sheets ---------------------------------------------------------------

static void sheet_open(const ground_t *ground)
{
    ui_overlay_close();
    s_overlay = box(s_screen, 0, 0, SCREEN_W, SCREEN_H, ground->base);
    lay_ground(s_overlay, ground->strip);
    s_ground = ground;
}

// A centred block of text across the sheet. It wraps; cut it with clamp().
static lv_obj_t *mid(const lv_font_t *font, uint32_t color, int y, const char *s)
{
    lv_obj_t *label = text(s_overlay, font, color, s);
    lv_obj_set_width(label, FULL_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    if (font == &font_px12) lv_obj_set_style_text_line_space(label, 4, 0);   // small type that wraps needs air
    lv_obj_set_pos(label, MARGIN, y);
    return label;
}

// Keep a block to a height; longer text ends in an ellipsis.
static lv_obj_t *clamp(lv_obj_t *label, int height)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, height);
    return label;
}

// The tag that names the sheet.
static lv_obj_t *tag(int y, const char *s)
{
    lv_obj_t *label = chip(s_overlay, &font_px24, s_ground->tag_bg, s_ground->tag_fg, s);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

// A line reversed out of a chip in the field's own colours: the line to read first.
static lv_obj_t *loud(int y, const char *s)
{
    lv_obj_t *label = chip(s_overlay, &font_px12, s_ground->ink, s_ground->base, s);
    lv_obj_set_style_pad_ver(label, 3, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y - 3);
    return label;
}

// The sheet's one number, centred, in the largest of the sizes that fits. Returns the y under it.
static int number(int y, const lv_font_t *const *sizes, const char *s)
{
    lv_obj_t *label = text(s_overlay, sizes[0], s_ground->number, s);
    fit(label, sizes, FULL_W);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return y + lv_obj_get_height(label);
}

static void countdown_step(void *bar_obj, int32_t value)
{
    lv_bar_set_value(bar_obj, value, LV_ANIM_OFF);
}

// A bar that drains over ttl_ms, so the holder can see how long is left to answer.
static void countdown_bar(int y, int ttl_ms)
{
    lv_obj_t *countdown = bar(s_overlay, (SCREEN_W - COUNTDOWN_W) / 2, y, COUNTDOWN_W, 8, C_INK, C_MINT);
    lv_obj_set_style_bg_opa(countdown, LV_OPA_30, LV_PART_MAIN);
    lv_bar_set_range(countdown, 0, 1000);
    lv_bar_set_value(countdown, 1000, LV_ANIM_OFF);
    lv_anim_t drain;
    lv_anim_init(&drain);
    lv_anim_set_var(&drain, countdown);
    lv_anim_set_exec_cb(&drain, countdown_step);
    lv_anim_set_values(&drain, 1000, 0);
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
    static const uint32_t COLORS[3] = { C_WHITE, C_BLUE, C_INK };
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

void ui_overlay_offer(const card_offer_t *offer)
{
    sheet_open(&G_ASK);
    tag(40, "新单");
    // A task the server told of is one the agent has already bid on, or that a buyer handed to it:
    // the sheet says which, and that OK only puts the sheet away. Nothing can be taken from here.
    const bool told = offer->remote;
    char line[40];
    snprintf(line, sizeof(line), "#%d  %s", offer->id,
             offer->direct ? "有人点名要你" : told ? "你的 agent 出价了" : "限时抢单");
    mid(&font_px12, s_ground->soft, 76, line);
    // Two lines at most, so a long title never runs into what is below it.
    clamp(mid(&font_px24, s_ground->ink, 96, offer->title), 48);

    char money[20];
    if (told && offer->amount_cents <= 0) {
        strlcpy(money, "--", sizeof(money));   // no price is known: better than "$0.00"
    } else {
        card_format_money(money, sizeof(money), "", offer->amount_cents);
    }
    mid(&font_px12, s_ground->soft, 154, told && !offer->direct ? "报价 USD" : "报酬 USD");
    number(168, SIZES_48, money);
    countdown_bar(228, offer->ttl_ms);
    mid(&font_px12, s_ground->ink, 246, told ? "按 OK 知道了" : "按 OK 抢单");
}

void ui_overlay_paid(const card_paid_t *paid)
{
    sheet_open(&G_DONE);
    tag(60, "到账");

    char money[20];
    card_format_money(money, sizeof(money), "", paid->net_cents);
    const int y = number(100, SIZES_72, money);
    mid(&font_px12, s_ground->soft, y + 6, "到手 USD");

    char line[CARD_TITLE_MAX + 16];
    snprintf(line, sizeof(line), "#%d %s", paid->id, paid->title);
    clamp(mid(&font_px12, s_ground->ink, y + 34, line), 12);
    mid(&font_px12, s_ground->ink, y + 52, "买家验收了");
    scatter_dots(false);
}

// A poster: the crab, big, waving its claws in its window over the new level.
void ui_overlay_level_up(int level)
{
    sheet_open(&G_SETUP);
    tag(16, "升级了");
    lv_obj_t *win = box(s_overlay, MARGIN, 52, FULL_W, 148, C_SKY);
    box(win, 0, 8 + CRAB_BIG_FLOOR_ROW, FULL_W, 148 - 8 - CRAB_BIG_FLOOR_ROW, C_MINT);
    s_poster_crab = lv_image_create(win);
    lv_image_set_src(s_poster_crab, &crab_big0);
    lv_obj_set_pos(s_poster_crab, (FULL_W - CRAB_BIG_W) / 2, 8);
    char line[16];
    snprintf(line, sizeof(line), "Lv.%d", level);
    number(206, SIZES_72, line);
    mid(&font_px12, s_ground->soft, 282, "接单更靠前");
}

void ui_overlay_miss(const card_offer_t *offer)
{
    sheet_open(&G_NONE);
    mid(&font_px36, s_ground->ink, 84, "手慢了");
    mid(&font_px12, s_ground->soft, 134, "单被别人抢走，连胜断了");
    clamp(mid(&font_px12, s_ground->soft, 156, offer->title), 12);

    char money[20];
    card_format_money(money, sizeof(money), "", offer->amount_cents);
    mid(&font_px12, s_ground->soft, 190, "错过 USD");
    number(204, SIZES_36, money);
}

// Who the money goes to, on one line.
static void payee(int y, const card_pay_t *pay)
{
    char line[CARD_NAME_MAX + 16];
    snprintf(line, sizeof(line), "付给 %s", pay->payee);
    clamp(mid(&font_px24, s_ground->ink, y, line), 24);
}

// The request sheet. `sending` is the same sheet once the hold has counted: the tag says so, the countdown is gone
// and the last line says the card is telling ClawHunt. Nothing else changes under the holder's thumb, and the
// screen does not change colour between the request and its result.
static void pay_sheet(const card_pay_t *pay, bool sending)
{
    sheet_open(&G_ASK);
    tag(48, sending ? "确认中" : "付款请求");
    payee(84, pay);

    char money[20];
    card_format_money(money, sizeof(money), "", pay->amount_cents);
    number(112, SIZES_48, money);

    char line[CARD_TITLE_MAX + 24];
    if (pay->code[0]) {
        // The requester's page shows the same digits: the holder can tell whose request this is.
        snprintf(line, sizeof(line), "核对码 %s", pay->code);
        lv_obj_align(chip(s_overlay, &font_px24, C_MINT, C_INK, line), LV_ALIGN_TOP_MID, 0, 168);
    } else if (pay->memo[0]) {
        clamp(mid(&font_px12, s_ground->soft, 176, pay->memo), 12);
    }
    if (pay->balance_cents >= 0) {
        card_format_money(money, sizeof(money), "", pay->balance_cents);
        snprintf(line, sizeof(line), "卡里还有 %s", money);
        mid(&font_px12, s_ground->soft, 206, line);
    } else {
        mid(&font_px12, s_ground->soft, 206, "试运行：只记录，不扣款");
    }
    if (sending) {
        // reversed out, as "keep holding" is: the line that changed is the line to read
        loud(246, "正在告诉 ClawHunt…");
        return;
    }
    countdown_bar(228, pay->ttl_ms);
    // a press turns this line into "keep holding": a chip without its ground until then
    s_pay_hint = loud(246, PAY_HINT);
    lv_obj_set_style_text_color(s_pay_hint, lv_color_hex(s_ground->ink), 0);
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
}

// The request once OK was held and the server wants the holder's secret: the same blue sheet, who is asking and
// how much, and the eight squares.
void ui_overlay_pay_secret(const card_pay_t *pay, int filled, int left_ms, const char *note)
{
    sheet_open(&G_ASK);
    tag(42, "输入口令");
    payee(78, pay);

    char money[20];
    card_format_money(money, sizeof(money), "", pay->amount_cents);
    number(104, SIZES_36, money);

    for (int i = 0; i < CARD_SECRET_LEN; i++) {
        lv_obj_t *slot = box(s_overlay, SLOT_X + i * SLOT_STEP, 152, SLOT_SIZE, SLOT_SIZE, C_WHITE);
        lv_obj_set_style_border_color(slot, lv_color_hex(C_WHITE), 0);
        lv_obj_set_style_border_width(slot, 2, 0);
        slot_set(slot, i < filled);
        s_slots[i] = slot;
    }
    if (note && note[0]) {
        loud(188, note);   // e.g. that the last try was wrong
    } else {
        mid(&font_px12, s_ground->ink, 188, "用上、下、OK 三个键按 8 下");
    }
    countdown_bar(210, left_ms);
    mid(&font_px12, s_ground->soft, 228, "按住下键：重新输入");
    mid(&font_px12, s_ground->soft, 246, "按住上键：不付了");
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
    lv_obj_set_style_text_color(s_pay_hint, lv_color_hex(s_ground->ink), 0);
    lv_obj_set_style_bg_opa(s_pay_hint, LV_OPA_TRANSP, 0);
}

// OK went down on a payment request: the line that says how to answer turns into "keep holding", reversed out.
// If the key is let go before the hold counts, the line goes back to the instruction.
void ui_overlay_pay_holding(void)
{
    if (!s_pay_hint) return;
    lv_label_set_text(s_pay_hint, "继续按住…");
    lv_obj_set_style_text_color(s_pay_hint, lv_color_hex(s_ground->base), 0);
    lv_obj_set_style_bg_opa(s_pay_hint, LV_OPA_COVER, 0);
    if (s_hint_timer) lv_timer_delete(s_hint_timer);
    s_hint_timer = lv_timer_create(pay_hint_restore, 900, NULL);
    lv_timer_set_repeat_count(s_hint_timer, 1);
}

void ui_overlay_pay_done(const card_pay_t *pay, bool paid)
{
    char money[20];
    card_format_money(money, sizeof(money), "", pay->amount_cents);
    if (!paid) {
        sheet_open(&G_NONE);
        mid(&font_px36, s_ground->ink, 92, "没付");
        mid(&font_px12, s_ground->soft, 142, pay->remote ? "这一笔已作废" : "这笔钱还在你卡里");
        mid(&font_px12, s_ground->soft, 182, "金额 USD");
        number(196, SIZES_36, money);
        return;
    }
    sheet_open(&G_DONE);
    tag(64, pay->remote ? "已确认" : "已付款");
    const int y = number(104, SIZES_72, money);
    payee(y + 10, pay);

    char line[CARD_TITLE_MAX + 24];
    if (pay->balance_cents >= 0) {
        card_format_money(money, sizeof(money), "", pay->balance_cents - pay->amount_cents);
        snprintf(line, sizeof(line), "卡里还有 %s", money);
        mid(&font_px12, s_ground->soft, y + 44, line);
    } else {
        mid(&font_px12, s_ground->soft, y + 44, "试运行，没有扣款");
    }
    scatter_dots(true);
}

void ui_overlay_pay_failed(const char *title, const char *detail)
{
    sheet_open(&G_NONE);
    lv_obj_t *big = text(s_overlay, &font_px36, s_ground->ink, title);
    fit(big, SIZES_36, FULL_W);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 88);
    clamp(mid(&font_px12, s_ground->soft, 142, detail), 60);   // four lines
}

// The pairing code, the thing to type, as big as it fits. The backend's codes look like KTRX-4PQM; such a code is
// set on two lines, the dash left out.
static void big_code(const char *code)
{
    const char *dash = strchr(code, '-');
    if (!dash || dash == code || !dash[1]) {
        number(150, SIZES_72, code);
        return;
    }
    char first[16];
    snprintf(first, sizeof(first), "%.*s", (int)(dash - code), code);
    lv_obj_t *lines[2] = { text(s_overlay, &font_px72, s_ground->number, first),
                           text(s_overlay, &font_px72, s_ground->number, dash + 1) };
    // both lines in the largest size in which both fit
    for (int i = 0; SIZES_72[i]; i++) {
        bool fits = true;
        for (int j = 0; j < 2; j++) {
            lv_obj_set_style_text_font(lines[j], SIZES_72[i], 0);
            lv_obj_update_layout(lines[j]);
            if (lv_obj_get_width(lines[j]) > FULL_W) fits = false;
        }
        if (fits) break;
    }
    const int h = lv_obj_get_height(lines[0]);
    lv_obj_align(lines[0], LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_align(lines[1], LV_ALIGN_TOP_MID, 0, 122 + h - h / 12);   // one pixel of the type's grid tighter than its leading
}

void ui_overlay_pairing(const card_pairing_t *pairing)
{
    sheet_open(&G_SETUP);
    tag(18, "绑定这张卡");
    mid(&font_px12, s_ground->soft, 54, "用手机打开");
    lv_obj_t *where = chip(s_overlay, &font_px12, C_MINT, C_INK, pairing->where);
    lv_obj_set_style_pad_ver(where, 3, 0);
    lv_obj_align(where, LV_ALIGN_TOP_MID, 0, 70);
    mid(&font_px12, s_ground->soft, 100, "登录，输入这串码");
    big_code(pairing->code);
    char line[32];
    snprintf(line, sizeof(line), "%d 分钟内有效", pairing->minutes);
    mid(&font_px12, s_ground->soft, 276, line);
}

// An item is "name" or "name\tvalue"; the value is set at the right of its row.
void ui_overlay_menu(const char *const *items, int count, int selected)
{
    sheet_open(&G_SETUP);
    tag(18, "设置");
    for (int i = 0; i < count; i++) {
        const bool on = i == selected;
        const char *tab = strchr(items[i], '\t');
        char name[32];
        snprintf(name, sizeof(name), "%.*s", tab ? (int)(tab - items[i]) : (int)strlen(items[i]), items[i]);
        lv_obj_t *row = box(s_overlay, MARGIN, 58 + i * 46, FULL_W, 40, on ? C_MINT : C_WHITE);
        if (!on) lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
        lv_obj_align(text(row, &font_px24, on ? C_INK : C_WHITE, name), LV_ALIGN_LEFT_MID, 10, 0);
        if (on) lv_obj_align(text(row, &font_px24, C_INK, ">"), LV_ALIGN_RIGHT_MID, -10, 0);
        if (tab) lv_obj_align(text(row, &font_px24, on ? C_INK : C_SKY, tab + 1), LV_ALIGN_RIGHT_MID, on ? -34 : -10, 0);
    }
    // What this card is, as two lines at the foot of the sheet.
    char line[48];
    snprintf(line, sizeof(line), "AGENT No.%d    固件 %s", s_agent_id, CARD_FW_VERSION);
    mid(&font_px12, s_ground->soft, 214, line);
    mid(&font_px12, s_ground->ink, 270, "上下键选择，OK 确认");
}

// The code a phone camera reads as "join this Wi-Fi network", on a white plate wide enough to be its quiet zone.
static void setup_code(const card_setup_t *setup)
{
    sheet_open(&G_SETUP);
    tag(8, "手机配网");
    char line[96];
    snprintf(line, sizeof(line), "WIFI:T:WPA;S:%s;P:%s;;", setup->ap_ssid, setup->ap_pass);
    lv_obj_t *plate = box(s_overlay, 38, 42, 164, 164, C_WHITE);
    lv_obj_t *code = lv_qrcode_create(plate);
    lv_qrcode_set_size(code, 140);
    lv_qrcode_set_dark_color(code, lv_color_hex(C_INK));
    lv_qrcode_set_light_color(code, lv_color_hex(C_WHITE));
    lv_qrcode_update(code, line, (uint32_t)strlen(line));
    lv_obj_center(code);
    // After a failed try that also cost the phone its place on the hotspot.
    static const char *const AGAIN[] = { "用相机扫码，连上卡的热点", "密码不对，重新扫码再试", "找不到网络，重新扫码再试",
                                         "没连上，重新扫码再试" };
    const char *say = AGAIN[setup->reason >= 0 && setup->reason < 4 ? setup->reason : 0];
    if (setup->reason) {
        loud(216, say);
    } else {
        mid(&font_px12, s_ground->ink, 216, say);
    }
    clamp(mid(&font_px24, s_ground->number, 236, setup->ap_ssid), 24);
    snprintf(line, sizeof(line), "密码 %s", setup->ap_pass);
    mid(&font_px12, s_ground->soft, 266, line);
    mid(&font_px12, s_ground->soft, 288, "按任意键退出");
}

// One of the plainer setup sheets: a tag, the network's name, two lines.
static void setup_note(const char *title, const char *name, const char *line_1, const char *line_2, const char *hint)
{
    tag(56, title);
    int y = 104;
    if (name[0]) {
        clamp(mid(&font_px24, s_ground->ink, y, name), 24);
        y += 42;
    }
    mid(&font_px12, s_ground->ink, y, line_1);
    mid(&font_px12, s_ground->soft, y + 22, line_2);
    if (hint[0]) mid(&font_px12, s_ground->soft, 288, hint);
}

void ui_overlay_setup(const card_setup_t *setup)
{
    static const char *const WHY[] = { "网络没有回应", "密码不对", "找不到这个网络（只认 2.4G）", "网络没有回应" };
    switch (setup->phase) {
    case CARD_SETUP_WAITING:
        setup_code(setup);
        break;
    case CARD_SETUP_JOINED:
        sheet_open(&G_SETUP);
        tag(40, "手机连上了");
        mid(&font_px12, s_ground->ink, 92, "在手机弹出的页面里");
        mid(&font_px12, s_ground->ink, 112, "选 Wi-Fi，输密码");
        mid(&font_px12, s_ground->soft, 160, "页面没弹出？用浏览器打开");
        number(180, SIZES_36, "192.168.4.1");
        mid(&font_px12, s_ground->soft, 288, "按任意键退出");
        break;
    case CARD_SETUP_TRYING:
        sheet_open(&G_ASK);
        setup_note("正在连接", setup->ssid, "大约 10 秒", "手机可能会短暂断开热点", "");
        break;
    case CARD_SETUP_DONE:
        sheet_open(&G_DONE);
        setup_note("连上了", setup->ssid, "卡记住了这个网络", "热点马上关闭", "");
        break;
    case CARD_SETUP_FAILED:
        sheet_open(&G_NONE);
        setup_note("没连上", setup->ssid, WHY[setup->reason >= 0 && setup->reason < 4 ? setup->reason : 0],
                   "在手机页面上再试一次", "按任意键退出");
        break;
    default:
        sheet_open(&G_SETUP);
        setup_note("手机配网", "", "正在开热点…", "", "");
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
    lv_obj_delete(s_overlay);   // also drops the dots and their animations
    s_overlay = NULL;
}
