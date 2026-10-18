#include "settings.h"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace
{
constexpr const wchar_t *run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct RegistryKey
{
    HKEY value = nullptr;
    ~RegistryKey()
    {
        if (value != nullptr)
            RegCloseKey(value);
    }
};

DWORD read_number(HKEY key, const wchar_t *name, DWORD fallback)
{
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return fallback;
    return value;
}

std::wstring read_text(HKEY key, const wchar_t *name, const std::wstring &fallback, size_t capacity)
{
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return fallback;
    if (bytes > (capacity * sizeof(wchar_t)))
        return fallback;
    std::vector<wchar_t> buffer((bytes / sizeof(wchar_t)) + 1, 0);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
        return fallback;
    return buffer.data();
}

void write_number(HKEY key, const wchar_t *name, DWORD value)
{
    const LSTATUS status =
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not save preferences (" + std::to_string(status) + ")");
}

void write_text(HKEY key, const wchar_t *name, const std::wstring &value)
{
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not save preferences (" + std::to_string(status) + ")");
}
} // namespace

Settings load_settings(const wchar_t *key_path)
{
    Settings settings;
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path, 0, KEY_QUERY_VALUE, &key.value) != ERROR_SUCCESS)
        return settings;
    settings.font = read_text(key.value, L"Font", settings.font, 128);
    if (settings.font.empty())
        settings.font = Settings{}.font;
    settings.background_path = read_text(key.value, L"BackgroundImage", L"", 32768);
    settings.time_zone = read_text(key.value, L"TimeZone", L"", 128);
    settings.label = read_text(key.value, L"Label", L"", 41);
    settings.netatmo_module_id = read_text(key.value, L"NetatmoModule", L"", 128);
    settings.theme = static_cast<int>(std::min<DWORD>(read_number(key.value, L"Theme", 0), 6));
    settings.font_size =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"FontSize", 76), 36, 110));
    settings.analog_font_size =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"AnalogFontSize", 20), 12, 32));
    settings.analog_face = static_cast<int>(std::min<DWORD>(read_number(key.value, L"AnalogFace", 0), 2));
    settings.reading_font_size =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"ReadingFontSize", 32), 28, 38));
    settings.font_style = static_cast<int>(std::min<DWORD>(read_number(key.value, L"FontStyle", 0), 3));
    settings.layout = static_cast<int>(std::min<DWORD>(read_number(key.value, L"Layout", 0), 2));
    settings.date_format = static_cast<int>(std::min<DWORD>(read_number(key.value, L"DateFormat", 0), 3));
    settings.interval_seconds =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"Interval", 10), 1, 86400));
    settings.codex_refresh_seconds =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"CodexRefreshSeconds", 60), 60, 86400));
    settings.netatmo_refresh_seconds =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"NetatmoRefreshSeconds", 300), 60, 86400));
    settings.netatmo_readings =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"NetatmoReadings", 13), 1, 15));
    settings.hub_brightness =
        static_cast<int>(std::clamp<DWORD>(read_number(key.value, L"Brightness", 100), 10, 100));
    settings.vertical_offset =
        std::clamp(static_cast<int>(read_number(key.value, L"VerticalOffset", 0)), -20, 20);
    settings.background_fit =
        static_cast<int>(std::min<DWORD>(read_number(key.value, L"BackgroundFit", 0), 1));
    settings.text_color = read_number(key.value, L"TextColor", settings.text_color) & 0xffffff;
    settings.accent_color = read_number(key.value, L"AccentColor", settings.accent_color) & 0xffffff;
    settings.image_dim = static_cast<int>(std::min<DWORD>(read_number(key.value, L"ImageDim", 35), 85));
    settings.background_dither =
        static_cast<int>(std::min<DWORD>(read_number(key.value, L"BackgroundDither", 4), 8));
    settings.use_24_hour = read_number(key.value, L"Use24Hour", 1) != 0;
    settings.show_seconds = read_number(key.value, L"ShowSeconds", 1) != 0;
    settings.show_date = read_number(key.value, L"ShowDate", 1) != 0;
    settings.show_weekday = read_number(key.value, L"ShowWeekday", 1) != 0;
    settings.show_codex_usage = read_number(key.value, L"ShowCodexUsage", 0) != 0;
    settings.show_netatmo = read_number(key.value, L"ShowNetatmo", 0) != 0;
    settings.pause_on_battery = read_number(key.value, L"PauseOnBattery", 0) != 0;
    settings.show_settings_on_launch = read_number(key.value, L"ShowSettingsOnLaunch", 1) != 0;
    return settings;
}

