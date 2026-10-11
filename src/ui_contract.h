#pragma once
#include "settings.h"

constexpr UINT settings_query_message = WM_APP + 4;
constexpr UINT settings_register_message = WM_APP + 5;
constexpr ULONG_PTR settings_apply_data = 0x41434C4B;

// Fixed UTF-16 layout shared with the on-demand settings process.
struct SettingsWire
{
    int version = 4;
    int theme;
    int font_size;
    int font_style;
    int layout;
    int date_format;
    int interval_seconds;
    int hub_brightness;
    int vertical_offset;
    int background_fit;
    int text_color;
    int accent_color;
    int image_dim;
    int flags; // Bit 5 is reserved for the removed lock-pause option.
    int start_windows;
    int analog_font_size;
    wchar_t font[128];
    wchar_t background_path[32768];
    wchar_t time_zone[128];
    wchar_t label[41];
    int codex_refresh_seconds;
    int background_dither;
};
static_assert(sizeof(SettingsWire) == 66204);
static_assert(offsetof(SettingsWire, font) == 64);
static_assert(offsetof(SettingsWire, background_path) == 320);
static_assert(offsetof(SettingsWire, time_zone) == 65856);
static_assert(offsetof(SettingsWire, label) == 66112);
static_assert(offsetof(SettingsWire, codex_refresh_seconds) == 66196);
static_assert(offsetof(SettingsWire, background_dither) == 66200);

Settings settings_from_wire(const SettingsWire &wire);
SettingsWire settings_to_wire(const Settings &settings, bool startup);
