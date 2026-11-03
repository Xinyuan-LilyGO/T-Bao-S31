/* T-Bao S31 PARLIO JPEG capture for OV2640 and OV3660 sensors. */
#include "app_camera.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/parlio_rx.h"
#include "esp_cam_io_parl.h"
#include "esp_cam_io_parl_sccb.h"
#include "esp_cam_sensor_io_parl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app_camera_preview.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "tbao_s31_board.h"

static const char *TAG = "APP_CAMERA";

#define PMIC_REVISION_REG       0x00
#define PMIC_DVDD_REG           0x03
#define PMIC_DOVDD_REG          0x05
#define PMIC_AVDD_REG           0x06
#define PMIC_DVDD_SEQ_REG       0x0A
#define PMIC_AVDD_SEQ_REG       0x0B
#define PMIC_ENABLE_REG         0x0E
#define PMIC_CAMERA_RAILS       0x0D
#define PMIC_REVISION           0x80

#if CONFIG_TBAO_CAMERA_OV3660
#define PMIC_DVDD_VALUE         0x7D
#elif CONFIG_TBAO_CAMERA_OV2640_CORE_1V3
#define PMIC_DVDD_VALUE         0x64
#else
#define PMIC_DVDD_VALUE         0x57
#endif

/* The SGM38121 PMIC shares the board I2C bus with the XL9555, the audio
 * codecs and the battery gauge.  Keep a failed/stretching PMIC transaction
 * short so it cannot hold the bus long enough for the XL9555 (100 ms) to time
 * out and lock the motion manager. */
#define CAMERA_I2C_TIMEOUT_MS   30
#define CAMERA_RECEIVE_TIMEOUT 100
#define CAMERA_STOP_WAIT_MS    1500
#define CAMERA_FRAME_ALLOC      ((TBAO_S31_CAMERA_FRAME_WIDTH * TBAO_S31_CAMERA_FRAME_HEIGHT) / 2 + 8192)

typedef struct {
    app_camera_frame_cb_t cb;
    void *ctx;
} frame_callback_t;

static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_task_exit_sem;
static StaticSemaphore_t s_task_exit_sem_storage;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static volatile bool s_running;
static unsigned s_refcount;
static uint32_t s_frame_count;

static frame_callback_t s_callbacks[APP_CAMERA_MAX_FRAME_CB];
static uint8_t *s_latest;
static size_t s_latest_capacity;
static size_t s_latest_len;

static i2c_master_bus_handle_t s_camera_i2c;
static esp_cam_io_parl_handle_t s_camera_io;
static esp_cam_sensor_io_parl_handle_t s_sensor;

static void ensure_mutex(void)
{
    /* Several tasks (the activity manager, page preview/inference workers and
     * MCP callbacks) can reach this on first use.  Create the primitives from
     * static storage under a spinlock so a boot-time race cannot hand out two
     * different mutexes, which would silently break the s_latest/s_task
     * serialization and corrupt the heap. */
    portENTER_CRITICAL(&s_init_lock);
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    }
    if (s_task_exit_sem == NULL) {
        s_task_exit_sem = xSemaphoreCreateBinaryStatic(&s_task_exit_sem_storage);
    }
    portEXIT_CRITICAL(&s_init_lock);
}

/* The PMIC shares the board I2C bus, so a transient timeout (another device
 * stretching the clock) must not abort the whole camera power sequence. */
#define PMIC_I2C_RETRY_COUNT 3

static esp_err_t pmic_read(i2c_master_dev_handle_t pmic, uint8_t reg, uint8_t *value)
{
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < PMIC_I2C_RETRY_COUNT; ++attempt) {
        ret = i2c_master_transmit_receive(pmic, &reg, 1, value, 1,
                                          CAMERA_I2C_TIMEOUT_MS);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ret;
}

static esp_err_t pmic_write_verified(i2c_master_dev_handle_t pmic, uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < PMIC_I2C_RETRY_COUNT; ++attempt) {
        uint8_t actual = 0;
        ret = i2c_master_transmit(pmic, command, sizeof(command),
                                  CAMERA_I2C_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = pmic_read(pmic, reg, &actual);
        }
        if (ret == ESP_OK && actual == value) {
            return ESP_OK;
        }
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "PMIC register 0x%02x readback 0x%02x != 0x%02x",
                     reg, actual, value);
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "PMIC register 0x%02x write 0x%02x failed: %s",
             reg, value, esp_err_to_name(ret));
    return ret;
}

