#pragma once
// gauge_ui.h  —  LVGL v8.3.x  —  Prado cluster
//
// Two screens:
//   1) GAUGE  — minimal "STEALTH" speedo: a thick full ring that fills with
//      speed (full at 100) and a big centred number (capped at 99 -> 2 digits).
//   2) DIAG   — the page "below" the gauge. Swipe UP on the gauge to reveal it,
//      swipe DOWN to go back. Shows live GPS output (speed / fix rate / sats /
//      HDOP), a DEMO toggle, and a GPS INFO button.
//
// The UI never talks to the speed driver directly; the .ino registers two
// callbacks (gps-status, demo-toggle) and pushes live values via gauge_ui_set_diag().
//
// lv_conf.h needs: LV_FONT_MONTSERRAT_14/20/22, LV_USE_ARC, LV_USE_SWITCH,
// LV_USE_BTN, LV_USE_FLEX (all on by default).

#include "lvgl.h"
#include "vehicle_data.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#define SPEED_RING_MAX  100   // ring is full at this speed
#define SPEED_SHOW_MAX  99    // number never shows more than this (2 digits)

// --- anti-jitter display tuning (Layer 3) -----------------------------------
// DISPLAY_DEADBAND: the smoothed speed must move at least this many mph from the
//   number currently shown before we change it -> kills flip-flop at rounding
//   boundaries (e.g. 39<->40). DISPLAY_REFRESH_MS: how often the readout is even
//   allowed to repaint (~8 Hz) regardless of how fast we tick.
#define DISPLAY_DEADBAND   0.7f   // mph
#define DISPLAY_REFRESH_MS 120    // ~8 Hz repaint cap

// With GPS there is a state the VSS never had: powered up but no fix yet, where
// a plain "0" would be a lie. The number is dimmed to the caption grey until the
// module has a fix — no extra widgets, no layout change. Set to 0 to disable.
#define GAUGE_DIM_ON_NO_FIX 1

// Big DIN-style speedo digits (Bahnschrift, 235px, glyphs 0-9). C linkage.
#ifdef __cplusplus
extern "C" {
#endif
extern const lv_font_t speed_font_lg;
#ifdef __cplusplus
}
#endif

// ----------------------------------------------------------- STEALTH palette --
#define ST_BG   lv_color_hex(0x000000)
#define ST_NUM  lv_color_hex(0xF2F2F2)
#define ST_TRK  lv_color_hex(0x1A1A1A)   // dim ring track
#define ST_IND  lv_color_hex(0xEDEDED)   // bright ring indicator
#define ST_CAP  lv_color_hex(0x595959)   // captions
#define ST_BTN  lv_color_hex(0x262626)   // button face

// --------------------------------------------------------------- shared state -
static lv_obj_t  *g_scr_gauge = NULL;   // page 1
static lv_obj_t  *g_scr_diag  = NULL;   // page 2 (below)
static lv_obj_t  *g_num;                // big speed number
static lv_obj_t  *g_arc;                // ring
static lv_coord_t g_d;                  // short edge of the display
static uint32_t   g_boot_until = 0;     // hold live arc value until intro ends
static uint32_t   g_toast_until = 0;
static uint32_t   g_disp_next   = 0;    // next tick the readout may repaint (Layer 3)
static int        g_shown       = 0;    // integer mph currently on screen

// diag widgets (same four value rows as before, now carrying GPS values)
static lv_obj_t  *d_speed, *d_hz, *d_sats, *d_hdop, *d_demo_sw, *d_status;
static bool       g_dimmed = false;     // tracks the no-fix number colour

// app callbacks (set from the .ino)
typedef void (*gauge_gps_cb_t)(void);
typedef void (*gauge_demo_cb_t)(bool);
static gauge_gps_cb_t  g_gps_cb  = NULL;
static gauge_demo_cb_t g_demo_cb = NULL;
inline void gauge_ui_set_gps_cb(gauge_gps_cb_t cb)   { g_gps_cb  = cb; }
inline void gauge_ui_set_demo_cb(gauge_demo_cb_t cb) { g_demo_cb = cb; }

// --------------------------------------------------------------- helpers ------
static inline int ui_spd(const VehicleData &d) {
  int s = (int)(d.speed_mph + 0.5f);
  return s < 0 ? 0 : s;
}

static void ui_sweep_arc_cb(void *o, int32_t v) { lv_arc_set_value((lv_obj_t *)o, v); }

