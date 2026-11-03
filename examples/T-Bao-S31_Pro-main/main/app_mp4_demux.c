/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_mp4_demux.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MP4_MAX_TRACKS 8
#define MP4_MAX_TABLE_ENTRIES 1000000U

#define TFHD_BASE_DATA_OFFSET_PRESENT 0x000001U
#define TFHD_SAMPLE_DESCRIPTION_INDEX_PRESENT 0x000002U
#define TFHD_DEFAULT_SAMPLE_DURATION_PRESENT 0x000008U
#define TFHD_DEFAULT_SAMPLE_SIZE_PRESENT 0x000010U
#define TFHD_DEFAULT_SAMPLE_FLAGS_PRESENT 0x000020U
#define TFHD_DEFAULT_BASE_IS_MOOF 0x020000U

#define TRUN_DATA_OFFSET_PRESENT 0x000001U
#define TRUN_FIRST_SAMPLE_FLAGS_PRESENT 0x000004U
#define TRUN_SAMPLE_DURATION_PRESENT 0x000100U
#define TRUN_SAMPLE_SIZE_PRESENT 0x000200U
#define TRUN_SAMPLE_FLAGS_PRESENT 0x000400U
#define TRUN_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT 0x000800U

#ifdef ESP_PLATFORM
#define APP_MP4_LOGE(...) ESP_LOGE("APP_MP4", __VA_ARGS__)
#else
#define APP_MP4_LOGE(...) do { } while (0)
#endif

typedef struct {
    uint32_t count;
    uint32_t delta;
} stts_entry_t;

typedef struct {
    bool valid;
    uint32_t track_id;
    uint32_t sample_description_index;
    uint32_t default_sample_duration;
    uint32_t default_sample_size;
    uint32_t default_sample_flags;
} mp4_trex_t;

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t sample_description_index;
} stsc_entry_t;

typedef struct {
    char handler[4];
    uint32_t track_id;
    app_mp4_video_codec_t video_codec;
    app_mp4_audio_codec_t audio_codec;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t timescale;
    uint64_t track_duration;

    stts_entry_t *stts;
    uint32_t stts_count;
    stsc_entry_t *stsc;
    uint32_t stsc_count;
    uint64_t *chunk_offsets;
    uint32_t chunk_count;
    uint32_t sample_count;
    uint32_t fixed_sample_size;
    uint32_t *sample_sizes;
    app_mp4_sample_t *samples;
    bool video_sample_requires_probe;
} mp4_track_t;

struct app_mp4_demux {
    FILE *file;
    uint64_t file_size;
    uint8_t *moov;
    size_t moov_size;
    app_mp4_video_info_t video_info;
    app_mp4_audio_info_t audio_info;
    app_mp4_sample_t *video_samples;
    uint32_t video_sample_capacity;
    app_mp4_sample_t *audio_samples;
    bool video_sample_requires_probe;
    bool fragmented;
    uint32_t video_track_id;
    mp4_trex_t video_trex;
    uint64_t fragment_decode_time;
};

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t header_size;
    const uint8_t *payload;
    size_t payload_size;
    char type[4];
} mp4_atom_t;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static bool type_is(const char type[4], const char expected[4])
{
    return memcmp(type, expected, 4) == 0;
}

static bool type_is_container(const char type[4])
{
    static const char containers[][4] = {
        {'m', 'o', 'o', 'v'}, {'t', 'r', 'a', 'k'}, {'m', 'd', 'i', 'a'},
        {'m', 'i', 'n', 'f'}, {'s', 't', 'b', 'l'}, {'e', 'd', 't', 's'},
        {'d', 'i', 'n', 'f'}, {'u', 'd', 't', 'a'}, {'m', 'v', 'e', 'x'},
        {'t', 'r', 'e', 'f'}, {'m', 'o', 'o', 'f'}, {'t', 'r', 'a', 'f'},
    };
    for (size_t i = 0; i < sizeof(containers) / sizeof(containers[0]); ++i) {
        if (type_is(type, containers[i])) {
            return true;
        }
    }
    return false;
}

