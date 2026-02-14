#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"

#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"

#include "esp_io_expander_tca9554.h"

#include "pin_config.h"
#include "wifi_manager.h"
#include "face_protocol.h"
#include "ws_server.h"
#include "audio.h"

static const char *TAG = "littleAI";

#ifndef FACE_DEVICE_NAME
#define FACE_DEVICE_NAME "littleAI-face"
#endif

// LVGL locking
static SemaphoreHandle_t s_lvgl_mux;

// Face state locking (used by WS server + LVGL task)
static SemaphoreHandle_t s_face_mux;

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;

static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;

static face_state_t s_face;

// LVGL objects
static lv_obj_t *o_left_eye;
static lv_obj_t *o_right_eye;
static lv_obj_t *o_left_pupil;
static lv_obj_t *o_right_pupil;
static lv_obj_t *o_left_lid;
static lv_obj_t *o_right_lid;
static lv_obj_t *o_left_blink;
static lv_obj_t *o_right_blink;
static lv_obj_t *o_mouth_bar;
static lv_obj_t *o_mouth;
static lv_obj_t *o_caption;

// Display params
#define LCD_HOST SPI2_HOST
#define TOUCH_HOST I2C_NUM_0

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL (16)
#endif

// Safer for SPI DMA: keep buffers small enough to fit in internal DMA-capable RAM
#define LVGL_BUF_HEIGHT (LCD_VRES / 8)
#define LVGL_TICK_PERIOD_MS 2

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static bool lvgl_lock(int timeout_ms) {
    const TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, ticks) == pdTRUE;
}

static void lvgl_unlock(void) {
    xSemaphoreGive(s_lvgl_mux);
}

static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

#if LCD_BIT_PER_PIXEL == 24
    // Convert LVGL's 32-bit color to RGB888 for panel
    uint8_t *to = (uint8_t *)color_map;
    uint8_t temp;
    uint16_t pixel_num = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);

    temp = color_map[0].ch.blue;
    *to++ = color_map[0].ch.red;
    *to++ = color_map[0].ch.green;
    *to++ = temp;
    for (int i = 1; i < pixel_num; i++) {
        *to++ = color_map[i].ch.red;
        *to++ = color_map[i].ch.green;
        *to++ = color_map[i].ch.blue;
    }
