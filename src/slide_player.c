/**
 * @file slide_player.c
 * @brief Slide player UI implementation
 */

#include "slide_player.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define SLIDE_PLAYER_TOTAL_SLIDES     32U
#define SLIDE_PLAYER_FIRST_SLIDE_NUM  1U
#define SLIDE_PLAYER_FS_ASSET_DIR     "A:/sdcard"
#define SLIDE_PLAYER_PATH_MAX_LEN     96
#define SLIDE_PLAYER_GESTURE_DEBOUNCE_US  180000LL
#define SLIDE_PLAYER_SD_READ_CHUNK_SIZE   4096U
#define SLIDE_PLAYER_SD_REQ_QUEUE_LEN      1U
#define SLIDE_PLAYER_SD_RES_QUEUE_LEN      1U
#define SLIDE_PLAYER_SD_TASK_STACK_SIZE    4096U
#define SLIDE_PLAYER_SD_TASK_PRIORITY      4U
#define SLIDE_PLAYER_SD_RESULT_POLL_MS     15U
#define SLIDE_PLAYER_SD_READ_PROBE_ENABLE   0U

static const char *TAG = "slide_player";

static lv_obj_t * g_slide_image;
static lv_obj_t * g_slide_index_label;
static uint32_t g_slide_index;
static uint32_t g_target_slide_index;
static uint32_t g_latest_request_id;
static char g_slide_image_path[SLIDE_PLAYER_PATH_MAX_LEN];
static int64_t g_last_switch_us;
static uint32_t g_switch_seq;
static bool g_display_done_pending;
static uint32_t g_display_done_request_id;
static uint32_t g_display_done_slide_index;
static uint64_t g_display_done_sd_cost_us;
static uint32_t g_display_done_sd_bytes;
static uint32_t g_display_done_sd_speed_kib_s;
static int64_t g_display_submit_start_us;
static size_t g_display_free8_before;
static size_t g_display_largest8_before;
static lv_display_t * g_display_with_refr_cb;

typedef struct {
    uint32_t request_id;
    uint32_t slide_index;
    char lvgl_path[SLIDE_PLAYER_PATH_MAX_LEN];
} slide_sd_request_t;

typedef struct {
    uint32_t request_id;
    uint32_t slide_index;
    char lvgl_path[SLIDE_PLAYER_PATH_MAX_LEN];
    bool ok;
    int error_no;
    uint32_t bytes_read;
    uint32_t speed_kib_s;
    uint64_t elapsed_us;
} slide_sd_result_t;

static QueueHandle_t g_sd_request_queue;
static QueueHandle_t g_sd_result_queue;
static TaskHandle_t g_sd_task_handle;
static lv_timer_t * g_sd_result_timer;
static bool g_sd_event_ids_ready;
static uint32_t g_evt_sd_request;
static uint32_t g_evt_sd_ready;

#if SLIDE_PLAYER_SD_READ_PROBE_ENABLE
/* Use static storage to avoid allocating large read buffers on a task stack. */
static uint8_t s_sd_read_probe_chunk[SLIDE_PLAYER_SD_READ_CHUNK_SIZE];
#endif

static uint32_t get_slide_count(void)
{
    return SLIDE_PLAYER_TOTAL_SLIDES;
}

