#include "hub.h"
#include "ui_contract.h"
#include "lock_session.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
int assertions = 0;
void check(bool condition, const char *message)
{
    if (condition == false)
        throw std::runtime_error(message);
    ++assertions;
}

SYSTEMTIME sample_time()
{
    SYSTEMTIME time = {};
    time.wYear = 2026;
    time.wMonth = 10;
    time.wDay = 10;
    time.wDayOfWeek = 6;
    time.wHour = 10;
    time.wMinute = 28;
    time.wSecond = 40;
    return time;
}

void write_image(const std::filesystem::path &path, const std::vector<BYTE> &bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(file.good(), "Could not save rendered test image");
}

void check_jpeg(const std::vector<BYTE> &bytes)
{
    IStream *stream = nullptr;
    check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "Could not make decode stream");
    ULONG written = 0;
    stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written);
    LARGE_INTEGER start = {};
    stream->Seek(start, STREAM_SEEK_SET, nullptr);
    {
        Gdiplus::Bitmap decoded(stream);
        check(decoded.GetLastStatus() == Gdiplus::Ok, "Generated JPEG is invalid");
        check((decoded.GetWidth() == 320) && (decoded.GetHeight() == 240), "JPEG dimensions differ");
    }
    stream->Release();
}

double jpeg_error(Gdiplus::Bitmap &original, const std::vector<BYTE> &bytes)
{
    Microsoft::WRL::ComPtr<IStream> stream;
    check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "Could not make quality decode stream");
    ULONG written = 0;
    stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written);
    LARGE_INTEGER start = {};
    stream->Seek(start, STREAM_SEEK_SET, nullptr);
    Gdiplus::Bitmap decoded(stream.Get());
    double error = 0;
    for (UINT y = 0; y < original.GetHeight(); ++y)
        for (UINT x = 0; x < original.GetWidth(); ++x)
        {
            Gdiplus::Color source, result;
            original.GetPixel(x, y, &source);
            decoded.GetPixel(x, y, &result);
            for (const int difference : {source.GetR() - result.GetR(), source.GetG() - result.GetG(),
                                         source.GetB() - result.GetB()})
                error += difference * difference;
        }
    return error / (original.GetWidth() * original.GetHeight() * 3.0);
}

CLSID image_encoder(const wchar_t *mime)
{
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    std::vector<BYTE> storage(size);
    auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo *>(storage.data());
    check(Gdiplus::GetImageEncoders(count, size, codecs) == Gdiplus::Ok, "Could not find reference encoder");
    for (UINT index = 0; index < count; ++index)
        if (wcscmp(codecs[index].MimeType, mime) == 0)
            return codecs[index].Clsid;
    throw std::runtime_error("Reference image encoder is unavailable");
}

void test_image_quality(const std::filesystem::path &output)
{
    ClockRenderer renderer;
    Settings settings;
    settings.layout = 2;
    settings.theme = 6;
    settings.font = L"Book Antiqua";
    settings.text_color = RGB(232, 217, 183);
    settings.accent_color = RGB(183, 163, 244);
    settings.show_date = false;
    settings.show_weekday = false;
    renderer.configure(settings);
    auto bitmap = renderer.render(sample_time());
    const auto jpeg = renderer.encode(*bitmap);
    write_image(output / L"luxury-analog.jpg", jpeg);
    const CLSID png = image_encoder(L"image/png");
    check(bitmap->Save((output / L"luxury-analog-source.png").c_str(), &png, nullptr) == Gdiplus::Ok,
          "Could not save the uncompressed face");

    Microsoft::WRL::ComPtr<IStream> legacy_stream;
    check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &legacy_stream)),
          "Could not make legacy JPEG stream");
    const CLSID legacy_encoder = image_encoder(L"image/jpeg");
    ULONG quality = 85;
    Gdiplus::EncoderParameters parameters = {};
    parameters.Count = 1;
    parameters.Parameter[0] = {Gdiplus::EncoderQuality, 1, Gdiplus::EncoderParameterValueTypeLong, &quality};
    check(bitmap->Save(legacy_stream.Get(), &legacy_encoder, &parameters) == Gdiplus::Ok,
          "Could not encode the quality reference");
    STATSTG stats = {};
    legacy_stream->Stat(&stats, STATFLAG_NONAME);
    std::vector<BYTE> legacy(static_cast<size_t>(stats.cbSize.QuadPart));
    LARGE_INTEGER start = {};
    legacy_stream->Seek(start, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    legacy_stream->Read(legacy.data(), static_cast<ULONG>(legacy.size()), &read);
    write_image(output / L"luxury-analog-legacy-quality.jpg", legacy);
    const double current_error = jpeg_error(*bitmap, jpeg);
    const double legacy_error = jpeg_error(*bitmap, legacy);
    check(current_error < 2.1, "Clock JPEG introduces visible quantization or color artifacts");
    check(current_error < (legacy_error * 0.15), "JPEG error was not reduced substantially");
    std::ofstream metrics(output / L"image-quality.json");
    metrics << "{\"jpegBytes\":" << jpeg.size() << ",\"legacyBytes\":" << legacy.size()
            << ",\"meanSquaredError\":" << current_error << ",\"legacyMeanSquaredError\":" << legacy_error
            << '}';

    int small_ink = 0;
    for (const int size : {12, 20, 32})
    {
        settings.analog_font_size = size;
        renderer.configure(settings);
        bitmap = renderer.render(sample_time());
        int ink = 0;
        for (int y = 26; y < 66; ++y)
            for (int x = 138; x < 182; ++x)
            {
                Gdiplus::Color pixel;
                bitmap->GetPixel(x, y, &pixel);
                ink += pixel.GetR() > 100 ? 1 : 0;
            }
        if (size == 12)
            small_ink = ink;
        if (size == 32)
            check(ink > (small_ink * 2), "Analog numeral size does not change the visible face");
        write_image(output / (L"analog-size-" + std::to_wstring(size) + L".jpg"), renderer.encode(*bitmap));
    }
    settings.show_seconds = false;
    renderer.configure(settings);
    SYSTEMTIME vertical_time = sample_time();
    vertical_time.wHour = 12;
    vertical_time.wMinute = 0;
    vertical_time.wSecond = 0;
    auto vertical = renderer.render(vertical_time);
    vertical_time.wHour = 3;
    vertical_time.wMinute = 15;
    auto horizontal = renderer.render(vertical_time);
    bool clear_numerals = true;
    for (int y = 26; y < 70; ++y)
        for (int x = 138; x < 182; ++x)
        {
            Gdiplus::Color before, after;
            vertical->GetPixel(x, y, &before);
            horizontal->GetPixel(x, y, &after);
            clear_numerals &= before.GetValue() == after.GetValue();
        }
    check(clear_numerals, "Clock hands overlap the largest cardinal numerals");

    Gdiplus::Bitmap noise(320, 240, PixelFormat24bppRGB);
    uint32_t random = 17;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
        {
            random = (random * 1664525) + 1013904223;
            noise.SetPixel(x, y,
                           Gdiplus::Color(255, random >> 24, (random >> 16) & 255, (random >> 8) & 255));
        }
    const auto complex = renderer.encode(noise);
    check(complex.size() <= 65515, "JPEG quality does not adapt to a complex background's size limit");
    check_jpeg(complex);
}

