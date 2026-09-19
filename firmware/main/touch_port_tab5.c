/* touch_port_tab5.c — the M5Stack Tab5's ST7123 touch (the panel's own
 * controller, I2C 0x55; board_tab5.h, docs/boards/m5stack-tab5.md) ->
 * tank_touch_hold / tank_touch_tap, with the same gesture timing as the sim's
 * mouse: press+release < 350 ms with < 24 px displacement = tap (fingertips
 * roll; thresholds are in tank px, 2 panel px each here); held > 300 ms = hold; a drag down from
 * the top edge = feed at that x; every touched frame streams to
 * tank_touch_drag (a moving stroke wipes algae; a horizontal slash through
 * a canopy trims it). Fish taps hit-test 38 px against the press-time fish
 * snapshot AND the current position - fish move during a tap. While the stats
 * card is up, a tap anywhere on empty glass dismisses it (hunting the same
 * fish again to close it was the old, cumbersome way) and does nothing else.
 * Coordinates are mapped from the portrait panel to the landscape tank (the
 * display port's turn, halved: see touch_port_poll). */
#include "touch_port.h"
#include "board_tab5.h"
#include "tank.h"
#include "render.h"
#include "setup.h"
#include "notice.h"
#include "audio_port.h"
#include "progression.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_lcd_panel_io.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include <math.h>

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;
static bool s_down; static int64_t s_press_us; static float s_px, s_py;
static float s_lx, s_ly;                          /* LAST touched position (release classification) */
static float s_fx[N_FISH_MAX], s_fy[N_FISH_MAX];  /* fish positions at press time */
static int s_sel = -1; static int64_t s_sel_us;   /* tapped fish -> stats card */
static bool s_ms;                                 /* milestones page up (its CLOSE button ends it) */
static bool s_cf; static int64_t s_cf_us; static int s_cf_ans;   /* reset confirm prompt */
static bool s_set;                                /* settings page up (CLOSE returns to the milestones page) */
static bool s_shop;                               /* the shop page up (CLOSE returns to the milestones page) */
static bool s_back;                               /* the settings page's CLOSE just brought the milestones page back: that
                                                     release must not reach the page as a tap on ITS CLOSE (same spot) */
static int  s_shop_act;                           /* an UNLOCK / MOVE tapped: the raw tap code, for main (one-shot) */
static int  s_set_what, s_set_val;                /* a segment tapped: SET_TAP_* + value, for main */
#define CONFIRM_TIMEOUT_US (20LL * 1000000)
static bool s_inverted;                           /* screen 180-flipped: mirror into tank space */
/* Fingers land a little BELOW where the eye aims - the pad rolls onto the
 * glass under the fingertip (phones shift their hit targets down for the
 * same reason; Strato saw it on the swatch rows, 2026-09-13). Reported
 * points move UP by this many px in displayed space; director `touch bias
 * <px>` tunes it live. */
static int s_bias_y = 5;                          /* the AMOLED's 10 px (~0.8 mm at 322 ppi) in the Tab5's
                                                     2-panel-px tank pixels (~294 ppi panel) */
void touch_port_set_bias(int px) { s_bias_y = px; }
int  touch_port_bias(void) { return s_bias_y; }

void touch_port_set_inverted(bool inverted) { s_inverted = inverted; }
static bool s_log;                                /* director `touch log on`: every press, native -> tank */
void touch_port_set_log(bool on) { s_log = on; }
/* When the ST7123 is read (the flash hunt, 2026-09-18): polled every
 * frame, the panel flashed white now and then, each flash one stretched
 * panel frame; with no reads, no flashes. So it is read the way its INT line
 * (GPIO 23, low when a report is ready) asks: after an INT edge, and while a
 * finger is down (the lift's report) - an idle tank is never read.
 * Director `touch poll int|always|off` switches it for the bench. */
