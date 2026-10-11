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

extern "C" __declspec(dllexport) int clock_preview_render(Preview *preview, const SettingsWire *wire,
                                                          BYTE *bytes, int capacity, wchar_t *error,
                                                          int error_capacity)
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
        GetSystemTime(&utc);
        const SYSTEMTIME time = preview->renderer.time_at(utc);
        auto bitmap = preview->renderer.render(time, load_codex_usage());
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
