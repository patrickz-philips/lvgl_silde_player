/**
 * @file slide_player.c
 * @brief Slide player UI and app-to-LVGL model bridge.
 */

#include "slide_player.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char * TAG = "slide_player_ui";
static const uint32_t GESTURE_DEBOUNCE_US = 180000U;
static const uint32_t RESULT_TIMER_PERIOD_MS = 15U;

static slide_player_model_t s_model;
static QueueHandle_t s_result_queue;
static lv_timer_t * s_result_timer;
static lv_obj_t * s_slide_frame;
static lv_obj_t * s_slide_media;
static uint32_t s_slide_index;
static uint32_t s_target_slide_index;
static uint32_t s_latest_request_id;
static int64_t s_last_switch_us;
static char s_image_path[SLIDE_PLAYER_IMAGE_PATH_MAX_LEN];

static lv_display_t * s_display_with_refresh_callback;
static bool s_display_done_pending;
static int64_t s_display_submit_start_us;
static uint32_t s_display_done_request_id;
static uint32_t s_display_done_slide_index;
static uint64_t s_display_done_sd_cost_us;
static uint32_t s_display_done_sd_bytes;
static uint32_t s_display_done_sd_speed_kib_s;
static size_t s_display_free_before;
static size_t s_display_largest_before;

typedef enum {
    SLIDE_MEDIA_NONE,
    SLIDE_MEDIA_IMAGE,
    SLIDE_MEDIA_GIF,
} slide_media_type_t;

static slide_media_type_t s_slide_media_type;

static bool is_gif_path(const char * path)
{
    const char * extension = strrchr(path, '.');
    return extension != NULL && strcasecmp(extension, ".gif") == 0;
}

static bool create_slide_media(slide_media_type_t media_type)
{
    if (s_slide_media != NULL && s_slide_media_type == media_type) {
        return true;
    }

    if (s_slide_media != NULL) {
        lv_obj_delete(s_slide_media);
    }

    s_slide_media = media_type == SLIDE_MEDIA_GIF
                        ? lv_gif_create(s_slide_frame)
                        : lv_image_create(s_slide_frame);
    if (s_slide_media == NULL) {
        s_slide_media_type = SLIDE_MEDIA_NONE;
        return false;
    }

    s_slide_media_type = media_type;
    lv_obj_add_flag(s_slide_media, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return true;
}

static const char * gesture_direction_name(lv_dir_t direction)
{
    switch (direction) {
        case LV_DIR_LEFT:
            return "LEFT";
        case LV_DIR_RIGHT:
            return "RIGHT";
        case LV_DIR_TOP:
            return "TOP";
        case LV_DIR_BOTTOM:
            return "BOTTOM";
        default:
            return "UNKNOWN";
    }
}

static void on_display_refresh_ready(lv_event_t * event)
{
    if (lv_event_get_code(event) != LV_EVENT_REFR_READY || !s_display_done_pending) {
        return;
    }

    const int64_t total_elapsed_us = esp_timer_get_time() - s_display_submit_start_us;
    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t stack_hwm_bytes =
        (size_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);

    ESP_LOGI(TAG,
             "[%u] Display done slide=%u total=%llu us sd=%llu us bytes=%u speed=%u KiB/s "
             "free=%u(%d) largest=%u(%d) stack_hwm=%u",
             (unsigned int)s_display_done_request_id,
             (unsigned int)(s_display_done_slide_index + 1U),
             (unsigned long long)total_elapsed_us,
             (unsigned long long)s_display_done_sd_cost_us,
             (unsigned int)s_display_done_sd_bytes,
             (unsigned int)s_display_done_sd_speed_kib_s,
             (unsigned int)free_after,
             (int)(free_after - s_display_free_before),
             (unsigned int)largest_after,
             (int)(largest_after - s_display_largest_before),
             (unsigned int)stack_hwm_bytes);

    s_display_done_pending = false;
}

static void apply_load_result(const slide_player_load_result_t * result)
{
    if (result->request_id != s_latest_request_id) {
        ESP_LOGI(TAG, "[%u] Dropped stale result; latest=%u",
                 (unsigned int)result->request_id,
                 (unsigned int)s_latest_request_id);
        return;
    }

    if (!result->success) {
        ESP_LOGW(TAG, "[%u] Keeping slide=%u because loading slide=%u failed",
                 (unsigned int)result->request_id,
                 (unsigned int)(s_slide_index + 1U),
                 (unsigned int)(result->slide_index + 1U));
        s_target_slide_index = s_slide_index;
        return;
    }

    if (s_slide_frame == NULL) {
        return;
    }

    lv_snprintf(s_image_path, sizeof(s_image_path), "%s", result->image_path);
    const slide_media_type_t media_type = is_gif_path(s_image_path)
                                              ? SLIDE_MEDIA_GIF
                                              : SLIDE_MEDIA_IMAGE;
    if (!create_slide_media(media_type)) {
        ESP_LOGE(TAG, "[%u] Failed to create slide media widget",
                 (unsigned int)result->request_id);
        s_target_slide_index = s_slide_index;
        return;
    }

    if (s_display_done_pending) {
        ESP_LOGW(TAG, "[%u] Replacing pending display measurement with request %u",
                 (unsigned int)s_display_done_request_id,
                 (unsigned int)result->request_id);
    }

    s_display_submit_start_us = esp_timer_get_time();
    s_display_free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_display_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s_display_done_request_id = result->request_id;
    s_display_done_slide_index = result->slide_index;
    s_display_done_sd_cost_us = result->elapsed_us;
    s_display_done_sd_bytes = result->bytes_read;
    s_display_done_sd_speed_kib_s = result->speed_kib_s;
    s_display_done_pending = true;

    ESP_LOGI(TAG, "[%u] Display submit slide=%u path=%s",
             (unsigned int)result->request_id,
             (unsigned int)(result->slide_index + 1U),
             s_image_path);

    if (media_type == SLIDE_MEDIA_GIF) {
        lv_gif_set_src(s_slide_media, s_image_path);
    } else {
        lv_image_set_src(s_slide_media, s_image_path);
    }
    lv_obj_center(s_slide_media);
    s_slide_index = result->slide_index;
    s_target_slide_index = result->slide_index;
}

static void result_timer_cb(lv_timer_t * timer)
{
    (void)timer;

    slide_player_load_result_t result;
    while (s_result_queue != NULL && xQueueReceive(s_result_queue, &result, 0U) == pdTRUE) {
        apply_load_result(&result);
    }
}

static bool request_slide(uint32_t slide_index, const char * reason)
{
    const uint32_t request_id = s_latest_request_id + 1U;
    if (!s_model.request_slide(slide_index, request_id, s_model.user_ctx)) {
        ESP_LOGW(TAG, "[%u] Slide request rejected (%s), slide=%u",
                 (unsigned int)request_id, reason,
                 (unsigned int)(slide_index + 1U));
        return false;
    }

    s_latest_request_id = request_id;
    s_target_slide_index = slide_index;
    return true;
}

bool slide_player_show_next(void)
{
    if (s_model.slide_count == 0U) {
        return false;
    }

    return request_slide((s_target_slide_index + 1U) % s_model.slide_count, "next");
}

bool slide_player_show_previous(void)
{
    if (s_model.slide_count == 0U) {
        return false;
    }

    const uint32_t previous_index = s_target_slide_index == 0U
                                        ? s_model.slide_count - 1U
                                        : s_target_slide_index - 1U;
    return request_slide(previous_index, "last");
}

bool slide_player_show_slide(uint32_t slide_number)
{
    if (slide_number == 0U || slide_number > s_model.slide_count) {
        return false;
    }

    return request_slide(slide_number - 1U, "number");
}

static void on_slide_gesture(lv_event_t * event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }

    lv_indev_t * input = lv_indev_active();
    if (input == NULL) {
        ESP_LOGW(TAG, "Gesture event has no active input device");
        return;
    }

    const lv_dir_t direction = lv_indev_get_gesture_dir(input);
    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - s_last_switch_us;
    if (elapsed_us >= 0 && elapsed_us < GESTURE_DEBOUNCE_US) {
        ESP_LOGD(TAG, "Dropped duplicate gesture dir=%s dt=%lld us",
                 gesture_direction_name(direction), (long long)elapsed_us);
        return;
    }

    uint32_t next_index = s_target_slide_index;
    if (direction == LV_DIR_LEFT && next_index + 1U < s_model.slide_count) {
        next_index++;
    } else if (direction == LV_DIR_RIGHT && next_index > 0U) {
        next_index--;
    } else if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) {
        return;
    }

    if (next_index == s_target_slide_index) {
        ESP_LOGI(TAG, "Gesture dir=%s ignored at slide=%u",
                 gesture_direction_name(direction),
                 (unsigned int)(s_target_slide_index + 1U));
        return;
    }

    if (request_slide(next_index, "gesture")) {
        s_last_switch_us = now_us;
    }
}

