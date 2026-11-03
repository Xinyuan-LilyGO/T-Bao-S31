#include "page_motion.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_language.h"
#include "app_sfx.h"
#include "actuator_control.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "motion_behavior.h"
#include "motion_manager.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

#define MOTION_REFRESH_MS 100U
#define MOTION_MANUAL_REFRESH_MS 50U

typedef struct {
    float linear;
    float angular;
    const char *label;
} motion_direction_t;

static const motion_direction_t s_forward = { .linear = 0.75f,
                                               .angular = 0.0f,
                                               .label = "前进" };
static const motion_direction_t s_left = { .linear = 0.0f,
                                            .angular = 0.70f,
                                            .label = "左转" };
static const motion_direction_t s_right = { .linear = 0.0f,
                                             .angular = -0.70f,
                                             .label = "右转" };
static const motion_direction_t s_reverse = { .linear = -0.65f,
                                               .angular = 0.0f,
                                               .label = "后退" };

static lv_obj_t *s_status_label;
static lv_obj_t *s_pose_label;
static lv_obj_t *s_output_label;
static lv_obj_t *s_notice_label;
static lv_obj_t *s_servo_label;
static lv_obj_t *s_servo_slider;
static bool s_manual_active;
static bool s_servo_active;
static motion_direction_t s_manual_direction;
static int s_servo_pulse_us;
static uint32_t s_last_manual_ms;
static uint32_t s_last_refresh_ms;

static void set_label_style(lv_obj_t *label, const lv_font_t *font,
                            lv_color_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static const char *motion_state_text(const motion_state_t *state)
{
    if (!state->available) {
        return app_language_text("不可用", "Unavailable");
    }
    if (state->faulted) {
        return app_language_text("故障锁定", "Fault locked");
    }
    if (state->failsafe_active) {
        return app_language_text("保护停车", "Failsafe stop");
    }
    if (state->sequence_active) {
        return app_language_text("动作中", "Action running");
    }
    if (state->current_motor_a_percent != 0 ||
        state->current_motor_b_percent != 0) {
        return app_language_text("运动中", "Moving");
    }
    return app_language_text("就绪", "Ready");
}

static void refresh_motion_state(void)
{
    motion_state_t state;
    motion_manager_get_state(&state);

    const int heading = (int)lroundf(state.pose.theta_rad * 180.0f /
                                     3.14159265358979323846f);
    if (s_status_label != NULL) {
        lv_label_set_text_fmt(s_status_label,
                              app_language_is_english() ? "Motion  %s" : "运动  %s",
                              motion_state_text(&state));
    }
    if (s_pose_label != NULL) {
        lv_label_set_text_fmt(
            s_pose_label,
            app_language_is_english() ? "Position %.1f, %.1f cm   Heading %d deg" :
                                        "位置 %.1f, %.1f cm   朝向 %d°",
            state.pose.x_cm, state.pose.y_cm, heading);
    }
    if (s_output_label != NULL) {
        lv_label_set_text_fmt(
            s_output_label,
            app_language_is_english() ? "Motor A %+d%%   B %+d%%" :
                                        "电机 A %+d%%   B %+d%%",
            state.current_motor_a_percent, state.current_motor_b_percent);
    }
    if (s_notice_label != NULL && state.faulted) {
        lv_label_set_text(s_notice_label,
                          app_language_text("执行器故障，已锁定；重启后检查接线",
                                            "Actuator fault locked; check wiring after restart"));
    }
    if (s_servo_label != NULL) {
        lv_label_set_text_fmt(
            s_servo_label,
            app_language_is_english() ? "Servo %d us" : "舵机 %d us",
            s_servo_pulse_us);
    }
}

static lv_obj_t *create_button(lv_obj_t *root, const char *text,
                               int x, int y, int width, int height,
                               uint32_t color, lv_event_cb_t callback,
                               void *user_data, lv_event_code_t event_code)
{
    lv_obj_t *button = lv_btn_create(root);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    ui_page_style_button(button, color, 0x2A3942, 10);
    lv_obj_add_event_cb(button, callback, event_code, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, app_language_text(text, NULL));
    set_label_style(label, &font_puhui_16_4, lv_color_white());
    lv_obj_center(label);
    return button;
}

static void motion_direction_cb(lv_event_t *event)
{
    const motion_direction_t *direction =
        (const motion_direction_t *)lv_event_get_user_data(event);
    if (direction == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        s_manual_direction = *direction;
        s_manual_active = true;
        s_last_manual_ms = ui_page_manager_now_ms();
        motion_behavior_submit_manual_control(direction->linear,
                                              direction->angular,
                                              s_servo_pulse_us,
                                              s_servo_active);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_manual_active = false;
        motion_behavior_release_manual();
        if (s_servo_active) {
            motion_behavior_submit_manual_servo(s_servo_pulse_us);
        }
    }
}

static void motion_servo_slider_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST) {
        return;
    }

    s_servo_pulse_us = (int)lv_slider_get_value(lv_event_get_target_obj(event));
    s_servo_active = true;
    motion_behavior_submit_manual_control(
        s_manual_active ? s_manual_direction.linear : 0.0f,
        s_manual_active ? s_manual_direction.angular : 0.0f,
        s_servo_pulse_us, true);
    if (s_servo_label != NULL) {
        lv_label_set_text_fmt(
            s_servo_label,
            app_language_is_english() ? "Servo %d us" : "舵机 %d us",
            s_servo_pulse_us);
    }
}

