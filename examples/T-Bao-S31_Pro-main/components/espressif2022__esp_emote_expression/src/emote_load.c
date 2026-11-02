/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "soc/soc_memory_layout.h"
#include "soc/ext_mem_defs.h"
#include <string.h>
#include <stdlib.h>

#include "expression_emote.h"

#include "emote_defs.h"
#include "emote_table.h"
#include "emote_layout.h"
#include "gfx.h"
#include "cJSON.h"

static const char *TAG = "Expression_load";

// Hash table implementation
#define ASSETS_HASH_TABLE_SIZE CONFIG_EMOTE_ASSETS_HASH_TABLE_SIZE

typedef struct assets_hash_entry_s {
    char *key;
    void *value;
    struct assets_hash_entry_s *next;
} assets_hash_entry_t;

struct assets_hash_table_s {
    char *name;
    assets_hash_entry_t *buckets[ASSETS_HASH_TABLE_SIZE];
};

static uint32_t emote_assets_hash_string(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

assets_hash_table_t *emote_assets_table_create(const char *name)
{
    assets_hash_table_t *ht = (assets_hash_table_t *)calloc(1, sizeof(assets_hash_table_t));
    if (ht && name) {
        ht->name = strdup(name);
        if (!ht->name) {
            free(ht);
            return NULL;
        }
    }
    return ht;
}

void emote_assets_table_destroy(assets_hash_table_t *ht)
{
    if (!ht) {
        return;
    }

    for (int i = 0; i < ASSETS_HASH_TABLE_SIZE; i++) {
        assets_hash_entry_t *entry = ht->buckets[i];
        while (entry) {
            assets_hash_entry_t *next = entry->next;
            free(entry->key);
            // Free the value if it's a dynamically allocated structure
            if (entry->value) {
                free(entry->value);
            }
            free(entry);
            entry = next;
        }
    }
    if (ht->name) {
        free(ht->name);
    }
    free(ht);
}

static esp_err_t emote_assets_table_set(assets_hash_table_t *ht, const char *key, void *value)
{
    esp_err_t ret = ESP_OK;
    assets_hash_entry_t *entry = NULL;

    ESP_GOTO_ON_FALSE(ht && key, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    uint32_t hash = emote_assets_hash_string(key) % ASSETS_HASH_TABLE_SIZE;

    entry = ht->buckets[hash];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            entry->value = value;
            return ESP_OK;
        }
        entry = entry->next;
    }

    entry = (assets_hash_entry_t *)malloc(sizeof(assets_hash_entry_t));
    ESP_GOTO_ON_FALSE(entry, ESP_ERR_NO_MEM, error, TAG, "Failed to allocate hash entry");

    entry->key = strdup(key);
    ESP_GOTO_ON_FALSE(entry->key, ESP_ERR_NO_MEM, error_free_entry, TAG, "Failed to duplicate key");

    entry->value = value;
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    return ESP_OK;

error_free_entry:
    free(entry);

error:
    return ret;
}