void test_background_dithering(const std::filesystem::path &output)
{
    const CLSID png = image_encoder(L"image/png");
    Gdiplus::Bitmap gradient(320, 240, PixelFormat24bppRGB);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
        {
            const BYTE value = x < 8 ? 0 : (x >= 312 ? 255 : static_cast<BYTE>(10 + (x / 3)));
            gradient.SetPixel(x, y, Gdiplus::Color(255, value, value, value));
        }
    const auto source_path = output / L"dither-gradient.png";
    check(gradient.Save(source_path.c_str(), &png, nullptr) == Gdiplus::Ok,
          "Could not save the dithering test gradient");
    Settings settings;
    settings.theme = 5;
    settings.background_path = source_path.wstring();
    settings.image_dim = 0;
    settings.show_weekday = false;
    settings.show_date = false;
    settings.show_seconds = false;
    settings.text_color = RGB(255, 255, 255);
    settings.background_dither = 0;
    ClockRenderer renderer;
    renderer.configure(settings);
    auto plain = renderer.render(sample_time());
    const auto plain_jpeg = renderer.encode(*plain);
    write_image(output / L"dither-off.jpg", plain_jpeg);
    settings.background_dither = 4;
    renderer.configure(settings);
    auto dithered = renderer.render(sample_time());
    const auto dithered_jpeg = renderer.encode(*dithered);
    write_image(output / L"dither-on.jpg", dithered_jpeg);
    check_jpeg(dithered_jpeg);
    check(dithered_jpeg.size() <= 65515, "Dithering exceeds the hub's image size limit");
    check(renderer.encode(*renderer.render(sample_time())) == dithered_jpeg,
          "Dithering flickers between identical clock frames");
    SYSTEMTIME later = sample_time();
    later.wMinute = 29;
    auto next = renderer.render(later);
    auto locked = renderer.render_locked(sample_time(), 3, true);
    bool bounded = true, neutral = true, stationary = true, endpoints = true, sharp = true;
    int changed = 0, white_pixels = 0;
    double bias = 0;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
        {
            Gdiplus::Color a, b;
            plain->GetPixel(x, y, &a);
            dithered->GetPixel(x, y, &b);
            if (y < 48)
            {
                const int delta = b.GetR() - a.GetR();
                bounded &= std::abs(delta) <= 4;
                neutral &= (b.GetR() == b.GetG()) && (b.GetG() == b.GetB());
                changed += delta != 0 ? 1 : 0;
                bias += delta;
                Gdiplus::Color c;
                next->GetPixel(x, y, &c);
                stationary &= b.GetValue() == c.GetValue();
                if ((y < 16) && ((x < 32) || (x >= 288)))
                {
                    locked->GetPixel(x, y, &c);
                    stationary &= b.GetValue() == c.GetValue();
                }
                if ((a.GetR() == 0) || (a.GetR() == 255))
                    endpoints &= a.GetValue() == b.GetValue();
            }
            if ((a.GetR() == 255) && (a.GetG() == 255) && (a.GetB() == 255) && (x < 300) && (y >= 80) &&
                (y < 170))
            {
                ++white_pixels;
                sharp &= a.GetValue() == b.GetValue();
            }
        }
    check(bounded && (changed > 10000), "Native-resolution dithering is missing or exceeds its strength");
    check(neutral && (std::abs(bias / (320 * 48)) < 0.08), "Dithering shifts background color or brightness");
    check(stationary, "Normal or locked clock updates move the background noise");
    check(endpoints, "Dithering lifts black or clips white");
    check(sharp && (white_pixels > 100), "Background dithering alters opaque clock text");

    Gdiplus::Bitmap decoded_plain((output / L"dither-off.jpg").c_str());
    Gdiplus::Bitmap decoded_dithered((output / L"dither-on.jpg").c_str());
    // RGB565 is a low-depth display model, not a claim about the hub's undocumented panel format.
    const auto quantize = [](BYTE value, int levels) {
        return std::round(value * levels / 255.0) * 255.0 / levels;
    };
    double plain_error = 0, dithered_error = 0;
    int blocks = 0;
    for (int y = 0; y < 48; y += 8)
        for (int x = 16; x < 304; x += 8)
        {
            double reference = 0, before = 0, after = 0;
            for (int dy = 0; dy < 8; ++dy)
                for (int dx = 0; dx < 8; ++dx)
                {
                    Gdiplus::Color a, b, c;
                    plain->GetPixel(x + dx, y + dy, &a);
                    decoded_plain.GetPixel(x + dx, y + dy, &b);
                    decoded_dithered.GetPixel(x + dx, y + dy, &c);
                    reference += a.GetR();
                    before += (quantize(b.GetR(), 31) + quantize(b.GetG(), 63) + quantize(b.GetB(), 31)) / 3;
                    after += (quantize(c.GetR(), 31) + quantize(c.GetG(), 63) + quantize(c.GetB(), 31)) / 3;
                }
            plain_error += std::pow((before - reference) / 64, 2);
            dithered_error += std::pow((after - reference) / 64, 2);
            ++blocks;
        }
    check(dithered_error < (plain_error * 0.5), "Dithering does not reduce low-depth banding after JPEG");
    std::ofstream metrics(output / L"dither-quality.json");
    metrics << "{\"offBytes\":" << plain_jpeg.size() << ",\"onBytes\":" << dithered_jpeg.size()
            << ",\"rgb565BlockMseOff\":" << (plain_error / blocks)
            << ",\"rgb565BlockMseOn\":" << (dithered_error / blocks) << '}';

    settings.background_dither = 8;
    renderer.configure(settings);
    auto stronger = renderer.render(sample_time());
    check(renderer.encode(*stronger) != dithered_jpeg,
          "Changing dithering strength leaves the cached background");
    settings.background_dither = 0;
    renderer.configure(settings);
    check(renderer.encode(*renderer.render(sample_time())) == plain_jpeg,
          "Turning dithering off leaves noise cached");
    settings.theme = 6;
    renderer.configure(settings);
    const auto black = renderer.encode(*renderer.render(sample_time()));
    settings.background_dither = 8;
    renderer.configure(settings);
    check(renderer.encode(*renderer.render(sample_time())) == black, "Pure black is affected by dithering");

    settings.layout = 2;
    settings.font = L"Book Antiqua";
    settings.text_color = RGB(232, 217, 183);
    settings.accent_color = RGB(183, 163, 244);
    for (int theme = 0; theme < 5; ++theme)
    {
        settings.theme = theme;
        settings.background_dither = 0;
        renderer.configure(settings);
        const auto off = renderer.encode(*renderer.render(sample_time()));
        settings.background_dither = 4;
        renderer.configure(settings);
        const auto on = renderer.encode(*renderer.render(sample_time()));
        check(off != on, "Built-in gradient ignores dithering");
        check(on.size() <= 65515, "Dithered analog clock exceeds the hub's size limit");
        write_image(output / (L"dither-analog-" + std::to_wstring(theme) + L"-off.jpg"), off);
        write_image(output / (L"dither-analog-" + std::to_wstring(theme) + L"-on.jpg"), on);
    }
}

void test_clock()
{
    Settings settings;
    SYSTEMTIME time = sample_time();
    auto text = format_clock(time, settings);
    check(text.time == L"10:28", "24-hour clock differs");
    check(text.detail == L":40", "Seconds differ");
    settings.use_24_hour = false;
    time.wHour = 0;
    text = format_clock(time, settings);
    check(text.time == L"12:28", "12-hour midnight differs");
    check(text.detail == L":40  ·  AM", "Midnight suffix differs");
    time.wHour = 12;
    text = format_clock(time, settings);
    check(text.time == L"12:28", "12-hour noon differs");
    check(text.detail == L":40  ·  PM", "Noon suffix differs");
    settings.show_seconds = false;
    check(format_clock(time, settings).detail == L"PM", "Hidden seconds differ");
    settings.use_24_hour = true;
    check(format_clock(time, settings).detail.empty(), "Hidden seconds still appear");
    settings.date_format = 2;
    check(format_clock(time, settings).date == L"2026-10-10", "ISO date differs");
    time.wSecond = 59;
    time.wMilliseconds = 900;
    check(next_update_schedule(time, 10, 0).delay_ms == 100, "10-second boundary is misaligned");
    check(next_update_schedule(time, 1, 0).delay_ms == 100, "One-second boundary is misaligned");
    time.wHour = 23;
    time.wMinute = 59;
    check(next_update_schedule(time, 86400, 0).delay_ms == 100, "Day boundary is misaligned");
    const DWORD irregular = next_update_schedule(time, 17, 0).delay_ms;
    check((irregular > 0) && (irregular <= 17000), "Irregular interval is invalid");
    time.wSecond = 40;
    time.wMilliseconds = 0;
    check(next_update_schedule(time, 10, 0).delay_ms == 10000, "Exact boundary loops without waiting");
    time.wSecond = 39;
    time.wMilliseconds = 499;
    const auto early = next_update_schedule(time, 10, 500);
    check(early.delay_ms == 1, "Early selection missed its boundary");
    const SYSTEMTIME target = utc_from_ticks(early.target_utc_ticks);
    check((target.wSecond == 40) && (target.wMilliseconds == 0), "Rendered target is not snapped");
    time.wMilliseconds = 500;
    check(next_update_schedule(time, 10, 500).delay_ms == 10000,
          "Exact early-selection boundary loops without waiting");
    time.wSecond = 57;
    time.wMilliseconds = 0;
    const auto midnight = next_update_schedule(time, 10, 2500);
    const SYSTEMTIME midnight_target = utc_from_ticks(midnight.target_utc_ticks);
    check(midnight.delay_ms == 500, "Upload preparation is not scheduled before selection");
    check((midnight_target.wDay == 11) && (midnight_target.wHour == 0) && (midnight_target.wMinute == 0) &&
              (midnight_target.wSecond == 0),
          "Future frame does not cross midnight correctly");
    const auto slow = next_update_schedule(time, 1, 2500);
    check((slow.delay_ms > 0) && (slow.delay_ms <= 1000), "Slow uploads cause a short-interval busy loop");
    const std::vector<BYTE> reference = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(image_crc(reference) == 0x29b1, "CRC16 reference vector differs");
}

void test_lock_session(const std::filesystem::path &output)
{
    LockSession session;
    const ULONGLONG wall_time = utc_ticks(sample_time());
    session.observe(true, true, wall_time, 1000);
    check(session.locked && session.lock_time_known, "A Windows lock event was not recorded");
    check(session.elapsed_minutes(60999) == 0, "Lock elapsed minutes rounded up");
    check(session.elapsed_minutes(61000) == 1, "Lock elapsed minutes missed their boundary");
    session.observe(true, true, wall_time + 600000000, 61000);
    check((session.locked_utc_ticks == wall_time) && (session.started_ms == 1000),
          "Duplicate lock notifications reset the lock time");
    check(session.elapsed_minutes(3610000) == 60, "An hour of lock time was lost");
    const auto next = next_lock_update(session, 2000, 2500);
    check((next.target_ms == 61000) && (next.elapsed_minutes == 1) && (next.delay_ms == 56500),
          "Lock updates are not aligned to elapsed minutes");
    const auto exact = next_lock_update(session, 58500, 2500);
    check((exact.target_ms == 121000) && (exact.delay_ms == 60000),
          "Lock minute preparation loops at an exact boundary");
    session.observe(false, true, wall_time, 5000000);
    check((session.locked == false) && (session.locked_utc_ticks == 0) &&
              (session.elapsed_minutes(5000000) == 0),
          "Unlock did not clear the lock episode");
    session.observe(true, false, wall_time, 5000000);
    check(session.locked && (session.lock_time_known == false), "Startup invented an earlier lock time");
    session.observe(false, true, wall_time, 5000001);
    session.observe(true, true, wall_time + 5000000, 5000002);
    check(session.lock_time_known && (session.started_ms == 5000002),
          "A later lock reused the previous time");
    check(format_locked_duration(0) == L"0 min", "Zero locked duration differs");
    check(format_locked_duration(59) == L"59 min", "Minute locked duration differs");
    check(format_locked_duration(60) == L"1 h 00 min", "Hour locked duration differs");
    check(format_locked_duration(1441) == L"1 d 00 h 01 min", "Day locked duration differs");

    ClockRenderer renderer;
    Settings settings;
    settings.theme = 6;
    settings.font = L"Book Antiqua";
    settings.text_color = RGB(232, 217, 183);
    settings.show_codex_usage = false;
    renderer.configure(settings);
    for (const ULONGLONG minute : {0ULL, 1ULL, 61ULL, 1441ULL, 525600ULL})
    {
        auto bitmap = renderer.render_locked(sample_time(), minute, true);
        auto image = renderer.encode(*bitmap);
        check(image.size() <= 65515, "Lock image exceeds the hub's size limit");
        check_jpeg(image);
        write_image(output / (L"locked-" + std::to_wstring(minute) + L".jpg"), image);
        Gdiplus::Color corner;
        bitmap->GetPixel(0, 0, &corner);
        check(corner.GetValue() == Gdiplus::Color(255, 0, 0, 0).GetValue(),
              "Lock screen lost its pure black background");
    }
    auto unknown = renderer.render_locked(sample_time(), 3, false);
    write_image(output / L"locked-startup.jpg", renderer.encode(*unknown));
}