static int s_poll_mode = TOUCH_POLL_INT;
static volatile bool s_int_pending;
static volatile uint32_t s_int_edges;
static uint32_t s_reads;
static int64_t s_read_until;                      /* keep reading until then: a finger just seen */
void touch_port_set_polling(int mode) { s_poll_mode = mode; s_int_pending = true; }
void touch_port_poll_stats(uint32_t *reads, uint32_t *int_edges) { *reads = s_reads; *int_edges = s_int_edges; }
static void IRAM_ATTR on_tp_int(void *arg) { (void)arg; s_int_pending = true; s_int_edges++; }

/* the ST7123's touch half answers only once the display port has released
 * LCD reset (expander 1 P4), so main calls this after display_port_init */
bool touch_port_init(void) {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    io_cfg.dev_addr = TAB5_ADDR_ST7123_TP; io_cfg.scl_speed_hz = 400000;
    if (!board_i2c_bus() || esp_lcd_new_panel_io_i2c(board_i2c_bus(), &io_cfg, &io) != ESP_OK) { ESP_LOGW(TAG, "no touch io"); return false; }
    esp_lcd_touch_config_t tp_cfg = { .x_max = TAB5_PANEL_W, .y_max = TAB5_PANEL_H, .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 }, .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 } };
    if (esp_lcd_touch_new_i2c_st7123(io, &tp_cfg, &s_tp) != ESP_OK) { ESP_LOGW(TAG, "no ST7123 touch"); return false; }
    const gpio_config_t int_cfg = { .pin_bit_mask = 1ULL << TAB5_TP_INT_GPIO, .mode = GPIO_MODE_INPUT,
                                    .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    esp_err_t e = gpio_config(&int_cfg);
    if (e == ESP_OK) { e = gpio_install_isr_service(0); if (e == ESP_ERR_INVALID_STATE) e = ESP_OK; }   /* already installed: fine */
    if (e == ESP_OK) e = gpio_isr_handler_add(TAB5_TP_INT_GPIO, on_tp_int, NULL);
    if (e != ESP_OK) { s_poll_mode = TOUCH_POLL_ALWAYS; ESP_LOGW(TAG, "touch INT on GPIO %d unavailable (%s): read every frame", TAB5_TP_INT_GPIO, esp_err_to_name(e)); }
    ESP_LOGI(TAG, "ST7123 touch ready, read %s", s_poll_mode == TOUCH_POLL_INT ? "on its INT line (GPIO 23)" : "every frame");
    return true;
}

