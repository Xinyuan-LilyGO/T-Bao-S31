/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small, read-only ISO-BMFF indexer for local MP4 playback.
 *
 * This deliberately handles the sample tables needed by the device player
 * instead of attempting to be a general-purpose MOV parser. The compressed
 * samples remain on the SD card and are read on demand.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MP4_DEMUX_OK = 0,
    APP_MP4_DEMUX_ERR_INVALID_ARG,
    APP_MP4_DEMUX_ERR_NO_MEMORY,
    APP_MP4_DEMUX_ERR_IO,
    APP_MP4_DEMUX_ERR_NOT_MP4,
    APP_MP4_DEMUX_ERR_UNSUPPORTED,
    APP_MP4_DEMUX_ERR_CORRUPT,
    APP_MP4_DEMUX_ERR_NOT_FOUND,
    APP_MP4_DEMUX_ERR_BUFFER_TOO_SMALL,
} app_mp4_demux_err_t;

typedef enum {
    APP_MP4_VIDEO_CODEC_NONE = 0,
    APP_MP4_VIDEO_CODEC_MJPEG,
    APP_MP4_VIDEO_CODEC_H264,
} app_mp4_video_codec_t;

typedef enum {
    APP_MP4_AUDIO_CODEC_NONE = 0,
    APP_MP4_AUDIO_CODEC_AAC,
    APP_MP4_AUDIO_CODEC_MP3,
} app_mp4_audio_codec_t;

typedef struct {
    uint64_t offset;
    uint32_t size;
    uint64_t pts_ms;
    uint32_t duration_ms;
} app_mp4_sample_t;

typedef struct {
    app_mp4_video_codec_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t timescale;
    uint64_t duration_ms;
    uint32_t sample_count;
} app_mp4_video_info_t;

typedef struct {
    app_mp4_audio_codec_t codec;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t timescale;
    uint64_t duration_ms;
    uint32_t sample_count;
} app_mp4_audio_info_t;

typedef struct app_mp4_demux app_mp4_demux_t;

/** Open a local MP4 and index its moov atom. */
app_mp4_demux_err_t app_mp4_demux_open(const char *path,
                                       app_mp4_demux_t **out_demux);

/** Parse an in-memory moov atom. Intended for host-side parser tests. */
app_mp4_demux_err_t app_mp4_demux_parse_moov(const uint8_t *moov,
                                             size_t moov_size,
                                             uint64_t file_size,
                                             app_mp4_demux_t **out_demux);

void app_mp4_demux_close(app_mp4_demux_t *demux);

const app_mp4_video_info_t *app_mp4_demux_video_info(
    const app_mp4_demux_t *demux);
const app_mp4_audio_info_t *app_mp4_demux_audio_info(
    const app_mp4_demux_t *demux);

app_mp4_demux_err_t app_mp4_demux_get_video_sample(
    const app_mp4_demux_t *demux, uint32_t index, app_mp4_sample_t *out_sample);

/** Read one indexed video sample from the MP4 file. */
app_mp4_demux_err_t app_mp4_demux_read_video_sample(
    app_mp4_demux_t *demux, uint32_t index, uint8_t *buffer,
    size_t buffer_size, app_mp4_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