void test_rendering(const std::filesystem::path &output)
{
    std::filesystem::create_directories(output);
    ClockRenderer renderer;
    Settings settings;
    const SYSTEMTIME time = sample_time();
    Settings digital_preview;
    digital_preview.show_seconds = false;
    digital_preview.show_date = false;
    digital_preview.show_weekday = false;
    renderer.configure(digital_preview);
    write_image(output / L"readme-digital.jpg", renderer.encode(*renderer.render(time)));
    for (int layout = 0; layout < 3; ++layout)
    {
        settings.layout = layout;
        for (int theme : {0, 1, 2, 3, 4, 6})
        {
            settings.theme = theme;
            renderer.configure(settings);
            auto bitmap = renderer.render(time);
            auto image = renderer.encode(*bitmap);
            check((image.size() > 1000) && (image.size() <= 65515), "Encoded image exceeds limits");
            check_jpeg(image);
            if (theme == 6)
            {
                for (const auto point : {Gdiplus::Point(0, 0), Gdiplus::Point(319, 0), Gdiplus::Point(0, 239),
                                         Gdiplus::Point(319, 239)})
                {
                    Gdiplus::Color pixel;
                    bitmap->GetPixel(point.X, point.Y, &pixel);
                    check((pixel.GetR() == 0) && (pixel.GetG() == 0) && (pixel.GetB() == 0),
                          "Pure black background contains a gradient");
                }
                write_image(output / (L"black-layout-" + std::to_wstring(layout) + L".jpg"), image);
                if (layout == 2)
                {
                    for (const auto point : {Gdiplus::Point(160, 12), Gdiplus::Point(308, 120),
                                             Gdiplus::Point(160, 228), Gdiplus::Point(12, 120)})
                    {
                        Gdiplus::Color pixel;
                        bitmap->GetPixel(point.X, point.Y, &pixel);
                        check(pixel.GetR() > 200, "Analog hour marks do not reach the display perimeter");
                    }
                    for (const auto region :
                         {Gdiplus::Rect(143, 26, 34, 30), Gdiplus::Rect(270, 105, 24, 30),
                          Gdiplus::Rect(143, 184, 34, 30), Gdiplus::Rect(26, 105, 24, 30)})
                    {
                        bool labeled = false;
                        for (int y = region.Y; y < region.GetBottom(); ++y)
                            for (int x = region.X; x < region.GetRight(); ++x)
                            {
                                Gdiplus::Color pixel;
                                bitmap->GetPixel(x, y, &pixel);
                                labeled |= pixel.GetR() > 150;
                            }
                        check(labeled, "Analog cardinal hour label is missing");
                    }
                }
            }
            if (theme == 0)
                write_image(output / (L"layout-" + std::to_wstring(layout) + L".jpg"), image);
            if (layout == 0)
                write_image(output / (L"theme-" + std::to_wstring(theme) + L".jpg"), image);
        }
    }
    settings.layout = 0;
    settings.font = L"Arial";
    settings.font_style = Gdiplus::FontStyleBold | Gdiplus::FontStyleItalic;
    settings.font_size = 110;
    settings.vertical_offset = 20;
    settings.label = L"Kyiv";
    renderer.configure(settings);
    auto custom = renderer.render(time);
    auto jpeg = renderer.encode(*custom);
    const uint16_t crc = image_crc(jpeg);
    distinguish_crc(jpeg, crc);
    check(image_crc(jpeg) != crc, "CRC collision was not resolved");
    check_jpeg(jpeg);
    write_image(output / L"custom-font.jpg", jpeg);
    for (int layout = 0; layout < 3; ++layout)
    {
        settings.layout = layout;
        settings.vertical_offset = 0;
        renderer.configure(settings);
        auto reference = renderer.render(time);
        for (int offset : {-20, 20})
        {
            settings.vertical_offset = offset;
            renderer.configure(settings);
            auto shifted = renderer.render(time);
            bool margins_match = true;
            for (int y = 0; y < 240; ++y)
            {
                if ((y > 50) && (y < 196))
                    continue;
                for (int x = 0; x < 320; ++x)
                {
                    Gdiplus::Color a, b;
                    reference->GetPixel(x, y, &a);
                    shifted->GetPixel(x, y, &b);
                    margins_match &= a.GetValue() == b.GetValue();
                }
            }
            check(margins_match, "Moving the clock overlaps the heading or date");
            write_image(output /
                            (L"offset-" + std::to_wstring(layout) + L"-" + std::to_wstring(offset) + L".jpg"),
                        renderer.encode(*shifted));
        }
    }
    settings.layout = 0;
    settings.theme = 5;
    Gdiplus::Bitmap wide(640, 240, PixelFormat24bppRGB);
    {
        Gdiplus::Graphics graphics(&wide);
        graphics.Clear(Gdiplus::Color(255, 240, 20, 30));
        Gdiplus::SolidBrush green(Gdiplus::Color(255, 20, 220, 30));
        graphics.FillRectangle(&green, 320, 0, 320, 240);
    }
    write_image(output / L"wide-background.jpg", renderer.encode(wide));
    settings.background_path = (output / L"wide-background.jpg").wstring();
    settings.image_dim = 0;
    for (int fit = 0; fit < 2; ++fit)
    {
        settings.background_fit = fit;
        renderer.configure(settings);
        auto bitmap = renderer.render(time);
        check_jpeg(renderer.encode(*bitmap));
        Gdiplus::Color corner;
        bitmap->GetPixel(10, 10, &corner);
        check(fit == 0 ? corner.GetR() > 200 : corner.GetR() < 20,
              "Background fill/fit did not crop or letterbox correctly");
    }
    settings.background_fit = 0;
    settings.image_dim = 50;
    renderer.configure(settings);
    auto dimmed = renderer.render(time);
    Gdiplus::Color corner;
    dimmed->GetPixel(10, 10, &corner);
    check((corner.GetR() > 100) && (corner.GetR() < 140), "Image dimming was not applied");
    const CLSID png = image_encoder(L"image/png");
    for (const int scale : {1, 2})
    {
        Gdiplus::Bitmap uniform(320 * scale, 240 * scale, PixelFormat24bppRGB);
        Gdiplus::Graphics source(&uniform);
        source.Clear(Gdiplus::Color(255, 240, 240, 240));
        const auto path = output / (L"uniform-wallpaper-" + std::to_wstring(scale) + L".png");
        check(uniform.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok, "Could not save wallpaper fixture");
        settings.background_path = path.wstring();
        settings.background_dither = 0;
        for (const int dim : {0, 50, 85})
        {
            settings.image_dim = dim;
            renderer.configure(settings);
            auto frame = renderer.render(time);
            write_image(output / (L"wallpaper-edge-" + std::to_wstring(scale) + L"-" + std::to_wstring(dim) +
                                  L".jpg"),
                        renderer.encode(*frame));
            Gdiplus::Color interior;
            frame->GetPixel(10, 10, &interior);
            for (const auto &point : {Gdiplus::Point(0, 0), Gdiplus::Point(160, 0), Gdiplus::Point(0, 120),
                                      Gdiplus::Point(319, 0), Gdiplus::Point(0, 239)})
            {
                Gdiplus::Color edge;
                frame->GetPixel(point.X, point.Y, &edge);
                check(std::abs(edge.GetR() - interior.GetR()) <= 1,
                      ("Wallpaper edge differs from interior at " + std::to_string(point.X) + "," +
                       std::to_string(point.Y) + ": " + std::to_string(edge.GetR()) + " vs " +
                       std::to_string(interior.GetR()))
                          .c_str());
            }
        }
    }
    settings.background_path = (output / L"missing.jpg").wstring();
    bool rejected = false;
    try
    {
        renderer.configure(settings);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Missing background was accepted");
    settings.theme = 0;
    settings.time_zone = L"UTC";
    renderer.configure(settings);
    SYSTEMTIME utc = {};
    GetSystemTime(&utc);
    const SYSTEMTIME zone_time = renderer.current_time();
    check((zone_time.wHour == utc.wHour) && (zone_time.wMinute == utc.wMinute),
          "UTC zone conversion differs");
    const SYSTEMTIME future = utc_from_ticks(utc_ticks(utc) + 200000000);
    const SYSTEMTIME rendered_future = renderer.time_at(future);
    check((rendered_future.wDay == future.wDay) && (rendered_future.wHour == future.wHour) &&
              (rendered_future.wMinute == future.wMinute) && (rendered_future.wSecond == future.wSecond),
          "Future clock frame uses the current time instead of its target time");
}

void test_preferences()
{
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId());
    const std::wstring key = L"Software\\AnkerClock.Tests." + suffix;
    const std::wstring run_value = L"AnkerClock.Tests." + suffix;
    struct Cleanup
    {
        std::wstring key;
        std::wstring value;
        ~Cleanup()
        {
            RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
            HKEY run = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                              KEY_SET_VALUE, &run) == ERROR_SUCCESS)
            {
                RegDeleteValueW(run, value.c_str());
                RegCloseKey(run);
            }
        }
    } cleanup{key, run_value};
    Settings settings;
    check(settings.show_codex_usage == false, "Codex queries are enabled before opting in");
    check(load_settings(key.c_str()).show_codex_usage == false, "Fresh preferences enable Codex queries");
    settings.font = L"Consolas";
    settings.background_path = L"C:\\Photos\\Україна.png";
    settings.time_zone = L"FLE Standard Time";
    settings.label = L"Kyiv";
    settings.theme = 5;
    settings.font_size = 54;
    settings.analog_font_size = 28;
    settings.analog_face = 2;
    settings.reading_font_size = 28;
    settings.font_style = 1;
    settings.layout = 2;
    settings.date_format = 2;
    settings.interval_seconds = 17;
    settings.hub_brightness = 44;
    settings.vertical_offset = -12;
    settings.background_fit = 1;
    settings.text_color = RGB(3, 4, 5);
    settings.accent_color = RGB(10, 20, 30);
    settings.image_dim = 72;
    settings.background_dither = 6;
    settings.use_24_hour = false;
    settings.show_seconds = false;
    settings.show_date = false;
    settings.show_weekday = false;
    settings.pause_on_battery = true;
    settings.show_settings_on_launch = false;
    settings.show_codex_usage = true;
    save_settings(settings, key.c_str());
    check(load_settings(key.c_str()) == settings, "Preferences did not round-trip");
    HKEY legacy_preferences = nullptr;
    check(RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &legacy_preferences) ==
              ERROR_SUCCESS,
          "Could not open legacy gallery preferences");
    for (const DWORD legacy_gallery : {0UL, 1UL})
    {
        check(RegSetValueExW(legacy_preferences, L"ExclusiveGallery", 0, REG_DWORD,
                             reinterpret_cast<const BYTE *>(&legacy_gallery),
                             sizeof(legacy_gallery)) == ERROR_SUCCESS,
              "Could not set legacy gallery preference");
        check(load_settings(key.c_str()) == settings, "Legacy gallery preference still changes settings");
    }
    RegCloseKey(legacy_preferences);
    settings.show_netatmo = true;
    settings.netatmo_module_id = L"living";
    settings.netatmo_readings = netatmo_temperature | netatmo_co2;
    settings.netatmo_refresh_seconds = 60;
    save_settings(settings, key.c_str());
    check(load_settings(key.c_str()) == settings, "Netatmo preferences did not round-trip");
    settings.theme = 6;
    save_settings(settings, key.c_str());
    check(load_settings(key.c_str()) == settings, "Pure black background did not persist");
    check(starts_with_windows(run_value.c_str()) == false, "Isolated startup value already exists");
    set_start_with_windows(true, run_value.c_str(), executable_path());
    check(starts_with_windows(run_value.c_str()), "Startup was not enabled");
    set_start_with_windows(false, run_value.c_str(), executable_path());
    check(starts_with_windows(run_value.c_str()) == false, "Startup was not disabled");
    settings.font.assign(128, L'A');
    settings.background_path.assign(32768, L'B');
    settings.time_zone.assign(128, L'C');
    settings.label.assign(41, L'D');
    save_settings(settings, key.c_str());
    const Settings bounded = load_settings(key.c_str());
    check(bounded.font == Settings{}.font, "Oversized saved font was not reset");
    check(bounded.background_path.empty(), "Oversized saved background path was accepted");
    check(bounded.time_zone.empty(), "Oversized saved time zone was accepted");
    check(bounded.label.empty(), "Oversized saved heading was accepted");
    check(settings_from_wire(settings_to_wire(bounded, false)) == bounded,
          "Recovered preferences could not cross the UI boundary");
    settings.font.clear();
    save_settings(settings, key.c_str());
    check(load_settings(key.c_str()).font == Settings{}.font, "Empty saved font prevented opening settings");
    settings.background_dither = 100;
    save_settings(settings, key.c_str());
    check(load_settings(key.c_str()).background_dither == 8, "Saved dithering strength is not bounded");
    HKEY preferences = nullptr;
    check(RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &preferences) == ERROR_SUCCESS,
          "Could not open isolated dithering preferences");
    RegDeleteValueW(preferences, L"BackgroundDither");
    RegCloseKey(preferences);
    check(load_settings(key.c_str()).background_dither == 4, "Existing preferences do not enable dithering");
}

