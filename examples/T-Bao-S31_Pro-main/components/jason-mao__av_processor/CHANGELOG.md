# Changelog

## v0.5.8

### Feature

- Update gmf version to v0.8

## v0.5.7

### Feature

- Revert esp32s3 dvp camera init

## v0.5.6

### Feature

- Video render support stream mode
- Add `video_render_get_image_resolution` API

### Bugfix
- Fixed a issue when called `audio_feeder_stop`

## v0.5.5

### Feature

- Support MP3 format to feed
- support esp-capture v0.7.9

## v0.5.5

### Feature

- Support esp_board_manager version greater than or equal to 0.5
- Add `video_capture_fetch_frame_acquire` and `video_capture_fetch_frame_release` APIs to support video capture stream and fetch modes.

## v0.5.4

### Bugfix

- Fix API redefintion issue

## v0.5.3

### Feature

- Add `audio_recorder_get_afe_element_handle` API

## v0.5.2

### Bugfix

- Fix `feeder` and `audio prompt` playback conflict issue

## v0.5.1

### Bugfix

- Fix incorrect duration calculation when reading G711 encoded data.

## v0.5.0

### Features

- Initial version of `av_processor`
