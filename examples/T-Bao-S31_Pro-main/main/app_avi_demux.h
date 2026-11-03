/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small, read-only AVI indexer for local MJPEG playback.
 *
 * The parser intentionally supports the classic RIFF AVI layout used by the
 * device player: one MJPEG video stream and frame chunks inside LIST/movi.
 * Encoded frames stay on the SD card and are read on demand.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_AVI_DEMUX_OK = 0,
    APP_AVI_DEMUX_ERR_INVALID_ARG,
    APP_AVI_DEMUX_ERR_NO_MEMORY,
    APP_AVI_DEMUX_ERR_IO,
    APP_AVI_DEMUX_ERR_NOT_AVI,
    APP_AVI_DEMUX_ERR_UNSUPPORTED,
    APP_AVI_DEMUX_ERR_CORRUPT,
    APP_AVI_DEMUX_ERR_NOT_FOUND,
    APP_AVI_DEMUX_ERR_BUFFER_TOO_SMALL,
} app_avi_demux_err_t;

typedef enum {
    APP_AVI_VIDEO_CODEC_NONE = 0,
    APP_AVI_VIDEO_CODEC_MJPEG,
} app_avi_video_codec_t;

typedef struct {
    uint64_t offset;
    uint32_t size;
    uint64_t pts_ms;
    uint32_t duration_ms;
} app_avi_sample_t;

typedef struct {
    app_avi_video_codec_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint64_t duration_ms;
    uint32_t sample_count;
} app_avi_video_info_t;

typedef struct app_avi_demux app_avi_demux_t;

/** Open a classic RIFF AVI and index its MJPEG frames. */
app_avi_demux_err_t app_avi_demux_open(const char *path,
                                       app_avi_demux_t **out_demux);

void app_avi_demux_close(app_avi_demux_t *demux);

const app_avi_video_info_t *app_avi_demux_video_info(
    const app_avi_demux_t *demux);

app_avi_demux_err_t app_avi_demux_get_video_sample(
    const app_avi_demux_t *demux, uint32_t index,
    app_avi_sample_t *out_sample);

/** Read one indexed MJPEG frame from the AVI file. */
app_avi_demux_err_t app_avi_demux_read_video_sample(
    app_avi_demux_t *demux, uint32_t index, uint8_t *buffer,
    size_t buffer_size, app_avi_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