static void motion_stop_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    s_manual_active = false;
    s_servo_active = false;
    motion_behavior_release_manual();
    motion_manager_stop_all();
    if (s_notice_label != NULL) {
        lv_label_set_text(s_notice_label, app_language_text("已停车；松开方向键也会自动停车", "Stopped; releasing the direction key also stops"));
    }
}

static void motion_spin_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    s_manual_active = false;
    s_servo_active = false;
    motion_behavior_release_manual();
    motion_manager_stop_all();
    if (motion_behavior_start_named(MOTION_BEHAVIOR_ACTION_SPIN_360) != ESP_OK) {
        if (s_notice_label != NULL) {
            lv_label_set_text(s_notice_label, app_language_text("运动不可用或动作被拒绝", "Motion unavailable or action rejected"));
        }
        return;
    }
    if (s_notice_label != NULL) {
        lv_label_set_text(s_notice_label, app_language_text("原地转圈约 3 秒；请确认车在桌面中央", "Spins for about 3 seconds; keep the car centered"));
    }
}

static void motion_reset_pose_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    s_manual_active = false;
    s_servo_active = false;
    motion_behavior_release_manual();
    motion_manager_stop_all();
    motion_manager_reset_pose();
    if (s_notice_label != NULL) {
        lv_label_set_text(s_notice_label, app_language_text("请先把车实际放回桌面中心，再使用此坐标", "Place the car at the desk center before using this pose"));
    }
    refresh_motion_state();
}