// 0 -> full -> 0 power-on sweep so the ring "wakes up".
static void ui_intro(void *obj, lv_anim_exec_xcb_t cb) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, 0, SPEED_RING_MAX);
  lv_anim_set_time(&a, 650);
  lv_anim_set_playback_time(&a, 500);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
  g_boot_until = lv_tick_get() + 1250;
}

static lv_obj_t *ui_rect(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t c, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  return o;
}

static lv_obj_t *ui_caption(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_letter_space(l, 4, 0);
  lv_label_set_text(l, txt);
  return l;
}

static lv_obj_t *ui_diag_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

// --------------------------------------------------------------- events -------
static void diag_gps_event(lv_event_t *e)  { if (g_gps_cb)  g_gps_cb(); }
static void diag_demo_event(lv_event_t *e) {
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  if (g_demo_cb) g_demo_cb(on);
}

// --------------------------------------------------------------- builders -----
static void build_gauge(lv_obj_t *p) {
  const lv_coord_t d = g_d;
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p, ST_BG, 0);

  // thick full ring, outer edge at the glass
  g_arc = lv_arc_create(p);
  lv_obj_set_size(g_arc, d, d);
  lv_obj_set_style_pad_all(g_arc, 0, LV_PART_MAIN);
  lv_obj_center(g_arc);
  lv_arc_set_rotation(g_arc, 270);
  lv_arc_set_bg_angles(g_arc, 0, 360);
  lv_arc_set_range(g_arc, 0, SPEED_RING_MAX);
  lv_arc_set_value(g_arc, 0);
  lv_obj_remove_style(g_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(g_arc, d / 22, LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_arc, ST_TRK, LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_arc, d / 22, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_arc, ST_IND, LV_PART_INDICATOR);

  // fine reference tick at 12 o'clock
  lv_obj_t *tk = ui_rect(p, 2, d / 18, ST_CAP, LV_OPA_COVER);
  lv_obj_align(tk, LV_ALIGN_TOP_MID, 0, d * 6 / 100);

  g_num = lv_label_create(p);
  lv_obj_set_style_text_font(g_num, &speed_font_lg, 0);
  lv_obj_set_style_text_color(g_num, ST_NUM, 0);
  lv_label_set_text(g_num, "0");
  lv_obj_align(g_num, LV_ALIGN_CENTER, 0, -d * 2 / 100);

  lv_obj_t *u = ui_caption(p, "mph", &lv_font_montserrat_22, ST_CAP);
  lv_obj_set_style_text_letter_space(u, 1, 0);
  lv_obj_align(u, LV_ALIGN_BOTTOM_MID, 0, -d * 13 / 100);
}

static void build_diag(lv_obj_t *p) {
  const lv_coord_t d = g_d;
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(p, ST_BG, 0);

  // centred vertical stack (kept inside the round glass)
  lv_obj_t *col = lv_obj_create(p);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, d * 74 / 100, d * 90 / 100);
  lv_obj_center(col);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, d * 2 / 100, 0);

  lv_obj_t *title = lv_label_create(col);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, ST_NUM, 0);
  lv_label_set_text(title, "DIAGNOSTICS");

  d_speed = ui_diag_label(col, &lv_font_montserrat_20, ST_NUM);
  d_hz    = ui_diag_label(col, &lv_font_montserrat_14, ST_CAP);
  d_sats  = ui_diag_label(col, &lv_font_montserrat_14, ST_CAP);
  d_hdop  = ui_diag_label(col, &lv_font_montserrat_14, ST_CAP);

  // DEMO toggle row
  lv_obj_t *row = lv_obj_create(col);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, d * 4 / 100, 0);
  lv_obj_t *dl = lv_label_create(row);
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(dl, ST_CAP, 0);
  lv_label_set_text(dl, "DEMO");
  d_demo_sw = lv_switch_create(row);
  lv_obj_set_size(d_demo_sw, d * 20 / 100, d * 10 / 100);   // bigger toggle
  lv_obj_add_event_cb(d_demo_sw, diag_demo_event, LV_EVENT_VALUE_CHANGED, NULL);

  // GPS INFO button — same geometry as the old CALIBRATE button; GPS needs no
  // calibration, so tapping it reports link/fix state instead.
  lv_obj_t *btn = lv_btn_create(col);
  lv_obj_set_size(btn, d * 52 / 100, d * 15 / 100);
  lv_obj_set_style_bg_color(btn, ST_BTN, 0);
  lv_obj_set_style_radius(btn, d * 4 / 100, 0);
  lv_obj_add_event_cb(btn, diag_gps_event, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(btn);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(bl, ST_NUM, 0);
  lv_label_set_text(bl, "GPS INFO");
  lv_obj_center(bl);

  // status line, directly below the button (auto-clears)
  d_status = lv_label_create(col);
  lv_obj_set_style_text_font(d_status, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(d_status, ST_NUM, 0);
  lv_label_set_text(d_status, "");
}

// -------------------------------------------------------------- public API ----
inline void gauge_ui_init() {
  g_d = LV_MIN(lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));

  g_scr_gauge = lv_obj_create(NULL);
  g_scr_diag  = lv_obj_create(NULL);
  build_gauge(g_scr_gauge);
  build_diag(g_scr_diag);

  lv_scr_load(g_scr_gauge);     // start on the gauge
  ui_intro(g_arc, ui_sweep_arc_cb);
}