void test_ui_contract()
{
    Settings settings;
    settings.font = L"Book Antiqua";
    settings.label = L"Час · 東京";
    settings.background_path = L"C:\\Photos\\Україна.png";
    settings.theme = 6;
    settings.interval_seconds = 86400;
    settings.analog_font_size = 32;
    settings.analog_face = 2;
    settings.reading_font_size = 38;
    settings.background_dither = 8;
    settings.show_netatmo = true;
    settings.netatmo_module_id = L"station";
    settings.netatmo_refresh_seconds = 86400;
    settings.netatmo_readings = 15;
    const SettingsWire wire = settings_to_wire(settings, true);
    check(settings_from_wire(wire) == settings, "UI settings changed in the binary contract");
    check(wire.start_windows == 1, "Startup flag did not cross the UI boundary");
    check((wire.flags & 128) == 0, "Removed gallery toggle is still exported to Settings");
    auto legacy_gallery_option = wire;
    legacy_gallery_option.flags |= 128;
    check(settings_from_wire(legacy_gallery_option) == settings,
          "Deprecated gallery flag still changes settings");
    auto legacy_lock_option = wire;
    legacy_lock_option.flags |= 32;
    check(settings_from_wire(legacy_lock_option) == settings,
          "Deprecated lock-pause flag still changes settings");
    const auto reject = [](const SettingsWire &invalid) {
        bool rejected = false;
        try
        {
            settings_from_wire(invalid);
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        check(rejected, "Malformed UI settings were accepted");
    };
    auto invalid = wire;
    invalid.version = 1;
    reject(invalid);
    invalid = wire;
    invalid.analog_font_size = 11;
    reject(invalid);
    invalid.analog_font_size = 33;
    reject(invalid);
    invalid = wire;
    invalid.analog_face = -1;
    reject(invalid);
    invalid.analog_face = 3;
    reject(invalid);
    invalid = wire;
    invalid.reading_font_size = 27;
    reject(invalid);
    invalid.reading_font_size = 39;
    reject(invalid);
    invalid = wire;
    invalid.interval_seconds = 0;
    reject(invalid);
    invalid.interval_seconds = 86401;
    reject(invalid);
    invalid = wire;
    invalid.flags = 1024;
    reject(invalid);
    invalid = wire;
    invalid.codex_refresh_seconds = 59;
    reject(invalid);
    invalid.codex_refresh_seconds = 86401;
    reject(invalid);
    invalid = wire;
    invalid.background_dither = -1;
    reject(invalid);
    invalid.background_dither = 9;
    reject(invalid);
    invalid = wire;
    invalid.netatmo_refresh_seconds = 59;
    reject(invalid);
    invalid.netatmo_refresh_seconds = 86401;
    reject(invalid);
    invalid = wire;
    invalid.netatmo_readings = 0;
    reject(invalid);
    invalid.netatmo_readings = 16;
    reject(invalid);
    invalid = wire;
    invalid.layout = 2;
    invalid.flags |= 256;
    reject(invalid);
    invalid.netatmo_readings = 13;
    check(settings_from_wire(invalid).netatmo_readings == 13,
          "Three weather readings and Codex were rejected");
    invalid = wire;
    std::fill(std::begin(invalid.netatmo_module_id), std::end(invalid.netatmo_module_id), L'A');
    reject(invalid);
    invalid = wire;
    invalid.start_windows = 2;
    reject(invalid);
    invalid = wire;
    std::fill(std::begin(invalid.label), std::end(invalid.label), L'A');
    reject(invalid);
    invalid = wire;
    invalid.font[0] = 0;
    reject(invalid);
    invalid = wire;
    invalid.theme = 5;
    invalid.background_path[0] = 0;
    reject(invalid);
    for (auto member : {&Settings::font, &Settings::time_zone, &Settings::label, &Settings::background_path})
    {
        Settings oversized = settings;
        (oversized.*member).assign(member == &Settings::background_path ? 32768 : 128, L'A');
        bool rejected = false;
        try
        {
            settings_to_wire(oversized, false);
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        check(rejected, "Oversized UI text aborted instead of reporting an error");
    }
}

void test_transfer_cancellation()
{
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE change = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    {
        HubConnection hub(stop, change);
        bool interrupted = false;
        try
        {
            hub.gallery();
        }
        catch (const std::exception &error)
        {
            interrupted = std::string(error.what()) == "Clock update interrupted";
        }
        check(interrupted, "A changed clock setting did not cancel a pending hub operation");
        check(WaitForSingleObject(change, 0) == WAIT_OBJECT_0,
              "Transfer cancellation consumed the worker wake");
    }
    CloseHandle(change);
    CloseHandle(stop);
}

void test_hub(const std::filesystem::path &output)
{
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    try
    {
        HubConnection hub(stop);
        hub.connect();
        std::wcout << L"Connected: " << hub.port_name() << L'\n';
        hub.initialize();
        hub.set_brightness(100);
        ClockRenderer renderer;
        Settings settings;
        renderer.configure(settings);
        std::vector<BYTE> expected;
        std::ofstream results(output / L"hub-results.json");
        results << "{\"frames\":[";
        for (int frame = 0; frame < 3; ++frame)
        {
            auto bitmap = renderer.render(renderer.current_time());
            expected = renderer.encode(*bitmap);
            const FrameResult result = hub.replace_image(expected);
            const auto gallery = hub.gallery();
            check(gallery[3] == 1, "Old images remained in the clock gallery");
            write_image(output / (L"hub-frame-" + std::to_wstring(frame) + L".jpg"), expected);
            if (frame > 0)
                results << ',';
            results << "{\"bytes\":" << result.bytes << ",\"uploadMs\":" << result.elapsed_ms
                    << ",\"crc\":" << result.crc << '}';
            std::cout << "Frame " << frame << ": " << result.bytes << " bytes, " << result.elapsed_ms
                      << " ms\n";
            if (frame < 2)
                WaitForSingleObject(stop, 10000);
        }
        WaitForSingleObject(stop, 1500);
        const auto actual = hub.read_image();
        check(actual == expected, "Physical clock image differs from rendered source");
        write_image(output / L"physical-clock.jpg", actual);
        results << "],\"byteVerified\":true,\"galleryCount\":1}";
    }
    catch (...)
    {
        CloseHandle(stop);
        throw;
    }
    CloseHandle(stop);
}

void probe_hub(const std::filesystem::path &output)
{
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    try
    {
        HubConnection hub(stop);
        hub.connect();
        const auto gallery = hub.gallery();
        const auto current = hub.current_image();
        check(gallery[3] <= 8, "Hub gallery count is invalid");
        check((gallery[3] == 0) || ((current[2] < 8) && ((gallery[5] & (1 << current[2])) != 0)),
              "Hub selected slot is inconsistent with its gallery");
        std::ofstream result(output / L"hub-probe.json");
        result << "{\"galleryCount\":" << static_cast<int>(gallery[3])
               << ",\"galleryBitmap\":" << static_cast<int>(gallery[5])
               << ",\"selectedSlot\":" << static_cast<int>(current[2])
               << ",\"selectedCrc\":" << ((current[3] << 8) | current[4]) << ",\"galleryModified\":false}";
        std::wcout << L"Port " << hub.port_name() << L", gallery " << static_cast<int>(gallery[3])
                   << L", selected slot " << static_cast<int>(current[2]) << L"; queries only\n";
    }
    catch (...)
    {
        CloseHandle(stop);
        throw;
    }
    CloseHandle(stop);
}

void test_hub_recovery(const std::filesystem::path &output)
{
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    try
    {
        HubConnection hub(stop);
        hub.connect();
        hub.initialize();
        const auto before = hub.gallery();
        const auto original = hub.read_image();
        const FrameResult result = hub.replace_image(original);
        check(result.crc != image_crc(original), "Existing image identifier was reused");
        auto expected = original;
        distinguish_crc(expected, image_crc(original));
        check(result.crc == image_crc(expected), "Collision changed the JPEG unexpectedly");
        WaitForSingleObject(stop, 1500);
        check(hub.read_image() == expected, "Collision-safe JPEG differs on the device");
        const auto journal = [&](uint16_t pending, uint16_t previous, bool had_previous, BYTE count) {
            const BYTE record[] = {'A',
                                   'C',
                                   1,
                                   static_cast<BYTE>(had_previous),
                                   static_cast<BYTE>(pending >> 8),
                                   static_cast<BYTE>(pending),
                                   static_cast<BYTE>(previous >> 8),
                                   static_cast<BYTE>(previous),
                                   count};
            HKEY key = nullptr;
            check(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnkerClock", 0, nullptr, 0, KEY_SET_VALUE,
                                  nullptr, &key, nullptr) == ERROR_SUCCESS,
                  "Could not open transfer journal");
            const LSTATUS saved = RegSetValueExW(key, L"PendingFrame", 0, REG_BINARY, record, sizeof(record));
            RegCloseKey(key);
            check(saved == ERROR_SUCCESS, "Could not simulate an interrupted transfer");
        };
        const auto recovered = [&] {
            DWORD bytes = 0;
            check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\AnkerClock", L"PendingFrame", RRF_RT_REG_BINARY,
                               nullptr, nullptr, &bytes) == ERROR_FILE_NOT_FOUND,
                  "Completed transfer journal was not removed");
            const auto current = hub.current_image();
            check(((current[3] << 8) | current[4]) == result.crc, "Recovery selected another image");
            const auto after = hub.gallery();
            check(after[3] == 1, "Recovery retained old gallery images");
        };
        journal(image_crc(original), result.crc, true, before[3]);
        hub.initialize();
        recovered();
        journal(result.crc, image_crc(original), true, before[3]);
        hub.initialize();
        recovered();
        journal(result.crc, image_crc(original), false, 1);
        hub.initialize();
        recovered();
        write_image(output / L"physical-recovery.jpg", expected);
    }
    catch (...)
    {
        CloseHandle(stop);
        throw;
    }
    CloseHandle(stop);
}