/* Registering/removing an I2C device on the shared board bus on every camera
 * power change churns the bus and can make concurrent XL9555 / battery-gauge
 * transactions time out.  Register the PMIC once and keep the handle: the
 * board bus lives for the whole application anyway. */
static i2c_master_dev_handle_t s_camera_pmic_dev;

static esp_err_t camera_pmic_get(i2c_master_dev_handle_t *out)
{
    if (s_camera_pmic_dev != NULL) {
        *out = s_camera_pmic_dev;
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = tbao_s31_board_get_i2c_bus(&bus);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TBAO_S31_PMIC_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(bus, &config, &s_camera_pmic_dev);
    if (ret != ESP_OK) {
        s_camera_pmic_dev = NULL;
        return ret;
    }
    *out = s_camera_pmic_dev;
    return ESP_OK;
}

static esp_err_t camera_power_set(bool enabled)
{
    i2c_master_dev_handle_t pmic = NULL;
    esp_err_t ret = camera_pmic_get(&pmic);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t revision = 0;
    uint8_t enable_mask = 0;
    uint8_t sequence = 0;
    if (enabled) {
        ret = pmic_read(pmic, PMIC_REVISION_REG, &revision);
        if (ret == ESP_OK && revision != PMIC_REVISION) {
            ESP_LOGE(TAG, "unexpected SGM38121 revision 0x%02x", revision);
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        if (ret == ESP_OK) {
            ret = pmic_read(pmic, PMIC_ENABLE_REG, &enable_mask);
        }
        if (ret == ESP_OK && (enable_mask & PMIC_CAMERA_RAILS)) {
            ret = pmic_write_verified(pmic, PMIC_ENABLE_REG,
                                      enable_mask & (uint8_t)~PMIC_CAMERA_RAILS);
        }
        if (ret == ESP_OK) {
            ret = pmic_read(pmic, PMIC_DVDD_SEQ_REG, &sequence);
        }
        if (ret == ESP_OK && (sequence & 0x0F)) {
            ret = pmic_write_verified(pmic, PMIC_DVDD_SEQ_REG, sequence & 0xF0);
        }
        if (ret == ESP_OK) {
            ret = pmic_read(pmic, PMIC_AVDD_SEQ_REG, &sequence);
        }
        if (ret == ESP_OK && sequence != 0) {
            ret = pmic_write_verified(pmic, PMIC_AVDD_SEQ_REG, 0);
        }
        if (ret == ESP_OK) {
            ret = pmic_write_verified(pmic, PMIC_DVDD_REG, PMIC_DVDD_VALUE);
        }
        if (ret == ESP_OK) {
            ret = pmic_write_verified(pmic, PMIC_DOVDD_REG, 0xB1);
        }
        if (ret == ESP_OK) {
            ret = pmic_write_verified(pmic, PMIC_AVDD_REG, 0xB1);
        }
        if (ret == ESP_OK) {
            ret = pmic_write_verified(pmic, PMIC_ENABLE_REG,
                                      enable_mask | PMIC_CAMERA_RAILS);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            ESP_LOGI(TAG, "SGM38121 revision=0x%02x DVDD=0x%02x DOVDD=0xb1 AVDD=0xb1",
                     revision, PMIC_DVDD_VALUE);
        }
    } else {
        ret = pmic_read(pmic, PMIC_ENABLE_REG, &enable_mask);
        if (ret == ESP_OK && (enable_mask & PMIC_CAMERA_RAILS)) {
            ret = pmic_write_verified(pmic, PMIC_ENABLE_REG,
                                      enable_mask & (uint8_t)~PMIC_CAMERA_RAILS);
        }
    }

    /* The PMIC device handle is cached for the application lifetime. */
    return ret;
}

static esp_err_t create_camera_i2c(void)
{
    i2c_master_bus_config_t config = {
        .i2c_port = TBAO_S31_CAMERA_I2C_PORT,
        .sda_io_num = TBAO_S31_CAMERA_SDA,
        .scl_io_num = TBAO_S31_CAMERA_SCL,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    return i2c_new_master_bus(&config, &s_camera_i2c);
}

static void free_latest(void)
{
    if (s_latest != NULL) {
        heap_caps_free(s_latest);
        s_latest = NULL;
    }
    s_latest_capacity = 0;
    s_latest_len = 0;
}

/* Diagnostic: localise internal-heap corruption.  The checkpoint name is
 * printed *before* the heap walk, so if the walk itself faults, the last line
 * in the panic log identifies the step after which the heap was already bad. */
#define CAMERA_HEAP_TRACE 1
#if CAMERA_HEAP_TRACE
static void camera_heap_checkpoint(const char *step)
{
    ESP_LOGI(TAG, "heap checkpoint [%s] ...", step);
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool ok = heap_caps_check_integrity_all(true);
    ESP_LOGI(TAG, "heap checkpoint [%s]: internal free=%u largest=%u integrity=%s",
             step, (unsigned)free_int, (unsigned)largest_int, ok ? "ok" : "CORRUPT");
}
#else
static void camera_heap_checkpoint(const char *step)
{
    (void)step;
}
#endif

static esp_err_t configure_sensor_and_io(void)
{
    esp_cam_sensor_io_parl_config_t sensor_config = {
        .pwdn_io = GPIO_NUM_NC,
        .reset_io = TBAO_S31_CAMERA_RESET,
        .xclk_io = TBAO_S31_CAMERA_XCLK,
        .sda_io = GPIO_NUM_NC,
        .scl_io = GPIO_NUM_NC,
        .xclk_hz = TBAO_S31_CAMERA_XCLK_HZ,
        .pixel_format = ESP_CAM_IO_PARL_PIXFORMAT_JPEG,
        .frame_size = ESP_CAM_IO_PARL_FRAMESIZE_320X320,
        .ledc_timer = LEDC_TIMER_1,
        /* Channel 1 belongs to the actuator driver (motor AIN1).  Reusing it
         * for XCLK silently re-routes the motor channel to GPIO55 and leaves
         * the motor dead after the camera is closed. */
        .ledc_channel = LEDC_CHANNEL_6,
        .jpeg_quality = TBAO_S31_CAMERA_JPEG_QUALITY,
        .i2c_port = TBAO_S31_CAMERA_I2C_PORT,
    };

    esp_err_t ret = sccb_use_port(TBAO_S31_CAMERA_I2C_PORT);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_cam_new_sensor_io_parl(&sensor_config, &s_sensor);
    if (ret != ESP_OK) {
        return ret;
    }
    camera_heap_checkpoint("sensor init");

    if (s_sensor->id.PID != ESP_CAM_IO_PARL_OV2640_PID &&
        s_sensor->id.PID != ESP_CAM_IO_PARL_OV3660_PID) {
        ESP_LOGE(TAG, "unsupported sensor PID 0x%04x", s_sensor->id.PID);
        return ESP_ERR_NOT_SUPPORTED;
    }
#if CONFIG_TBAO_CAMERA_OV3660
    if (s_sensor->id.PID != ESP_CAM_IO_PARL_OV3660_PID) {
        ESP_LOGE(TAG, "configured OV3660 power profile but sensor is PID 0x%04x",
                 s_sensor->id.PID);
        return ESP_ERR_INVALID_RESPONSE;
    }
#else
    if (s_sensor->id.PID != ESP_CAM_IO_PARL_OV2640_PID) {
        ESP_LOGE(TAG, "configured OV2640 power profile but sensor is PID 0x%04x",
                 s_sensor->id.PID);
        return ESP_ERR_INVALID_RESPONSE;
    }
#endif

    ESP_LOGI(TAG, "sensor PID=0x%04x address=0x%02x",
             s_sensor->id.PID, s_sensor->sccb_address);

    esp_cam_io_parl_config_t io_config = {
        .data_width = 8,
        .queue_frames = 1,
        .fill_mode = ESP_CAM_IO_PARL_QUEUE_LATEST,
        .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .pclk_io = TBAO_S31_CAMERA_PCLK,
        .pclk_sample_edge = ESP_CAM_IO_PARL_PCLK_POS,
        .vsync_io = GPIO_NUM_NC,
        .de_io = TBAO_S31_CAMERA_HREF,
        .hsync_io = GPIO_NUM_NC,
        .data_io = {
            TBAO_S31_CAMERA_DATA0,
            TBAO_S31_CAMERA_DATA1,
            TBAO_S31_CAMERA_DATA2,
            TBAO_S31_CAMERA_DATA3,
            TBAO_S31_CAMERA_DATA4,
            TBAO_S31_CAMERA_DATA5,
            TBAO_S31_CAMERA_DATA6,
            TBAO_S31_CAMERA_DATA7,
        },
    };
    /* Create the PARLIO RX unit once and keep it for the lifetime of the
     * application.  Deleting and re-creating it on every camera session leaves
     * the GDMA channel / interrupt teardown racy: a late RX interrupt can run
     * parlio_rx_default_desc_done_callback() with an already-freed rx_unit and
     * dereference rx_unit->dma_link (load access fault, plus internal heap
     * corruption).  Keeping the unit alive makes such an interrupt harmless:
     * dma_link and the payload/staging buffers stay valid. */
    if (s_camera_io == NULL) {
        ret = esp_cam_new_io_parl(&io_config, &s_camera_io);
        if (ret != ESP_OK) {
            return ret;
        }
        camera_heap_checkpoint("io created");

        ret = esp_cam_io_parl_set_alloc_size(s_camera_io, CAMERA_FRAME_ALLOC,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    ret = esp_cam_io_parl_enable(s_camera_io, true);
    if (ret != ESP_OK) {
        return ret;
    }
    camera_heap_checkpoint("io enabled");
    ret = esp_cam_sensor_io_parl_connect(s_camera_io);
    if (ret != ESP_OK) {
        return ret;
    }
    camera_heap_checkpoint("io connected");

    if (s_sensor->id.PID == ESP_CAM_IO_PARL_OV2640_PID && s_sensor->set_res_raw != NULL) {
        int sensor_ret = s_sensor->set_res_raw(
            s_sensor, 1, 0, 0, 0, 100, 0, 600, 600,
            TBAO_S31_CAMERA_FRAME_WIDTH, TBAO_S31_CAMERA_FRAME_HEIGHT, true, false);
        if (sensor_ret != 0) {
            ESP_LOGE(TAG, "OV2640 square crop setup failed: %d", sensor_ret);
            return ESP_ERR_INVALID_STATE;
        }
    }
    camera_heap_checkpoint("sensor crop");

    esp_err_t start_ret = esp_cam_io_parl_start(s_camera_io);
    if (start_ret == ESP_OK) {
        camera_heap_checkpoint("io started");
    }
    return start_ret;
}

static void camera_hw_deinit(void)
{
    if (s_camera_io != NULL) {
        if (s_sensor != NULL) {
            (void)esp_cam_sensor_io_parl_disconnect();
        }
        camera_heap_checkpoint("io disconnect");
        /* Stop the RX unit and its parser task, but keep the handle allocated:
         * it is re-enabled by the next camera session.  See the comment in
         * configure_sensor_and_io() for why the unit must not be re-created. */
        (void)esp_cam_io_parl_disable(s_camera_io);
        camera_heap_checkpoint("io disable");
    }
    if (s_sensor != NULL) {
        (void)esp_cam_del_sensor_io_parl();
        camera_heap_checkpoint("sensor delete");
        s_sensor = NULL;
    }
    if (s_camera_i2c != NULL) {
        (void)i2c_del_master_bus(s_camera_i2c);
        camera_heap_checkpoint("i2c delete");
        s_camera_i2c = NULL;
    }
    (void)gpio_set_level(TBAO_S31_CAMERA_RESET, 0);
    (void)gpio_reset_pin(TBAO_S31_CAMERA_RESET);
    (void)camera_power_set(false);
    /* Deliberately keep the hardware JPEG decoder engine alive across camera
     * sessions.  Re-creating it on every page entry costs internal DMA memory
     * churn and, when XiaoZhi is active (internal RAM is tight), the engine
     * creation can fail and is retried every 200 ms - which is what made the
     * preview appear only after several seconds.  The engine is small and does
     * not depend on the camera hardware. */
    camera_heap_checkpoint("camera stop done");
}

static esp_err_t camera_hw_init(void)
{
    esp_err_t ret = camera_power_set(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera power init failed: %s", esp_err_to_name(ret));
        /* camera_power_set() may have enabled only part of the rail sequence;
         * always run the inverse operation before returning to the activity
         * manager so a failed start cannot leave the sensor powered. */
        (void)camera_power_set(false);
        return ret;
    }
    camera_heap_checkpoint("power on");

    ret = create_camera_i2c();
    if (ret == ESP_OK) {
        ret = configure_sensor_and_io();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera hardware init failed: %s", esp_err_to_name(ret));
        camera_hw_deinit();
        return ret;
    }
    ESP_LOGI(TAG, "camera ready: JPEG %dx%d, PARLIO data GPIO46..53 HREF GPIO57 PCLK GPIO54",
             TBAO_S31_CAMERA_FRAME_WIDTH, TBAO_S31_CAMERA_FRAME_HEIGHT);
    camera_heap_checkpoint("camera start done");
    return ESP_OK;
}

static void camera_task(void *arg)
{
    (void)arg;
    bool first_frame_checked = false;
    while (s_running) {
        esp_cam_io_parl_trans_t frame = {0};
        esp_err_t ret = esp_cam_io_parl_receive(s_camera_io, &frame, CAMERA_RECEIVE_TIMEOUT);
        if (ret != ESP_OK) {
            if (s_running) {
                ESP_LOGW(TAG, "PARLIO receive failed: %s", esp_err_to_name(ret));
            }
            continue;
        }

        frame_callback_t callbacks[APP_CAMERA_MAX_FRAME_CB];
        size_t callback_count = 0;
        if (frame.buffer != NULL && frame.length > 0) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_latest_capacity < frame.length) {
                uint8_t *new_latest = heap_caps_malloc(frame.length,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (new_latest != NULL) {
                    if (s_latest != NULL) {
                        heap_caps_free(s_latest);
                    }
                    s_latest = new_latest;
                    s_latest_capacity = frame.length;
                }
            }
            if (s_latest != NULL && s_latest_capacity >= frame.length) {
                memcpy(s_latest, frame.buffer, frame.length);
                s_latest_len = frame.length;
                s_frame_count++;
            }
            for (size_t i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
                if (s_callbacks[i].cb != NULL) {
                    callbacks[callback_count++] = s_callbacks[i];
                }
            }
            xSemaphoreGive(s_mutex);

            if (s_frame_count == 1 || (s_frame_count % 30U) == 0U) {
                ESP_LOGI(TAG, "new JPEG frame=%u len=%u callbacks=%u",
                         (unsigned)s_frame_count, (unsigned)frame.length,
                         (unsigned)callback_count);
            }

            for (size_t i = 0; i < callback_count; i++) {
                callbacks[i].cb(frame.buffer, frame.length, callbacks[i].ctx);
            }
        }
        if (frame.buffer != NULL) {
            (void)esp_cam_io_parl_free_buffer(&frame);
        }
        if (!first_frame_checked && s_frame_count > 0) {
            first_frame_checked = true;
            camera_heap_checkpoint("first frame");
        }
    }
    /* The capture task owns the PARLIO receive loop. Complete the hardware
     * teardown here, before publishing task exit, so a stop timeout cannot
     * leave an old PARLIO handle behind for the next start. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    free_latest();
    xSemaphoreGive(s_mutex);
    camera_hw_deinit();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_task_exit_sem != NULL) {
        (void)xSemaphoreGive(s_task_exit_sem);
    }
    xSemaphoreGive(s_mutex);
    /* This task uses xTaskCreatePinnedToCoreWithCaps(); the owner must reclaim
     * its stack with vTaskDeleteWithCaps() after the task is suspended. */
    vTaskSuspend(NULL);
}

esp_err_t app_camera_start(void)
{
    ensure_mutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_running) {
        s_refcount++;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (s_task != NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "camera start rejected while previous task exits");
        return ESP_ERR_INVALID_STATE;
    }
    while (s_task_exit_sem != NULL &&
           xSemaphoreTake(s_task_exit_sem, 0) == pdTRUE) {
    }
    xSemaphoreGive(s_mutex);

    esp_err_t ret = camera_hw_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_running = true;
    s_refcount = 1;
    s_frame_count = 0;
    free_latest();
    /* Keep the capture loop above the background AFE fetch task (prio 5):
     * while XiaoZhi listens, the mic pipeline runs continuously and used to
     * delay the frame copies, which made the preview skip frames. */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        camera_task, "tbao_cam", 12 * 1024, NULL, 6, &s_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        s_running = false;
        s_refcount = 0;
        free_latest();
        xSemaphoreGive(s_mutex);
        camera_hw_deinit();
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "camera capture task started");
    return ESP_OK;
}

esp_err_t app_camera_stop(void)
{
    ensure_mutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_running && s_task == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (s_running && s_refcount > 1) {
        s_refcount--;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    s_refcount = 0;
    s_running = false;
    TaskHandle_t task = s_task;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "stopping camera capture task");
    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        if (s_task_exit_sem == NULL ||
            xSemaphoreTake(s_task_exit_sem,
                           pdMS_TO_TICKS(CAMERA_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "camera task did not finish exit teardown before timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskSuspend(task);
        vTaskDeleteWithCaps(task);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_task == task) {
            s_task = NULL;
        }
        xSemaphoreGive(s_mutex);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool task_alive = s_task != NULL;
    xSemaphoreGive(s_mutex);
    if (task_alive) {
        ESP_LOGE(TAG, "camera task handle still active after exit signal");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "camera capture task stopped and hardware released");
    return ESP_OK;
}

esp_err_t app_camera_capture_jpeg(uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;
    ensure_mutex();
    if (s_mutex == NULL || !app_camera_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t start_count;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    start_count = s_frame_count;
    xSemaphoreGive(s_mutex);
    int waited = 0;
    while (waited++ < 300) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool ready = s_latest != NULL && s_latest_len > 0 && s_frame_count != start_count;
        xSemaphoreGive(s_mutex);
        if (ready) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_latest == NULL || s_latest_len == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_TIMEOUT;
    }
    uint8_t *copy = heap_caps_malloc(s_latest_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_latest, s_latest_len);
    *out_buf = copy;
    *out_len = s_latest_len;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void app_camera_release_jpeg(uint8_t *buf)
{
    if (buf != NULL) {
        heap_caps_free(buf);
    }
}

esp_err_t app_camera_copy_latest_jpeg(uint8_t *out_buf, size_t out_capacity,
                                      size_t *out_len, uint32_t *out_sequence)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;
    if (out_sequence != NULL) {
        *out_sequence = 0;
    }
    ensure_mutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_latest == NULL || s_latest_len == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out_len = s_latest_len;
    if (out_sequence != NULL) {
        *out_sequence = s_frame_count;
    }
    if (out_capacity < s_latest_len) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_buf, s_latest, s_latest_len);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t app_camera_get_latest_jpeg_info(size_t *out_len, uint32_t *out_sequence)
{
    if (out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;
    if (out_sequence != NULL) {
        *out_sequence = 0;
    }
    ensure_mutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_latest == NULL || s_latest_len == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out_len = s_latest_len;
    if (out_sequence != NULL) {
        *out_sequence = s_frame_count;
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t app_camera_register_frame_cb(app_camera_frame_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_mutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
        if (s_callbacks[i].cb == cb) {
            s_callbacks[i].ctx = ctx;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
        if (s_callbacks[i].cb == NULL) {
            s_callbacks[i].cb = cb;
            s_callbacks[i].ctx = ctx;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t app_camera_unregister_frame_cb(app_camera_frame_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_mutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
        if (s_callbacks[i].cb == cb) {
            s_callbacks[i].cb = NULL;
            s_callbacks[i].ctx = NULL;
            found = true;
        }
    }
    xSemaphoreGive(s_mutex);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool app_camera_is_running(void)
{
    ensure_mutex();
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool running = s_running;
    xSemaphoreGive(s_mutex);
    return running;
}
