# ESP-Emote-Expression

ESP-Emote-Expression is an ESP-IDF component for managing emote animations and UI display.This component provides rich emote animation management, UI element control, and event handling capabilities.

## Features

- **Emote Animation Management**: Supports emoji animation playback, including eye expressions, listening animations, etc.
- **Boot Animation**: Supports custom boot animations with configurable auto-stop and deletion
- **UI Element Management**: Supports various UI elements such as labels, images, timers, QR codes, etc.
- **Event Handling**: Supports multiple system events (speaking, listening, system messages, battery status, etc.)
- **Dialog Animation**: Supports urgent dialog animations with configurable auto-stop time
- **Resource Management**: Supports loading resources from file paths or partitions, using memory mapping for performance optimization
- **Layout Configuration**: Supports defining UI layouts through JSON configuration files

## Dependencies

- ESP-IDF >= 5.0
- `espressif/cmake_utilities` >= 0.*
- `espressif/esp_mmap_assets` >= 1.*
- `espressif2022/esp_emote_gfx` (Graphics rendering component)
- `espressif2022/esp_emote_assets` (Resource management component)

## Quick Start

### 1. Add Component Dependency

Add the following to your `idf_component.yml` file:

```yaml
dependencies:
  espressif2022/esp_emote_expression:
    version: "*"
```

### 2. Initialize Component

```c
#include "expression_emote.h"

// Flush callback (required for display output)
static void flush_callback(int x_start, int y_start, int x_end, int y_end, 
                          const void *data, emote_handle_t handle)
{
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    // Send data to display (e.g., esp_lcd_panel_draw_bitmap)
    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, data);
}

// Update callback (optional, for monitoring animation events)
static void update_callback(gfx_player_event_t event, const void *obj, 
                            emote_handle_t handle)
{
    if (event == GFX_PLAYER_EVENT_ALL_FRAME_DONE) {
        ESP_LOGI(TAG, "Animation completed for object: %p", obj);
    }
}

// Configuration structure
emote_config_t config = {
    .flags = {
        .swap = true,
        .double_buffer = true,
        .buff_dma = false,
        .buff_spiram = false
    },
    .gfx_emote = {
        .h_res = 360,
        .v_res = 360,
        .fps = 30
    },
    .buffers = {
        .buf_pixels = 360 * 16  // Buffer size in pixels (typically h_res * lines)
    },
    .task = {
        .task_priority = 5,
        .task_stack = 4096,
        .task_affinity = -1,  // -1 = no affinity, or specify CPU core (0 or 1)
        .task_stack_in_ext = false
    },
    .flush_cb = flush_callback,   // Required
    .update_cb = update_callback, // Optional
    .user_data = NULL              // Optional
};

// Initialize
emote_handle_t handle = emote_init(&config);
if (handle == NULL) {
    ESP_LOGE(TAG, "Failed to initialize emote");
    // Handle error
}
```

### 3. Load Resources

```c
// Load resources from partition with memory mapping
emote_data_t data = {
    .type = EMOTE_SOURCE_PARTITION,
    .source = {
        .partition_label = "anim_icon",
    },
    .flags = {
        .mmap_enable = true,  // Use memory mapping for better performance
    },
};

esp_err_t ret = emote_mount_and_load_assets(handle, &data);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to load assets: %s", esp_err_to_name(ret));
    // Handle error
}

// Alternative: Load from file path
emote_data_t file_data = {
    .type = EMOTE_SOURCE_PATH,
    .source = {
        .path = "/spiffs/asset_test.bin",
    },
};
ret = emote_mount_and_load_assets(handle, &file_data);
```

### 4. Use API

```c
// Set emoji animation on eye object
emote_set_anim_emoji(handle, "happy");

// Set event message
emote_set_event_msg(handle, EMOTE_MGR_EVT_SPEAK, "Hello, I'm esp_emote_expression!");
emote_set_event_msg(handle, EMOTE_MGR_EVT_LISTEN, NULL);
emote_set_event_msg(handle, EMOTE_MGR_EVT_IDLE, NULL);

// Set QR code
emote_set_qrcode_data(handle, "https://www.esp32.com");

// Insert emergency dialog animation with auto-stop timer
emote_insert_anim_dialog(handle, "angry", 5000);  // Auto-stop after 5 seconds

// Wait for emergency dialog to complete
esp_err_t ret = emote_wait_emerg_dlg_done(handle, 10000);  // 10 second timeout
if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Emergency dialog completed");
}

// Update battery status (format: "charging,percentage" or "not_charging,percentage")
emote_set_event_msg(handle, EMOTE_MGR_EVT_BAT, "0,50");   // Not charging, 50%
emote_set_event_msg(handle, EMOTE_MGR_EVT_BAT, "1,100");  // Charging, 100%
```

