/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "expression_emote.h"

#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "emote_defs.h"
#include "emote_table.h"
#include "emote_layout.h"
#include "widget/gfx_font_lvgl.h"

static const char *TAG = "Expression_init";

static void emote_flush_cb_wrapper(gfx_disp_t *disp, int x1, int y1, int x2, int y2, const void *data)
{
    emote_handle_t self = (emote_handle_t)gfx_disp_get_user_data(disp);
    if (self) {
        /* The gfx core may flush a display that gfx_disp_add() has not yet
         * returned, so handle->gfx_disp can still be NULL here.  Publish it
         * from the display itself; emote_notify_flush_finished() needs it to
         * release the render task's WAIT_FLUSH_DONE wait. */
        if (self->gfx_disp == NULL) {
            self->gfx_disp = disp;
        }
        if (self->flush_cb) {
            self->flush_cb(x1, y1, x2, y2, data, self);
        }
    }
}

static void emote_update_cb_wrapper(gfx_disp_t *disp, gfx_disp_event_t event, const void *obj)
{
    emote_handle_t self = (emote_handle_t)gfx_disp_get_user_data(disp);
    if (!self) {
        return;
    }

    // Check if emergency dialog animation is done
    if (obj == self->def_objects[EMOTE_DEF_OBJ_ANIM_EMERG_DLG].obj &&
            event == GFX_DISP_EVENT_ALL_FRAME_DONE) {
        if (self->emerg_dlg_done_sem) {
            xSemaphoreGive(self->emerg_dlg_done_sem);
        }
    }

    if (self && self->update_cb) {
        self->update_cb(event, obj, self);
    }
}

emote_handle_t emote_init(const emote_config_t *config)
{
    esp_err_t ret = ESP_OK;
    emote_handle_t handle = NULL;
    gfx_obj_t *obj_default = NULL;

    ESP_GOTO_ON_FALSE(config, ESP_ERR_INVALID_ARG, error, TAG, "config is NULL");

    // Allocate handle
    handle = (emote_handle_t)calloc(1, sizeof(struct emote_s));
    ESP_GOTO_ON_FALSE(handle, ESP_ERR_NO_MEM, error, TAG, "Failed to allocate emote manager handle");

    memset(handle, 0, sizeof(struct emote_s));

    // Initialize bat_percent
    handle->bat_percent = -1;
    handle->flush_cb = config->flush_cb;
    handle->update_cb = config->update_cb;

    handle->h_res = config->gfx_emote.h_res;
    handle->v_res = config->gfx_emote.v_res;
    handle->user_data = config->user_data;

    gfx_core_config_t gfx_cfg = {
        .fps = config->gfx_emote.fps,
        .task = {
            .task_priority = config->task.task_priority,
            .task_stack = config->task.task_stack,
            .task_affinity = config->task.task_affinity,
            .task_stack_caps = config->task.task_stack_in_ext ?
            (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT),
        }
    };

    handle->gfx_handle = gfx_emote_init(&gfx_cfg);
    ESP_GOTO_ON_FALSE(handle->gfx_handle, ESP_ERR_INVALID_STATE, error, TAG, "Failed to initialize emote_gfx");

    /* Add default display */
    gfx_disp_config_t disp_cfg = {
        .h_res = config->gfx_emote.h_res,
        .v_res = config->gfx_emote.v_res,
        .flush_cb = emote_flush_cb_wrapper,
        .update_cb = emote_update_cb_wrapper,
        .user_data = handle,
        .flags = {
            .swap = config->flags.swap,
            .buff_dma = config->flags.buff_dma,
            .buff_spiram = config->flags.buff_spiram,
            .double_buffer = config->flags.double_buffer
        },
        .buffers = { .buf1 = NULL, .buf2 = NULL, .buf_pixels = config->buffers.buf_pixels },
    };
    handle->gfx_disp = gfx_disp_add(handle->gfx_handle, &disp_cfg);
    ESP_GOTO_ON_FALSE(handle->gfx_disp != NULL, ESP_FAIL, error, TAG, "Failed to add display");

    /* The gfx render task starts as soon as the display is added and can
     * complete its first flush before the rest of init finishes.  Mark the
     * handle initialized now, otherwise emote_notify_flush_finished() rejects
     * that first completion and gfx_render blocks forever on WAIT_FLUSH_DONE. */
    handle->is_initialized = true;

    // Default set
    gfx_emote_lock(handle->gfx_handle);
    gfx_disp_set_bg_color(handle->gfx_disp, GFX_COLOR_HEX(EMOTE_DEF_BG_COLOR));

    obj_default = emote_create_obj_by_name(handle, EMT_DEF_ELEM_DEFAULT_LABEL);
    ESP_GOTO_ON_FALSE(obj_default, ESP_ERR_INVALID_STATE, error_unlock, TAG, "Failed to create default label");
    gfx_obj_set_size(obj_default, handle->h_res, EMOTE_DEF_LABEL_HEIGHT);

    gfx_emote_unlock(handle->gfx_handle);
    ESP_LOGI(TAG, "Create default label: [%p]", obj_default);
    (void)ret;  // ret is used by ESP_GOTO_ON_FALSE macro but not returned by this function
    return handle;

error_unlock:
    if (handle && handle->gfx_handle) {
        gfx_emote_unlock(handle->gfx_handle);
    }

error:
    if (handle) {
        if (handle->gfx_handle) {
            gfx_emote_deinit(handle->gfx_handle);
            handle->gfx_handle = NULL;
        }
        free(handle);
    }
    return NULL;
}

bool emote_deinit(emote_handle_t handle)
{
    if (!handle) {
        return false;
    }

    if (!handle->is_initialized) {
        return true;
    }

    // Unload assets (this will cleanup hash tables, fonts, objects, custom objects created by load, etc.)
    emote_unload_assets(handle);

    // Unmount assets
    emote_unmount_assets(handle);

    // Cleanup default label
    gfx_obj_t *obj_default = handle->def_objects[EMOTE_DEF_OBJ_LEBAL_DEFAULT].obj;
    if (obj_default) {
        gfx_obj_delete(obj_default);
        handle->def_objects[EMOTE_DEF_OBJ_LEBAL_DEFAULT].obj = NULL;
    }

    // Deinit engine
    if (handle->gfx_handle) {
        gfx_emote_deinit(handle->gfx_handle);
        handle->gfx_handle = NULL;
    }

    handle->is_initialized = false;

    // Free handle memory
    free(handle);
    return true;
}

bool emote_is_initialized(emote_handle_t handle)
{
    return handle && handle->is_initialized;
}

esp_err_t emote_wait_emerg_dlg_done(emote_handle_t handle, uint32_t timeout_ms)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->emerg_dlg_done_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (xSemaphoreTake(handle->emerg_dlg_done_sem, timeout_ticks) == pdTRUE) {
        return ESP_OK;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}

void *emote_get_user_data(emote_handle_t handle)
{
    return handle ? handle->user_data : NULL;
}