void capture_hub(const std::filesystem::path &output)
{
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    try
    {
        HubConnection hub(stop);
        hub.connect();
        const auto current = hub.current_image();
        const auto image = hub.read_image();
        check(image_crc(image) == ((current[3] << 8) | current[4]),
              "Selected image CRC differs from readback");
        check_jpeg(image);
        write_image(output / L"physical-app-clock.jpg", image);
    }
    catch (...)
    {
        CloseHandle(stop);
        throw;
    }
    CloseHandle(stop);
}
void test_codex_discovery(const std::filesystem::path &output)
{
    struct Environment
    {
        const wchar_t *name;
        std::wstring value;
        Environment(const wchar_t *variable) : name(variable)
        {
            wchar_t buffer[32768] = {};
            const DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
            check(length < 32768, "Test environment variable is too long");
            value.assign(buffer, length);
        }
        ~Environment()
        {
            SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
        }
    } saved_path(L"PATH"), saved_local(L"LOCALAPPDATA");
    struct Directory
    {
        std::filesystem::path previous = std::filesystem::current_path();
        ~Directory()
        {
            std::error_code error;
            std::filesystem::current_path(previous, error);
        }
    } saved_directory;
    const auto root =
        std::filesystem::absolute(output) / L"codex-discovery" / std::to_wstring(GetCurrentProcessId());
    const auto path_directory = root / L"path with spaces";
    const auto installed = root / L"local" / L"OpenAI" / L"Codex" / L"bin";
    std::filesystem::create_directories(path_directory);
    std::filesystem::current_path(root);
    write_image(root / L"codex.exe", {});
    check(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"local").c_str()) != FALSE,
          "Could not isolate Codex discovery");
    check(SetEnvironmentVariableW(L"PATH", L";.;;path with spaces;") != FALSE,
          "Could not isolate Codex PATH");
    bool rejected = false;
    try
    {
        find_codex_executable();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Codex discovery accepted an executable from the working directory");
    write_image(path_directory / L"codex.exe", {});
    check(SetEnvironmentVariableW(L"PATH", (L"\"" + path_directory.wstring() + L"\"").c_str()) != FALSE,
          "Could not set explicit Codex PATH");
    check(find_codex_executable() == (path_directory / L"codex.exe").wstring(),
          "Codex was not found in a quoted absolute PATH entry");
    std::filesystem::create_directories(installed / L"older");
    std::filesystem::create_directories(installed / L"newer");
    std::filesystem::create_directories(installed / L"invalid" / L"codex.exe");
    write_image(installed / L"older" / L"codex.exe", {});
    write_image(installed / L"newer" / L"codex.exe", {});
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(installed / L"older" / L"codex.exe", now - std::chrono::hours(1));
    std::filesystem::last_write_time(installed / L"newer" / L"codex.exe", now);
    check(find_codex_executable() == (path_directory / L"codex.exe").wstring(),
          "Installed fallback overrides an explicit Codex PATH");
    check(SetEnvironmentVariableW(L"PATH", L";.;;path with spaces;") != FALSE, "Could not reset Codex PATH");
    check(find_codex_executable() == (installed / L"newer" / L"codex.exe").wstring(),
          "Codex desktop fallback did not select the newest regular executable");
}

