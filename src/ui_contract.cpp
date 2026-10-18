#include "ui_contract.h"
#include <stdexcept>
#include <bit>

namespace
{
void require_range(int value, int minimum, int maximum)
{
    if ((value < minimum) || (value > maximum))
        throw std::runtime_error("A clock setting is outside its supported range");
}

template <size_t Size> std::wstring read_string(const wchar_t (&value)[Size])
{
    const size_t length = wcsnlen_s(value, Size);
    if (length == Size)
        throw std::runtime_error("A clock setting contains invalid text");
    return {value, length};
}

template <size_t Size> void write_string(wchar_t (&target)[Size], const std::wstring &value)
{
    if (value.size() >= Size)
        throw std::runtime_error("A saved clock setting contains too much text");
    wcscpy_s(target, value.c_str());
}
} // namespace

Settings settings_from_wire(const SettingsWire &wire)
{
    require_range(wire.version, 6, 6);
    require_range(wire.theme, 0, 6);
    require_range(wire.font_size, 36, 110);
    require_range(wire.analog_font_size, 12, 32);
    require_range(wire.analog_face, 0, 2);
    require_range(wire.reading_font_size, 28, 38);
    require_range(wire.font_style, 0, 3);
    require_range(wire.layout, 0, 2);
    require_range(wire.date_format, 0, 3);
    require_range(wire.interval_seconds, 1, 86400);
    require_range(wire.codex_refresh_seconds, 60, 86400);
    require_range(wire.netatmo_refresh_seconds, 60, 86400);
    require_range(wire.netatmo_readings, 1, 15);
    require_range(wire.hub_brightness, 10, 100);
    require_range(wire.vertical_offset, -20, 20);
    require_range(wire.background_fit, 0, 1);
    require_range(wire.text_color, 0, 0xffffff);
    require_range(wire.accent_color, 0, 0xffffff);
    require_range(wire.image_dim, 0, 85);
    require_range(wire.background_dither, 0, 8);
    require_range(wire.flags, 0, 1023);
    require_range(wire.start_windows, 0, 1);
    Settings settings;
    settings.font = read_string(wire.font);
    settings.background_path = read_string(wire.background_path);
    settings.time_zone = read_string(wire.time_zone);
    settings.label = read_string(wire.label);
    settings.netatmo_module_id = read_string(wire.netatmo_module_id);
    if (settings.font.empty())
        throw std::runtime_error("Choose a font for the clock");
    if ((wire.theme == 5) && settings.background_path.empty())
        throw std::runtime_error("Choose a background image first");
    settings.theme = wire.theme;
    settings.font_size = wire.font_size;
    settings.analog_font_size = wire.analog_font_size;
    settings.analog_face = wire.analog_face;
    settings.reading_font_size = wire.reading_font_size;
    settings.font_style = wire.font_style;
    settings.layout = wire.layout;
    settings.date_format = wire.date_format;
    settings.interval_seconds = wire.interval_seconds;
    settings.codex_refresh_seconds = wire.codex_refresh_seconds;
    settings.netatmo_refresh_seconds = wire.netatmo_refresh_seconds;
    settings.netatmo_readings = wire.netatmo_readings;
    settings.hub_brightness = wire.hub_brightness;
    settings.vertical_offset = wire.vertical_offset;
    settings.background_fit = wire.background_fit;
    settings.text_color = static_cast<COLORREF>(wire.text_color);
    settings.accent_color = static_cast<COLORREF>(wire.accent_color);
    settings.image_dim = wire.image_dim;
    settings.background_dither = wire.background_dither;
    settings.use_24_hour = (wire.flags & 1) != 0;
    settings.show_seconds = (wire.flags & 2) != 0;
    settings.show_date = (wire.flags & 4) != 0;
    settings.show_weekday = (wire.flags & 8) != 0;
    settings.show_codex_usage = (wire.flags & 256) != 0;
    settings.show_netatmo = (wire.flags & 512) != 0;
    if ((settings.show_netatmo) && (settings.show_codex_usage) && (settings.layout == 2) &&
        (std::popcount(static_cast<unsigned int>(settings.netatmo_readings)) > 3))
        throw std::runtime_error("Choose up to three Netatmo readings while Codex is shown");
    settings.pause_on_battery = (wire.flags & 16) != 0;
    settings.show_settings_on_launch = (wire.flags & 64) != 0;
    return settings;
}

SettingsWire settings_to_wire(const Settings &settings, bool startup)
{
    SettingsWire wire = {};
    wire.theme = settings.theme;
    wire.font_size = settings.font_size;
    wire.analog_font_size = settings.analog_font_size;
    wire.analog_face = settings.analog_face;
    wire.reading_font_size = settings.reading_font_size;
    wire.font_style = settings.font_style;
    wire.layout = settings.layout;
    wire.date_format = settings.date_format;
    wire.interval_seconds = settings.interval_seconds;
    wire.codex_refresh_seconds = settings.codex_refresh_seconds;
    wire.netatmo_refresh_seconds = settings.netatmo_refresh_seconds;
    wire.netatmo_readings = settings.netatmo_readings;
    wire.hub_brightness = settings.hub_brightness;
    wire.vertical_offset = settings.vertical_offset;
    wire.background_fit = settings.background_fit;
    wire.text_color = static_cast<int>(settings.text_color);
    wire.accent_color = static_cast<int>(settings.accent_color);
    wire.image_dim = settings.image_dim;
    wire.background_dither = settings.background_dither;
    wire.flags = (settings.use_24_hour ? 1 : 0) | (settings.show_seconds ? 2 : 0) |
                 (settings.show_date ? 4 : 0) | (settings.show_weekday ? 8 : 0) |
                 (settings.pause_on_battery ? 16 : 0) | (settings.show_settings_on_launch ? 64 : 0) |
                 (settings.show_codex_usage ? 256 : 0) | (settings.show_netatmo ? 512 : 0);
    wire.start_windows = startup ? 1 : 0;
    write_string(wire.font, settings.font);
    write_string(wire.background_path, settings.background_path);
    write_string(wire.time_zone, settings.time_zone);
    write_string(wire.label, settings.label);
    write_string(wire.netatmo_module_id, settings.netatmo_module_id);
    return wire;
}