static const char *gesture_dir_to_str(lv_dir_t dir)
{
    switch(dir) {
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

static void update_slide_label(uint32_t slide_index)
{
    if(g_slide_index_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(
        g_slide_index_label,
        "%u/%u",
        (unsigned int)(slide_index + 1U),
        (unsigned int)get_slide_count()
    );
    lv_obj_align(g_slide_index_label, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static bool build_lvgl_slide_path(uint32_t slide_index, char *out_path, size_t out_size)
{
    if(out_path == NULL || out_size == 0U) {
        return false;
    }

    int written = lv_snprintf(
        out_path,
        out_size,
        "%s/%u.png",
        SLIDE_PLAYER_FS_ASSET_DIR,
        (unsigned int)(slide_index + SLIDE_PLAYER_FIRST_SLIDE_NUM)
    );

    return (written > 0) && ((size_t)written < out_size);
}

static bool lvgl_path_to_posix_path(const char *lvgl_path, char *posix_path, size_t posix_path_size)
{
    if(lvgl_path == NULL || posix_path == NULL || posix_path_size == 0U) {
        return false;
    }

    const char *src = lvgl_path;
    if(lvgl_path[0] != '\0' && lvgl_path[1] == ':') {
        src = &lvgl_path[2];
    }

    size_t src_len = strlen(src);
    if((src_len + 1U) > posix_path_size) {
        return false;
    }

    memcpy(posix_path, src, src_len + 1U);
    return true;
}

static bool sd_read_probe_file(
    const char *lvgl_path,
    uint32_t *bytes_read,
    uint64_t *elapsed_us,
    uint32_t *speed_kib_s,
    int *error_no
)
{
    char posix_path[SLIDE_PLAYER_PATH_MAX_LEN];
    if(!lvgl_path_to_posix_path(lvgl_path, posix_path, sizeof(posix_path))) {
        return false;
    }

    if(bytes_read != NULL) {
        *bytes_read = 0U;
    }
    if(elapsed_us != NULL) {
        *elapsed_us = 0U;
    }
    if(speed_kib_s != NULL) {
        *speed_kib_s = 0U;
    }
    if(error_no != NULL) {
        *error_no = 0;
    }

    FILE *fp = fopen(posix_path, "rb");
    if(fp == NULL) {
        if(error_no != NULL) {
            *error_no = errno;
        }
        return false;
    }

    int64_t start_us = esp_timer_get_time();
    size_t total_bytes = 0U;
    bool read_ok = true;
#if SLIDE_PLAYER_SD_READ_PROBE_ENABLE
    while(true) {
        size_t read_len = fread(s_sd_read_probe_chunk, 1U, sizeof(s_sd_read_probe_chunk), fp);
        total_bytes += read_len;
        if(read_len < sizeof(s_sd_read_probe_chunk)) {
            if(feof(fp) != 0) {
                break;
            }

            if(ferror(fp) != 0) {
                read_ok = false;
                if(error_no != NULL) {
                    *error_no = errno;
                }
            }
            break;
        }
    }
#else
    uint8_t probe_header[16];
    size_t head_len = fread(probe_header, 1U, sizeof(probe_header), fp);
    if(head_len == 0U && ferror(fp) != 0) {
        if(error_no != NULL) {
            *error_no = errno;
        }
        fclose(fp);
        return false;
    }

    if(fseek(fp, 0, SEEK_END) != 0) {
        if(error_no != NULL) {
            *error_no = errno;
        }
        fclose(fp);
        return false;
    }

    long file_size = ftell(fp);
    if(file_size < 0) {
        if(error_no != NULL) {
            *error_no = errno;
        }
        fclose(fp);
        return false;
    }

    total_bytes = (size_t)file_size;
#endif

    int64_t elapsed_time_us = esp_timer_get_time() - start_us;
    fclose(fp);

    uint32_t speed = 0U;
#if SLIDE_PLAYER_SD_READ_PROBE_ENABLE
    if(elapsed_time_us > 0) {
        speed = (uint32_t)(((uint64_t)total_bytes * 1000000ULL) / (1024ULL * (uint64_t)elapsed_time_us));
    }
#endif

    if(bytes_read != NULL) {
        *bytes_read = (uint32_t)total_bytes;
    }
    if(elapsed_us != NULL) {
        *elapsed_us = (uint64_t)elapsed_time_us;
    }
    if(speed_kib_s != NULL) {
        *speed_kib_s = speed;
    }

    return read_ok;
}

static void slide_sd_reader_task(void *arg)
{
    (void)arg;

    slide_sd_request_t request;
    while(true) {
        if(xQueueReceive(g_sd_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        slide_sd_result_t result = {
            .request_id = request.request_id,
            .slide_index = request.slide_index,
        };
        memcpy(result.lvgl_path, request.lvgl_path, sizeof(result.lvgl_path));

        result.ok = sd_read_probe_file(
            request.lvgl_path,
            &result.bytes_read,
            &result.elapsed_us,
            &result.speed_kib_s,
            &result.error_no
        );

        if(!result.ok) {
            ESP_LOGW(
                TAG,
                "[%u] SD read failed path=%s errno=%d (%s)",
                (unsigned int)result.request_id,
                result.lvgl_path,
                result.error_no,
                strerror(result.error_no)
            );
        }

        if(xQueueOverwrite(g_sd_result_queue, &result) != pdTRUE) {
            ESP_LOGW(TAG, "[%u] SD result queue overwrite failed", (unsigned int)result.request_id);
        }
    }
}

static void on_display_refr_ready(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_REFR_READY) {
        return;
    }

    if(!g_display_done_pending) {
        return;
    }

    int64_t total_elapsed_us = esp_timer_get_time() - g_display_submit_start_us;
    size_t free8_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest8_after = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    UBaseType_t stack_hwm_words = uxTaskGetStackHighWaterMark(NULL);
    size_t stack_hwm_bytes = (size_t)stack_hwm_words * sizeof(StackType_t);

    ESP_LOGI(
        TAG,
        "[%u] display done(real) slide=%u total=%llu us sd_cost=%llu us bytes=%u speed=%u KiB/s free8=%u(%d) largest8=%u(%d) stack_hwm=%u",
        (unsigned int)g_display_done_request_id,
        (unsigned int)(g_display_done_slide_index + 1U),
        (unsigned long long)total_elapsed_us,
        (unsigned long long)g_display_done_sd_cost_us,
        (unsigned int)g_display_done_sd_bytes,
        (unsigned int)g_display_done_sd_speed_kib_s,
        (unsigned int)free8_after,
        (int)(free8_after - g_display_free8_before),
        (unsigned int)largest8_after,
        (int)(largest8_after - g_display_largest8_before),
        (unsigned int)stack_hwm_bytes
    );

    g_display_done_pending = false;
}

static void apply_loaded_slide(const slide_sd_result_t *result)
{
    if(result == NULL || !result->ok || g_slide_image == NULL) {
        return;
    }

    lv_snprintf(g_slide_image_path, sizeof(g_slide_image_path), "%s", result->lvgl_path);

    if(g_display_done_pending) {
        ESP_LOGW(
            TAG,
            "[%u] Previous display completion log pending. Override with request %u",
            (unsigned int)g_display_done_request_id,
            (unsigned int)result->request_id
        );
    }

    g_display_submit_start_us = esp_timer_get_time();
    g_display_free8_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    g_display_largest8_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    g_display_done_request_id = result->request_id;
    g_display_done_slide_index = result->slide_index;
    g_display_done_sd_cost_us = result->elapsed_us;
    g_display_done_sd_bytes = result->bytes_read;
    g_display_done_sd_speed_kib_s = result->speed_kib_s;
    g_display_done_pending = true;

    UBaseType_t stack_hwm_before_words = uxTaskGetStackHighWaterMark(NULL);
    size_t stack_hwm_before_bytes = (size_t)stack_hwm_before_words * sizeof(StackType_t);
    ESP_LOGI(
        TAG,
        "[%u] display submit slide=%u path=%s free8=%u largest8=%u stack_hwm=%u",
        (unsigned int)result->request_id,
        (unsigned int)(result->slide_index + 1U),
        g_slide_image_path,
        (unsigned int)g_display_free8_before,
        (unsigned int)g_display_largest8_before,
        (unsigned int)stack_hwm_before_bytes
    );

    lv_image_set_src(g_slide_image, g_slide_image_path);
    lv_obj_center(g_slide_image);
    g_slide_index = result->slide_index;

    update_slide_label(g_slide_index);
}

static void show_slide_sync_fallback(uint32_t slide_index)
{
    if(!build_lvgl_slide_path(slide_index, g_slide_image_path, sizeof(g_slide_image_path))) {
        ESP_LOGE(TAG, "Failed to build fallback image path for slide=%u", (unsigned int)(slide_index + 1U));
        return;
    }

    lv_image_set_src(g_slide_image, g_slide_image_path);
    lv_obj_center(g_slide_image);
    g_slide_index = slide_index;
    update_slide_label(g_slide_index);
}

static void on_sd_request_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != (lv_event_code_t)g_evt_sd_request) {
        return;
    }

    const slide_sd_request_t *request = (const slide_sd_request_t *)lv_event_get_param(e);
    if(request == NULL || g_sd_request_queue == NULL) {
        return;
    }

    if(xQueueOverwrite(g_sd_request_queue, request) != pdTRUE) {
        ESP_LOGW(TAG, "[%u] Failed to queue SD request", (unsigned int)request->request_id);
        return;
    }

    ESP_LOGI(
        TAG,
        "[%u] SD request queued slide=%u path=%s",
        (unsigned int)request->request_id,
        (unsigned int)(request->slide_index + 1U),
        request->lvgl_path
    );
}

static void on_sd_ready_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != (lv_event_code_t)g_evt_sd_ready) {
        return;
    }

    const slide_sd_result_t *result = (const slide_sd_result_t *)lv_event_get_param(e);
    if(result == NULL) {
        return;
    }

    if(result->request_id != g_latest_request_id) {
        ESP_LOGI(
            TAG,
            "[%u] Drop stale SD result. latest=%u",
            (unsigned int)result->request_id,
            (unsigned int)g_latest_request_id
        );
        return;
    }

    if(!result->ok) {
        ESP_LOGW(
            TAG,
            "[%u] Keep current slide=%u because SD read failed",
            (unsigned int)result->request_id,
            (unsigned int)(g_slide_index + 1U)
        );
        g_target_slide_index = g_slide_index;
        return;
    }

    apply_loaded_slide(result);
}

static void on_sd_result_timer(lv_timer_t *timer)
{
    (void)timer;
    if(g_sd_result_queue == NULL) {
        return;
    }

    slide_sd_result_t result;
    while(xQueueReceive(g_sd_result_queue, &result, 0U) == pdTRUE) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_send_event(scr, (lv_event_code_t)g_evt_sd_ready, &result);
    }
}

static bool ensure_sd_pipeline_ready(void)
{
    if(!g_sd_event_ids_ready) {
        g_evt_sd_request = lv_event_register_id();
        g_evt_sd_ready = lv_event_register_id();
        g_sd_event_ids_ready = true;
    }

    if(g_sd_request_queue == NULL) {
        g_sd_request_queue = xQueueCreate(SLIDE_PLAYER_SD_REQ_QUEUE_LEN, sizeof(slide_sd_request_t));
        if(g_sd_request_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create SD request queue");
            return false;
        }
    }

    if(g_sd_result_queue == NULL) {
        g_sd_result_queue = xQueueCreate(SLIDE_PLAYER_SD_RES_QUEUE_LEN, sizeof(slide_sd_result_t));
        if(g_sd_result_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create SD result queue");
            return false;
        }
    }

    if(g_sd_task_handle == NULL) {
        BaseType_t task_ok = xTaskCreate(
            slide_sd_reader_task,
            "slide_sd_reader",
            SLIDE_PLAYER_SD_TASK_STACK_SIZE,
            NULL,
            SLIDE_PLAYER_SD_TASK_PRIORITY,
            &g_sd_task_handle
        );
        if(task_ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create SD reader task");
            g_sd_task_handle = NULL;
            return false;
        }
    }

    if(g_sd_result_timer == NULL) {
        g_sd_result_timer = lv_timer_create(on_sd_result_timer, SLIDE_PLAYER_SD_RESULT_POLL_MS, NULL);
        if(g_sd_result_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create SD result timer");
            return false;
        }
    }

    return true;
}

static bool request_slide_load(uint32_t slide_index, const char *reason)
{
    if(!ensure_sd_pipeline_ready()) {
        return false;
    }

    slide_sd_request_t request = {
        .request_id = ++g_switch_seq,
        .slide_index = slide_index,
    };
    if(!build_lvgl_slide_path(slide_index, request.lvgl_path, sizeof(request.lvgl_path))) {
        ESP_LOGE(TAG, "[%u] Build slide path failed for slide=%u", (unsigned int)request.request_id, (unsigned int)(slide_index + 1U));
        return false;
    }

    g_latest_request_id = request.request_id;
    g_target_slide_index = slide_index;

    ESP_LOGI(
        TAG,
        "[%u] LVGL request SD load (%s) slide=%u path=%s",
        (unsigned int)request.request_id,
        reason,
        (unsigned int)(slide_index + 1U),
        request.lvgl_path
    );

    lv_obj_t *scr = lv_screen_active();
    lv_obj_send_event(scr, (lv_event_code_t)g_evt_sd_request, &request);
    return true;
}

static void on_slide_gesture(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_GESTURE) {
        return;
    }

    uint32_t slide_count = get_slide_count();
    if(slide_count == 0U) {
        return;
    }

    lv_indev_t * indev = lv_indev_active();
    if(indev == NULL) {
        ESP_LOGW(TAG, "Gesture event without active input device");
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    int64_t now_us = esp_timer_get_time();
    int64_t dt_us = now_us - g_last_switch_us;

    if(dt_us >= 0 && dt_us < SLIDE_PLAYER_GESTURE_DEBOUNCE_US) {
        ESP_LOGW(
            TAG,
            "Drop duplicated gesture dir=%s dt=%lld us (debounce=%lld us)",
            gesture_dir_to_str(dir),
            (long long)dt_us,
            (long long)SLIDE_PLAYER_GESTURE_DEBOUNCE_US
        );
        return;
    }

    uint32_t prev_index = g_target_slide_index;
    uint32_t next_index = prev_index;

    if(dir == LV_DIR_LEFT) {
        if((next_index + 1U) < slide_count) {
            next_index++;
        }
    }
    else if(dir == LV_DIR_RIGHT) {
        if(next_index > 0U) {
            next_index--;
        }
    }
    else {
        ESP_LOGD(TAG, "Ignore non-horizontal gesture dir=%s", gesture_dir_to_str(dir));
        return;
    }

    if(prev_index == next_index) {
        ESP_LOGI(
            TAG,
            "Gesture dir=%s ignored at boundary index=%u",
            gesture_dir_to_str(dir),
            (unsigned int)(prev_index + 1U)
        );
        return;
    }

    g_last_switch_us = now_us;

    if(!request_slide_load(next_index, "gesture")) {
        ESP_LOGW(TAG, "Fallback to synchronous display update for slide=%u", (unsigned int)(next_index + 1U));
        g_target_slide_index = next_index;
        show_slide_sync_fallback(next_index);
    }
}

void slide_player_ui_init(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_display_t * disp = lv_obj_get_display(scr);
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if(disp != NULL && g_display_with_refr_cb != disp) {
        lv_display_add_event_cb(disp, on_display_refr_ready, LV_EVENT_REFR_READY, NULL);
        g_display_with_refr_cb = disp;
    }

    lv_obj_t * frame = lv_obj_create(scr);
    lv_obj_set_size(frame, WIDGET_SCREEN_WIDTH, WIDGET_SCREEN_HEIGHT);
    lv_obj_center(frame);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_set_style_border_width(frame, 0, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_GESTURE_BUBBLE);

    ensure_sd_pipeline_ready();

    if(g_sd_event_ids_ready) {
        lv_obj_add_event_cb(scr, on_sd_request_event, (lv_event_code_t)g_evt_sd_request, NULL);
        lv_obj_add_event_cb(scr, on_sd_ready_event, (lv_event_code_t)g_evt_sd_ready, NULL);
    }
    lv_obj_add_event_cb(scr, on_slide_gesture, LV_EVENT_GESTURE, NULL);

    g_slide_image = lv_image_create(frame);
    lv_obj_add_flag(g_slide_image, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_center(g_slide_image);

    g_slide_index_label = lv_label_create(scr);
    lv_obj_set_style_text_color(g_slide_index_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_color(g_slide_index_label, lv_color_hex(0x282828), 0);
    lv_obj_set_style_bg_opa(g_slide_index_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(g_slide_index_label, 8, 0);
    lv_obj_set_style_pad_ver(g_slide_index_label, 2, 0);
    lv_obj_set_style_radius(g_slide_index_label, 8, 0);
    lv_obj_add_flag(g_slide_index_label, LV_OBJ_FLAG_GESTURE_BUBBLE);

    g_slide_index = 0U;
    g_target_slide_index = 0U;
    g_latest_request_id = 0U;
    g_last_switch_us = 0;
    g_switch_seq = 0U;
    g_display_done_pending = false;

    if(!request_slide_load(0U, "initial")) {
        ESP_LOGW(TAG, "Initial SD async request failed, use sync fallback");
        show_slide_sync_fallback(0U);
    }
}