void test_codex_usage(const std::filesystem::path &output)
{
    const ULONGLONG now = utc_ticks(sample_time());
    const auto weekly = parse_codex_usage(
        LR"({"id":2,"result":{"rateLimits":{"primary":{"usedPercent":99}},"rateLimitsByLimitId":{"codex":{"primary":{"usedPercent":9,"windowDurationMins":10080,"resetsAt":1792295944},"secondary":null},"other":{"primary":{"usedPercent":100}}}}})",
        now);
    check((weekly.count == 1) && (weekly.windows[0].remaining_percent == 91),
          "Codex remaining percentage or bucket priority differs");
    check(weekly.windows[0].duration_minutes == 10080, "Weekly Codex window was mislabeled");
    auto text = format_codex_usage(weekly, now, 60);
    check(text.value == L"91%", "Weekly usage text differs");
    const auto check_empty = [](const CodexUsageText &value) {
        check((value.value.empty()) && (value.remaining_percent == -1),
              "Unavailable Codex usage still shows text or a progress bar");
    };
    text = format_codex_usage(weekly, now + 1200000000, 60);
    check_empty(text);
    check(format_codex_usage(weekly, now + 1199999999, 60).value == L"91%",
          "Fresh cached usage was hidden before its freshness deadline");
    check_empty(format_codex_usage(weekly, now - 1, 60));
    auto failed = weekly;
    failed.refresh_failed = true;
    check_empty(format_codex_usage(failed, now, 60));
    auto recovered = failed;
    recovered.refresh_failed = false;
    check(format_codex_usage(recovered, now, 60).value == L"91%",
          "Successful Codex refresh did not restore the percentage");
    const auto both = parse_codex_usage(
        LR"({"id":2,"result":{"rateLimits":{"primary":{"usedPercent":28,"windowDurationMins":300,"resetsAt":1792295944},"secondary":{"usedPercent":9,"windowDurationMins":10080,"resetsAt":1792295944}}}})",
        now);
    text = format_codex_usage(both, now, 60);
    check((both.count == 2) && (text.value == L"72%") && (text.remaining_percent == 72),
          "Multiple Codex windows lost their individual allowances");
    const auto no_reset = parse_codex_usage(
        LR"({"result":{"rateLimits":{"primary":{"usedPercent":9,"windowDurationMins":null,"resetsAt":null}}}})",
        now);
    check((no_reset.count == 1) && (format_codex_usage(no_reset, now, 60).value == L"91%"),
          "Nullable quota metadata prevented showing available usage");
    const auto no_windows =
        parse_codex_usage(LR"({"result":{"rateLimits":{"primary":null,"secondary":null}}})", now);
    check(no_windows.count == 0, "Missing quota windows were represented as unused quota");
    check_empty(format_codex_usage(no_windows, now, 60));
    check_empty(format_codex_usage(CodexUsage{}, now, 60));
    auto unavailable = CodexUsage{};
    unavailable.refresh_failed = true;
    check_empty(format_codex_usage(unavailable, now, 60));
    auto expired = weekly;
    expired.windows[0].resets_at = 1;
    check_empty(format_codex_usage(expired, now, 60));
    const auto capped = parse_codex_usage(
        LR"({"result":{"rateLimits":{"primary":{"usedPercent":101},"secondary":{"usedPercent":-5}}}})", now);
    check((capped.windows[0].remaining_percent == 0) && (capped.windows[1].remaining_percent == 100),
          "Usage percentages were not bounded");
    check(format_codex_usage(capped, now, 60).value == L"0%", "An exhausted quota was hidden as unavailable");
    for (const wchar_t *invalid :
         {L"{", LR"({"error":{"message":"Sign in required"}})",
          LR"({"result":{"rateLimits":{"primary":{"usedPercent":null}}}})",
          LR"({"result":{"rateLimits":{"primary":{"usedPercent":9,"resetsAt":-1}}}})",
          LR"({"result":{"rateLimits":{"primary":{"usedPercent":9,"windowDurationMins":1.5}}}})",
          LR"({"result":{"rateLimitsByLimitId":{"other":{}},"rateLimits":{}}})"})
    {
        bool rejected = false;
        try
        {
            parse_codex_usage(invalid, now);
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        check(rejected, "Invalid usage data was accepted");
    }
    SYSTEMTIME time = sample_time();
    time.wSecond = 57;
    time.wMilliseconds = 499;
    auto next = next_update_schedule(time, 60, 2500);
    check((next.delay_ms == 1) && (utc_from_ticks(next.target_utc_ticks).wSecond == 0),
          "Codex minute query is not 2.5 seconds before :00");
    time.wMilliseconds = 500;
    check(next_update_schedule(time, 60, 2500).delay_ms == 60000,
          "Codex query loops at the exact preparation boundary");
    Settings settings;
    settings.layout = 2;
    settings.theme = 6;
    // Keep glyph-size assertions independent of optional fonts installed on the machine.
    settings.font = L"Arial";
    settings.text_color = RGB(232, 217, 183);
    settings.accent_color = RGB(183, 163, 244);
    settings.show_seconds = false;
    settings.show_weekday = false;
    settings.show_date = true;
    settings.show_codex_usage = true;
    ClockRenderer renderer;
    renderer.configure(settings);
    CodexUsage fresh = weekly;
    SYSTEMTIME utc = {};
    GetSystemTime(&utc);
    fresh.fetched_utc_ticks = utc_ticks(utc);
    fresh.windows[0].resets_at = 0;
    auto failed_fresh = fresh;
    failed_fresh.refresh_failed = true;
    auto expired_fresh = fresh;
    expired_fresh.windows[0].resets_at = 1;
    auto stale_fresh = fresh;
    stale_fresh.fetched_utc_ticks -= 1200000000;
    auto missing_timestamp = fresh;
    missing_timestamp.fetched_utc_ticks = 0;
    const auto same_pixels = [](Gdiplus::Bitmap &a, Gdiplus::Bitmap &b) {
        for (UINT y = 0; y < a.GetHeight(); ++y)
            for (UINT x = 0; x < a.GetWidth(); ++x)
            {
                Gdiplus::Color first, second;
                a.GetPixel(x, y, &first);
                b.GetPixel(x, y, &second);
                if (first.GetValue() != second.GetValue())
                    return false;
            }
        return true;
    };
    auto blank = renderer.render(sample_time());
    auto blank_locked = renderer.render_locked(sample_time(), 1, true);
    for (const auto &unavailable_usage :
         {CodexUsage{}, unavailable, no_windows, failed_fresh, stale_fresh, expired_fresh, missing_timestamp})
    {
        auto analog = renderer.render(sample_time(), unavailable_usage);
        auto locked = renderer.render_locked(sample_time(), 1, true, unavailable_usage);
        check(same_pixels(*analog, *blank), "Unavailable Codex usage leaves ink on the analog clock");
        check(same_pixels(*locked, *blank_locked), "Unavailable Codex usage leaves ink on the lock screen");
    }
    const auto blank_jpeg = renderer.encode(*blank);
    write_image(output / L"codex-unavailable.jpg", blank_jpeg);
    write_image(output / L"codex-locked-unavailable.jpg", renderer.encode(*blank_locked));
    auto fresh_both = both;
    fresh_both.fetched_utc_ticks = fresh.fetched_utc_ticks;
    for (int index = 0; index < fresh_both.count; ++index)
        fresh_both.windows[index].resets_at = 0;
    for (const auto &variant :
         {std::pair{L"codex-weekly.jpg", fresh}, std::pair{L"codex-both.jpg", fresh_both},
          std::pair{L"codex-stale.jpg", failed}, std::pair{L"codex-pending.jpg", CodexUsage{}}})
    {
        auto image = renderer.render(sample_time(), variant.second);
        write_image(output / variant.first, renderer.encode(*image));
    }
    auto enabled = renderer.render(sample_time(), fresh);
    settings.show_codex_usage = false;
    renderer.configure(settings);
    auto disabled = renderer.render(sample_time(), fresh);
    int inset_difference = 0;
    int perimeter_difference = 0;
    for (UINT y = 0; y < 240; ++y)
        for (UINT x = 0; x < 320; ++x)
        {
            Gdiplus::Color a, b;
            enabled->GetPixel(x, y, &a);
            disabled->GetPixel(x, y, &b);
            if (a.GetValue() == b.GetValue())
                continue;
            if ((x >= 56) && (x <= 129) && (y >= 95) && (y <= 150))
                ++inset_difference;
            else if ((x < 180) && (y < 145))
                ++perimeter_difference;
        }
    check(inset_difference > 80, "Codex allowance was not drawn in the free inset opposite the date");
    check(perimeter_difference == 0, "Codex inset moved the hands or numerals");

    settings.layout = 0;
    for (const bool known : {true, false})
    {
        settings.show_codex_usage = false;
        renderer.configure(settings);
        auto plain = renderer.render_locked(sample_time(), 61, known);
        settings.show_codex_usage = true;
        renderer.configure(settings);
        auto locked = renderer.render_locked(sample_time(), 61, known, fresh);
        write_image(output / (known ? L"codex-locked.jpg" : L"codex-locked-startup.jpg"),
                    renderer.encode(*locked));
        int usage_pixels = 0;
        int earliest_ink = 240;
        int latest_ink = 0;
        int header_difference = 0;
        for (UINT y = 0; y < 240; ++y)
            for (UINT x = 0; x < 320; ++x)
            {
                Gdiplus::Color a, b;
                locked->GetPixel(x, y, &a);
                plain->GetPixel(x, y, &b);
                if ((y < 194) && (a.GetValue() != b.GetValue()))
                    ++header_difference;
                if ((y >= 199) && (std::max({a.GetR(), a.GetG(), a.GetB()}) > 64))
                {
                    ++usage_pixels;
                    earliest_ink = std::min(earliest_ink, static_cast<int>(y));
                    latest_ink = std::max(latest_ink, static_cast<int>(y));
                }
            }
        check(header_difference == 0, "Locked usage changed the clock or lock badge");
        check((usage_pixels > 150) && ((latest_ink - earliest_ink) >= 20),
              "Locked usage was missing or too small to read");
    }
    for (const auto &variant :
         {std::pair{L"codex-locked-stale.jpg", failed}, std::pair{L"codex-locked-pending.jpg", CodexUsage{}}})
    {
        auto image = renderer.render_locked(sample_time(), 1, true, variant.second);
        const auto jpeg = renderer.encode(*image);
        check(jpeg.size() <= 65515, "Locked usage image exceeds the hub's size limit");
        check_jpeg(jpeg);
        write_image(output / variant.first, jpeg);
    }
}

void test_netatmo(const std::filesystem::path &output)
{
    const auto credentials =
        parse_netatmo_credentials(L"client_id = example-client\nclient_secret: example-secret\naccess_token "
                                  L"= 'example-access|token'\nrefresh_token = example-refresh\n");
    check(credentials.access_token == L"example-access|token", "Netatmo text credentials did not parse");
    const auto json_credentials = parse_netatmo_credentials(
        LR"({"client_id":"example-client","client_secret":"example-secret","access_token":"example-access|token","refresh_token":"example-refresh"})");
    check(json_credentials.refresh_token == credentials.refresh_token,
          "Netatmo JSON credentials did not parse");
    bool rejected = false;
    try
    {
        parse_netatmo_credentials(L"client_id = example-client");
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Incomplete Netatmo credentials were accepted");
    const std::wstring key = L"Software\\AnkerClock.Tests.Netatmo." + std::to_wstring(GetCurrentProcessId());
    struct Cleanup
    {
        std::wstring key;
        ~Cleanup()
        {
            RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        }
    } cleanup{key};
    save_netatmo_credentials(credentials, key.c_str());
    const auto restored = load_netatmo_credentials(key.c_str());
    check((restored.client_id == credentials.client_id) &&
              (restored.client_secret == credentials.client_secret) &&
              (restored.access_token == credentials.access_token) &&
              (restored.refresh_token == credentials.refresh_token),
          "Protected Netatmo credentials did not round-trip");
    DWORD type = 0, bytes = 0;
    check(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Credentials", RRF_RT_REG_BINARY, &type, nullptr,
                       &bytes) == ERROR_SUCCESS,
          "Netatmo credentials were not stored as a protected binary blob");
    SYSTEMTIME utc = {};
    GetSystemTime(&utc);
    const ULONGLONG now = utc_ticks(utc);
    const auto unix_stamp = std::to_wstring((now - 116444736000000000) / 10000000);
    const std::wstring response =
        LR"({"body":{"devices":[{"_id":"station","module_name":"Station","reachable":true,"data_type":["Temperature","Humidity","CO2","Noise"],"dashboard_data":{"time_utc":)" +
        unix_stamp +
        LR"(,"Temperature":21.9,"Humidity":44,"CO2":704,"Noise":40},"modules":[{"_id":"living","module_name":"Living Room","reachable":true,"data_type":["Temperature","Humidity","CO2"],"dashboard_data":{"time_utc":)" +
        unix_stamp +
        LR"(,"Temperature":20.7,"Humidity":49,"CO2":676}},{"_id":"bedroom","module_name":"Bedroom","reachable":false,"data_type":["Temperature","Humidity","CO2"]}]}]}})";
    auto data = parse_netatmo_data(response, now);
    check(data.modules.size() == 3, "Netatmo module discovery lost a module");
    check(data.modules[0].available_readings == 15, "Main station lost noise or other readings");
    check(data.modules[1].supported_readings == 7, "Indoor module falsely supports noise");
    check(data.modules[2].available_readings == 0, "Offline module invented zero-valued measurements");
    check(selected_netatmo_module(data, L"living", now, 60) == &data.modules[1],
          "Selected module was ignored");
    check(selected_netatmo_module(data, L"missing", now, 60) == nullptr,
          "Missing module fell back to another room");
    check(selected_netatmo_module(data, L"bedroom", now, 60) == nullptr, "Offline room retained readings");
    check(selected_netatmo_module(data, L"station", now + 12000000000, 60) == &data.modules[0],
          "Old cached readings disappeared instead of being marked stale");
    auto old_measurement = data;
    old_measurement.modules[0].measured_utc_ticks -= 36000000000;
    check(selected_netatmo_module(old_measurement, L"station", now, 60) == &old_measurement.modules[0],
          "Old station measurements disappeared instead of being marked stale");
    check(netatmo_module_options(data).find(L"living\tLiving Room\t7") != std::wstring::npos,
          "Module choices lost capability metadata");
    Settings settings;
    settings.layout = 2;
    settings.theme = 6;
    settings.font = L"Arial";
    settings.show_netatmo = true;
    settings.netatmo_module_id = L"station";
    settings.netatmo_readings = 15;
    check(netatmo_diagnostics(data, settings, now).find(L"Weather available for display.") == 0,
          "Fresh weather was diagnosed as hidden");
    check(netatmo_diagnostics(data, settings, now).find(L"Last successful fetch:") != std::wstring::npos,
          "Weather diagnostics omitted the successful fetch timestamp");
    check(netatmo_selection(data, L"station", now + 12000000000, 60).availability ==
              NetatmoAvailability::cache_expired,
          "Expired weather cache lost its reason");
    check(netatmo_diagnostics(data, settings, now + 12000000000).find(L"fetch is too old") !=
              std::wstring::npos,
          "Expired cache was reported as connected weather");
    check(netatmo_selection(old_measurement, L"station", now, 60).availability ==
              NetatmoAvailability::measurement_expired,
          "Old measurement lost its reason");
    check(netatmo_diagnostics(old_measurement, settings, now)
                  .find(L"Measurements become stale after 60 minutes") != std::wstring::npos,
          "Fresh fetch with old measurements omitted the display cutoff");
    check(netatmo_selection(data, L"bedroom", now, 60).availability == NetatmoAvailability::module_offline,
          "Offline module was misdiagnosed");
    check(netatmo_selection(data, L"missing", now, 60).availability == NetatmoAvailability::module_missing,
          "Missing module was misdiagnosed");
    check(netatmo_selection(NetatmoData{}, L"station", now, 60).availability == NetatmoAvailability::no_cache,
          "Missing cache was misdiagnosed");
    check(netatmo_selection(data, L"station", now - 1, 60).availability ==
              NetatmoAvailability::invalid_fetch_time,
          "Future fetch timestamp was accepted");
    auto missing_measurement = data;
    missing_measurement.modules[0].measured_utc_ticks = 0;
    check(netatmo_selection(missing_measurement, L"station", now, 60).availability ==
              NetatmoAvailability::no_measurement,
          "Missing measurement timestamp was accepted");
    missing_measurement.modules[0].measured_utc_ticks = now + 1;
    check(netatmo_selection(missing_measurement, L"station", now, 60).availability ==
              NetatmoAvailability::invalid_measurement_time,
          "Future measurement timestamp was accepted");
    auto partial = data;
    partial.modules[0].available_readings = netatmo_temperature;
    check(netatmo_diagnostics(partial, settings, now).find(L"some selected readings are missing") !=
              std::wstring::npos,
          "Partial measurements were reported as complete");
    partial.modules[0].available_readings = 0;
    check(netatmo_diagnostics(partial, settings, now).find(L"none of the selected readings") !=
              std::wstring::npos,
          "Empty measurements were reported as available");
    auto disabled = settings;
    disabled.show_netatmo = false;
    check(netatmo_diagnostics(data, disabled, now).find(L"readings are turned off") != std::wstring::npos,
          "Disabled weather omitted its display reason");
    disabled.layout = 0;
    check(netatmo_diagnostics(data, disabled, now).find(L"this clock face") != std::wstring::npos,
          "Unsupported clock face omitted its display reason");
    ClockRenderer renderer;
    renderer.configure(settings);
    auto image = renderer.render(sample_time(), CodexUsage{}, data);
    const auto jpeg = renderer.encode(*image);
    check_jpeg(jpeg);
    check(jpeg.size() <= 65515, "Netatmo clock exceeds the hub image limit");
    write_image(output / L"netatmo-station.jpg", jpeg);
    int value_pixels = 0, first_ink = 240, last_ink = 0;
    for (UINT y = 14; y < 53; ++y)
        for (UINT x = 216; x < 318; ++x)
        {
            Gdiplus::Color pixel;
            image->GetPixel(x, y, &pixel);
            if (std::max({pixel.GetR(), pixel.GetG(), pixel.GetB()}) > 100)
            {
                ++value_pixels;
                first_ink = std::min(first_ink, static_cast<int>(y));
                last_ink = std::max(last_ink, static_cast<int>(y));
            }
        }
    check((value_pixels > 180) && ((last_ink - first_ink) >= 20), "Netatmo values were missing or too small");
    settings.netatmo_readings = 13;
    settings.show_codex_usage = true;
    renderer.configure(settings);
    CodexUsage usage;
    usage.count = 1;
    usage.fetched_utc_ticks = now;
    usage.windows[0].remaining_percent = 91;
    auto without_usage = renderer.render(sample_time(), CodexUsage{}, data);
    auto with_usage = renderer.render(sample_time(), usage, data);
    int usage_pixels = 0, weather_changes = 0;
    for (UINT y = 0; y < 240; ++y)
        for (UINT x = 0; x < 320; ++x)
        {
            Gdiplus::Color a, b;
            without_usage->GetPixel(x, y, &a);
            with_usage->GetPixel(x, y, &b);
            if (a.GetValue() == b.GetValue())
                continue;
            if ((x >= 214) && (y >= 178))
                ++usage_pixels;
            if ((x >= 214) && (y < 174))
                ++weather_changes;
        }
    check(usage_pixels > 80, "Codex usage disappeared when Netatmo was enabled");
    check(weather_changes == 0, "Codex usage overwrote weather readings");
    write_image(output / L"netatmo-codex.jpg", renderer.encode(*with_usage));
    auto stale_frame = renderer.render(sample_time(), usage, old_measurement);
    auto stale_locked = renderer.render_locked(sample_time(), 61, true, usage, old_measurement);
    int badge_changes[3] = {}, outside_badges = 0, locked_reading_changes = 0;
    for (UINT y = 0; y < 240; ++y)
        for (UINT x = 0; x < 320; ++x)
        {
            Gdiplus::Color fresh, stale, locked;
            with_usage->GetPixel(x, y, &fresh);
            stale_frame->GetPixel(x, y, &stale);
            stale_locked->GetPixel(x, y, &locked);
            if (fresh.GetValue() != stale.GetValue())
            {
                const int row = static_cast<int>(y) / 58;
                const bool in_badge = (row < 3) && (x >= 197) && (x < 217) &&
                                      (y >= static_cast<UINT>(36 + (58 * row))) &&
                                      (y < static_cast<UINT>(55 + (58 * row)));
                if (in_badge)
                    ++badge_changes[row];
                else
                    ++outside_badges;
            }
            if (x >= 187)
                locked_reading_changes += stale.GetValue() != locked.GetValue() ? 1 : 0;
        }
    check((badge_changes[0] > 25) && (badge_changes[1] > 25) && (badge_changes[2] > 25),
          "Stale weather omitted an icon badge");
    check(outside_badges == 0, "Stale badges changed the reading values or Codex row");
    check(locked_reading_changes == 0, "Stale weather badges disappeared on the lock display");
    auto stale_cache = data;
    stale_cache.fetched_utc_ticks -= 12000000000;
    auto cached_frame = renderer.render(sample_time(), usage, stale_cache);
    const auto stale_jpeg = renderer.encode(*stale_frame);
    check(stale_jpeg.size() <= 65515, "Stale clock badges exceed the hub image limit");
    write_image(output / L"netatmo-stale.jpg", stale_jpeg);
    const auto changed_pixels = [](Gdiplus::Bitmap &first, Gdiplus::Bitmap &second, UINT begin, UINT end) {
        int changes = 0;
        for (UINT y = 0; y < 240; ++y)
            for (UINT x = begin; x < end; ++x)
            {
                Gdiplus::Color a, b;
                first.GetPixel(x, y, &a);
                second.GetPixel(x, y, &b);
                changes += a.GetValue() != b.GetValue() ? 1 : 0;
            }
        return changes;
    };
    check(changed_pixels(*stale_frame, *cached_frame, 187, 320) == 0,
          "Stale fetch and stale measurement badges use different display behavior");
    for (int face = 0; face < 3; ++face)
    {
        settings.analog_face = face;
        renderer.configure(settings);
        auto variant = renderer.render(sample_time(), usage, data);
        check(changed_pixels(*with_usage, *variant, 187, 320) == 0,
              "Clock face selection changed the reading column");
        if (face > 0)
            check(changed_pixels(*with_usage, *variant, 0, 187) > 80, "Analog styles draw the same face");
        const auto encoded = renderer.encode(*variant);
        check(encoded.size() <= 65515, "Analog face with readings exceeds the hub image limit");
        write_image(output / (L"netatmo-face-" + std::to_wstring(face) + L".jpg"), encoded);
        SYSTEMTIME date_time = sample_time();
        date_time.wDay = 13;
        auto dated = renderer.render(date_time, usage, data);
        write_image(output / (L"netatmo-date-face-" + std::to_wstring(face) + L".jpg"),
                    renderer.encode(*dated));
        write_image(output / (L"netatmo-date-locked-face-" + std::to_wstring(face) + L".jpg"),
                    renderer.encode(*renderer.render_locked(date_time, 61, true, usage, data)));
        settings.show_date = false;
        renderer.configure(settings);
        auto hidden_date = renderer.render(date_time, usage, data);
        settings.show_date = true;
        renderer.configure(settings);
        date_time.wDay = 14;
        auto next_day = renderer.render(date_time, usage, data);
        date_time.wDay = 13;
        date_time.wMonth = 11;
        auto next_month = renderer.render(date_time, usage, data);
        int date_changes = 0, outside_date_changes = 0, day_changes = 0, month_changes = 0;
        for (UINT y = 0; y < 240; ++y)
            for (UINT x = 0; x < 320; ++x)
            {
                Gdiplus::Color visible, hidden, day, month;
                dated->GetPixel(x, y, &visible);
                hidden_date->GetPixel(x, y, &hidden);
                next_day->GetPixel(x, y, &day);
                next_month->GetPixel(x, y, &month);
                const bool in_date = (x >= 100) && (x < (face == 2 ? 152U : 146U)) && (y >= 90) && (y < 150);
                if (visible.GetValue() != hidden.GetValue())
                    in_date ? ++date_changes : ++outside_date_changes;
                if (in_date && (y < 123))
                    day_changes += visible.GetValue() != day.GetValue() ? 1 : 0;
                if (in_date && (y >= 123))
                    month_changes += visible.GetValue() != month.GetValue() ? 1 : 0;
            }
        check((date_changes > 80) && (outside_date_changes == 0),
              "Show date changed the hands, numeral 3 or weather column outside the middle inset");
        check((day_changes > 20) && (month_changes > 20),
              "Stacked date did not update the day above the month");
        auto locked = renderer.render_locked(sample_time(), 61, true, usage, data);
        check(changed_pixels(*variant, *locked, 187, 320) == 0,
              "Lock overlay changed the weather or Codex reading column");
        write_image(output / (L"netatmo-locked-face-" + std::to_wstring(face) + L".jpg"),
                    renderer.encode(*locked));
        auto next_minute = renderer.render_locked(sample_time(), 62, true, usage, data);
        auto unknown = renderer.render_locked(sample_time(), 61, false, usage, data);
        SYSTEMTIME later = sample_time();
        later.wMinute = (later.wMinute + 1) % 60;
        auto live = renderer.render_locked(later, 61, true, usage, data);
        int dial_changes = 0, live_changes = 0, duration_changes = 0, unknown_changes = 0;
        int outside_footer_changes = 0;
        for (UINT y = 0; y < 240; ++y)
            for (UINT x = 0; x < 184; ++x)
            {
                Gdiplus::Color plain, overlay, next, observed, updated;
                variant->GetPixel(x, y, &plain);
                locked->GetPixel(x, y, &overlay);
                next_minute->GetPixel(x, y, &next);
                unknown->GetPixel(x, y, &observed);
                live->GetPixel(x, y, &updated);
                if ((y >= 30) && (y < 212))
                {
                    dial_changes += plain.GetValue() != overlay.GetValue() ? 1 : 0;
                    live_changes += overlay.GetValue() != updated.GetValue() ? 1 : 0;
                }
                duration_changes += overlay.GetValue() != next.GetValue() ? 1 : 0;
                unknown_changes += overlay.GetValue() != observed.GetValue() ? 1 : 0;
                if ((y < 214) &&
                    ((overlay.GetValue() != next.GetValue()) || (overlay.GetValue() != observed.GetValue())))
                    ++outside_footer_changes;
            }
        check(dial_changes == 0, "Lock overlay moved or obscured the square dial");
        check(live_changes > 20, "Locked clock hands do not show the current time");
        check((duration_changes > 20) && (unknown_changes > 20) && (outside_footer_changes == 0),
              "Lock duration or its startup lower bound was missing or changed the dial");
        auto long_lock = renderer.render_locked(sample_time(), 525600, false, usage, data);
        write_image(output / (L"netatmo-locked-long-" + std::to_wstring(face) + L".jpg"),
                    renderer.encode(*long_lock));
    }
    auto extremes = data;
    extremes.modules[0].values[0] = -15;
    extremes.modules[0].values[2] = 4000;
    extremes.modules[0].values[3] = 95;
    auto full_usage = usage;
    full_usage.windows[0].remaining_percent = 100;
    settings.reading_font_size = 38;
    renderer.configure(settings);
    auto largest = renderer.render(sample_time(), full_usage, extremes);
    write_image(output / L"netatmo-long-values.jpg", renderer.encode(*largest));
    int edge_ink = 0;
    for (UINT y = 0; y < 240; ++y)
        for (UINT x = 317; x < 320; ++x)
        {
            Gdiplus::Color pixel;
            largest->GetPixel(x, y, &pixel);
            edge_ink += std::max({pixel.GetR(), pixel.GetG(), pixel.GetB()}) > 64 ? 1 : 0;
        }
    check(edge_ink == 0, "Long readings or their units are clipped at the screen edge");
    settings.reading_font_size = 28;
    renderer.configure(settings);
    auto smaller = renderer.render(sample_time(), usage, data);
    check(changed_pixels(*with_usage, *smaller, 187, 320) > 80, "Reading size does not change the display");
    settings.reading_font_size = 32;
    settings.analog_face = 0;
    renderer.configure(settings);
    const auto check_ink = [&](Gdiplus::Bitmap &bitmap, int row, COLORREF expected) {
        int pixels = 0;
        const UINT top = 8 + (58 * row);
        for (UINT y = top; y < (top + 50); ++y)
            for (UINT x = 217; x < 318; ++x)
            {
                Gdiplus::Color pixel;
                bitmap.GetPixel(x, y, &pixel);
                if ((std::abs(pixel.GetR() - GetRValue(expected)) <= 15) &&
                    (std::abs(pixel.GetG() - GetGValue(expected)) <= 15) &&
                    (std::abs(pixel.GetB() - GetBValue(expected)) <= 15))
                    ++pixels;
            }
        if (pixels <= 50)
            write_image(output / L"severity-failure.jpg", renderer.encode(bitmap));
        check(pixels > 50, ("Reading severity color is missing or outside row " + std::to_string(row) + " (" +
                            std::to_string(pixels) + " pixels)")
                               .c_str());
    };
    for (const auto &[value, expected] :
         {std::pair{999, RGB(138, 172, 155)}, std::pair{1000, RGB(186, 164, 111)},
          std::pair{1999, RGB(186, 164, 111)}, std::pair{2000, RGB(186, 130, 132)}})
    {
        auto colored = data;
        colored.modules[0].values[2] = value;
        check_ink(*renderer.render(sample_time(), usage, colored), 1, expected);
    }
    for (const auto &[value, expected] :
         {std::pair{54, RGB(138, 172, 155)}, std::pair{55, RGB(186, 164, 111)},
          std::pair{84, RGB(186, 164, 111)}, std::pair{85, RGB(186, 130, 132)}})
    {
        auto colored = data;
        colored.modules[0].values[3] = value;
        check_ink(*renderer.render(sample_time(), usage, colored), 2, expected);
    }
    for (const auto &[value, expected] :
         {std::pair{50, RGB(138, 172, 155)}, std::pair{49, RGB(186, 164, 111)},
          std::pair{20, RGB(186, 164, 111)}, std::pair{19, RGB(186, 130, 132)}})
    {
        auto colored = usage;
        colored.windows[0].remaining_percent = value;
        check_ink(*renderer.render(sample_time(), colored, data), 3, expected);
    }
    settings.netatmo_module_id = L"living";
    settings.netatmo_readings = 7;
    renderer.configure(settings);
    write_image(output / L"netatmo-living.jpg",
                renderer.encode(*renderer.render(sample_time(), CodexUsage{}, data)));
    settings.netatmo_readings = 4;
    renderer.configure(settings);
    write_image(output / L"netatmo-co2.jpg",
                renderer.encode(*renderer.render(sample_time(), CodexUsage{}, data)));
}

void probe_codex(const std::filesystem::path &output)
{
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CodexUsage usage;
    const ULONGLONG started = GetTickCount64();
    try
    {
        usage = query_codex_usage(stop);
    }
    catch (...)
    {
        CloseHandle(stop);
        throw;
    }
    CloseHandle(stop);
    check(usage.count > 0, "Live Codex query did not return any usage windows");
    save_codex_usage(usage);
    const auto cached = load_codex_usage();
    check((cached.count == usage.count) && (cached.fetched_utc_ticks == usage.fetched_utc_ticks) &&
              (cached.windows[0].remaining_percent == usage.windows[0].remaining_percent),
          "Live Codex query did not persist for the settings preview");
    Settings settings = load_settings(L"Software\\AnkerClock");
    settings.layout = 2;
    settings.show_codex_usage = true;
    ClockRenderer renderer;
    renderer.configure(settings);
    auto image = renderer.render(renderer.current_time(), usage);
    write_image(output / L"codex-live-clock.jpg", renderer.encode(*image));
    std::cout << "Codex query completed in " << (GetTickCount64() - started) << " ms; "
              << usage.windows[0].remaining_percent << "% remaining\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    try
    {
        const std::wstring_view mode = argc > 1 ? argv[1] : L"--render";
        if (mode == L"--help")
        {
            std::cout << "AnkerClockTests [mode] [output-directory] [--allow-gallery-clear]\n"
                         "  --render          Offline core tests (default)\n"
                         "  --codex           Query local Codex and update its usage cache\n"
                         "  --probe           Read hub metadata\n"
                         "  --capture-current Read the current hub image\n"
                         "  --hub-current     Show test frames; removes old gallery images\n"
                         "  --hub-recovery    Exercise transfer recovery on the connected hub\n"
                         "  --hub             Clear the gallery and send test frames; requires\n"
                         "                    --allow-gallery-clear after the output directory\n";
            return 0;
        }
        if ((mode != L"--render") && (mode != L"--codex") && (mode != L"--probe") &&
            (mode != L"--capture-current") && (mode != L"--hub-current") && (mode != L"--hub-recovery") &&
            (mode != L"--hub"))
            throw std::runtime_error("Unknown test mode; use --help");
        if ((argc > 4) ||
            ((argc == 4) && ((mode != L"--hub") || (wcscmp(argv[3], L"--allow-gallery-clear") != 0))))
            throw std::runtime_error("Unexpected test argument; use --help");
        if ((mode == L"--hub") && ((argc != 4) || (wcscmp(argv[3], L"--allow-gallery-clear") != 0)))
            throw std::runtime_error(
                "Gallery clearing requires an output directory and --allow-gallery-clear");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        GraphicsRuntime graphics;
        const std::filesystem::path output = argc > 2 ? argv[2] : L"test-output";
        test_clock();
        test_rendering(output);
        test_image_quality(output);
        test_background_dithering(output);
        test_lock_session(output);
        test_preferences();
        test_ui_contract();
        test_codex_discovery(output);
        test_codex_usage(output);
        test_netatmo(output);
        test_transfer_cancellation();
        if ((argc > 1) && (wcscmp(argv[1], L"--codex") == 0))
            probe_codex(output);
        if ((argc > 1) && (wcscmp(argv[1], L"--hub") == 0))
            test_hub(output);
        if ((argc > 1) && (wcscmp(argv[1], L"--hub-current") == 0))
            test_hub(output);
        if ((argc > 1) && (wcscmp(argv[1], L"--probe") == 0))
            probe_hub(output);
        if ((argc > 1) && (wcscmp(argv[1], L"--hub-recovery") == 0))
            test_hub_recovery(output);
        if ((argc > 1) && (wcscmp(argv[1], L"--capture-current") == 0))
            capture_hub(output);
        std::cout << assertions << " assertions passed\n";
        CoUninitialize();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