static void *emote_assets_table_get(assets_hash_table_t *ht, const char *key)
{
    if (!ht || !key) {
        return NULL;
    }

    uint32_t hash = emote_assets_hash_string(key) % ASSETS_HASH_TABLE_SIZE;

    assets_hash_entry_t *entry = ht->buckets[hash];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

const void *emote_acquire_data(emote_handle_t handle, const void *data_ref, size_t size, void **output_ptr)
{
    if (!handle) {
        return NULL;
    }

    mmap_assets_handle_t asset_handle = handle->assets_handle;
    bool is_DBUS = false;
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    is_DBUS = ((size_t)data_ref >= SOC_MMU_FLASH_VADDR_BASE);
#else
    is_DBUS = ((size_t)data_ref >= SOC_MMU_DBUS_VADDR_BASE);
#endif
    if (is_DBUS || asset_handle == NULL) {
        if (output_ptr && *output_ptr) {
            free(*output_ptr);
            *output_ptr = NULL;
        }
        return data_ref;
    }

    if (output_ptr && *output_ptr) {
        free(*output_ptr);
        *output_ptr = NULL;
    }

    uint8_t *buffer = (uint8_t *)malloc(size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory: %zu bytes", size);
        return NULL;
    }

    mmap_assets_copy_mem(asset_handle, (size_t)data_ref, buffer, size);

    if (output_ptr) {
        *output_ptr = buffer;
    }

    return buffer;
}

esp_err_t emote_get_asset_data_by_name(emote_handle_t handle, const char *name,
                                       const uint8_t **data, size_t *size)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(name && data && size && handle && handle->assets_handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    mmap_assets_handle_t asset_handle = handle->assets_handle;

    int fileNum = mmap_assets_get_stored_files(asset_handle);
    for (int i = 0; i < fileNum; i++) {
        const char *file_name = mmap_assets_get_name(asset_handle, i);
        if (file_name && strcmp(file_name, name) == 0) {
            const uint8_t *file_data = mmap_assets_get_mem(asset_handle, i);
            size_t file_size = mmap_assets_get_size(asset_handle, i);
            if (file_data && file_size > 0) {
                *data = file_data;
                *size = file_size;
                return ESP_OK;
            }
        }
    }

    ret = ESP_ERR_NOT_FOUND;
    ESP_LOGE(TAG, "Asset file not found: %s", name);

error:
    return ret;
}

static esp_err_t emote_find_data_by_key(emote_handle_t handle, assets_hash_table_t *ht, const char *key, void **result)
{
    if (!handle || !ht || !key || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = emote_assets_table_get(ht, key);
    return *result ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t emote_unmount_assets(emote_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->assets_handle) {
        ESP_LOGI(TAG, "Unmounting assets handle");
        mmap_assets_del(handle->assets_handle);
        handle->assets_handle = NULL;
    }

    return ESP_OK;
}

esp_err_t emote_mount_assets(emote_handle_t handle, const emote_data_t *data)
{
    esp_err_t ret = ESP_OK;
    mmap_assets_config_t asset_config;
    int num = 0;

    ESP_GOTO_ON_FALSE(handle && data, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    // Unmount existing assets first
    ret = emote_unmount_assets(handle);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to unmount existing assets");

    memset(&asset_config, 0, sizeof(asset_config));

    if (data->type == EMOTE_SOURCE_PATH) {
        ESP_LOGI(TAG, "Loading assets from file: path=%s", data->source.path);
        asset_config.partition_label = data->source.path;
        asset_config.flags.use_fs = true;
        asset_config.flags.full_check = true;
    } else if (data->type == EMOTE_SOURCE_PARTITION) {
        ESP_LOGI(TAG, "Loading assets from partition: label=%s", data->source.partition_label);
        asset_config.partition_label = data->source.partition_label;
        asset_config.flags.mmap_enable = data->flags.mmap_enable;
        asset_config.flags.full_check = true;
    } else {
        ret = ESP_ERR_INVALID_ARG;
        ESP_LOGE(TAG, "Unknown source type");
        goto error;
    }

    ret = mmap_assets_new(&asset_config, &handle->assets_handle);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to create mmap assets: %s", esp_err_to_name(ret));

    num = mmap_assets_get_stored_files(handle->assets_handle);
    ESP_GOTO_ON_FALSE(num > 0, ESP_ERR_NOT_FOUND, error_cleanup, TAG, "No files found in assets");

    for (int i = 0; i < num; i++) {
        const char *name = mmap_assets_get_name(handle->assets_handle, i);
        ESP_LOGD(TAG, "Found file: %d, %s", i, name);
    }

    return ESP_OK;

error_cleanup:
    emote_unmount_assets(handle);

error:
    return ret;
}

static esp_err_t emote_load_emojis(emote_handle_t handle, cJSON *root)
{
    esp_err_t ret = ESP_OK;
    cJSON *emojiCollection = NULL;
    int emojiCount = 0;

    ESP_GOTO_ON_FALSE(handle && root, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    emojiCollection = cJSON_GetObjectItem(root, "emoji_collection");
    if (!cJSON_IsArray(emojiCollection)) {
        return ESP_OK;
    }

    emojiCount = cJSON_GetArraySize(emojiCollection);
    ESP_LOGI(TAG, "Found %d emoji items", emojiCount);

    for (int i = 0; i < emojiCount; i++) {
        cJSON *icon = cJSON_GetArrayItem(emojiCollection, i);
        if (!cJSON_IsObject(icon)) {
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(icon, "name");
        cJSON *file = cJSON_GetObjectItem(icon, "file");
        if (!cJSON_IsString(name) || !cJSON_IsString(file)) {
            continue;
        }

        const uint8_t *emojiData = NULL;
        size_t emojiSize = 0;
        ret = emote_get_asset_data_by_name(handle, file->valuestring, &emojiData, &emojiSize);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get emoji data for: %s", file->valuestring);
            continue;
        }

        bool loopValue = false;
        int fpsValue = 0;

        cJSON *eaf = cJSON_GetObjectItem(icon, "eaf");
        if (cJSON_IsObject(eaf)) {
            cJSON *loop = cJSON_GetObjectItem(eaf, "loop");
            cJSON *fps = cJSON_GetObjectItem(eaf, "fps");
            loopValue = loop ? cJSON_IsTrue(loop) : false;
            fpsValue = fps ? fps->valueint : 0;
        }

        emoji_data_t *emoji_data = (emoji_data_t *)malloc(sizeof(emoji_data_t));
        ESP_GOTO_ON_FALSE(emoji_data, ESP_ERR_NO_MEM, error, TAG, "Failed to allocate emoji data");

        emoji_data->data = emojiData;
        emoji_data->size = emojiSize;
        emoji_data->fps = fpsValue;
        emoji_data->loop = loopValue;

        ESP_LOGD(TAG, "set emoji data: %s", name->valuestring);
        ret = emote_assets_table_set(handle->emoji_table, name->valuestring, emoji_data);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set emoji data for: %s", name->valuestring);
            free(emoji_data);
        }
    }

    return ESP_OK;

error:
    return ret;
}

static esp_err_t emote_load_icons(emote_handle_t handle, cJSON *root)
{
    esp_err_t ret = ESP_OK;
    cJSON *iconCollection = NULL;
    int iconCount = 0;

    ESP_GOTO_ON_FALSE(handle && root, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    iconCollection = cJSON_GetObjectItem(root, "icon_collection");
    if (!cJSON_IsArray(iconCollection)) {
        return ESP_OK;
    }

    iconCount = cJSON_GetArraySize(iconCollection);
    ESP_LOGI(TAG, "Found %d icon items", iconCount);

    for (int i = 0; i < iconCount; i++) {
        cJSON *icon = cJSON_GetArrayItem(iconCollection, i);
        if (!cJSON_IsObject(icon)) {
            continue;
        }

        cJSON *name = cJSON_GetObjectItem(icon, "name");
        cJSON *file = cJSON_GetObjectItem(icon, "file");
        if (!cJSON_IsString(name) || !cJSON_IsString(file)) {
            continue;
        }

        const uint8_t *iconData = NULL;
        size_t iconSize = 0;
        ret = emote_get_asset_data_by_name(handle, file->valuestring, &iconData, &iconSize);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get icon data for: %s", file->valuestring);
            continue;
        }

        icon_data_t *icon_data = (icon_data_t *)malloc(sizeof(icon_data_t));
        ESP_GOTO_ON_FALSE(icon_data, ESP_ERR_NO_MEM, error, TAG, "Failed to allocate icon data");

        icon_data->data = iconData;
        icon_data->size = iconSize;

        ESP_LOGD(TAG, "set icon data: %s", name->valuestring);
        ret = emote_assets_table_set(handle->icon_table, name->valuestring, icon_data);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set icon data for: %s", name->valuestring);
            free(icon_data);
        }
    }

    return ESP_OK;

error:
    return ret;
}

static esp_err_t emote_load_layouts(emote_handle_t handle, cJSON *root)
{
    esp_err_t ret = ESP_OK;
    cJSON *layoutJson = NULL;
    int layoutCount = 0;

    ESP_GOTO_ON_FALSE(handle && root, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    layoutJson = cJSON_GetObjectItem(root, "layout");
    if (!cJSON_IsArray(layoutJson)) {
        return ESP_OK;
    }

    layoutCount = cJSON_GetArraySize(layoutJson);
    ESP_LOGI(TAG, "Found %d layout items", layoutCount);

    for (int i = 0; i < layoutCount; i++) {
        cJSON *layout = cJSON_GetArrayItem(layoutJson, i);
        if (!cJSON_IsObject(layout)) {
            continue;
        }

        cJSON *type = cJSON_GetObjectItem(layout, "type");
        cJSON *name = cJSON_GetObjectItem(layout, "name");
        if (!cJSON_IsString(type) || !cJSON_IsString(name)) {
            ESP_LOGE(TAG, "Invalid layout item %d: missing required fields", i);
            continue;
        }

        const char *typeStr = type->valuestring;
        const char *obj_name = name->valuestring;

        ret = ESP_ERR_INVALID_ARG;
        if (strcmp(typeStr, "anim") == 0) {
            ret = emote_apply_anim_layout(handle, obj_name, layout);
        } else if (strcmp(typeStr, "image") == 0) {
            ret = emote_apply_image_layout(handle, obj_name, layout);
        } else if (strcmp(typeStr, "label") == 0) {
            ret = emote_apply_label_layout(handle, obj_name, layout);
        } else if (strcmp(typeStr, "timer") == 0) {
            ret = emote_apply_timer_layout(handle, obj_name, layout);
        } else if (strcmp(typeStr, "qrcode") == 0) {
            ret = emote_apply_qrcode_layout(handle, obj_name, layout);
        } else {
            ESP_LOGE(TAG, "Unknown type: %s", typeStr);
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply layout for %s: %s", obj_name, esp_err_to_name(ret));
        }
    }

    if (ret == ESP_OK) {
        gfx_obj_t *obj_default = handle->def_objects[EMOTE_DEF_OBJ_LEBAL_DEFAULT].obj;
        if (obj_default) {
            gfx_obj_delete(obj_default);
            handle->def_objects[EMOTE_DEF_OBJ_LEBAL_DEFAULT].obj = NULL;
        }
    }

    return ESP_OK;

error:
    return ret;
}

static esp_err_t emote_load_fonts(emote_handle_t handle, cJSON *root)
{
    esp_err_t ret = ESP_OK;
    cJSON *font = NULL;
    const uint8_t *fontData = NULL;
    size_t fontSize = 0;
    const void *src_data = NULL;

    ESP_GOTO_ON_FALSE(handle && root, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    font = cJSON_GetObjectItem(root, "text_font");
    if (!cJSON_IsString(font)) {
        return ESP_OK;
    }

    const char *fontsTextFile = font->valuestring;
    ESP_LOGI(TAG, "Foundfont: %s", fontsTextFile);

    ret = emote_get_asset_data_by_name(handle, fontsTextFile, &fontData, &fontSize);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Font file not found: %s", fontsTextFile);

    src_data = emote_acquire_data(handle, fontData, fontSize, &handle->font_cache);
    ESP_GOTO_ON_FALSE(src_data, ESP_ERR_INVALID_STATE, error, TAG, "Failed to get font data");

    ret = emote_apply_fonts(handle, (uint8_t *)src_data);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to apply fonts: %s", esp_err_to_name(ret));

    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_load_assets(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;
    const uint8_t *asset_data = NULL;
    size_t asset_size = 0;
    void *internal_buf = NULL;
    const void *src_data = NULL;
    cJSON *root = NULL;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    // Create hash tables if they don't exist
    if (!handle->emoji_table) {
        handle->emoji_table = emote_assets_table_create("emoji");
        ESP_GOTO_ON_FALSE(handle->emoji_table, ESP_ERR_NO_MEM, error, TAG, "Failed to create emoji_table hash table");
    }

    if (!handle->icon_table) {
        handle->icon_table = emote_assets_table_create("icon");
        ESP_GOTO_ON_FALSE(handle->icon_table, ESP_ERR_NO_MEM, error, TAG, "Failed to create icon_table hash table");
    }

    // Create semaphore for emergency dialog animation completion
    if (!handle->emerg_dlg_done_sem) {
        handle->emerg_dlg_done_sem = xSemaphoreCreateBinary();
        ESP_GOTO_ON_FALSE(handle->emerg_dlg_done_sem, ESP_ERR_NO_MEM, error, TAG, "Failed to create emerg_dlg_done_sem");
    }

    ret = emote_get_asset_data_by_name(handle, EMOTE_INDEX_JSON_FILENAME, &asset_data, &asset_size);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to find %s in assets", EMOTE_INDEX_JSON_FILENAME);

    ESP_LOGI(TAG, "Found %s, size: %d", EMOTE_INDEX_JSON_FILENAME, (int)asset_size);

    src_data = emote_acquire_data(handle, asset_data, asset_size, &internal_buf);
    ESP_GOTO_ON_FALSE(src_data, ESP_ERR_INVALID_STATE, error, TAG, "Failed to resolve asset data");

    root = cJSON_ParseWithLength((const char *)src_data, asset_size);
    ESP_GOTO_ON_FALSE(root, ESP_ERR_INVALID_RESPONSE, error_free_buf, TAG, "Failed to parse %s", EMOTE_INDEX_JSON_FILENAME);

    ret = emote_load_emojis(handle, root);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load emojis: %s", esp_err_to_name(ret));
    }

    ret = emote_load_icons(handle, root);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load icons: %s", esp_err_to_name(ret));
    }

    ret = emote_load_layouts(handle, root);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load layouts: %s", esp_err_to_name(ret));
    }

    ret = emote_load_fonts(handle, root);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load fonts: %s", esp_err_to_name(ret));
    }

    cJSON_Delete(root);
    if (internal_buf) {
        free(internal_buf);
    }

    return ESP_OK;

error_free_buf:
    if (internal_buf) {
        free(internal_buf);
    }

error:
    return ret;
}

esp_err_t emote_unload_assets(emote_handle_t handle)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    // Cleanup objects
    if (handle->gfx_handle) {
        gfx_emote_lock(handle->gfx_handle);
        // Cleanup def_objects
        for (int i = EMOTE_DEF_OBJ_ANIM_EYE; i < EMOTE_DEF_OBJ_MAX; i++) {
            emote_def_obj_entry_t *entry = &handle->def_objects[i];
            if (entry->obj) {
                if (i == EMOTE_DEF_OBJ_TIMER_STATUS) {
                    gfx_timer_delete(handle->gfx_handle, (gfx_timer_handle_t)entry->obj);
                } else {
                    gfx_obj_delete(entry->obj);
                }
                entry->obj = NULL;
            }
            // Cleanup cache based on object type
            if (i >= EMOTE_DEF_OBJ_ANIM_EYE && i <= EMOTE_DEF_OBJ_ANIM_EMERG_DLG) {
                if (entry->data.anim) {
                    if (entry->data.anim->cache) {
                        free(entry->data.anim->cache);
                    }
                    free(entry->data.anim);
                    entry->data.anim = NULL;
                }
            } else if (i == EMOTE_DEF_OBJ_ICON_STATUS || i == EMOTE_DEF_OBJ_ICON_CHARGE) {
                if (entry->data.img) {
                    if (entry->data.img->cache) {
                        free(entry->data.img->cache);
                    }
                    free(entry->data.img);
                    entry->data.img = NULL;
                }
            }
        }

        // Cleanup custom objects created by load_layouts
        emote_custom_obj_entry_t *custom_entry = handle->custom_objects;
        while (custom_entry) {
            emote_custom_obj_entry_t *next = custom_entry->next;
            if (custom_entry->obj) {
                gfx_obj_delete(custom_entry->obj);
            }
            if (custom_entry->name) {
                free(custom_entry->name);
            }
            free(custom_entry);
            custom_entry = next;
        }
        handle->custom_objects = NULL;

        // Cleanup emergency dialog timer
        if (handle->dialog_timer) {
            gfx_timer_delete(handle->gfx_handle, handle->dialog_timer);
            handle->dialog_timer = NULL;
        }

        gfx_emote_unlock(handle->gfx_handle);
    }

    // Cleanup semaphore for emergency dialog animation completion
    if (handle->emerg_dlg_done_sem) {
        vSemaphoreDelete(handle->emerg_dlg_done_sem);
        handle->emerg_dlg_done_sem = NULL;
    }

    // Cleanup emoji table (destroy and recreate to clear all entries)
    if (handle->emoji_table) {
        emote_assets_table_destroy(handle->emoji_table);
        handle->emoji_table = NULL;
    }

    // Cleanup icon table (destroy and recreate to clear all entries)
    if (handle->icon_table) {
        emote_assets_table_destroy(handle->icon_table);
        handle->icon_table = NULL;
    }

    // Release font cache
    if (handle->font_cache) {
        free(handle->font_cache);
        handle->font_cache = NULL;
    }

    // Cleanup font
    if (handle->gfx_font) {
        gfx_font_lv_delete(handle->gfx_font);
        handle->gfx_font = NULL;
    }

    ESP_LOGI(TAG, "Unload assets");
    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_get_icon_data_by_name(emote_handle_t handle, const char *name, icon_data_t **icon)
{
    esp_err_t ret = ESP_OK;

    if (!handle || !name || !icon) {
        return ESP_ERR_INVALID_ARG;
    }
    ret = emote_find_data_by_key(handle, handle->icon_table, name, (void **)icon);
    return ret;
}

esp_err_t emote_get_emoji_data_by_name(emote_handle_t handle, const char *name, emoji_data_t **emoji)
{
    esp_err_t ret = ESP_OK;

    if (!handle || !name || !emoji) {
        return ESP_ERR_INVALID_ARG;
    }
    ret = emote_find_data_by_key(handle, handle->emoji_table, name, (void **)emoji);
    return ret;
}

esp_err_t emote_mount_and_load_assets(emote_handle_t handle, const emote_data_t *data)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle && data, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    ret = emote_mount_assets(handle, data);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to mount assets");

    ret = emote_load_assets(handle);
    ESP_GOTO_ON_FALSE(ret == ESP_OK, ret, error, TAG, "Failed to load assets data");

    return ESP_OK;

error:
    return ret;
}
