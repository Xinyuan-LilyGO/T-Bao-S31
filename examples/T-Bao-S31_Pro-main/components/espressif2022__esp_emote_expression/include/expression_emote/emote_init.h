/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===== OPAQUE HANDLE =====
typedef struct emote_s *emote_handle_t;

// ===== CONFIGURATION STRUCTURES =====
typedef enum {
    EMOTE_SOURCE_PATH = 0,
    EMOTE_SOURCE_PARTITION
} emote_source_type_t;

typedef struct {
    emote_source_type_t type;
    union {
        const char *path;
        const char *partition_label;
    } source;

    struct {
        uint8_t mmap_enable: 1;
    } flags;
} emote_data_t;

// ===== FLUSH READY CALLBACK =====
typedef void (*emote_flush_ready_cb_t)(int x_start, int y_start, int x_end, int y_end, const void *data, emote_handle_t manager);
typedef void (*emote_update_cb_t)(gfx_disp_event_t event, const void *obj, emote_handle_t manager);

typedef struct {
    struct {
        bool swap;
        bool double_buffer;
        bool buff_dma;
        bool buff_spiram;
    } flags;
    struct {
        int h_res;
        int v_res;
        int fps;
    } gfx_emote;
    struct {
        size_t buf_pixels;
    } buffers;
    struct {
        int task_priority;
        int task_stack;
        int task_affinity;
        bool task_stack_in_ext;
    } task;
    emote_flush_ready_cb_t flush_cb;  // Flush ready callback (can be NULL)
    emote_update_cb_t update_cb;      // Update callback (can be NULL)
    void *user_data;                  // User data (can be NULL)
} emote_config_t;

// ===== API FUNCTIONS =====

/**
 * @brief Initialize the emote manager
 * @param config Configuration structure
 * @return Handle to emote manager on success, NULL on failure
 */
emote_handle_t emote_init(const emote_config_t *config);

/**
 * @brief Deinitialize and cleanup the emote manager
 * @param handle Handle to emote manager
 * @return true on success, false on failure
 */
bool emote_deinit(emote_handle_t handle);

/**
 * @brief Check if manager is initialized
 * @param handle Handle to emote manager
 * @return true if initialized, false otherwise
 */
bool emote_is_initialized(emote_handle_t handle);


#ifdef __cplusplus
}
#endif