## API Reference

### Initialization and Cleanup

- `emote_init()` - Initialize emote manager
- `emote_deinit()` - Cleanup and release resources
- `emote_is_initialized()` - Check if initialized

### Resource Loading

- `emote_mount_and_load_assets()` - Mount and parse assets in one call
- `emote_mount_assets()` - Mount assets from source (partition or file path)
- `emote_unmount_assets()` - Unmount assets
- `emote_load_assets()` - Parse JSON and load emojis, icons, layouts, fonts
- `emote_unload_assets()` - Unload assets data (assets loaded by emote_load_assets)
- `emote_get_icon_data_by_name()` - Get parsed icon data by name
- `emote_get_emoji_data_by_name()` - Get parsed emoji data by name
- `emote_get_asset_data_by_name()` - Get raw asset file data by name

### Animation Control

- `emote_set_anim_emoji()` - Set emoji animation on eye object
- `emote_set_anim_visible()` - Set face visible or not
- `emote_set_dialog_anim()` - Set emergency dialog animation
- `emote_insert_anim_dialog()` - Insert emergency dialog animation with auto-stop timer
- `emote_stop_anim_dialog()` - Stop emergency dialog animation
- `emote_wait_emerg_dlg_done()` - Wait for emergency dialog animation to complete

### Events and Messages

- `emote_set_event_msg()` - Set event message (IDLE, SPEAK, LISTEN, SYS, SET, BAT, QRCODE)
- `emote_set_qrcode_data()` - Set QR code data

### Object Management

- `emote_get_obj_by_name()` - Get graphics object by name
- `emote_create_obj_by_type()` - Create custom object by type (anim, image, label, qrcode, timer)
- `emote_set_obj_visible()` - Set object visible or not
- `emote_lock()` - Lock the emote manager (for thread-safe operations)
- `emote_unlock()` - Unlock the emote manager

### Callbacks and Notifications

- `emote_notify_flush_finished()` - Notify that flush operation is finished
- `emote_notify_all_refresh()` - Notify that all refresh operations are finished
- `emote_get_user_data()` - Get user data pointer

## Event Types

The component supports the following event types:

- `EMOTE_MGR_EVT_IDLE` - Idle state
- `EMOTE_MGR_EVT_SPEAK` - Speaking event
- `EMOTE_MGR_EVT_LISTEN` - Listening event
- `EMOTE_MGR_EVT_SYS` - System event
- `EMOTE_MGR_EVT_SET` - Settings event
- `EMOTE_MGR_EVT_BAT` - Battery event
- `EMOTE_MGR_EVT_OFF` - Close title event

## Configuration

### Graphics Configuration

- `h_res` / `v_res` - Horizontal and vertical resolution
- `fps` - Frame rate
- `buf_pixels` - Buffer pixel count

### Task Configuration

- `task_priority` - Task priority
- `task_stack` - Task stack size
- `task_affinity` - CPU affinity
- `task_stack_in_ext` - Whether to use external memory

### Display Flags

- `swap` - Whether to swap byte order
- `double_buffer` - Whether to use double buffering
- `buff_dma` - Whether to use DMA buffer
- `buff_spiram` - Whether to use SPIRAM for buffers

### Callback Functions

- `flush_cb` - Flush ready callback: `void (*emote_flush_ready_cb_t)(int x_start, int y_start, int x_end, int y_end, const void *data, emote_handle_t handle)`
- `update_cb` - Update callback: `void (*emote_update_cb_t)(gfx_player_event_t event, const void *obj, emote_handle_t handle)`
- `user_data` - User data pointer (optional)

## Building Asset Files

Asset files (`.bin`) can be generated in two ways:

