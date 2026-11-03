/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_avi_demux.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVI_MAX_SAMPLES       1000000U
#define AVI_MAX_NESTING       8U
#define AVI_INITIAL_CAPACITY  128U

#ifdef ESP_PLATFORM
#define APP_AVI_LOGE(...) ESP_LOGE("APP_AVI", __VA_ARGS__)
#else
#define APP_AVI_LOGE(...) do { } while (0)
#endif

typedef struct {
    bool strh_seen;
    bool strf_seen;
    bool is_video;
    bool handler_mjpeg;
    bool compression_mjpeg;
    uint32_t scale;
    uint32_t rate;
    int32_t width;
    int32_t height;
} avi_stream_t;

struct app_avi_demux {
    FILE *file;
    uint64_t file_size;

    bool hdrl_seen;
    bool video_stream_seen;
    bool video_stream_supported;
    uint32_t video_stream_index;
    uint32_t video_stream_scale;
    uint32_t video_stream_rate;
    int32_t video_stream_width;
    int32_t video_stream_height;

    uint32_t avih_microseconds_per_frame;
    uint32_t avih_total_frames;
    uint32_t avih_width;
    uint32_t avih_height;

    uint64_t movi_start;
    uint64_t movi_end;

    app_avi_video_info_t video_info;
    app_avi_sample_t *video_samples;
    uint32_t video_sample_capacity;
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool fourcc_is(const void *value, const char expected[4])
{
    return memcmp(value, expected, 4) == 0;
}

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool fourcc_is_ignore_case(const void *value, const char expected[4])
{
    const uint8_t *bytes = (const uint8_t *)value;

    for (size_t i = 0; i < 4; ++i) {
        if (ascii_lower((char)bytes[i]) != ascii_lower(expected[i])) {
            return false;
        }
    }
    return true;
}

static app_avi_demux_err_t read_at(FILE *file, uint64_t file_size,
                                   uint64_t offset, void *buffer, size_t size)
{
    if (file == NULL || buffer == NULL || offset > file_size ||
        (uint64_t)size > file_size - offset || offset > (uint64_t)LONG_MAX) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        return APP_AVI_DEMUX_ERR_IO;
    }
    return fread(buffer, 1, size, file) == size
        ? APP_AVI_DEMUX_OK : APP_AVI_DEMUX_ERR_IO;
}

static app_avi_demux_err_t read_chunk_header(
    FILE *file, uint64_t file_size, uint64_t container_end, uint64_t offset,
    char type[4],
    uint32_t *payload_size, uint64_t *data_offset, uint64_t *next_offset)
{
    if (type == NULL || payload_size == NULL || data_offset == NULL ||
        next_offset == NULL || container_end > file_size ||
        offset > container_end || container_end - offset < 8) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }

    uint8_t header[8];
    app_avi_demux_err_t ret = read_at(file, file_size, offset, header,
                                      sizeof(header));
    if (ret != APP_AVI_DEMUX_OK) {
        return ret;
    }
    memcpy(type, header, 4);
    *payload_size = le32(header + 4);
    *data_offset = offset + sizeof(header);
    if ((uint64_t)*payload_size > container_end - *data_offset) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }

    uint64_t end = *data_offset + *payload_size;
    if ((*payload_size & 1U) != 0 && end < container_end) {
        end++;
    }
    if (end > container_end) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    *next_offset = end;
    return APP_AVI_DEMUX_OK;
}

static uint32_t abs_dimension(int32_t value)
{
    if (value == INT32_MIN) {
        return 0;
    }
    return value < 0 ? (uint32_t)(-value) : (uint32_t)value;
}

static app_avi_demux_err_t parse_avih(app_avi_demux_t *demux,
                                      uint64_t data_offset,
                                      uint32_t payload_size)
{
    if (payload_size < 40) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    uint8_t payload[56] = {0};
    size_t read_size = payload_size < sizeof(payload) ? payload_size :
                       sizeof(payload);
    app_avi_demux_err_t ret = read_at(demux->file, demux->file_size,
                                      data_offset, payload, read_size);
    if (ret != APP_AVI_DEMUX_OK) {
        return ret;
    }
    demux->avih_microseconds_per_frame = le32(payload);
    demux->avih_total_frames = le32(payload + 16);
    demux->avih_width = le32(payload + 32);
    demux->avih_height = le32(payload + 36);
    return APP_AVI_DEMUX_OK;
}