static app_mp4_demux_err_t atom_open(const uint8_t *data, size_t available,
                                     mp4_atom_t *out)
{
    if (data == NULL || out == NULL || available < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint64_t atom_size = be32(data);
    size_t header_size = 8;
    if (atom_size == 1) {
        if (available < 16) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        atom_size = be64(data + 8);
        header_size = 16;
    } else if (atom_size == 0) {
        atom_size = available;
    }
    if (atom_size < header_size || atom_size > available || atom_size > SIZE_MAX) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    memcpy(out->type, data + 4, 4);
    out->data = data;
    out->size = (size_t)atom_size;
    out->header_size = header_size;
    out->payload = data + header_size;
    out->payload_size = out->size - header_size;
    return APP_MP4_DEMUX_OK;
}

static bool count_is_valid(uint32_t count, size_t item_size)
{
    return count <= MP4_MAX_TABLE_ENTRIES &&
           (size_t)count <= SIZE_MAX / item_size;
}

static void free_track(mp4_track_t *track)
{
    if (track == NULL) {
        return;
    }
    free(track->stts);
    free(track->stsc);
    free(track->chunk_offsets);
    free(track->sample_sizes);
    free(track->samples);
    memset(track, 0, sizeof(*track));
}

static app_mp4_demux_err_t parse_mdhd(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 20) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint8_t version = atom->payload[0];
    if (version == 0) {
        if (atom->payload_size < 20) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->timescale = be32(atom->payload + 12);
        track->track_duration = be32(atom->payload + 16);
    } else if (version == 1) {
        if (atom->payload_size < 32) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->timescale = be32(atom->payload + 20);
        track->track_duration = be64(atom->payload + 24);
    } else {
        return APP_MP4_DEMUX_ERR_UNSUPPORTED;
    }
    return track->timescale == 0 ? APP_MP4_DEMUX_ERR_CORRUPT
                                 : APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_tkhd(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 20) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint8_t version = atom->payload[0];
    if (version == 0) {
        track->track_id = be32(atom->payload + 12);
    } else if (version == 1) {
        if (atom->payload_size < 32) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->track_id = be32(atom->payload + 20);
    } else {
        return APP_MP4_DEMUX_ERR_UNSUPPORTED;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_hdlr(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 12) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    memcpy(track->handler, atom->payload + 8, 4);
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_stsd(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint32_t entry_count = be32(atom->payload + 4);
    if (entry_count == 0) {
        return APP_MP4_DEMUX_ERR_UNSUPPORTED;
    }
    mp4_atom_t entry;
    app_mp4_demux_err_t ret = atom_open(atom->payload + 8,
                                        atom->payload_size - 8, &entry);
    if (ret != APP_MP4_DEMUX_OK) {
        return ret;
    }
    if (type_is(track->handler, "vide")) {
        if (type_is(entry.type, "mjpg") || type_is(entry.type, "jpeg")) {
            track->video_codec = APP_MP4_VIDEO_CODEC_MJPEG;
        } else if (type_is(entry.type, "mp4v")) {
            /* FFmpeg writes MJPEG-in-MP4 with an mp4v sample entry. The
             * elementary samples are still standalone JPEG frames, so defer
             * the final decision until the first sample can be inspected. */
            track->video_codec = APP_MP4_VIDEO_CODEC_MJPEG;
            track->video_sample_requires_probe = true;
        } else if (type_is(entry.type, "avc1") || type_is(entry.type, "avc3")) {
            track->video_codec = APP_MP4_VIDEO_CODEC_H264;
        } else {
            track->video_codec = APP_MP4_VIDEO_CODEC_NONE;
            return APP_MP4_DEMUX_ERR_UNSUPPORTED;
        }
        if (entry.size < 36) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->width = be16(entry.data + 32);
        track->height = be16(entry.data + 34);
        if (track->width == 0 || track->height == 0) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
    } else if (type_is(track->handler, "soun")) {
        if (type_is(entry.type, "mp4a") || type_is(entry.type, "enca")) {
            track->audio_codec = APP_MP4_AUDIO_CODEC_AAC;
        } else if (type_is(entry.type, "mp3 ") || type_is(entry.type, ".mp3")) {
            track->audio_codec = APP_MP4_AUDIO_CODEC_MP3;
        } else {
            track->audio_codec = APP_MP4_AUDIO_CODEC_NONE;
            return APP_MP4_DEMUX_ERR_UNSUPPORTED;
        }
        if (entry.size < 36) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->channels = be16(entry.data + 24);
        track->sample_rate = be32(entry.data + 32) >> 16;
        if (track->channels == 0 || track->sample_rate == 0) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_stts(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint32_t count = be32(atom->payload + 4);
    if (!count_is_valid(count, sizeof(stts_entry_t) * 2) ||
        atom->payload_size < 8 + (size_t)count * 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    free(track->stts);
    track->stts = calloc(count, sizeof(*track->stts));
    if (count != 0 && track->stts == NULL) {
        return APP_MP4_DEMUX_ERR_NO_MEMORY;
    }
    track->stts_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        track->stts[i].count = be32(atom->payload + 8 + i * 8);
        track->stts[i].delta = be32(atom->payload + 12 + i * 8);
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_stsc(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint32_t count = be32(atom->payload + 4);
    if (!count_is_valid(count, sizeof(stsc_entry_t) * 3) ||
        atom->payload_size < 8 + (size_t)count * 12) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    free(track->stsc);
    track->stsc = calloc(count, sizeof(*track->stsc));
    if (count != 0 && track->stsc == NULL) {
        return APP_MP4_DEMUX_ERR_NO_MEMORY;
    }
    track->stsc_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        track->stsc[i].first_chunk = be32(atom->payload + 8 + i * 12);
        track->stsc[i].samples_per_chunk = be32(atom->payload + 12 + i * 12);
        track->stsc[i].sample_description_index = be32(atom->payload + 16 + i * 12);
        if (track->stsc[i].first_chunk == 0 ||
            track->stsc[i].samples_per_chunk == 0 ||
            track->stsc[i].sample_description_index == 0) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_stsz(mp4_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (atom->payload_size < 12) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint32_t fixed_size = be32(atom->payload + 4);
    uint32_t count = be32(atom->payload + 8);
    if (!count_is_valid(count, sizeof(uint32_t))) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    size_t expected = 12 + (fixed_size == 0 ? (size_t)count * 4 : 0);
    if (atom->payload_size < expected) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    free(track->sample_sizes);
    track->sample_sizes = NULL;
    track->fixed_sample_size = fixed_size;
    track->sample_count = count;
    if (fixed_size == 0 && count != 0) {
        track->sample_sizes = calloc(count, sizeof(*track->sample_sizes));
        if (track->sample_sizes == NULL) {
            return APP_MP4_DEMUX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < count; ++i) {
            track->sample_sizes[i] = be32(atom->payload + 12 + i * 4);
        }
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_stco(mp4_track_t *track,
                                      const mp4_atom_t *atom, bool co64)
{
    if (atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint32_t count = be32(atom->payload + 4);
    size_t item_size = co64 ? sizeof(uint64_t) : sizeof(uint32_t);
    if (!count_is_valid(count, item_size) ||
        atom->payload_size < 8 + (size_t)count * item_size) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    free(track->chunk_offsets);
    track->chunk_offsets = calloc(count, sizeof(*track->chunk_offsets));
    if (count != 0 && track->chunk_offsets == NULL) {
        return APP_MP4_DEMUX_ERR_NO_MEMORY;
    }
    track->chunk_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        track->chunk_offsets[i] = co64
            ? be64(atom->payload + 8 + i * 8)
            : be32(atom->payload + 8 + i * 4);
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_trex(mp4_trex_t *trex,
                                      const mp4_atom_t *atom)
{
    if (trex == NULL || atom->payload_size < 24) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    trex->valid = true;
    trex->track_id = be32(atom->payload + 4);
    trex->sample_description_index = be32(atom->payload + 8);
    trex->default_sample_duration = be32(atom->payload + 12);
    trex->default_sample_size = be32(atom->payload + 16);
    trex->default_sample_flags = be32(atom->payload + 20);
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_mvex(const uint8_t *data, size_t size,
                                      mp4_trex_t *trex_entries,
                                      uint32_t *trex_count)
{
    if (data == NULL || trex_entries == NULL || trex_count == NULL) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    size_t pos = 0;
    while (pos < size) {
        mp4_atom_t atom;
        app_mp4_demux_err_t ret = atom_open(data + pos, size - pos, &atom);
        if (ret != APP_MP4_DEMUX_OK) {
            return ret;
        }
        if (type_is(atom.type, "trex")) {
            if (*trex_count >= MP4_MAX_TRACKS) {
                return APP_MP4_DEMUX_ERR_UNSUPPORTED;
            }
            ret = parse_trex(&trex_entries[*trex_count], &atom);
            if (ret != APP_MP4_DEMUX_OK) {
                return ret;
            }
            (*trex_count)++;
        }
        pos += atom.size;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_track_atoms(mp4_track_t *track,
                                             const uint8_t *data, size_t size)
{
    size_t pos = 0;
    while (pos < size) {
        mp4_atom_t atom;
        app_mp4_demux_err_t ret = atom_open(data + pos, size - pos, &atom);
        if (ret != APP_MP4_DEMUX_OK) {
            return ret;
        }
        if (type_is(atom.type, "tkhd")) {
            ret = parse_tkhd(track, &atom);
        } else if (type_is(atom.type, "mdhd")) {
            ret = parse_mdhd(track, &atom);
        } else if (type_is(atom.type, "hdlr")) {
            ret = parse_hdlr(track, &atom);
        } else if (type_is(atom.type, "stsd")) {
            ret = parse_stsd(track, &atom);
        } else if (type_is(atom.type, "stts")) {
            ret = parse_stts(track, &atom);
        } else if (type_is(atom.type, "stsc")) {
            ret = parse_stsc(track, &atom);
        } else if (type_is(atom.type, "stsz")) {
            ret = parse_stsz(track, &atom);
        } else if (type_is(atom.type, "stco")) {
            ret = parse_stco(track, &atom, false);
        } else if (type_is(atom.type, "co64")) {
            ret = parse_stco(track, &atom, true);
        } else if (type_is_container(atom.type)) {
            const uint8_t *child_data = atom.payload;
            size_t child_size = atom.payload_size;
            if (type_is(atom.type, "meta")) {
                if (child_size < 4) {
                    return APP_MP4_DEMUX_ERR_CORRUPT;
                }
                child_data += 4;
                child_size -= 4;
            }
            ret = parse_track_atoms(track, child_data, child_size);
        } else {
            ret = APP_MP4_DEMUX_OK;
        }
        if (ret != APP_MP4_DEMUX_OK && ret != APP_MP4_DEMUX_ERR_UNSUPPORTED) {
            return ret;
        }
        pos += atom.size;
    }
    return APP_MP4_DEMUX_OK;
}

static bool stts_delta_for_sample(const mp4_track_t *track, uint32_t index,
                                  uint32_t *out_delta)
{
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < track->stts_count; ++i) {
        uint64_t next = cursor + track->stts[i].count;
        if (index < next) {
            *out_delta = track->stts[i].delta;
            return true;
        }
        cursor = next;
    }
    return false;
}

static int stsc_entry_for_chunk(const mp4_track_t *track, uint32_t chunk)
{
    int found = -1;
    for (uint32_t i = 0; i < track->stsc_count; ++i) {
        if (track->stsc[i].first_chunk > chunk) {
            break;
        }
        found = (int)i;
    }
    return found;
}

static app_mp4_demux_err_t build_samples(mp4_track_t *track,
                                         uint64_t file_size)
{
    if (track->timescale == 0 || track->sample_count == 0 ||
        track->stts == NULL || track->stts_count == 0 ||
        track->stsc == NULL || track->stsc_count == 0 ||
        track->chunk_offsets == NULL || track->chunk_count == 0) {
        APP_MP4_LOGE("sample table incomplete for %c%c%c%c:"
                     " timescale=%" PRIu32 " samples=%" PRIu32
                     " stts=%" PRIu32 " stsc=%" PRIu32
                     " chunks=%" PRIu32,
                     track->handler[0], track->handler[1],
                     track->handler[2], track->handler[3],
                     track->timescale, track->sample_count,
                     track->stts_count, track->stsc_count,
                     track->chunk_count);
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    free(track->samples);
    track->samples = calloc(track->sample_count, sizeof(*track->samples));
    if (track->samples == NULL) {
        return APP_MP4_DEMUX_ERR_NO_MEMORY;
    }

    uint32_t sample_index = 0;
    uint64_t pts_ticks = 0;
    for (uint32_t chunk = 1; chunk <= track->chunk_count; ++chunk) {
        int stsc_index = stsc_entry_for_chunk(track, chunk);
        if (stsc_index < 0) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        uint32_t samples_per_chunk = track->stsc[stsc_index].samples_per_chunk;
        uint64_t offset = track->chunk_offsets[chunk - 1];
        for (uint32_t i = 0; i < samples_per_chunk; ++i) {
            if (sample_index >= track->sample_count) {
                APP_MP4_LOGE("sample count overflow for %c%c%c%c:"
                             " chunk=%" PRIu32 " sample=%" PRIu32
                             " declared=%" PRIu32,
                             track->handler[0], track->handler[1],
                             track->handler[2], track->handler[3],
                             chunk, sample_index, track->sample_count);
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            uint32_t sample_size = track->fixed_sample_size;
            if (sample_size == 0) {
                sample_size = track->sample_sizes[sample_index];
            }
            if (offset > file_size || sample_size > file_size - offset) {
                APP_MP4_LOGE("sample outside file for %c%c%c%c:"
                             " chunk=%" PRIu32 " sample=%" PRIu32
                             " offset=%" PRIu64 " size=%" PRIu32
                             " file_size=%" PRIu64,
                             track->handler[0], track->handler[1],
                             track->handler[2], track->handler[3],
                             chunk, sample_index, offset, sample_size,
                             file_size);
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            uint32_t delta = 0;
            if (!stts_delta_for_sample(track, sample_index, &delta)) {
                APP_MP4_LOGE("missing stts entry for %c%c%c%c:"
                             " sample=%" PRIu32 " stts=%" PRIu32,
                             track->handler[0], track->handler[1],
                             track->handler[2], track->handler[3],
                             sample_index, track->stts_count);
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            uint64_t start_ms = (pts_ticks * 1000ULL) / track->timescale;
            uint64_t end_ms = ((pts_ticks + delta) * 1000ULL) / track->timescale;
            track->samples[sample_index] = (app_mp4_sample_t) {
                .offset = offset,
                .size = sample_size,
                .pts_ms = start_ms,
                .duration_ms = (uint32_t)(end_ms > start_ms ? end_ms - start_ms : 1),
            };
            offset += sample_size;
            pts_ticks += delta;
            sample_index++;
        }
    }
    if (sample_index != track->sample_count) {
        APP_MP4_LOGE("sample count mismatch for %c%c%c%c:"
                     " indexed=%" PRIu32 " declared=%" PRIu32,
                     track->handler[0], track->handler[1],
                     track->handler[2], track->handler[3],
                     sample_index, track->sample_count);
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    return APP_MP4_DEMUX_OK;
}

static bool track_has_classic_sample_tables(const mp4_track_t *track)
{
    return track != NULL && track->sample_count != 0 &&
           track->stts_count != 0 && track->stsc_count != 0 &&
           track->chunk_count != 0;
}

static app_mp4_demux_err_t parse_moov_payload(app_mp4_demux_t *demux,
                                               const uint8_t *payload,
                                               size_t payload_size)
{
    mp4_track_t tracks[MP4_MAX_TRACKS] = {0};
    mp4_trex_t trex_entries[MP4_MAX_TRACKS] = {0};
    uint32_t track_count = 0;
    uint32_t trex_count = 0;
    size_t pos = 0;
    app_mp4_demux_err_t ret = APP_MP4_DEMUX_OK;
    while (pos < payload_size) {
        mp4_atom_t atom;
        ret = atom_open(payload + pos, payload_size - pos, &atom);
        if (ret != APP_MP4_DEMUX_OK) {
            goto fail;
        }
        if (type_is(atom.type, "trak")) {
            if (track_count >= MP4_MAX_TRACKS) {
                ret = APP_MP4_DEMUX_ERR_UNSUPPORTED;
                goto fail;
            }
            ret = parse_track_atoms(&tracks[track_count], atom.payload,
                                    atom.payload_size);
            if (ret != APP_MP4_DEMUX_OK && ret != APP_MP4_DEMUX_ERR_UNSUPPORTED) {
                goto fail;
            }
            track_count++;
        } else if (type_is(atom.type, "mvex")) {
            ret = parse_mvex(atom.payload, atom.payload_size,
                             trex_entries, &trex_count);
            if (ret != APP_MP4_DEMUX_OK) {
                goto fail;
            }
        }
        pos += atom.size;
    }

    bool video_selected = false;
    bool audio_selected = false;
    for (uint32_t i = 0; i < track_count; ++i) {
        mp4_track_t *track = &tracks[i];
        if (type_is(track->handler, "vide") &&
            track->video_codec != APP_MP4_VIDEO_CODEC_NONE && !video_selected) {
            demux->video_info.codec = track->video_codec;
            demux->video_info.width = track->width;
            demux->video_info.height = track->height;
            demux->video_info.timescale = track->timescale;
            demux->video_sample_requires_probe =
                track->video_sample_requires_probe;
            demux->video_track_id = track->track_id;
            if (track_has_classic_sample_tables(track)) {
                ret = build_samples(track, demux->file_size);
                if (ret != APP_MP4_DEMUX_OK) {
                    APP_MP4_LOGE("video sample index failed: ret=%d", ret);
                    goto fail;
                }
                demux->video_info.sample_count = track->sample_count;
                demux->video_info.duration_ms = track->sample_count == 0
                    ? 0
                    : track->samples[track->sample_count - 1].pts_ms +
                      track->samples[track->sample_count - 1].duration_ms;
                demux->video_samples = track->samples;
                track->samples = NULL;
            } else {
                demux->fragmented = true;
                for (uint32_t j = 0; j < trex_count; ++j) {
                    if (trex_entries[j].valid &&
                        (track->track_id == 0 ||
                         trex_entries[j].track_id == track->track_id)) {
                        demux->video_trex = trex_entries[j];
                        if (demux->video_track_id == 0) {
                            demux->video_track_id = trex_entries[j].track_id;
                        }
                        break;
                    }
                }
            }
            video_selected = true;
        } else if (type_is(track->handler, "soun") &&
                   track->audio_codec != APP_MP4_AUDIO_CODEC_NONE &&
                   !audio_selected) {
            if (track_has_classic_sample_tables(track)) {
                ret = build_samples(track, demux->file_size);
                if (ret != APP_MP4_DEMUX_OK) {
                    APP_MP4_LOGE("audio sample index failed: ret=%d", ret);
                    goto fail;
                }
                demux->audio_info.codec = track->audio_codec;
                demux->audio_info.sample_rate = track->sample_rate;
                demux->audio_info.channels = track->channels;
                demux->audio_info.timescale = track->timescale;
                demux->audio_info.sample_count = track->sample_count;
                demux->audio_info.duration_ms = track->sample_count == 0
                    ? 0
                    : track->samples[track->sample_count - 1].pts_ms +
                      track->samples[track->sample_count - 1].duration_ms;
                demux->audio_samples = track->samples;
                track->samples = NULL;
            }
            audio_selected = true;
        }
    }
    if (!video_selected) {
        ret = APP_MP4_DEMUX_ERR_NOT_FOUND;
        goto fail;
    }
    for (uint32_t i = 0; i < track_count; ++i) {
        free_track(&tracks[i]);
    }
    return APP_MP4_DEMUX_OK;

fail:
    for (uint32_t i = 0; i < track_count; ++i) {
        free_track(&tracks[i]);
    }
    return ret;
}

app_mp4_demux_err_t app_mp4_demux_parse_moov(const uint8_t *moov,
                                             size_t moov_size,
                                             uint64_t file_size,
                                             app_mp4_demux_t **out_demux)
{
    if (moov == NULL || moov_size == 0 || out_demux == NULL) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    *out_demux = NULL;
    mp4_atom_t atom;
    app_mp4_demux_err_t ret = atom_open(moov, moov_size, &atom);
    if (ret != APP_MP4_DEMUX_OK) {
        return ret;
    }
    if (!type_is(atom.type, "moov")) {
        return APP_MP4_DEMUX_ERR_NOT_MP4;
    }

    app_mp4_demux_t *demux = calloc(1, sizeof(*demux));
    if (demux == NULL) {
        return APP_MP4_DEMUX_ERR_NO_MEMORY;
    }
    demux->file_size = file_size;
    ret = parse_moov_payload(demux, atom.payload, atom.payload_size);
    if (ret != APP_MP4_DEMUX_OK) {
        app_mp4_demux_close(demux);
        return ret;
    }
    *out_demux = demux;
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t read_file_atom_header(FILE *file, uint64_t file_size,
                                                 uint64_t offset,
                                                 uint64_t *out_size,
                                                 size_t *out_header,
                                                 char out_type[4])
{
    if (offset > file_size || file_size - offset < 8 ||
        fseek(file, (long)offset, SEEK_SET) != 0) {
        APP_MP4_LOGE("atom header read failed at offset=%" PRIu64
                     " file_size=%" PRIu64, offset, file_size);
        return APP_MP4_DEMUX_ERR_IO;
    }
    uint8_t header[16];
    if (fread(header, 1, 8, file) != 8) {
        APP_MP4_LOGE("atom header fread failed at offset=%" PRIu64, offset);
        return APP_MP4_DEMUX_ERR_IO;
    }
    uint64_t size = be32(header);
    size_t header_size = 8;
    if (size == 1) {
        if (fread(header + 8, 1, 8, file) != 8) {
            return APP_MP4_DEMUX_ERR_IO;
        }
        size = be64(header + 8);
        header_size = 16;
    } else if (size == 0) {
        size = file_size - offset;
    }
    if (size < header_size || size > file_size - offset) {
        APP_MP4_LOGE("invalid atom at offset=%" PRIu64
                     " type=%c%c%c%c size=%" PRIu64
                     " header=%u remaining=%" PRIu64,
                     offset, header[4], header[5], header[6], header[7],
                     size, (unsigned)header_size, file_size - offset);
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    memcpy(out_type, header + 4, 4);
    *out_size = size;
    *out_header = header_size;
    return APP_MP4_DEMUX_OK;
}

typedef struct {
    uint32_t track_id;
    uint32_t flags;
    uint64_t base_data_offset;
    uint32_t default_sample_duration;
    uint32_t default_sample_size;
    uint32_t default_sample_flags;
    uint64_t next_data_offset;
    bool next_data_offset_valid;
    uint64_t decode_time;
} mp4_fragment_track_t;

static uint64_t ticks_to_ms(uint64_t ticks, uint32_t timescale)
{
    if (timescale == 0) {
        return 0;
    }
    return (ticks / timescale) * 1000ULL +
           ((ticks % timescale) * 1000ULL) / timescale;
}

static bool apply_composition_offset(uint64_t decode_time,
                                      int64_t composition_offset,
                                      uint64_t *out_pts)
{
    if (out_pts == NULL) {
        return false;
    }
    if (composition_offset >= 0) {
        uint64_t offset = (uint64_t)composition_offset;
        if (decode_time > UINT64_MAX - offset) {
            return false;
        }
        *out_pts = decode_time + offset;
    } else {
        uint64_t offset = (uint64_t)(-(composition_offset + 1)) + 1;
        *out_pts = decode_time > offset ? decode_time - offset : 0;
    }
    return true;
}

static app_mp4_demux_err_t append_fragment_sample(
    app_mp4_demux_t *demux, const app_mp4_sample_t *sample)
{
    if (demux == NULL || sample == NULL || sample->size == 0) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    uint32_t old_count = demux->video_info.sample_count;
    if (old_count >= MP4_MAX_TABLE_ENTRIES || old_count == UINT32_MAX) {
        return APP_MP4_DEMUX_ERR_UNSUPPORTED;
    }
    if (old_count == demux->video_sample_capacity) {
        uint32_t new_capacity = demux->video_sample_capacity == 0
            ? 64 : demux->video_sample_capacity * 2;
        if (new_capacity < old_count + 1 ||
            new_capacity > MP4_MAX_TABLE_ENTRIES) {
            new_capacity = old_count + 1;
        }
        if ((size_t)new_capacity > SIZE_MAX / sizeof(*demux->video_samples)) {
            return APP_MP4_DEMUX_ERR_NO_MEMORY;
        }
        app_mp4_sample_t *samples = realloc(
            demux->video_samples,
            (size_t)new_capacity * sizeof(*demux->video_samples));
        if (samples == NULL) {
            return APP_MP4_DEMUX_ERR_NO_MEMORY;
        }
        demux->video_samples = samples;
        demux->video_sample_capacity = new_capacity;
    }
    demux->video_samples[old_count] = *sample;
    demux->video_info.sample_count = old_count + 1;
    uint64_t end_ms = sample->pts_ms + sample->duration_ms;
    if (end_ms > demux->video_info.duration_ms) {
        demux->video_info.duration_ms = end_ms;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_tfhd(mp4_fragment_track_t *track,
                                      const mp4_atom_t *atom,
                                      uint64_t moof_offset,
                                      const mp4_trex_t *trex)
{
    if (track == NULL || atom == NULL || atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    /* tfhd supplies per-fragment defaults. Keep the decode time selected by
     * tfdt or the previous fragment when this traf has no tfdt box. */
    uint64_t decode_time = track->decode_time;
    memset(track, 0, sizeof(*track));
    track->decode_time = decode_time;
    track->flags = be32(atom->payload) & 0x00ffffffU;
    track->track_id = be32(atom->payload + 4);
    track->base_data_offset = moof_offset;
    if (trex != NULL && trex->valid && trex->track_id == track->track_id) {
        track->default_sample_duration = trex->default_sample_duration;
        track->default_sample_size = trex->default_sample_size;
        track->default_sample_flags = trex->default_sample_flags;
    }

    size_t pos = 8;
    if ((track->flags & TFHD_BASE_DATA_OFFSET_PRESENT) != 0) {
        if (atom->payload_size < pos + 8) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->base_data_offset = be64(atom->payload + pos);
        pos += 8;
    } else if ((track->flags & TFHD_DEFAULT_BASE_IS_MOOF) != 0) {
        track->base_data_offset = moof_offset;
    }
    if ((track->flags & TFHD_SAMPLE_DESCRIPTION_INDEX_PRESENT) != 0) {
        if (atom->payload_size < pos + 4) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        pos += 4;
    }
    if ((track->flags & TFHD_DEFAULT_SAMPLE_DURATION_PRESENT) != 0) {
        if (atom->payload_size < pos + 4) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->default_sample_duration = be32(atom->payload + pos);
        pos += 4;
    }
    if ((track->flags & TFHD_DEFAULT_SAMPLE_SIZE_PRESENT) != 0) {
        if (atom->payload_size < pos + 4) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->default_sample_size = be32(atom->payload + pos);
        pos += 4;
    }
    if ((track->flags & TFHD_DEFAULT_SAMPLE_FLAGS_PRESENT) != 0) {
        if (atom->payload_size < pos + 4) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->default_sample_flags = be32(atom->payload + pos);
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_tfdt(mp4_fragment_track_t *track,
                                      const mp4_atom_t *atom)
{
    if (track == NULL || atom == NULL || atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint8_t version = atom->payload[0];
    if (version == 0) {
        track->decode_time = be32(atom->payload + 4);
    } else if (version == 1) {
        if (atom->payload_size < 12) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        track->decode_time = be64(atom->payload + 4);
    } else {
        return APP_MP4_DEMUX_ERR_UNSUPPORTED;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_fragment_trun(
    app_mp4_demux_t *demux, mp4_fragment_track_t *track,
    const mp4_atom_t *atom)
{
    if (demux == NULL || track == NULL || atom == NULL ||
        atom->payload_size < 8) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint8_t version = atom->payload[0];
    uint32_t flags = be32(atom->payload) & 0x00ffffffU;
    uint32_t sample_count = be32(atom->payload + 4);
    if (!count_is_valid(sample_count, sizeof(app_mp4_sample_t))) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }

    size_t pos = 8;
    if ((flags & TRUN_DATA_OFFSET_PRESENT) != 0) {
        if (atom->payload_size < pos + 4) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        int32_t data_offset = (int32_t)be32(atom->payload + pos);
        if (data_offset >= 0) {
            uint64_t offset = (uint32_t)data_offset;
            if (track->base_data_offset > UINT64_MAX - offset) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            track->next_data_offset = track->base_data_offset + offset;
        } else {
            uint64_t offset = (uint64_t)(-(int64_t)data_offset);
            if (track->base_data_offset < offset) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            track->next_data_offset = track->base_data_offset - offset;
        }
        track->next_data_offset_valid = true;
        pos += 4;
    } else if (!track->next_data_offset_valid) {
        track->next_data_offset = track->base_data_offset;
        track->next_data_offset_valid = true;
    }

    if ((flags & TRUN_FIRST_SAMPLE_FLAGS_PRESENT) != 0) {
        if (atom->payload_size < pos + 4) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        pos += 4;
    }

    for (uint32_t i = 0; i < sample_count; ++i) {
        uint32_t duration = track->default_sample_duration;
        uint32_t size = track->default_sample_size;
        int64_t composition_offset = 0;
        if ((flags & TRUN_SAMPLE_DURATION_PRESENT) != 0) {
            if (atom->payload_size < pos + 4) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            duration = be32(atom->payload + pos);
            pos += 4;
        }
        if ((flags & TRUN_SAMPLE_SIZE_PRESENT) != 0) {
            if (atom->payload_size < pos + 4) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            size = be32(atom->payload + pos);
            pos += 4;
        }
        if ((flags & TRUN_SAMPLE_FLAGS_PRESENT) != 0) {
            if (atom->payload_size < pos + 4) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            pos += 4;
        }
        if ((flags & TRUN_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT) != 0) {
            if (atom->payload_size < pos + 4) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            uint32_t raw_offset = be32(atom->payload + pos);
            composition_offset = version == 1
                ? (int32_t)raw_offset : (int64_t)raw_offset;
            pos += 4;
        }
        if (size == 0 || duration == 0 ||
            track->next_data_offset > demux->file_size ||
            size > demux->file_size - track->next_data_offset) {
            APP_MP4_LOGE("invalid fragmented sample: track=%" PRIu32
                         " offset=%" PRIu64 " size=%" PRIu32
                         " duration=%" PRIu32 " file_size=%" PRIu64,
                         track->track_id, track->next_data_offset, size,
                         duration, demux->file_size);
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }

        uint64_t end_decode_time = track->decode_time + duration;
        if (end_decode_time < track->decode_time) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        uint64_t pts_ticks = 0;
        uint64_t end_pts_ticks = 0;
        if (!apply_composition_offset(track->decode_time,
                                      composition_offset, &pts_ticks) ||
            !apply_composition_offset(end_decode_time,
                                      composition_offset, &end_pts_ticks)) {
            return APP_MP4_DEMUX_ERR_CORRUPT;
        }
        uint64_t pts_ms = ticks_to_ms(pts_ticks, demux->video_info.timescale);
        uint64_t end_ms = ticks_to_ms(end_pts_ticks,
                                      demux->video_info.timescale);
        uint64_t duration_ms = end_ms > pts_ms ? end_ms - pts_ms : 1;
        app_mp4_sample_t sample = {
            .offset = track->next_data_offset,
            .size = size,
            .pts_ms = pts_ms,
            .duration_ms = duration_ms > UINT32_MAX
                ? UINT32_MAX : (uint32_t)duration_ms,
        };
        app_mp4_demux_err_t ret = append_fragment_sample(demux, &sample);
        if (ret != APP_MP4_DEMUX_OK) {
            return ret;
        }
        track->next_data_offset += size;
        track->decode_time = end_decode_time;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_fragment_traf(
    app_mp4_demux_t *demux, const uint8_t *data, size_t size,
    uint64_t moof_offset)
{
    mp4_fragment_track_t track = {
        .base_data_offset = moof_offset,
        .decode_time = demux->fragment_decode_time,
    };
    size_t pos = 0;
    bool tfhd_seen = false;
    bool video_trun_seen = false;
    while (pos < size) {
        mp4_atom_t atom;
        app_mp4_demux_err_t ret = atom_open(data + pos, size - pos, &atom);
        if (ret != APP_MP4_DEMUX_OK) {
            return ret;
        }
        if (type_is(atom.type, "tfhd")) {
            ret = parse_tfhd(&track, &atom, moof_offset,
                             &demux->video_trex);
            if (ret != APP_MP4_DEMUX_OK) {
                return ret;
            }
            tfhd_seen = true;
        } else if (type_is(atom.type, "tfdt")) {
            if (!tfhd_seen) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            ret = parse_tfdt(&track, &atom);
            if (ret != APP_MP4_DEMUX_OK) {
                return ret;
            }
        } else if (type_is(atom.type, "trun")) {
            if (!tfhd_seen) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            if (track.track_id == demux->video_track_id) {
                ret = parse_fragment_trun(demux, &track, &atom);
                if (ret != APP_MP4_DEMUX_OK) {
                    return ret;
                }
                video_trun_seen = true;
            }
        }
        pos += atom.size;
    }
    if (track.track_id == demux->video_track_id && video_trun_seen) {
        demux->fragment_decode_time = track.decode_time;
    }
    if (track.track_id == demux->video_track_id && !video_trun_seen) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t parse_fragment_moof(
    app_mp4_demux_t *demux, const uint8_t *data, size_t size,
    uint64_t moof_offset)
{
    mp4_atom_t moof;
    app_mp4_demux_err_t ret = atom_open(data, size, &moof);
    if (ret != APP_MP4_DEMUX_OK || !type_is(moof.type, "moof")) {
        return ret == APP_MP4_DEMUX_OK ? APP_MP4_DEMUX_ERR_NOT_MP4 : ret;
    }
    size_t pos = 0;
    while (pos < moof.payload_size) {
        mp4_atom_t atom;
        ret = atom_open(moof.payload + pos, moof.payload_size - pos, &atom);
        if (ret != APP_MP4_DEMUX_OK) {
            return ret;
        }
        if (type_is(atom.type, "traf")) {
            ret = parse_fragment_traf(demux, atom.payload,
                                       atom.payload_size, moof_offset);
            if (ret != APP_MP4_DEMUX_OK) {
                return ret;
            }
        }
        pos += atom.size;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t index_fragmented_video(app_mp4_demux_t *demux)
{
    if (demux == NULL || demux->file == NULL || !demux->fragmented) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    uint64_t pos = 0;
    bool found_moof = false;
    while (pos < demux->file_size) {
        uint64_t atom_size = 0;
        size_t header_size = 0;
        char type[4];
        app_mp4_demux_err_t ret = read_file_atom_header(
            demux->file, demux->file_size, pos, &atom_size,
            &header_size, type);
        (void)header_size;
        if (ret != APP_MP4_DEMUX_OK) {
            return ret;
        }
        if (type_is(type, "moof")) {
            if (atom_size > SIZE_MAX) {
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            uint8_t *moof = malloc((size_t)atom_size);
            if (moof == NULL) {
                return APP_MP4_DEMUX_ERR_NO_MEMORY;
            }
            bool read_ok = fseek(demux->file, (long)pos, SEEK_SET) == 0 &&
                           fread(moof, 1, (size_t)atom_size,
                                 demux->file) == (size_t)atom_size;
            if (!read_ok) {
                free(moof);
                return APP_MP4_DEMUX_ERR_IO;
            }
            ret = parse_fragment_moof(demux, moof, (size_t)atom_size, pos);
            free(moof);
            if (ret != APP_MP4_DEMUX_OK) {
                return ret;
            }
            found_moof = true;
        }
        pos += atom_size;
    }
    if (!found_moof || demux->video_info.sample_count == 0) {
        APP_MP4_LOGE("fragmented MP4 has no video samples");
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    return APP_MP4_DEMUX_OK;
}

static app_mp4_demux_err_t probe_video_sample_codec(app_mp4_demux_t *demux)
{
    if (!demux->video_sample_requires_probe || demux->file == NULL) {
        return APP_MP4_DEMUX_OK;
    }
    if (demux->video_samples == NULL || demux->video_info.sample_count == 0) {
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }

    const app_mp4_sample_t *sample = &demux->video_samples[0];
    if (sample->size < 2 || sample->offset > (uint64_t)LONG_MAX ||
        fseek(demux->file, (long)sample->offset, SEEK_SET) != 0) {
        APP_MP4_LOGE("first video sample seek failed: offset=%" PRIu64
                     " size=%" PRIu32, sample->offset, sample->size);
        return APP_MP4_DEMUX_ERR_CORRUPT;
    }
    uint8_t header[2];
    if (fread(header, 1, sizeof(header), demux->file) != sizeof(header)) {
        APP_MP4_LOGE("first video sample read failed: offset=%" PRIu64
                     " size=%" PRIu32, sample->offset, sample->size);
        return APP_MP4_DEMUX_ERR_IO;
    }
    if (header[0] != 0xFF || header[1] != 0xD8) {
        APP_MP4_LOGE("mp4v first sample is not JPEG: header=%02x%02x",
                     header[0], header[1]);
        return APP_MP4_DEMUX_ERR_UNSUPPORTED;
    }
    demux->video_sample_requires_probe = false;
    return APP_MP4_DEMUX_OK;
}

app_mp4_demux_err_t app_mp4_demux_open(const char *path,
                                       app_mp4_demux_t **out_demux)
{
    if (path == NULL || out_demux == NULL) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    *out_demux = NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return APP_MP4_DEMUX_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return APP_MP4_DEMUX_ERR_IO;
    }
    long file_length = ftell(file);
    if (file_length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return APP_MP4_DEMUX_ERR_IO;
    }
    uint64_t file_size = (uint64_t)file_length;
    uint64_t pos = 0;
    uint8_t *moov = NULL;
    size_t moov_size = 0;
    while (pos < file_size) {
        uint64_t atom_size = 0;
        size_t header_size = 0;
        char type[4];
        app_mp4_demux_err_t ret = read_file_atom_header(
            file, file_size, pos, &atom_size, &header_size, type);
        (void)header_size;
        if (ret != APP_MP4_DEMUX_OK) {
            APP_MP4_LOGE("top-level atom parse failed at offset=%" PRIu64
                         " ret=%d", pos, ret);
            fclose(file);
            return ret;
        }
        if (type_is(type, "moov")) {
            if (atom_size > SIZE_MAX) {
                fclose(file);
                return APP_MP4_DEMUX_ERR_CORRUPT;
            }
            free(moov);
            moov = malloc((size_t)atom_size);
            if (moov == NULL) {
                fclose(file);
                return APP_MP4_DEMUX_ERR_NO_MEMORY;
            }
            if (fseek(file, (long)pos, SEEK_SET) != 0 ||
                fread(moov, 1, (size_t)atom_size, file) != (size_t)atom_size) {
                free(moov);
                fclose(file);
                return APP_MP4_DEMUX_ERR_IO;
            }
            moov_size = (size_t)atom_size;
        }
        pos += atom_size;
    }
    if (moov == NULL) {
        fclose(file);
        return APP_MP4_DEMUX_ERR_NOT_MP4;
    }

    app_mp4_demux_t *demux = NULL;
    app_mp4_demux_err_t ret = app_mp4_demux_parse_moov(
        moov, moov_size, file_size, &demux);
    if (ret != APP_MP4_DEMUX_OK) {
        APP_MP4_LOGE("moov parse failed: size=%u file_size=%" PRIu64
                     " ret=%d", (unsigned)moov_size, file_size, ret);
        free(moov);
        fclose(file);
        return ret;
    }
    demux->file = file;
    demux->moov = moov;
    demux->moov_size = moov_size;
    if (demux->fragmented) {
        ret = index_fragmented_video(demux);
        if (ret != APP_MP4_DEMUX_OK) {
            APP_MP4_LOGE("fragmented video index failed: ret=%d", ret);
            app_mp4_demux_close(demux);
            return ret;
        }
    }
    ret = probe_video_sample_codec(demux);
    if (ret != APP_MP4_DEMUX_OK) {
        APP_MP4_LOGE("video sample probe failed: ret=%d", ret);
        app_mp4_demux_close(demux);
        return ret;
    }
    *out_demux = demux;
    return APP_MP4_DEMUX_OK;
}

void app_mp4_demux_close(app_mp4_demux_t *demux)
{
    if (demux == NULL) {
        return;
    }
    if (demux->file != NULL) {
        fclose(demux->file);
    }
    free(demux->moov);
    free(demux->video_samples);
    free(demux->audio_samples);
    free(demux);
}

const app_mp4_video_info_t *app_mp4_demux_video_info(
    const app_mp4_demux_t *demux)
{
    return demux != NULL && demux->video_info.codec != APP_MP4_VIDEO_CODEC_NONE
        ? &demux->video_info : NULL;
}

const app_mp4_audio_info_t *app_mp4_demux_audio_info(
    const app_mp4_demux_t *demux)
{
    return demux != NULL && demux->audio_info.codec != APP_MP4_AUDIO_CODEC_NONE
        ? &demux->audio_info : NULL;
}

app_mp4_demux_err_t app_mp4_demux_get_video_sample(
    const app_mp4_demux_t *demux, uint32_t index, app_mp4_sample_t *out_sample)
{
    if (demux == NULL || out_sample == NULL) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    if (demux->video_samples == NULL || index >= demux->video_info.sample_count) {
        return APP_MP4_DEMUX_ERR_NOT_FOUND;
    }
    *out_sample = demux->video_samples[index];
    return APP_MP4_DEMUX_OK;
}

app_mp4_demux_err_t app_mp4_demux_read_video_sample(
    app_mp4_demux_t *demux, uint32_t index, uint8_t *buffer,
    size_t buffer_size, app_mp4_sample_t *out_sample)
{
    if (demux == NULL || demux->file == NULL || buffer == NULL) {
        return APP_MP4_DEMUX_ERR_INVALID_ARG;
    }
    app_mp4_sample_t sample;
    app_mp4_demux_err_t ret = app_mp4_demux_get_video_sample(
        demux, index, &sample);
    if (ret != APP_MP4_DEMUX_OK) {
        return ret;
    }
    if (buffer_size < sample.size) {
        return APP_MP4_DEMUX_ERR_BUFFER_TOO_SMALL;
    }
    if (sample.offset > (uint64_t)LONG_MAX ||
        fseek(demux->file, (long)sample.offset, SEEK_SET) != 0 ||
        fread(buffer, 1, sample.size, demux->file) != sample.size) {
        return APP_MP4_DEMUX_ERR_IO;
    }
    if (out_sample != NULL) {
        *out_sample = sample;
    }
    return APP_MP4_DEMUX_OK;
}