### Option 1: Online Tool (Recommended for Quick Testing)

Use the online graphics generation tool to create asset files:

- **URL**: https://gfx-gen-tool.pages.dev/
- Upload your assets (emojis, icons, layouts, fonts) and configure settings
- Download the generated `.bin` file
- Flash the file to your partition

### Option 2: Local Build (Recommended for Development)

Use the CMake function provided by `esp_emote_assets` component to build assets during compilation:

```cmake
# In your CMakeLists.txt
set(RESOLUTION "360_360")  # or "320_240", "1024_600", etc.
set(ASSETS_FILE "${CMAKE_BINARY_DIR}/asset_test.bin")

# Build assets bin file
build_speaker_assets_bin("anim_icon" ${RESOLUTION} ${ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH})

# Flash to partition
esptool_py_flash_to_partition(flash "anim_icon" "${ASSETS_FILE}")
```

**Function Parameters:**
- `partition_name`: Target partition name (e.g., "anim_icon")
- `resolution`: Resolution string (e.g., "360_360", "320_240")
- `output_path`: Output file path for the generated `.bin` file
- `name_length`: Maximum file name length (optional, typically from `CONFIG_MMAP_FILE_NAME_LENGTH`)

The build function will automatically:
- Collect assets from the `esp_emote_assets` component
- Generate `index.json` with asset metadata
- Package everything into a binary file
- Validate partition size

**For detailed documentation on asset building, configuration, and build scripts, please refer to:**
- [ESP Emote Assets Component Documentation](https://components.espressif.com/components/espressif2022/esp_emote_assets)

## Custom Objects Example

You can create and manage custom UI objects:

```c
// Create a custom label
gfx_obj_t *custom_label = emote_create_obj_by_type(handle, EMOTE_OBJ_TYPE_LABEL, "custom_label");
if (custom_label) {
    emote_lock(handle);
    gfx_label_set_text(custom_label, "Custom Label");
    gfx_label_set_color(custom_label, GFX_COLOR_HEX(0xFF0000));
    gfx_obj_set_size(custom_label, 200, 30);
    gfx_obj_align(custom_label, GFX_ALIGN_CENTER, 0, 0);
    emote_unlock(handle);
}

// Create a custom image from icon data
icon_data_t *icon_data = NULL;
emote_get_icon_data_by_name(handle, "icon_tips", &icon_data);
gfx_image_dsc_t img_dsc = {0};
memcpy(&img_dsc.header, icon_data->data, sizeof(gfx_image_header_t));
img_dsc.data = (const uint8_t *)icon_data->data + sizeof(gfx_image_header_t);
img_dsc.data_size = icon_data->size - sizeof(gfx_image_header_t);

gfx_obj_t *custom_img = emote_create_obj_by_type(handle, EMOTE_OBJ_TYPE_IMAGE, "custom_image");
emote_lock(handle);
gfx_img_set_src(custom_img, &img_dsc);
gfx_obj_set_visible(custom_img, true);
gfx_obj_align(custom_img, GFX_ALIGN_CENTER, 0, 50);
emote_unlock(handle);

// Create a custom animation from emoji data
emoji_data_t *emoji_data = NULL;
emote_get_emoji_data_by_name(handle, "happy", &emoji_data);
gfx_obj_t *custom_anim = emote_create_obj_by_type(handle, EMOTE_OBJ_TYPE_ANIM, "custom_anim");
emote_lock(handle);
gfx_anim_set_src(custom_anim, emoji_data->data, emoji_data->size);
gfx_anim_set_segment(custom_anim, 0, 0xFFFF, emoji_data->fps, emoji_data->loop);
gfx_anim_start(custom_anim);
gfx_obj_set_visible(custom_anim, true);
gfx_obj_align(custom_anim, GFX_ALIGN_CENTER, 0, 0);
emote_unlock(handle);
```

## Examples

For complete examples, please refer to the test applications in the `test_apps` directory. The test app demonstrates:

- Basic emote operations (animations, events, QR codes)
- Loading assets from partition (with/without memory mapping)
- Loading assets from file path
- Creating and managing custom UI objects
- Using callbacks for flush and update events

## License

Apache-2.0

## Contributing

Issues and Pull Requests are welcome.