static app_avi_demux_err_t parse_strh(avi_stream_t *stream,
                                      FILE *file, uint64_t file_size,
                                      uint64_t data_offset,
                                      uint32_t payload_size)
{
    if (payload_size < 48) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    uint8_t payload[48];
    app_avi_demux_err_t ret = read_at(file, file_size, data_offset, payload,
                                      sizeof(payload));
    if (ret != APP_AVI_DEMUX_OK) {
        return ret;
    }
    stream->strh_seen = true;
    stream->is_video = fourcc_is(payload, "vids");
    stream->handler_mjpeg = fourcc_is_ignore_case(payload + 4, "MJPG");
    stream->scale = le32(payload + 20);
    stream->rate = le32(payload + 24);
    return APP_AVI_DEMUX_OK;
}

static app_avi_demux_err_t parse_strf(avi_stream_t *stream,
                                      FILE *file, uint64_t file_size,
                                      uint64_t data_offset,
                                      uint32_t payload_size)
{
    /* Audio WAVEFORMATEX blocks are commonly shorter than a video
     * BITMAPINFOHEADER, so only require the 40-byte form for video streams. */
    if (payload_size < 40 && stream->is_video) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    if (payload_size < 40) {
        return APP_AVI_DEMUX_OK;
    }
    uint8_t payload[40];
    app_avi_demux_err_t ret = read_at(file, file_size, data_offset, payload,
                                      sizeof(payload));
    if (ret != APP_AVI_DEMUX_OK) {
        return ret;
    }
    stream->strf_seen = true;
    stream->width = (int32_t)le32(payload + 4);
    stream->height = (int32_t)le32(payload + 8);
    stream->compression_mjpeg = fourcc_is_ignore_case(payload + 16, "MJPG");
    return APP_AVI_DEMUX_OK;
}

static app_avi_demux_err_t parse_strl(app_avi_demux_t *demux,
                                      uint64_t list_start, uint64_t list_end,
                                      avi_stream_t *out_stream)
{
    avi_stream_t stream = {0};
    uint64_t pos = list_start;
    while (pos < list_end) {
        if (list_end - pos < 8) {
            return APP_AVI_DEMUX_ERR_CORRUPT;
        }
        char type[4];
        uint32_t payload_size = 0;
        uint64_t data_offset = 0;
        uint64_t next = 0;
        app_avi_demux_err_t ret = read_chunk_header(
            demux->file, demux->file_size, list_end, pos, type,
            &payload_size, &data_offset, &next);
        if (ret != APP_AVI_DEMUX_OK || next > list_end || next <= pos) {
            return ret == APP_AVI_DEMUX_OK ? APP_AVI_DEMUX_ERR_CORRUPT : ret;
        }
        if (fourcc_is(type, "strh")) {
            ret = parse_strh(&stream, demux->file, demux->file_size,
                             data_offset, payload_size);
        } else if (fourcc_is(type, "strf")) {
            ret = parse_strf(&stream, demux->file, demux->file_size,
                             data_offset, payload_size);
        }
        if (ret != APP_AVI_DEMUX_OK) {
            return ret;
        }
        pos = next;
    }
    if (out_stream != NULL) {
        *out_stream = stream;
    }
    return APP_AVI_DEMUX_OK;
}