#endif

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    uint16_t x, y;
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(tp);
    bool pressed = esp_lcd_touch_get_coordinates(tp, &x, &y, NULL, &cnt, 1);
    if (pressed && cnt > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_tick(void *arg) {
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void create_face_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    int cx = LCD_HRES / 2;
    int cy = LCD_VRES / 2 - 20;

    // Black background + rounded-rectangle eyes (darkish blue)
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const lv_color_t eye_fill = lv_color_hex(0x0B1F4A);   // very dark blue
    const lv_color_t eye_edge = lv_color_hex(0x1D4ED8);   // blue edge
    const lv_color_t accent   = lv_color_hex(0x60A5FA);   // lighter blue accent

    int eye_w = 120;
    int eye_h = 80;
    int eye_r = 20; // corner radius
    int pupil_r = 14;
    int eye_dx = 100;

    o_left_eye = lv_obj_create(scr);
    lv_obj_clear_flag(o_left_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o_left_eye, eye_w, eye_h);
    lv_obj_set_style_radius(o_left_eye, eye_r, 0);
    lv_obj_set_style_bg_color(o_left_eye, eye_fill, 0);
    lv_obj_set_style_bg_opa(o_left_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o_left_eye, 3, 0);
    lv_obj_set_style_border_color(o_left_eye, eye_edge, 0);
    lv_obj_set_pos(o_left_eye, cx - eye_dx - eye_w / 2, cy - eye_h / 2);

    o_right_eye = lv_obj_create(scr);
    lv_obj_clear_flag(o_right_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o_right_eye, eye_w, eye_h);
    lv_obj_set_style_radius(o_right_eye, eye_r, 0);
    lv_obj_set_style_bg_color(o_right_eye, eye_fill, 0);
    lv_obj_set_style_bg_opa(o_right_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o_right_eye, 3, 0);
    lv_obj_set_style_border_color(o_right_eye, eye_edge, 0);
    lv_obj_set_pos(o_right_eye, cx + eye_dx - eye_w / 2, cy - eye_h / 2);

    o_left_pupil = lv_obj_create(scr);
    lv_obj_clear_flag(o_left_pupil, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o_left_pupil, pupil_r * 2, pupil_r * 2);
    lv_obj_set_style_radius(o_left_pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o_left_pupil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o_left_pupil, LV_OPA_COVER, 0);
    // a subtle ring so the pupil reads against very dark blue
    lv_obj_set_style_border_width(o_left_pupil, 2, 0);
    lv_obj_set_style_border_color(o_left_pupil, accent, 0);

    o_right_pupil = lv_obj_create(scr);
    lv_obj_clear_flag(o_right_pupil, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o_right_pupil, pupil_r * 2, pupil_r * 2);
    lv_obj_set_style_radius(o_right_pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o_right_pupil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o_right_pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o_right_pupil, 2, 0);
    lv_obj_set_style_border_color(o_right_pupil, accent, 0);

    // Lids (for blink): black overlays that hide the eyes (background is black)
    o_left_lid = lv_obj_create(scr);
    lv_obj_clear_flag(o_left_lid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o_left_lid, eye_w + 6, eye_h + 6);
    lv_obj_set_style_radius(o_left_lid, eye_r, 0);
    lv_obj_set_style_bg_color(o_left_lid, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o_left_lid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o_left_lid, 0, 0);
    lv_obj_set_pos(o_left_lid, cx - eye_dx - eye_w / 2 - 3, cy - eye_h / 2 - 3);
    lv_obj_add_flag(o_left_lid, LV_OBJ_FLAG_HIDDEN);

    o_right_lid = lv_obj_create(scr);
    lv_obj_clear_flag(o_right_lid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o_right_lid, eye_w + 6, eye_h + 6);
    lv_obj_set_style_radius(o_right_lid, eye_r, 0);
    lv_obj_set_style_bg_color(o_right_lid, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o_right_lid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o_right_lid, 0, 0);
    lv_obj_set_pos(o_right_lid, cx + eye_dx - eye_w / 2 - 3, cy - eye_h / 2 - 3);
    lv_obj_add_flag(o_right_lid, LV_OBJ_FLAG_HIDDEN);

    // Blink lines: blue bars to show a "closed eye"
    o_left_blink = lv_obj_create(scr);
    lv_obj_clear_flag(o_left_blink, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o_left_blink, eye_edge, 0);
    lv_obj_set_style_bg_opa(o_left_blink, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o_left_blink, 3, 0);
    lv_obj_set_style_border_width(o_left_blink, 0, 0);
    lv_obj_set_size(o_left_blink, eye_w - 16, 6);
    lv_obj_set_pos(o_left_blink, cx - eye_dx - eye_w / 2 + 8, cy - 3);
    lv_obj_add_flag(o_left_blink, LV_OBJ_FLAG_HIDDEN);

    o_right_blink = lv_obj_create(scr);
    lv_obj_clear_flag(o_right_blink, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o_right_blink, eye_edge, 0);
    lv_obj_set_style_bg_opa(o_right_blink, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o_right_blink, 3, 0);
    lv_obj_set_style_border_width(o_right_blink, 0, 0);
    lv_obj_set_size(o_right_blink, eye_w - 16, 6);
    lv_obj_set_pos(o_right_blink, cx + eye_dx - eye_w / 2 + 8, cy - 3);
    lv_obj_add_flag(o_right_blink, LV_OBJ_FLAG_HIDDEN);

    // Mouth (simple): a thick bar for resting/angry, and a label for glyph expressions.
    o_mouth_bar = lv_obj_create(scr);
    lv_obj_clear_flag(o_mouth_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o_mouth_bar, eye_edge, 0);
    lv_obj_set_style_bg_opa(o_mouth_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o_mouth_bar, 6, 0);
    lv_obj_set_style_border_width(o_mouth_bar, 0, 0);
    lv_obj_set_size(o_mouth_bar, 80, 10);
    lv_obj_align(o_mouth_bar, LV_ALIGN_CENTER, 0, 120);

    o_mouth = lv_label_create(scr);
    lv_obj_set_style_text_color(o_mouth, lv_color_white(), 0);
    lv_label_set_text(o_mouth, "");
    lv_obj_set_style_text_font(o_mouth, &lv_font_montserrat_16, 0);
    lv_obj_align(o_mouth, LV_ALIGN_CENTER, 0, 105);
    lv_obj_add_flag(o_mouth, LV_OBJ_FLAG_HIDDEN);

    o_caption = lv_label_create(scr);
    lv_obj_set_width(o_caption, LCD_HRES - 20);
    lv_label_set_long_mode(o_caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(o_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(o_caption, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(o_caption, lv_color_white(), 0);
    // subtle dark backdrop (background is black)
    lv_obj_set_style_bg_color(o_caption, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o_caption, LV_OPA_60, 0);
    lv_obj_set_style_radius(o_caption, 8, 0);
    lv_obj_set_style_pad_all(o_caption, 6, 0);
    lv_label_set_text(o_caption, "");
    lv_obj_align(o_caption, LV_ALIGN_TOP_MID, 0, 10);

    // initial pupil positions (centered within each eye)
    lv_obj_set_pos(o_left_pupil, cx - eye_dx - pupil_r, cy - pupil_r);
    lv_obj_set_pos(o_right_pupil, cx + eye_dx - pupil_r, cy - pupil_r);
}

static void apply_face_state(uint32_t now_ms) {
    if (!s_face_mux) return;
    if (xSemaphoreTake(s_face_mux, pdMS_TO_TICKS(10)) != pdTRUE) return;

    // Geometry constants (keep in sync with create_face_ui())
    const int eye_w = 120;
    const int eye_r_base = 20;
    const int eye_dx = 100;
    const int pupil_r = 14;

    int cx = LCD_HRES / 2;
    int cy = LCD_VRES / 2 - 20;

    // Eye openness: either expression-driven (default) or sticky override via WS.
    // Range: 0.0 (closed/squint) .. 1.0 (wide open)
    float open01;
    if (s_face.eye_open_override) {
        open01 = s_face.eye_open;
    } else {
        switch (s_face.expression) {
            case EXPR_SURPRISED: open01 = 1.0f; break;
            case EXPR_HAPPY: open01 = 0.85f; break;
            case EXPR_SAD: open01 = 0.55f; break;
            case EXPR_THINKING: open01 = 0.35f; break;
            case EXPR_ANGRY: open01 = 0.25f; break;
            case EXPR_SLEEPING: open01 = 0.05f; break;
            default: open01 = 0.80f; break;
        }
    }
    if (open01 < 0.0f) open01 = 0.0f;
    if (open01 > 1.0f) open01 = 1.0f;

    // Blink overrides (also treat sleeping as essentially "closed")
    bool blink_active = (s_face.blink_until_ms && now_ms < s_face.blink_until_ms) || (s_face.expression == EXPR_SLEEPING);

    const int eye_h_min = 18;
    const int eye_h_max = 96;
    int eye_h = eye_h_min + (int)((eye_h_max - eye_h_min) * open01);

    int eye_r = eye_r_base;
    if (eye_r > eye_h / 2) eye_r = eye_h / 2;

    int left_eye_x = cx - eye_dx - eye_w / 2;
    int left_eye_y = cy - eye_h / 2;
    int right_eye_x = cx + eye_dx - eye_w / 2;
    int right_eye_y = cy - eye_h / 2;

    // Apply eye shape (lets us "squint" by changing height)
    lv_obj_set_pos(o_left_eye, left_eye_x, left_eye_y);
    lv_obj_set_size(o_left_eye, eye_w, eye_h);
    lv_obj_set_style_radius(o_left_eye, eye_r, 0);

    lv_obj_set_pos(o_right_eye, right_eye_x, right_eye_y);
    lv_obj_set_size(o_right_eye, eye_w, eye_h);
    lv_obj_set_style_radius(o_right_eye, eye_r, 0);

    // Update lids + blink lines to match eye geometry
    lv_obj_set_pos(o_left_lid, left_eye_x - 3, left_eye_y - 3);
    lv_obj_set_size(o_left_lid, eye_w + 6, eye_h + 6);
    lv_obj_set_style_radius(o_left_lid, eye_r, 0);

    lv_obj_set_pos(o_right_lid, right_eye_x - 3, right_eye_y - 3);
    lv_obj_set_size(o_right_lid, eye_w + 6, eye_h + 6);
    lv_obj_set_style_radius(o_right_lid, eye_r, 0);

    lv_obj_set_pos(o_left_blink, left_eye_x + 8, left_eye_y + eye_h / 2 - 3);
    lv_obj_set_size(o_left_blink, eye_w - 16, 6);

    lv_obj_set_pos(o_right_blink, right_eye_x + 8, right_eye_y + eye_h / 2 - 3);
    lv_obj_set_size(o_right_blink, eye_w - 16, 6);

    // gaze -> pupil offset (clamp to stay inside the eye)
    int max_px = (eye_w / 2) - pupil_r - 8;
    int max_py = (eye_h / 2) - pupil_r - 6;
    if (max_py < 0) max_py = 0;

    int px = (int)(s_face.gaze_x * (float)max_px);
    int py = (int)(s_face.gaze_y * (float)max_py);
    if (px < -max_px) px = -max_px;
    if (px > max_px) px = max_px;
    if (py < -max_py) py = -max_py;
    if (py > max_py) py = max_py;

    bool pupils_visible = !blink_active && (eye_h >= (pupil_r * 2 + 10));
    if (pupils_visible) {
        lv_obj_clear_flag(o_left_pupil, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(o_right_pupil, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_pos(o_left_pupil,
                       left_eye_x + eye_w / 2 - pupil_r + px,
                       left_eye_y + eye_h / 2 - pupil_r + py);

        lv_obj_set_pos(o_right_pupil,
                       right_eye_x + eye_w / 2 - pupil_r + px,
                       right_eye_y + eye_h / 2 - pupil_r + py);
    } else {
        lv_obj_add_flag(o_left_pupil, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o_right_pupil, LV_OBJ_FLAG_HIDDEN);
    }

    // caption ttl
    if (s_face.caption_until_ms && now_ms > s_face.caption_until_ms) {
        s_face.caption[0] = 0;
        s_face.caption_until_ms = 0;
    }
    lv_label_set_text(o_caption, s_face.caption);

    // viseme ttl
    if (s_face.viseme_until_ms && now_ms > s_face.viseme_until_ms) {
        strcpy(s_face.viseme, "rest");
        s_face.viseme_weight = 0.0f;
        s_face.viseme_until_ms = 0;
    }

    // blink
    if (blink_active) {
        lv_obj_clear_flag(o_left_lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(o_right_lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(o_left_blink, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(o_right_blink, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o_left_lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o_right_lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o_left_blink, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o_right_blink, LV_OBJ_FLAG_HIDDEN);
        s_face.blink_until_ms = 0;
    }

    // mouth expression
    bool use_label = false;
    const char *label = "";

    switch (s_face.expression) {
        case EXPR_HAPPY: use_label = true; label = ")"; break;
        case EXPR_SAD: use_label = true; label = "("; break;
        // Surprised: render as a big opened mouth using the same mouth bar (not a tiny "O" glyph)
        case EXPR_THINKING: use_label = true; label = "..."; break;
        case EXPR_SLEEPING: use_label = true; label = "z"; break;
        default: break;
    }

    // If the rig is explicitly driving mouth openness, always render the mouth bar.
    if (s_face.mouth_open_override) {
        use_label = false;
    }

    if (use_label) {
        lv_obj_add_flag(o_mouth_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(o_mouth, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(o_mouth, label);
    } else {
        lv_obj_clear_flag(o_mouth_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o_mouth, LV_OBJ_FLAG_HIDDEN);

        // Configure the bar: closed mouth (filled) vs open mouth (filled, taller).
        if (s_face.expression == EXPR_ANGRY) {
            lv_obj_set_size(o_mouth_bar, 90, 12);
            lv_obj_align(o_mouth_bar, LV_ALIGN_CENTER, 0, 120);
            lv_obj_set_style_radius(o_mouth_bar, 2, 0);
            lv_obj_set_style_bg_opa(o_mouth_bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(o_mouth_bar, 0, 0);
        } else {
            // Mouth openness: either rig-driven (sticky override) or viseme/expression-driven.
            float mopen01 = 0.0f;
            if (s_face.mouth_open_override) {
                mopen01 = s_face.mouth_open;
            } else {
                if (s_face.expression == EXPR_SURPRISED) {
                    mopen01 = 1.0f;
                } else if (strcmp(s_face.viseme, "rest") != 0 && s_face.viseme_weight > 0.1f) {
                    mopen01 = s_face.viseme_weight;
                }
            }
            if (mopen01 < 0.0f) mopen01 = 0.0f;
            if (mopen01 > 1.0f) mopen01 = 1.0f;

            if (mopen01 > 0.05f) {
                // Make it look like the same thick line, just "opening" vertically.
                int mw = (s_face.expression == EXPR_SURPRISED) ? 120 : 96;
                int mh = 10 + (int)(mopen01 * 54.0f); // 10..64-ish
                if (mh > 72) mh = 72;

                lv_obj_set_size(o_mouth_bar, mw, mh);
                lv_obj_align(o_mouth_bar, LV_ALIGN_CENTER, 0, 120);
                lv_obj_set_style_radius(o_mouth_bar, mh / 2, 0);
                lv_obj_set_style_bg_opa(o_mouth_bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(o_mouth_bar, 0, 0);
            } else {
                // Resting mouth: thick filled bar
                lv_obj_set_size(o_mouth_bar, 96, 10);
                lv_obj_align(o_mouth_bar, LV_ALIGN_CENTER, 0, 120);
                lv_obj_set_style_radius(o_mouth_bar, 6, 0);
                lv_obj_set_style_bg_opa(o_mouth_bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(o_mouth_bar, 0, 0);
            }
        }
    }

    xSemaphoreGive(s_face_mux);
}

static void lvgl_task(void *arg) {
    while (1) {
        if (lvgl_lock(50)) {
            lv_timer_handler();
            apply_face_state((uint32_t)(esp_timer_get_time() / 1000));
            lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void i2c_scan_bus(i2c_port_t port) {
    ESP_LOGI(TAG, "I2C scan on port %d...", (int)port);
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", addr);
        }
    }
}

static void maybe_init_io_expander(i2c_port_t port) {
    // Waveshare reference design uses a TCA9554 IO expander to enable power rails.
    // On some boards it's required for the touch controller to respond on I2C.
    esp_io_expander_handle_t io_expander = NULL;
    esp_err_t err = esp_io_expander_new_i2c_tca9554(port, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No TCA9554 IO expander at addr 000 (0x20): %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "TCA9554 IO expander present; toggling pins 0/1/2");
    esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0, 0);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 0);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0, 1);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 1);
    esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2, 1);
}

static void init_display_and_lvgl(void) {
    // NOTE: esp_lcd QSPI init needs the panel IO callback user_ctx set at creation time,
    // and the FT5x06 touch driver expects an I2C "panel io" handle (not an I2C port number).

    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    static lv_indev_drv_t indev_drv;

    // -----------------
    // I2C (touch + optional IO expander)
    // -----------------
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0));

    // Helpful when bringing up new boards
    i2c_scan_bus(TOUCH_HOST);
    maybe_init_io_expander(TOUCH_HOST);
    i2c_scan_bus(TOUCH_HOST);

    // -----------------
    // LCD (QSPI / SH8601)
    // -----------------
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        LCD_SCLK,
        LCD_SDIO0,
        LCD_SDIO1,
        LCD_SDIO2,
        LCD_SDIO3,
        LCD_HRES * LCD_VRES * LCD_BIT_PER_PIXEL / 8);

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(LCD_CS, notify_flush_ready, &disp_drv);
    io_cfg.pclk_hz = 80 * 1000 * 1000;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_cfg,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Color invert: leave OFF by default; turning it on can make the whole UI look like a photo negative.
    {
        esp_err_t e = esp_lcd_panel_invert_color(s_panel, false);
        if (e != ESP_OK && e != ESP_ERR_NOT_SUPPORTED) {
            ESP_ERROR_CHECK(e);
        }
    }

    // Explicitly set orientation.
    // NOTE: The SH8601 component we use may not support swap_xy; ignore NOT_SUPPORTED.
    {
        esp_err_t e;
        e = esp_lcd_panel_swap_xy(s_panel, false);
        if (e != ESP_OK && e != ESP_ERR_NOT_SUPPORTED) {
            ESP_ERROR_CHECK(e);
        }
        // Mirror settings: we previously mirrored X to compensate for an orientation issue,
        // but that makes text render backwards. Prefer no mirror by default.
        e = esp_lcd_panel_mirror(s_panel, false, false);
        if (e != ESP_OK && e != ESP_ERR_NOT_SUPPORTED) {
            ESP_ERROR_CHECK(e);
        }
    }

    // Brightness (0..255)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, 0x51, (uint8_t[]){0xFF}, 1));

    // -----------------
    // TOUCH (I2C / FT3168 via FT5x06 driver)
    // -----------------
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_HRES,
        .y_max = LCD_VRES,
        .rst_gpio_num = -1,
        .int_gpio_num = TP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch));

    // -----------------
    // LVGL
    // -----------------
    lv_init();
    s_lvgl_mux = xSemaphoreCreateMutex();

    // draw buffers in internal DMA-capable memory (QSPI/SPI DMA can't reliably read PSRAM)
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LCD_HRES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(LCD_HRES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_HRES * LVGL_BUF_HEIGHT);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_HRES;
    disp_drv.ver_res = LCD_VRES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = s_panel;

    s_disp = lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = s_touch;
    s_indev = lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // UI
    if (lvgl_lock(1000)) {
        create_face_ui();
        lvgl_unlock();
    }

    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 2, NULL);
}

void app_main(void) {
    // Reduce noisy touch I2C error logs (we'll add our own if needed)
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("FT5x06", ESP_LOG_NONE);

    ESP_LOGI(TAG, "%s boot", FACE_DEVICE_NAME);
    face_state_init(&s_face);
    s_face_mux = xSemaphoreCreateMutex();

    init_display_and_lvgl();

    // Audio (ES8311 + speaker)
    // Uses the same I2C bus as touch (I2C_NUM_0).
    audio_config_t acfg = {
        .sample_rate_hz = 16000,
        .volume_percent = 75,
    };
    esp_err_t ae = audio_init(&acfg);
    if (ae == ESP_OK) {
        audio_beep(880, 120);
        audio_beep(1320, 120);
    } else {
        ESP_LOGW(TAG, "audio_init failed: %s", esp_err_to_name(ae));
    }

    // Wi-Fi manager: auto-connect or start AP portal
    wifi_manager_start();

    // WebSocket control plane
    ws_server_config_t ws_cfg = {
        .face = &s_face,
        .face_mutex = s_face_mux,
    };
    ESP_ERROR_CHECK(ws_server_start(&ws_cfg));
    ESP_LOGI(TAG, "WS: ws://<device-ip>:8080/ws");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
