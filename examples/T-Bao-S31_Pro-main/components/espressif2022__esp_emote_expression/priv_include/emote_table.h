/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "expression_emote.h"
#include "esp_mmap_assets.h"
#include "emote_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===== Hash Table Management =====
/**
 * @brief  Create a new assets hash table
 *
 * @param[in]  name  Name of the hash table (for logging/debugging, can be NULL)
 *
 * @return
 *       - Pointer to hash table  On success
 *       - NULL                  Fail to create hash table
 */
assets_hash_table_t *emote_assets_table_create(const char *name);

/**
 * @brief  Destroy and free assets hash table
 *
 * @param[in]  ht  Hash table to destroy
 */
void emote_assets_table_destroy(assets_hash_table_t *ht);

// ===== Asset Data Acquisition =====
/**
 * @brief  Acquire asset data with caching support
 *
 * @param[in]   handle        Emote handle
 * @param[in]   data_ref      Reference to data
 * @param[in]   size          Size of data
 * @param[out]  output_ptr    Output pointer to store cached data pointer
 *
 * @return
 *       - Pointer to data  On success
 *       - NULL             Fail to acquire data
 */
const void *emote_acquire_data(emote_handle_t handle, const void *data_ref, size_t size, void **output_ptr);

#ifdef __cplusplus
}
#endif