static app_avi_demux_err_t parse_hdrl(app_avi_demux_t *demux,
                                      uint64_t list_start, uint64_t list_end)
{
    uint64_t pos = list_start;
    uint32_t stream_index = 0;
    while (pos < list_end) {
        if (list_end - pos < 8) {
            return APP_AVI_DEMUX_ERR_CORRUPT;
        }
        char type[4];
        uint32_t payload_size = 0;
        uint64_t data_offset = 0;
        uint64_t next = 0;
        app_avi_demux_err_t ret = read_chunk_header(
            demux->file, demux->file_size, list_end, pos, type,
            &payload_size, &data_offset, &next);
        if (ret != APP_AVI_DEMUX_OK || next > list_end || next <= pos) {
            return ret == APP_AVI_DEMUX_OK ? APP_AVI_DEMUX_ERR_CORRUPT : ret;
        }
        if (fourcc_is(type, "avih")) {
            ret = parse_avih(demux, data_offset, payload_size);
        } else if (fourcc_is(type, "LIST")) {
            if (payload_size < 4) {
                return APP_AVI_DEMUX_ERR_CORRUPT;
            }
            uint8_t list_type[4];
            ret = read_at(demux->file, demux->file_size, data_offset,
                          list_type, sizeof(list_type));
            if (ret == APP_AVI_DEMUX_OK && fourcc_is(list_type, "strl")) {
                avi_stream_t stream;
                ret = parse_strl(demux, data_offset + 4,
                                 data_offset + payload_size, &stream);
                if (ret == APP_AVI_DEMUX_OK && stream.is_video &&
                    !demux->video_stream_seen) {
                    demux->video_stream_seen = true;
                    demux->video_stream_supported =
                        stream.handler_mjpeg || stream.compression_mjpeg;
                    demux->video_stream_index = stream_index;
                    demux->video_stream_scale = stream.scale;
                    demux->video_stream_rate = stream.rate;
                    demux->video_stream_width = stream.width;
                    demux->video_stream_height = stream.height;
                }
                stream_index++;
            }
        }
        if (ret != APP_AVI_DEMUX_OK) {
            return ret;
        }
        pos = next;
    }
    demux->hdrl_seen = true;
    return APP_AVI_DEMUX_OK;
}

static app_avi_demux_err_t append_video_sample(app_avi_demux_t *demux,
                                               uint64_t offset,
                                               uint32_t size)
{
    if (size == 0 || demux->video_info.sample_count >= AVI_MAX_SAMPLES) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    if (demux->video_info.sample_count == demux->video_sample_capacity) {
        uint32_t new_capacity = demux->video_sample_capacity == 0
            ? AVI_INITIAL_CAPACITY : demux->video_sample_capacity * 2U;
        if (new_capacity < demux->video_sample_capacity ||
            new_capacity > AVI_MAX_SAMPLES) {
            new_capacity = AVI_MAX_SAMPLES;
        }
        if (new_capacity < demux->video_info.sample_count + 1U ||
            (size_t)new_capacity > SIZE_MAX / sizeof(app_avi_sample_t)) {
            return APP_AVI_DEMUX_ERR_NO_MEMORY;
        }
        app_avi_sample_t *samples = realloc(
            demux->video_samples,
            (size_t)new_capacity * sizeof(app_avi_sample_t));
        if (samples == NULL) {
            return APP_AVI_DEMUX_ERR_NO_MEMORY;
        }
        demux->video_samples = samples;
        demux->video_sample_capacity = new_capacity;
    }

    uint64_t index = demux->video_info.sample_count;
    uint64_t pts_ms = 0;
    uint64_t next_pts_ms = 0;
    if (demux->video_info.frame_rate_num != 0 &&
        demux->video_info.frame_rate_den != 0) {
        pts_ms = index * 1000ULL * demux->video_info.frame_rate_den /
                 demux->video_info.frame_rate_num;
        next_pts_ms = (index + 1ULL) * 1000ULL *
                      demux->video_info.frame_rate_den /
                      demux->video_info.frame_rate_num;
    } else {
        pts_ms = index * 33ULL;
        next_pts_ms = (index + 1ULL) * 33ULL;
    }
    uint64_t duration_ms = next_pts_ms > pts_ms ? next_pts_ms - pts_ms : 1;
    demux->video_samples[index] = (app_avi_sample_t) {
        .offset = offset,
        .size = size,
        .pts_ms = pts_ms,
        .duration_ms = duration_ms > UINT32_MAX ? UINT32_MAX :
                       (uint32_t)duration_ms,
    };
    demux->video_info.sample_count++;
    demux->video_info.duration_ms = pts_ms + duration_ms;
    return APP_AVI_DEMUX_OK;
}

static bool is_video_chunk(const char type[4], uint32_t stream_index)
{
    if (stream_index > 99 || (type[2] != 'd' && type[2] != 'D') ||
        (type[3] != 'c' && type[3] != 'C' &&
         type[3] != 'b' && type[3] != 'B')) {
        return false;
    }
    const char stream_id[2] = {
        (char)('0' + stream_index / 10U),
        (char)('0' + stream_index % 10U),
    };
    return type[0] == stream_id[0] && type[1] == stream_id[1];
}

