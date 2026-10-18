#pragma once
#include <windows.h>
#include <string>

struct Settings
{
    std::wstring font = L"Segoe UI Light";
    std::wstring background_path;
    std::wstring time_zone;
    std::wstring label;
    std::wstring netatmo_module_id;
    int theme = 0;
    int font_size = 76;
    int analog_font_size = 20;
    int analog_face = 0;
    int reading_font_size = 32;
    int font_style = 0;
    int layout = 0;
    int date_format = 0;
    int interval_seconds = 10;
    int codex_refresh_seconds = 60;
    int netatmo_refresh_seconds = 300;
    int netatmo_readings = 13;
    int hub_brightness = 100;
    int vertical_offset = 0;
    int background_fit = 0;
    COLORREF text_color = RGB(245, 248, 255);
    COLORREF accent_color = RGB(144, 184, 255);
    int image_dim = 35;
    int background_dither = 4;
    bool use_24_hour = true;
    bool show_seconds = true;
    bool show_date = true;
    bool show_weekday = true;
    bool show_codex_usage = false;
    bool show_netatmo = false;
    bool pause_on_battery = false;
    bool show_settings_on_launch = true;
    bool operator==(const Settings &) const = default;
};

Settings load_settings(const wchar_t *key_path);
void save_settings(const Settings &settings, const wchar_t *key_path);
bool starts_with_windows(const wchar_t *value_name);
void set_start_with_windows(bool enabled, const wchar_t *value_name, const std::wstring &exe_path);
std::wstring executable_path();
std::wstring error_message(DWORD error);
std::wstring widen(const std::string &text);
void throw_windows_error(const char *operation);
