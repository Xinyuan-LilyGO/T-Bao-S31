# Changelog

All notable changes to this project will be documented in this file.

## [1.0.2] - 2026-05-19

- Use the managed `espressif/cjson` component instead of the removed ESP-IDF `json` component.
- Use `espressif/esp_vocat` BSP for ESP32-S3 test apps on ESP-IDF 6.0 and later.
- Split test app hardware initialization from emote test cases.

## [1.0.1] - 2026-05-11

- Fix battery status timer to update only the label, avoiding asset reads from the render task.
- Update battery event handling to refresh battery icons immediately after parsing battery state.

## [1.0.0] - 2026-02-13

- Add unload interface
- Add visibale API
- Add build action for P4
- Adapt to emote_gfx 3.0.0+ API

## [0.1.0] - 2026-01-05

### Added
- User resource access interfaces for accessing asset data:

## [0.0.1] - 2025-01-XX

### Added
- Initial release: emote animation management, UI element control, event handling, resource management, and system status display features