static app_avi_demux_err_t scan_movi_chunks(app_avi_demux_t *demux,
                                            uint64_t start, uint64_t end,
                                            uint32_t depth)
{
    if (depth > AVI_MAX_NESTING || start > end || end > demux->file_size) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }
    uint64_t pos = start;
    while (pos < end) {
        if (end - pos < 8) {
            return APP_AVI_DEMUX_ERR_CORRUPT;
        }
        char type[4];
        uint32_t payload_size = 0;
        uint64_t data_offset = 0;
        uint64_t next = 0;
        app_avi_demux_err_t ret = read_chunk_header(
            demux->file, demux->file_size, end, pos, type, &payload_size,
            &data_offset, &next);
        if (ret != APP_AVI_DEMUX_OK || next > end || next <= pos) {
            return ret == APP_AVI_DEMUX_OK ? APP_AVI_DEMUX_ERR_CORRUPT : ret;
        }
        if (fourcc_is(type, "LIST") || fourcc_is(type, "RIFF")) {
            if (payload_size < 4) {
                return APP_AVI_DEMUX_ERR_CORRUPT;
            }
            ret = scan_movi_chunks(demux, data_offset + 4,
                                   data_offset + payload_size, depth + 1U);
        } else if (is_video_chunk(type, demux->video_stream_index)) {
            ret = append_video_sample(demux, data_offset, payload_size);
        }
        if (ret != APP_AVI_DEMUX_OK) {
            return ret;
        }
        pos = next;
    }
    return APP_AVI_DEMUX_OK;
}

static app_avi_demux_err_t finalize_video_info(app_avi_demux_t *demux)
{
    if (!demux->hdrl_seen || !demux->video_stream_seen ||
        !demux->video_stream_supported || demux->movi_start >= demux->movi_end) {
        return APP_AVI_DEMUX_ERR_UNSUPPORTED;
    }

    uint32_t width = abs_dimension(demux->video_stream_width);
    uint32_t height = abs_dimension(demux->video_stream_height);
    if (width == 0) {
        width = demux->avih_width;
    }
    if (height == 0) {
        height = demux->avih_height;
    }
    if (width == 0 || height == 0) {
        return APP_AVI_DEMUX_ERR_CORRUPT;
    }

    uint32_t rate = demux->video_stream_rate;
    uint32_t scale = demux->video_stream_scale;
    if (rate == 0 || scale == 0) {
        if (demux->avih_microseconds_per_frame == 0) {
            return APP_AVI_DEMUX_ERR_CORRUPT;
        }
        rate = 1000000U;
        scale = demux->avih_microseconds_per_frame;
    }

    demux->video_info.codec = APP_AVI_VIDEO_CODEC_MJPEG;
    demux->video_info.width = width;
    demux->video_info.height = height;
    demux->video_info.frame_rate_num = rate;
    demux->video_info.frame_rate_den = scale;
    return APP_AVI_DEMUX_OK;
}