// Page navigation (the diag page sits "below" the gauge).
inline void gauge_ui_show_diag(void) {
  if (lv_scr_act() != g_scr_diag)
    lv_scr_load_anim(g_scr_diag, LV_SCR_LOAD_ANIM_MOVE_TOP, 250, 0, false);
}
inline void gauge_ui_show_gauge(void) {
  if (lv_scr_act() != g_scr_gauge)
    lv_scr_load_anim(g_scr_gauge, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 250, 0, false);
}

// Brief status message shown below the GPS INFO button (auto-clears).
inline void gauge_ui_toast(const char *msg) {
  if (!d_status) return;
  lv_label_set_text(d_status, msg);
  g_toast_until = lv_tick_get() + 1800;
}

inline void gauge_ui_toast_fmt(const char *fmt, ...) {
  char buf[32];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  gauge_ui_toast(buf);
}

// Push live values to the diag page (cheap; safe to call every tick).
// hdop_x10 is HDOP * 10 as an integer: LVGL's printf has no float support
// unless LV_SPRINTF_USE_FLOAT is enabled, so tenths are formatted by hand.
inline void gauge_ui_set_diag(float mph, int fix_hz, int sats, int hdop_x10,
                              bool demo, bool fix, bool holding) {
  if (!d_speed) return;
  lv_label_set_text_fmt(d_speed, "%d mph", (int)(mph + 0.5f));
  lv_label_set_text_fmt(d_hz,    "fix  %d Hz", fix_hz);
  lv_label_set_text_fmt(d_sats,  "sats  %d", sats);
  if      (fix)     lv_label_set_text_fmt(d_hdop, "hdop  %d.%d", hdop_x10 / 10, hdop_x10 % 10);
  else if (holding) lv_label_set_text(d_hdop, "holding");
  else              lv_label_set_text(d_hdop, "no fix");
  if (demo) lv_obj_add_state(d_demo_sw, LV_STATE_CHECKED);
  else      lv_obj_clear_state(d_demo_sw, LV_STATE_CHECKED);
}

inline void gauge_ui_update(const VehicleData &d, bool has_fix = true) {
  const uint32_t now = lv_tick_get();

  // Toast auto-clear runs every call (independent of the display refresh cap).
  if (g_toast_until && now > g_toast_until) {
    if (d_status) lv_label_set_text(d_status, "");
    g_toast_until = 0;
  }

#if GAUGE_DIM_ON_NO_FIX
  // "0" with no fix is a lie, so grey the number until the module has one.
  // Only touched on a state change, so it costs nothing on a normal tick.
  const bool want_dim = !has_fix;
  if (want_dim != g_dimmed) {
    g_dimmed = want_dim;
    lv_obj_set_style_text_color(g_num, g_dimmed ? ST_CAP : ST_NUM, 0);
  }
#else
  (void)has_fix;
#endif

  // Layer 3a: cap the repaint rate (~DISPLAY_REFRESH_MS), so we don't redraw on
  // every tick. Filtering still runs upstream at the full tick rate.
  if (now < g_disp_next) return;
  g_disp_next = now + DISPLAY_REFRESH_MS;

  const float smoothed = d.speed_mph < 0.0f ? 0.0f : d.speed_mph;

  // Layer 3b: hysteresis — only move the shown number once the smoothed value has
  // drifted at least DISPLAY_DEADBAND from it (kills 39<->40 boundary flicker).
  if (fabsf(smoothed - (float)g_shown) >= DISPLAY_DEADBAND)
    g_shown = (int)lroundf(smoothed);

  const int show = g_shown > SPEED_SHOW_MAX ? SPEED_SHOW_MAX : g_shown;  // cap at 99
  const int ring = g_shown > SPEED_RING_MAX ? SPEED_RING_MAX : g_shown;  // full at 100
  lv_label_set_text_fmt(g_num, "%d", show);
  if (now > g_boot_until)
    lv_arc_set_value(g_arc, ring);
}