void save_settings(const Settings &settings, const wchar_t *key_path)
{
    RegistryKey key;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, key_path, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                           &key.value, nullptr);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not open preferences (" + std::to_string(status) + ")");
    write_text(key.value, L"Font", settings.font);
    write_text(key.value, L"BackgroundImage", settings.background_path);
    write_text(key.value, L"TimeZone", settings.time_zone);
    write_text(key.value, L"Label", settings.label);
    write_text(key.value, L"NetatmoModule", settings.netatmo_module_id);
    write_number(key.value, L"Theme", static_cast<DWORD>(settings.theme));
    write_number(key.value, L"FontSize", static_cast<DWORD>(settings.font_size));
    write_number(key.value, L"AnalogFontSize", static_cast<DWORD>(settings.analog_font_size));
    write_number(key.value, L"AnalogFace", static_cast<DWORD>(settings.analog_face));
    write_number(key.value, L"ReadingFontSize", static_cast<DWORD>(settings.reading_font_size));
    write_number(key.value, L"FontStyle", static_cast<DWORD>(settings.font_style));
    write_number(key.value, L"Layout", static_cast<DWORD>(settings.layout));
    write_number(key.value, L"DateFormat", static_cast<DWORD>(settings.date_format));
    write_number(key.value, L"Interval", static_cast<DWORD>(settings.interval_seconds));
    write_number(key.value, L"CodexRefreshSeconds", static_cast<DWORD>(settings.codex_refresh_seconds));
    write_number(key.value, L"NetatmoRefreshSeconds", static_cast<DWORD>(settings.netatmo_refresh_seconds));
    write_number(key.value, L"NetatmoReadings", static_cast<DWORD>(settings.netatmo_readings));
    write_number(key.value, L"Brightness", static_cast<DWORD>(settings.hub_brightness));
    write_number(key.value, L"VerticalOffset", static_cast<DWORD>(settings.vertical_offset));
    write_number(key.value, L"BackgroundFit", static_cast<DWORD>(settings.background_fit));
    write_number(key.value, L"TextColor", settings.text_color);
    write_number(key.value, L"AccentColor", settings.accent_color);
    write_number(key.value, L"ImageDim", static_cast<DWORD>(settings.image_dim));
    write_number(key.value, L"BackgroundDither", static_cast<DWORD>(settings.background_dither));
    write_number(key.value, L"Use24Hour", settings.use_24_hour ? 1 : 0);
    write_number(key.value, L"ShowSeconds", settings.show_seconds ? 1 : 0);
    write_number(key.value, L"ShowDate", settings.show_date ? 1 : 0);
    write_number(key.value, L"ShowWeekday", settings.show_weekday ? 1 : 0);
    write_number(key.value, L"ShowCodexUsage", settings.show_codex_usage ? 1 : 0);
    write_number(key.value, L"ShowNetatmo", settings.show_netatmo ? 1 : 0);
    write_number(key.value, L"PauseOnBattery", settings.pause_on_battery ? 1 : 0);
    write_number(key.value, L"ShowSettingsOnLaunch", settings.show_settings_on_launch ? 1 : 0);
}

bool starts_with_windows(const wchar_t *value_name)
{
    DWORD bytes = 0;
    return RegGetValueW(HKEY_CURRENT_USER, run_key, value_name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) ==
           ERROR_SUCCESS;
}

void set_start_with_windows(bool enabled, const wchar_t *value_name, const std::wstring &exe_path)
{
    RegistryKey key;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                           &key.value, nullptr);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not change Windows startup (" + std::to_string(status) + ")");
    if (enabled)
        write_text(key.value, value_name, L"\"" + exe_path + L"\" --startup");
    else
    {
        const LSTATUS removed = RegDeleteValueW(key.value, value_name);
        if ((removed != ERROR_SUCCESS) && (removed != ERROR_FILE_NOT_FOUND))
            throw std::runtime_error("Could not remove Windows startup entry");
    }
}

std::wstring executable_path()
{
    std::vector<wchar_t> buffer(32768, 0);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if ((length == 0) || (length >= buffer.size()))
        throw_windows_error("Read executable path");
    return std::wstring(buffer.data(), length);
}

std::wstring error_message(DWORD error)
{
    wchar_t *buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring message = buffer != nullptr ? buffer : L"Windows error " + std::to_wstring(error);
    if (buffer != nullptr)
        LocalFree(buffer);
    while ((message.empty() == false) && ((message.back() == L'\r') || (message.back() == L'\n')))
        message.pop_back();
    return message;
}

std::wstring widen(const std::string &text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void throw_windows_error(const char *operation)
{
    const DWORD error = GetLastError();
    throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(error) + ")");
}