app_avi_demux_err_t app_avi_demux_open(const char *path,
                                       app_avi_demux_t **out_demux)
{
    if (path == NULL || out_demux == NULL) {
        return APP_AVI_DEMUX_ERR_INVALID_ARG;
    }
    *out_demux = NULL;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return APP_AVI_DEMUX_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return APP_AVI_DEMUX_ERR_IO;
    }
    long file_length = ftell(file);
    if (file_length < 12 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return APP_AVI_DEMUX_ERR_IO;
    }

    app_avi_demux_t *demux = calloc(1, sizeof(*demux));
    if (demux == NULL) {
        fclose(file);
        return APP_AVI_DEMUX_ERR_NO_MEMORY;
    }
    demux->file = file;
    demux->file_size = (uint64_t)file_length;

    uint8_t riff_header[12];
    app_avi_demux_err_t ret = read_at(file, demux->file_size, 0,
                                      riff_header, sizeof(riff_header));
    if (ret != APP_AVI_DEMUX_OK) {
        app_avi_demux_close(demux);
        return ret;
    }
    if (!fourcc_is(riff_header, "RIFF") ||
        !fourcc_is(riff_header + 8, "AVI ")) {
        app_avi_demux_close(demux);
        return APP_AVI_DEMUX_ERR_NOT_AVI;
    }

    uint64_t pos = 12;
    while (pos < demux->file_size) {
        if (demux->file_size - pos < 8) {
            app_avi_demux_close(demux);
            return APP_AVI_DEMUX_ERR_CORRUPT;
        }
        char type[4];
        uint32_t payload_size = 0;
        uint64_t data_offset = 0;
        uint64_t next = 0;
        ret = read_chunk_header(file, demux->file_size, demux->file_size,
                                pos, type, &payload_size, &data_offset,
                                &next);
        if (ret != APP_AVI_DEMUX_OK || next <= pos) {
            app_avi_demux_close(demux);
            return ret == APP_AVI_DEMUX_OK ? APP_AVI_DEMUX_ERR_CORRUPT : ret;
        }
        if (fourcc_is(type, "LIST")) {
            if (payload_size < 4) {
                app_avi_demux_close(demux);
                return APP_AVI_DEMUX_ERR_CORRUPT;
            }
            uint8_t list_type[4];
            ret = read_at(file, demux->file_size, data_offset, list_type,
                          sizeof(list_type));
            if (ret == APP_AVI_DEMUX_OK && fourcc_is(list_type, "hdrl")) {
                ret = parse_hdrl(demux, data_offset + 4,
                                 data_offset + payload_size);
            } else if (ret == APP_AVI_DEMUX_OK && fourcc_is(list_type, "movi") &&
                       demux->movi_start == 0) {
                demux->movi_start = data_offset + 4;
                demux->movi_end = data_offset + payload_size;
            }
            if (ret != APP_AVI_DEMUX_OK) {
                app_avi_demux_close(demux);
                return ret;
            }
        }
        pos = next;
    }

    ret = finalize_video_info(demux);
    if (ret == APP_AVI_DEMUX_OK) {
        ret = scan_movi_chunks(demux, demux->movi_start, demux->movi_end, 0);
    }
    if (ret != APP_AVI_DEMUX_OK || demux->video_info.sample_count == 0) {
        APP_AVI_LOGE("AVI index failed: ret=%d samples=%" PRIu32,
                     ret, demux->video_info.sample_count);
        app_avi_demux_close(demux);
        return ret == APP_AVI_DEMUX_OK ? APP_AVI_DEMUX_ERR_CORRUPT : ret;
    }
    *out_demux = demux;
    return APP_AVI_DEMUX_OK;
}

void app_avi_demux_close(app_avi_demux_t *demux)
{
    if (demux == NULL) {
        return;
    }
    if (demux->file != NULL) {
        fclose(demux->file);
    }
    free(demux->video_samples);
    free(demux);
}

const app_avi_video_info_t *app_avi_demux_video_info(
    const app_avi_demux_t *demux)
{
    return demux != NULL && demux->video_info.codec != APP_AVI_VIDEO_CODEC_NONE
        ? &demux->video_info : NULL;
}

app_avi_demux_err_t app_avi_demux_get_video_sample(
    const app_avi_demux_t *demux, uint32_t index,
    app_avi_sample_t *out_sample)
{
    if (demux == NULL || out_sample == NULL) {
        return APP_AVI_DEMUX_ERR_INVALID_ARG;
    }
    if (demux->video_samples == NULL ||
        index >= demux->video_info.sample_count) {
        return APP_AVI_DEMUX_ERR_NOT_FOUND;
    }
    *out_sample = demux->video_samples[index];
    return APP_AVI_DEMUX_OK;
}

app_avi_demux_err_t app_avi_demux_read_video_sample(
    app_avi_demux_t *demux, uint32_t index, uint8_t *buffer,
    size_t buffer_size, app_avi_sample_t *out_sample)
{
    if (demux == NULL || demux->file == NULL || buffer == NULL) {
        return APP_AVI_DEMUX_ERR_INVALID_ARG;
    }
    app_avi_sample_t sample;
    app_avi_demux_err_t ret = app_avi_demux_get_video_sample(
        demux, index, &sample);
    if (ret != APP_AVI_DEMUX_OK) {
        return ret;
    }
    if (buffer_size < sample.size) {
        return APP_AVI_DEMUX_ERR_BUFFER_TOO_SMALL;
    }
    if (sample.offset > (uint64_t)LONG_MAX ||
        fseek(demux->file, (long)sample.offset, SEEK_SET) != 0 ||
        fread(buffer, 1, sample.size, demux->file) != sample.size) {
        return APP_AVI_DEMUX_ERR_IO;
    }
    if (out_sample != NULL) {
        *out_sample = sample;
    }
    return APP_AVI_DEMUX_OK;
}