esp_err_t page_motion_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }

    s_status_label = NULL;
    s_pose_label = NULL;
    s_output_label = NULL;
    s_notice_label = NULL;
    s_servo_label = NULL;
    s_servo_slider = NULL;
    s_manual_active = false;
    s_servo_active = false;
    s_servo_pulse_us = ACTUATOR_SERVO_PULSE_CENTER_US;
    s_last_manual_ms = 0;
    s_last_refresh_ms = 0;

    ui_page_style_root(root);
    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_motion, "移动", "手动驾驶");

    s_status_label = lv_label_create(root);
    set_label_style(s_status_label, &font_puhui_16_4,
                    lv_color_hex(0x65D6FF));
    lv_obj_set_width(s_status_label, LV_HOR_RES - 70);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 55);

    s_pose_label = lv_label_create(root);
    set_label_style(s_pose_label, &font_puhui_16_4,
                    lv_color_hex(0xD5D9E0));
    lv_obj_set_width(s_pose_label, LV_HOR_RES - 16);
    lv_obj_align(s_pose_label, LV_ALIGN_TOP_MID, 0, 74);

    s_output_label = lv_label_create(root);
    set_label_style(s_output_label, &font_puhui_16_4,
                    lv_color_hex(0xA7ADB8));
    lv_obj_set_width(s_output_label, LV_HOR_RES - 16);
    lv_obj_align(s_output_label, LV_ALIGN_TOP_MID, 0, 93);

    create_button(root, s_forward.label, 120, 116, 80, 36, 0x2094FA,
                  motion_direction_cb, (void *)&s_forward, LV_EVENT_ALL);
    create_button(root, s_left.label, 30, 158, 80, 36, 0x34C759,
                  motion_direction_cb, (void *)&s_left, LV_EVENT_ALL);
    create_button(root, "急停", 120, 158, 80, 36, 0xE5484D,
                  motion_stop_cb, NULL, LV_EVENT_CLICKED);
    create_button(root, s_right.label, 210, 158, 80, 36, 0x34C759,
                  motion_direction_cb, (void *)&s_right, LV_EVENT_ALL);
    create_button(root, s_reverse.label, 120, 200, 80, 36, 0x2094FA,
                  motion_direction_cb, (void *)&s_reverse, LV_EVENT_ALL);

    create_button(root, "转一圈", 45, 244, 105, 34, 0xAF52DE,
                  motion_spin_cb, NULL, LV_EVENT_CLICKED);
    create_button(root, "回中心", 170, 244, 105, 34, 0x5A6575,
                  motion_reset_pose_cb, NULL, LV_EVENT_CLICKED);

    s_servo_label = lv_label_create(root);
    set_label_style(s_servo_label, &font_puhui_16_4,
                    lv_color_hex(0xD5D9E0));
    lv_obj_set_width(s_servo_label, LV_HOR_RES - 90);
    lv_obj_align(s_servo_label, LV_ALIGN_TOP_MID, 0, 282);

    s_servo_slider = lv_slider_create(root);
    lv_obj_set_size(s_servo_slider, LV_HOR_RES - 90, 16);
    lv_obj_align(s_servo_slider, LV_ALIGN_TOP_MID, 0, 306);
    lv_slider_set_range(s_servo_slider, ACTUATOR_SERVO_PULSE_MIN_US,
                        ACTUATOR_SERVO_PULSE_MAX_US);
    lv_slider_set_value(s_servo_slider, s_servo_pulse_us, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_servo_slider, lv_color_hex(0x30383E),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_servo_slider, lv_color_hex(0xFFB26B),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_servo_slider, lv_color_hex(0xFFF1E5),
                              LV_PART_KNOB);
    lv_obj_add_event_cb(s_servo_slider, motion_servo_slider_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_servo_slider, motion_servo_slider_cb,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_servo_slider, motion_servo_slider_cb,
                        LV_EVENT_PRESS_LOST, NULL);

    s_notice_label = lv_label_create(root);
    set_label_style(s_notice_label, &font_puhui_16_4,
                    lv_color_hex(0x8E8E93));
    lv_obj_set_width(s_notice_label, LV_HOR_RES - 20);
    lv_label_set_long_mode(s_notice_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_notice_label, LV_ALIGN_TOP_MID, 0, 338);
    lv_label_set_text(s_notice_label, app_language_text("按住方向键移动 · 松开自动停车 · 滑动控制舵机", "Hold a direction key to move; release to stop · Slide to control the servo"));

    refresh_motion_state();
    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_motion_on_exit(void)
{
    s_manual_active = false;
    s_servo_active = false;
    motion_behavior_release_manual();
    motion_manager_stop_all();
    s_status_label = NULL;
    s_pose_label = NULL;
    s_output_label = NULL;
    s_notice_label = NULL;
    s_servo_label = NULL;
    s_servo_slider = NULL;
    return ESP_OK;
}

void page_motion_on_tick(uint32_t ms)
{
    if (s_manual_active && ms - s_last_manual_ms >= MOTION_MANUAL_REFRESH_MS) {
        s_last_manual_ms = ms;
        motion_behavior_submit_manual_control(
            s_manual_direction.linear, s_manual_direction.angular,
            s_servo_pulse_us, s_servo_active);
    } else if (s_servo_active &&
               ms - s_last_manual_ms >= MOTION_MANUAL_REFRESH_MS) {
        s_last_manual_ms = ms;
        motion_behavior_submit_manual_servo(s_servo_pulse_us);
    }
    if (ms - s_last_refresh_ms >= MOTION_REFRESH_MS) {
        s_last_refresh_ms = ms;
        refresh_motion_state();
    }
}

const ui_page_t page_motion = {
    .id = PAGE_MOTION,
    .name = "motion",
    .on_enter = page_motion_on_enter,
    .on_exit = page_motion_on_exit,
    .on_tick = page_motion_on_tick,
};
