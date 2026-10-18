#include "ui_contract.h"
#include "renderer.h"
#include <stdexcept>

namespace
{
struct Preview
{
    GraphicsRuntime graphics;
    ClockRenderer renderer;
    Settings settings;
    bool configured = false;
    ULONGLONG snapshot_ticks = 0;
    CodexUsage snapshot_usage;
    NetatmoData snapshot_weather;
};

void write_error(wchar_t *error, int capacity, const std::exception &exception)
{
    if ((error != nullptr) && (capacity > 0))
        wcsncpy_s(error, static_cast<size_t>(capacity), widen(exception.what()).c_str(), _TRUNCATE);
}
} // namespace

extern "C" __declspec(dllexport) void *clock_preview_open(wchar_t *error, int capacity)
{
    try
    {
        return new Preview;
    }
    catch (const std::exception &exception)
    {
        write_error(error, capacity, exception);
        return nullptr;
    }
}

extern "C" __declspec(dllexport) void clock_preview_close(Preview *preview)
{
    delete preview;
}

extern "C" __declspec(dllexport) int clock_settings_load(SettingsWire *wire, wchar_t *error, int capacity)
{
    try
    {
        *wire = settings_to_wire(load_settings(L"Software\\AnkerClock"), starts_with_windows(L"AnkerClock"));
        return 1;
    }
    catch (const std::exception &exception)
    {
        write_error(error, capacity, exception);
        return 0;
    }
}

namespace
{
int render_preview(Preview *preview, const SettingsWire *wire, ULONGLONG frame_utc_ticks, BYTE *bytes,
                   int capacity, wchar_t *error, int error_capacity)
{
    try
    {
        const Settings settings = settings_from_wire(*wire);
        if ((preview->configured == false) || (preview->settings != settings))
        {
            preview->renderer.configure(settings);
            preview->settings = settings;
            preview->configured = true;
        }
        SYSTEMTIME utc = {};
        if (frame_utc_ticks == 0)
            GetSystemTime(&utc);
        else
            utc = utc_from_ticks(frame_utc_ticks);
        const SYSTEMTIME time = preview->renderer.time_at(utc);
        if ((frame_utc_ticks != 0) && (preview->snapshot_ticks != frame_utc_ticks))
        {
            preview->snapshot_usage = load_codex_usage();
            preview->snapshot_weather = load_netatmo_data();
            preview->snapshot_ticks = frame_utc_ticks;
        }
        auto bitmap =
            frame_utc_ticks == 0
                ? preview->renderer.render(time, load_codex_usage(), load_netatmo_data())
                : preview->renderer.render(time, preview->snapshot_usage, preview->snapshot_weather);
        const auto image = preview->renderer.encode(*bitmap);
        if (image.size() > static_cast<size_t>(capacity))
            throw std::runtime_error("Clock preview exceeds the image buffer");
        memcpy(bytes, image.data(), image.size());
        return static_cast<int>(image.size());
    }
    catch (const std::exception &exception)
    {
        write_error(error, error_capacity, exception);
        return 0;
    }
}
} // namespace

extern "C" __declspec(dllexport) int clock_preview_render(Preview *preview, const SettingsWire *wire,
                                                          BYTE *bytes, int capacity, wchar_t *error,
                                                          int error_capacity)
{
    return render_preview(preview, wire, 0, bytes, capacity, error, error_capacity);
}

extern "C" __declspec(dllexport) int clock_preview_render_at(Preview *preview, const SettingsWire *wire,
                                                             ULONGLONG frame_utc_ticks, BYTE *bytes,
                                                             int capacity, wchar_t *error, int error_capacity)
{
    return render_preview(preview, wire, frame_utc_ticks, bytes, capacity, error, error_capacity);
}

extern "C" __declspec(dllexport) int clock_netatmo_import(const wchar_t *path, wchar_t *error, int capacity)
{
    try
    {
        import_netatmo_credentials(path);
        return 1;
    }
    catch (const std::exception &exception)
    {
        write_error(error, capacity, exception);
        return 0;
    }
}

extern "C" __declspec(dllexport) int clock_netatmo_modules(wchar_t *result, int capacity)
{
    try
    {
        const auto options = netatmo_module_options(load_netatmo_data());
        if ((result == nullptr) || (capacity <= 0) || (options.size() >= static_cast<size_t>(capacity)))
            return 0;
        wcscpy_s(result, static_cast<size_t>(capacity), options.c_str());
        return 1;
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

extern "C" __declspec(dllexport) int clock_netatmo_diagnostics(const SettingsWire *wire, wchar_t *result,
                                                               int capacity)
{
    try
    {
        SYSTEMTIME now = {};
        GetSystemTime(&now);
        const auto diagnostics =
            netatmo_diagnostics(load_netatmo_data(), settings_from_wire(*wire), utc_ticks(now));
        if ((result == nullptr) || (capacity <= 0) || (diagnostics.size() >= static_cast<size_t>(capacity)))
            return 0;
        wcscpy_s(result, static_cast<size_t>(capacity), diagnostics.c_str());
        return 1;
    }
    catch (const std::exception &)
    {
        return 0;
    }
}
