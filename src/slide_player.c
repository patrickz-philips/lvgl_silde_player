/**
 * @file slide_player.c
 * @brief Slide player UI implementation
 */

#include "slide_player.h"

#include "lvgl/lvgl.h"

LV_IMAGE_DECLARE(_1);
LV_IMAGE_DECLARE(_2);
LV_IMAGE_DECLARE(_3);
LV_IMAGE_DECLARE(_4);
LV_IMAGE_DECLARE(_5);
LV_IMAGE_DECLARE(_6);
LV_IMAGE_DECLARE(_7);
LV_IMAGE_DECLARE(_8);
LV_IMAGE_DECLARE(_9);
LV_IMAGE_DECLARE(_10);
LV_IMAGE_DECLARE(_11);
LV_IMAGE_DECLARE(_12);
LV_IMAGE_DECLARE(_13);
LV_IMAGE_DECLARE(_14);
LV_IMAGE_DECLARE(_15);
LV_IMAGE_DECLARE(_16);
LV_IMAGE_DECLARE(_17);
LV_IMAGE_DECLARE(_18);
LV_IMAGE_DECLARE(_19);
LV_IMAGE_DECLARE(_20);
LV_IMAGE_DECLARE(_21);
LV_IMAGE_DECLARE(_22);
LV_IMAGE_DECLARE(_23);
LV_IMAGE_DECLARE(_24);
LV_IMAGE_DECLARE(_25);
LV_IMAGE_DECLARE(_26);
LV_IMAGE_DECLARE(_27);
LV_IMAGE_DECLARE(_28);
LV_IMAGE_DECLARE(_29);
LV_IMAGE_DECLARE(_30);
LV_IMAGE_DECLARE(_31);
LV_IMAGE_DECLARE(_32);

static const lv_image_dsc_t * const g_slide_images[] = {
    &_1,  &_2,  &_3,  &_4,
    &_5,  &_6,  &_7,  &_8,
    &_9,  &_10, &_11, &_12,
    &_13, &_14, &_15, &_16,
    &_17, &_18, &_19, &_20,
    &_21, &_22, &_23, &_24,
    &_25, &_26, &_27, &_28,
    &_29, &_30, &_31, &_32,
};

static lv_obj_t * g_slide_image;
static lv_obj_t * g_slide_index_label;
static uint32_t g_slide_index;

static uint32_t get_slide_count(void)
{
    return (uint32_t)(sizeof(g_slide_images) / sizeof(g_slide_images[0]));
}

static void update_slide_view(void)
{
    uint32_t slide_count = get_slide_count();
    if(slide_count == 0U || g_slide_image == NULL) {
        return;
    }

    lv_image_set_src(g_slide_image, g_slide_images[g_slide_index]);
    lv_obj_center(g_slide_image);

    if(g_slide_index_label != NULL) {
        lv_label_set_text_fmt(
            g_slide_index_label,
            "%u/%u",
            (unsigned int)(g_slide_index + 1U),
            (unsigned int)slide_count
        );
        lv_obj_align(g_slide_index_label, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
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

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if(dir == LV_DIR_LEFT) {
        if((g_slide_index + 1U) < slide_count) {
            g_slide_index++;
            update_slide_view();
        }
    }
    else if(dir == LV_DIR_RIGHT) {
        if(g_slide_index > 0U) {
            g_slide_index--;
            update_slide_view();
        }
    }
}

void slide_player_ui_init(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t * frame = lv_obj_create(scr);
    lv_obj_set_size(frame, WIDGET_SCREEN_WIDTH, WIDGET_SCREEN_HEIGHT);
    lv_obj_center(frame);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_set_style_border_width(frame, 0, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(scr, on_slide_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(frame, on_slide_gesture, LV_EVENT_GESTURE, NULL);

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

    g_slide_index = 0U;
    update_slide_view();
}