bool slide_player_post_load_result(const slide_player_load_result_t * result)
{
    if (result == NULL || s_result_queue == NULL) {
        return false;
    }

    return xQueueOverwrite(s_result_queue, result) == pdTRUE;
}

esp_err_t slide_player_ui_init(const slide_player_model_t * model)
{
    if (model == NULL || model->slide_count == 0U || model->request_slide == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_result_queue == NULL) {
        s_result_queue = xQueueCreate(1U, sizeof(slide_player_load_result_t));
        if (s_result_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xQueueReset(s_result_queue);
    }

    if (s_result_timer == NULL) {
        s_result_timer = lv_timer_create(result_timer_cb, RESULT_TIMER_PERIOD_MS, NULL);
        if (s_result_timer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_model = *model;
    s_slide_index = 0U;
    s_target_slide_index = 0U;
    s_latest_request_id = 0U;
    s_last_switch_us = 0;
    s_display_done_pending = false;

    lv_obj_t * screen = lv_screen_active();
    lv_display_t * display = lv_obj_get_display(screen);
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_clean(screen);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (s_display_with_refresh_callback != display) {
        lv_display_add_event_cb(display, on_display_refresh_ready, LV_EVENT_REFR_READY, NULL);
        s_display_with_refresh_callback = display;
    }

    lv_obj_t * frame = lv_obj_create(screen);
    lv_obj_set_size(frame,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_center(frame);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_set_style_border_width(frame, 0, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_add_event_cb(screen, on_slide_gesture, LV_EVENT_GESTURE, NULL);

    s_slide_frame = frame;
    s_slide_media = NULL;
    s_slide_media_type = SLIDE_MEDIA_NONE;
    if (!create_slide_media(SLIDE_MEDIA_IMAGE)) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_center(s_slide_media);

    if (!request_slide(0U, "initial")) {
        return ESP_FAIL;
    }

    lv_timer_ready(s_result_timer);
    return ESP_OK;
}