/* call every frame from the tank task */
void touch_port_poll(tank_t *t) {
    int64_t now = esp_timer_get_time();
    if (s_cf && now - s_cf_us > CONFIRM_TIMEOUT_US) touch_port_confirm_answer(-1);   /* nobody answered: keep the tank */
    if (!s_tp) return;
    esp_lcd_touch_point_data_t pt[1]; uint8_t n = 0;
    bool touched = false;
    bool read = s_poll_mode == TOUCH_POLL_ALWAYS ||
                (s_poll_mode == TOUCH_POLL_INT && (s_int_pending || s_down || now < s_read_until));
    if (read) {
        s_int_pending = false;
        esp_lcd_touch_read_data(s_tp); s_reads++;
        touched = esp_lcd_touch_get_data(s_tp, pt, &n, 1) == ESP_OK && n > 0;
        if (touched) s_read_until = now + 100000;
        static bool first;
        if (!first && s_int_edges) { first = true; ESP_LOGI(TAG, "touch INT seen: reads follow it"); }
    }
    /* native portrait (x 0..719, y 0..1279) -> the view (u across, v down,
     * the SD-card side at the bottom; docs/boards/m5stack-tab5.md): u = 1279-y,
     * v = x -> tank = view / 2. Flipped screen: mirror both, so downstream
     * gestures live in displayed space */
    float vu = touched ? (float)(TAB5_PANEL_H - 1 - pt[0].y) : 0, vv = touched ? (float)pt[0].x : 0;
    float tx = touched ? (s_inverted ? (float)(TANK_W - 1) - vu * 0.5f : vu * 0.5f) : s_lx;
    float ty = touched ? (s_inverted ? (float)(TANK_H - 1) - vv * 0.5f : vv * 0.5f) - s_bias_y : s_ly;
    if (touched && tx < 0) tx = 0;
    if (touched && tx > TANK_W - 1) tx = TANK_W - 1;
    if (touched && ty < 0) ty = 0;
    if (touched && ty > TANK_H - 1) ty = TANK_H - 1;
    if (touched && !s_down) {
        if (s_log) ESP_LOGI(TAG, "press: native %u,%u -> view %.0f,%.0f -> tank %.0f,%.0f (bias %d%s)",
                            pt[0].x, pt[0].y, vu, vv, tx, ty, s_bias_y, s_inverted ? ", flipped" : "");
        audio_port_prewarm();                   /* the release's cue plays warm */
        s_press_us = now; s_px = tx; s_py = ty;
        /* snapshot the school: the user aims at where a fish WAS - by release
           a darting fish has moved and the finger hid it the whole time */
        for (int i = 0; i < t->n_fish && i < N_FISH_MAX; i++) { s_fx[i] = t->fish[i].x; s_fy[i] = t->fish[i].y; }
    }
    bool su = setup_active();                                /* before the touch: BEGIN's release is not a tank tap */
    if (s_set && !s_cf && !su) {                             /* the settings page owns the glass: segments, the seconds wheel, CLOSE */
        int v = 0, r = render_settings_touch(t, tx, ty, touched, &v);
        if (r) ESP_LOGI(TAG, "settings: %s %d", r == SET_TAP_CLOSE ? "CLOSE" : r == SET_TAP_BRIGHT ? "brightness" : r == SET_TAP_VOLUME ? "volume"
                                                  : r == SET_TAP_LIGHT ? "lights out" : "idle seconds", v);
        if (r == SET_TAP_CLOSE) { s_set = false; s_ms = true; s_back = true; }   /* back to the milestones page (2026-09-16); the release is spent */
        else if (r == SET_TAP_BRIGHT || r == SET_TAP_VOLUME || r == SET_TAP_LIGHT || r == SET_TAP_IDLE) { s_set_what = r; s_set_val = v; }
    }
    if (su && !s_cf) {
        bool birth = setup_is_birth(); int who = setup_fish(), place = setup_item();
        setup_touch(t, tx, ty, touched);                     /* taps and the letter wheel, classified in setup.c */
        if (!setup_active()) {
            if (birth) ESP_LOGI(TAG, "birth flow done: %s named and saved", who >= 0 && who < t->n_fish ? t->fish[who].name : "?");
            else if (place >= 0) ESP_LOGI(TAG, "placed: %s at x %.0f, %s layer, saved", SD_ITEMS[place].name, tank_decor_x(t, place),
                                          tank_decor_z(t, place) == DECOR_Z_BACK ? "BEHIND" : tank_decor_z(t, place) == DECOR_Z_FRONT ? "IN FRONT" : "AMONG");
            else ESP_LOGI(TAG, "setup done: %s + %s", t->fish[0].name, t->fish[1].name);
        }
    }
    bool modal = s_ms || s_set || s_shop || s_cf || su;               /* a page or a prompt owns the glass */
    if (touched) { s_lx = tx; s_ly = ty; if (!modal) tank_touch_drag(t, tx, ty); }  /* stroke = wipe/slash */
    if (touched && !modal && now - s_press_us > 300000 && fabsf(ty - s_py) < 30) tank_touch_hold(t, tx, ty);
    if (!touched && s_down) {
        /* release: classify with the LAST touched position (the old code fell
           back to the PRESS position here, so dx/dy were always 0 - every
           quick swipe read as a tap and the drag-feed could never fire) */
        float dx = s_lx - s_px, dy = s_ly - s_py;
        if (s_cf) {                     /* the prompt owns the glass: a press AND release on the
                                           same button answers it, nothing else counts - not
                                           even the tap that opened it (it began before) */
            int h = s_press_us > s_cf_us ? render_confirm_hit(s_px, s_py) : 0;
            if (h && h == render_confirm_hit(s_lx, s_ly)) touch_port_confirm_answer(h);
            goto released;
        }
        if (su) {                       /* the setup had the glass (setup_touch above); just the log:
                                           where the finger landed vs what it hit, in case this panel
                                           reports fingers offset from where they feel */
            ESP_LOGI(TAG, "setup touch press %.0f,%.0f release %.0f,%.0f -> %s", s_px, s_py, s_lx, s_ly,
                     setup_hit_name(setup_active() ? setup_hit(s_px, s_py) : 0));
            s_sel = -1; goto released;
        }
        if (now - s_press_us < 350000 && dx * dx + dy * dy < 24 * 24) {
            if (notice_current()) { notice_dismiss(); ESP_LOGI(TAG, "tap closed the announcement"); goto released; }
            if (s_set || s_back) { s_back = false; goto released; }   /* the settings page had the glass (render_settings_touch above) */
            if (s_shop) {                                           /* the shop: a row's modal, UNLOCK, HOW TO EARN, CLOSE */
                int r = render_shop_tap(t, s_px, s_py);
                ESP_LOGI(TAG, "shop tap at %.0f,%.0f -> %s", s_px, s_py, r == SHOP_TAP_CLOSE ? "CLOSE" : r >= SHOP_TAP_MOVE ? "MOVE" : r >= SHOP_TAP_BUY ? "UNLOCK" : r == SHOP_TAP_KEPT ? "modal" : "nothing");
                if (r == SHOP_TAP_CLOSE) { s_shop = false; render_shop_leave(); s_ms = true; }   /* back to the milestones page (2026-09-16) */
                else if (r >= SHOP_TAP_BUY) s_shop_act = r;   /* main.c buys (and plays the cue) or opens the placement page */
                goto released;
            }
            if (s_ms) {                                             /* the page: badges open a modal, the CLOSE
                                                                       button ends it, the brightness row cycles */
                int r = render_milestones_tap(t, s_px, s_py);     /* CLOSE / SETTINGS / the sand dollar, detail modal, nothing */
                ESP_LOGI(TAG, "page tap at %.0f,%.0f (release %.0f,%.0f) -> %s", s_px, s_py, s_lx, s_ly,
                         r == MS_TAP_CLOSE ? "CLOSE" : r == MS_TAP_SETTINGS ? "SETTINGS" : r == MS_TAP_SHOP ? "SHOP" : r == MS_TAP_KEPT ? "detail" : "nothing");
                if (r != MS_TAP_CLOSE && r != MS_TAP_SETTINGS && r != MS_TAP_SHOP) goto released;   /* only a button leaves the page */
                s_ms = false; s_sel = -1; s_set = r == MS_TAP_SETTINGS; s_shop = r == MS_TAP_SHOP;
                progression_ack_milestones(t); render_milestones_leave();   /* everything shown is now "seen" */
                goto released;
            }
            if (s_sel >= 0 && s_sel != RENDER_CARD_SNAIL && RENDER_CARD_HIT(s_px, s_py)) {   /* a tap ON the card (or the slop
                ESP_LOGI(TAG, "card tap at %.0f,%.0f -> milestones", s_px, s_py);         under its MORE button) = milestones page */
                s_ms = true; goto released;
            }
            /* fish first; only an empty tap reaches the water. 38 px radius
               (a fingertip on this 322 ppi panel covers ~60 px) against BOTH
               the press-time snapshot and the current position - whichever is
               closer - so a fish that moved mid-tap still registers. */
            int best = -1; float bd = 38 * 38;
            for (int i = 0; i < t->n_fish; i++) {
                float ax = s_fx[i] - s_px, ay = s_fy[i] - s_py;
                float bx = t->fish[i].x - s_px, by = t->fish[i].y - s_py;
                float d2a = ax * ax + ay * ay, d2b = bx * bx + by * by;
                float d2 = d2a < d2b ? d2a : d2b;
                if (d2 < bd) { bd = d2; best = i; }
            }
            if (best >= 0) { s_sel = (best == s_sel) ? -1 : best; s_sel_us = now; }
            else if (tank_snail_hit(t, s_px, s_py)) {   /* the snail: its card (2026-09-16), the fish first */
                s_sel = s_sel == RENDER_CARD_SNAIL ? -1 : RENDER_CARD_SNAIL; s_sel_us = now;
                ESP_LOGI(TAG, "snail tapped: card %s (%d spots grazed)", s_sel >= 0 ? "up" : "down", (int)t->snail_grazed); }
            else if (s_sel >= 0) s_sel = -1;   /* card up: a tap on empty glass just
                                                  dismisses it - it is NOT a tank tap
                                                  (no feed, no light-toggle burst) */
            else tank_touch_tap(t, s_px, s_py);
        }
        else if (!s_ms && s_py < 60 && dy >= 40) tank_feed(t, s_lx, 3);  /* drag down from the top = feed */
    }
released:
    s_down = touched;
    if (s_sel >= t->n_fish && s_sel != RENDER_CARD_SNAIL) s_sel = -1;   /* fresh tank / save load */
    if (s_sel >= 0 && now - s_sel_us > 10 * 1000000) s_sel = -1; /* auto-dismiss */
}

