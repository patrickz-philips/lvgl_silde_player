/**
 * @file slide_player.h
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLIDE_PLAYER_IMAGE_PATH_MAX_LEN 96U

typedef bool (*slide_player_load_request_cb_t)(uint32_t slide_index, uint32_t request_id, void * user_ctx);

typedef struct {
	uint32_t slide_count;
	slide_player_load_request_cb_t request_slide;
	void * user_ctx;
} slide_player_model_t;

typedef struct {
	uint32_t request_id;
	uint32_t slide_index;
	char image_path[SLIDE_PLAYER_IMAGE_PATH_MAX_LEN];
	bool success;
	int error_no;
	uint32_t bytes_read;
	uint32_t speed_kib_s;
	uint64_t elapsed_us;
} slide_player_load_result_t;

esp_err_t slide_player_ui_init(const slide_player_model_t * model);
bool slide_player_post_load_result(const slide_player_load_result_t * result);
bool slide_player_show_next(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