int touch_port_selected(void) { return s_sel; }
bool touch_port_milestones(void) { return s_ms; }
void touch_port_show_milestones(bool on) { if (s_ms && !on) render_milestones_leave(); s_ms = on; }
void touch_port_dismiss(void) { s_sel = -1; if (s_ms) render_milestones_leave(); if (s_shop) render_shop_leave(); s_ms = false; s_set = false; s_shop = false; }

/* ---- reset confirm prompt ---- */
void touch_port_confirm_open(void) {
    s_cf = true; s_cf_us = esp_timer_get_time(); s_cf_ans = 0;
    s_sel = -1; s_ms = false; s_set = false; s_shop = false; render_shop_leave();   /* it replaces the card / the pages */
    ESP_LOGI(TAG, "reset prompt up (YES / NO on the glass; NO by itself in %d s)", (int)(CONFIRM_TIMEOUT_US / 1000000));
}
bool touch_port_confirm_answer(int ans) {
    if (!s_cf) return false;
    s_cf = false; s_cf_ans = ans > 0 ? 1 : -1;
    return true;
}
bool  touch_port_confirm_up(void)   { return s_cf; }
float touch_port_confirm_frac(void) {
    if (!s_cf) return 0;
    float f = 1.0f - (esp_timer_get_time() - s_cf_us) / (float)CONFIRM_TIMEOUT_US;
    return f < 0 ? 0 : f;
}
int  touch_port_confirm_take(void)  { int a = s_cf_ans; s_cf_ans = 0; return a; }
bool touch_port_pressed_since(int64_t us) { return s_down && s_press_us > us; }
bool touch_port_settings(void) { return s_set; }
void touch_port_show_settings(bool on) { s_set = on; if (on) { s_ms = false; s_sel = -1; } }
int  touch_port_take_setting(int *value) { int w = s_set_what; *value = s_set_val; s_set_what = 0; return w; }
bool touch_port_shop(void) { return s_shop; }
void touch_port_show_shop(bool on) { if (s_shop && !on) render_shop_leave(); s_shop = on; if (on) { s_ms = false; s_set = false; s_sel = -1; } }
int  touch_port_take_shop(void) { int r = s_shop_act; s_shop_act = 0; return r; }
