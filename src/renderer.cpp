#include "renderer.h"
#include "lock_session.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

using namespace Gdiplus;

namespace
{
Color text_color(COLORREF value, BYTE alpha)
{
    return Color(alpha, GetRValue(value), GetGValue(value), GetBValue(value));
}

void require_jpeg(HRESULT status)
{
    if (FAILED(status))
        throw std::runtime_error("Windows could not encode the clock image");
}

void draw_centered(Graphics &graphics, const std::wstring &text, const wchar_t *face, float size, float y,
                   float height, COLORREF color, BYTE alpha, int style)
{
    Font font(face, size, style, UnitPixel);
    SolidBrush brush(text_color(color, alpha));
    StringFormat format(StringFormat::GenericTypographic());
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    graphics.DrawString(text.c_str(), -1, &font, RectF(10, y, 300, height), &format, &brush);
}

void dither_background(Bitmap &bitmap, int strength)
{
    strength = std::clamp(strength, 0, 8);
    if (strength == 0)
        return;
    // Offline void-and-cluster ranks. A fixed tile keeps the background steady between clock updates.
    constexpr BYTE ranks[16][16] = {
        {137, 9, 164, 99, 178, 128, 158, 101, 17, 176, 141, 210, 19, 120, 70, 225},
        {110, 243, 48, 118, 40, 78, 254, 37, 114, 197, 47, 106, 232, 181, 89, 32},
        {204, 66, 183, 233, 13, 202, 56, 214, 148, 242, 83, 14, 147, 51, 253, 161},
        {20, 88, 140, 216, 154, 131, 172, 94, 6, 69, 167, 218, 129, 189, 1, 123},
        {228, 169, 45, 25, 92, 65, 27, 231, 133, 192, 31, 111, 239, 74, 100, 198},
        {57, 116, 241, 108, 200, 249, 184, 109, 41, 152, 205, 59, 24, 174, 44, 146},
        {84, 10, 194, 63, 163, 3, 144, 220, 75, 248, 121, 91, 226, 135, 209, 245},
        {157, 179, 138, 221, 125, 55, 82, 29, 175, 54, 15, 185, 160, 7, 107, 28},
        {229, 76, 23, 42, 97, 240, 211, 156, 104, 217, 143, 234, 62, 79, 188, 127},
        {53, 102, 255, 203, 171, 16, 187, 119, 22, 193, 96, 38, 117, 251, 35, 213},
        {12, 190, 115, 151, 71, 136, 86, 50, 252, 77, 2, 168, 206, 149, 93, 166},
        {237, 142, 58, 5, 235, 34, 219, 165, 145, 223, 130, 238, 52, 18, 134, 72},
        {36, 215, 105, 182, 95, 195, 113, 11, 67, 43, 180, 81, 112, 199, 230, 177},
        {87, 162, 46, 224, 155, 49, 244, 173, 212, 103, 201, 30, 159, 64, 4, 122},
        {26, 250, 126, 21, 68, 139, 85, 33, 124, 153, 8, 227, 132, 247, 98, 186},
        {60, 196, 80, 208, 236, 0, 191, 222, 73, 246, 61, 90, 170, 39, 207, 150}};
    const Rect bounds(0, 0, bitmap.GetWidth(), bitmap.GetHeight());
    BitmapData data = {};
    if (bitmap.LockBits(&bounds, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &data) != Ok)
        throw std::runtime_error("Could not smooth the background gradient");
    for (UINT y = 0; y < data.Height; ++y)
    {
        auto row = static_cast<BYTE *>(data.Scan0) + (static_cast<ptrdiff_t>(y) * data.Stride);
        for (UINT x = 0; x < data.Width; ++x)
        {
            const int noise = (2 * ranks[y % 16][x % 16]) - 255;
            for (int channel = 0; channel < 3; ++channel)
            {
                BYTE &value = row[(x * 4) + channel];
                // Taper at the endpoints without a clipping bias; black remains exactly black.
                const int amplitude = std::min({strength, static_cast<int>(value), 255 - value});
                const int scaled = noise * amplitude;
                value = static_cast<BYTE>(value + ((scaled + (scaled < 0 ? -127 : 127)) / 255));
            }
        }
    }
    if (bitmap.UnlockBits(&data) != Ok)
        throw std::runtime_error("Could not finish smoothing the background gradient");
}

std::unique_ptr<Bitmap> build_background(const Settings &settings)
{
    auto bitmap = std::make_unique<Bitmap>(320, 240, PixelFormat32bppPARGB);
    Graphics graphics(bitmap.get());
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    if (settings.theme == 6)
    {
        graphics.Clear(Color(255, 0, 0, 0));
        return bitmap;
    }
    if (settings.theme == 5)
    {
        Bitmap source(settings.background_path.c_str());
        if ((source.GetLastStatus() != Ok) || (source.GetWidth() == 0) || (source.GetHeight() == 0))
            throw std::runtime_error("Could not open the background image. Choose a JPEG, PNG, or BMP file.");
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.Clear(Color(255, 9, 12, 19));
        if (settings.background_fit == 0)
        {
            const float scale = std::max(320.0f / source.GetWidth(), 240.0f / source.GetHeight());
            const float width = 320.0f / scale;
            const float height = 240.0f / scale;
            graphics.DrawImage(&source, RectF(0, 0, 320, 240), (source.GetWidth() - width) * 0.5f,
                               (source.GetHeight() - height) * 0.5f, width, height, UnitPixel);
        }
        else
        {
            const float scale = std::min(320.0f / source.GetWidth(), 240.0f / source.GetHeight());
            const float width = source.GetWidth() * scale;
            const float height = source.GetHeight() * scale;
            graphics.DrawImage(&source, RectF((320 - width) * 0.5f, (240 - height) * 0.5f, width, height));
        }
        SolidBrush dim(Color(static_cast<BYTE>((settings.image_dim * 255) / 100), 0, 0, 0));
        graphics.FillRectangle(&dim, 0, 0, 320, 240);
        return bitmap;
    }

    const Color tops[] = {Color(255, 19, 25, 47), Color(255, 21, 39, 54), Color(255, 53, 30, 40),
                          Color(255, 12, 41, 60), Color(255, 26, 29, 34)};
    const Color bottoms[] = {Color(255, 6, 9, 20), Color(255, 5, 18, 26), Color(255, 18, 12, 20),
                             Color(255, 4, 13, 30), Color(255, 9, 10, 13)};
    const Color glows[] = {Color(80, 100, 91, 195), Color(65, 54, 174, 163), Color(65, 208, 116, 63),
                           Color(60, 42, 124, 184), Color(30, 114, 124, 140)};
    const int theme = std::clamp(settings.theme, 0, 4);
    LinearGradientBrush gradient(Point(0, 0), Point(320, 240), tops[theme], bottoms[theme]);
    graphics.FillRectangle(&gradient, 0, 0, 320, 240);
    GraphicsPath shape;
    shape.AddEllipse(-130, -190, 600, 420);
    PathGradientBrush glow(&shape);
    glow.SetCenterColor(glows[theme]);
    Color edge(0, glows[theme].GetR(), glows[theme].GetG(), glows[theme].GetB());
    int count = 1;
    glow.SetSurroundColors(&edge, &count);
    graphics.FillPath(&glow, &shape);
    return bitmap;
}
} // namespace

GraphicsRuntime::GraphicsRuntime()
{
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok)
        throw std::runtime_error("Windows graphics initialization failed");
}

GraphicsRuntime::~GraphicsRuntime()
{
    if (token != 0)
        GdiplusShutdown(token);
}

ClockText format_clock(const SYSTEMTIME &time, const Settings &settings)
{
    wchar_t buffer[100] = {};
    ClockText text;
    int hour = time.wHour;
    if (settings.use_24_hour == false)
    {
        hour %= 12;
        if (hour == 0)
            hour = 12;
    }
    swprintf_s(buffer, settings.use_24_hour ? L"%02d:%02d" : L"%d:%02d", hour, time.wMinute);
    text.time = buffer;
    if (settings.show_seconds)
    {
        swprintf_s(buffer, L":%02d", time.wSecond);
        text.detail = buffer;
    }
    if (settings.use_24_hour == false)
    {
        if (text.detail.empty() == false)
            text.detail += L"  ·  ";
        text.detail += time.wHour < 12 ? L"AM" : L"PM";
    }
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &time, L"dddd", buffer, 100, nullptr);
    text.weekday = buffer;
    CharUpperBuffW(text.weekday.data(), static_cast<DWORD>(text.weekday.size()));
    const wchar_t *formats[] = {L"d MMMM", L"ddd, d MMM", L"yyyy-MM-dd", L"MM/dd/yyyy"};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &time, formats[settings.date_format], buffer, 100, nullptr);
    text.date = buffer;
    return text;
}

ULONGLONG utc_ticks(const SYSTEMTIME &utc_time)
{
    FILETIME file_time = {};
    if (SystemTimeToFileTime(&utc_time, &file_time) == FALSE)
        throw std::runtime_error("The system clock returned an invalid date");
    ULARGE_INTEGER ticks;
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    return ticks.QuadPart;
}

SYSTEMTIME utc_from_ticks(ULONGLONG ticks)
{
    ULARGE_INTEGER value;
    value.QuadPart = ticks;
    const FILETIME file_time = {value.LowPart, value.HighPart};
    SYSTEMTIME time = {};
    if (FileTimeToSystemTime(&file_time, &time) == FALSE)
        throw std::runtime_error("The clock update returned an invalid date");
    return time;
}

UpdateSchedule next_update_schedule(const SYSTEMTIME &utc_time, int interval_seconds, DWORD lead_ms)
{
    const ULONGLONG now = utc_ticks(utc_time);
    const ULONGLONG interval = static_cast<ULONGLONG>(interval_seconds) * 10000000;
    const ULONGLONG lead = static_cast<ULONGLONG>(lead_ms) * 10000;
    const ULONGLONG target = (((now + lead) / interval) + 1) * interval;
    return {target, static_cast<DWORD>((target - lead - now + 9999) / 10000)};
}

uint16_t image_crc(const std::vector<BYTE> &image)
{
    uint32_t crc = 0xffff;
    for (const BYTE value : image)
    {
        crc ^= static_cast<uint32_t>(value) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) != 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
    return static_cast<uint16_t>(crc);
}

void distinguish_crc(std::vector<BYTE> &image, uint16_t previous_crc)
{
    if (image_crc(image) != previous_crc)
        return;
    if ((image.size() < 2) || ((image.size() + 5) > 65520))
        throw std::runtime_error("Image cannot be distinguished from the previous clock frame");
    // A JPEG comment changes the firmware's image identifier without changing any pixels.
    image.insert(image.end() - 2, {0xff, 0xfe, 0, 3, 0});
    if (image_crc(image) == previous_crc)
        image[image.size() - 3] = 1;
}

ClockRenderer::ClockRenderer()
{
    require_jpeg(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&_jpeg_factory)));
}

void ClockRenderer::configure(const Settings &settings)
{
    Settings validated = settings;
    FontFamily family(settings.font.c_str());
    if (family.GetLastStatus() != Ok)
        validated.font = L"Segoe UI";
    else if (family.IsStyleAvailable(settings.font_style) == FALSE)
        validated.font_style = FontStyleRegular;
    if ((_background == nullptr) || (_settings.theme != settings.theme) ||
        (_settings.background_path != settings.background_path) ||
        (_settings.image_dim != settings.image_dim) ||
        (_settings.background_fit != settings.background_fit) ||
        (_settings.background_dither != settings.background_dither))
    {
        auto background = build_background(settings);
        if (settings.theme != 6)
            dither_background(*background, settings.background_dither);
        _background = std::move(background);
    }
    if ((_settings.time_zone != settings.time_zone) || (_background == nullptr))
    {
        _use_zone = false;
        for (DWORD index = 0; settings.time_zone.empty() == false; ++index)
        {
            DYNAMIC_TIME_ZONE_INFORMATION zone = {};
            if (EnumDynamicTimeZoneInformation(index, &zone) != ERROR_SUCCESS)
                break;
            if (settings.time_zone == zone.TimeZoneKeyName)
            {
                _zone = zone;
                _use_zone = true;
                break;
            }
        }
    }
    _settings = validated;
}

SYSTEMTIME ClockRenderer::current_time() const
{
    SYSTEMTIME utc = {};
    GetSystemTime(&utc);
    return time_at(utc);
}

SYSTEMTIME ClockRenderer::time_at(const SYSTEMTIME &utc_time) const
{
    DYNAMIC_TIME_ZONE_INFORMATION local = {};
    if ((_use_zone == false) && (GetDynamicTimeZoneInformation(&local) == TIME_ZONE_ID_INVALID))
        throw_windows_error("Read Windows time zone");
    SYSTEMTIME time = {};
    if (SystemTimeToTzSpecificLocalTimeEx(_use_zone ? &_zone : &local, &utc_time, &time) == FALSE)
        throw_windows_error("Convert clock time zone");
    return time;
}

std::unique_ptr<Bitmap> ClockRenderer::render(const SYSTEMTIME &time) const
{
    return render(time, CodexUsage{});
}

std::unique_ptr<Bitmap> ClockRenderer::render(const SYSTEMTIME &time, const CodexUsage &usage) const
{
    return render_frame(time, false, 0, false, usage);
}

std::unique_ptr<Bitmap> ClockRenderer::render_locked(const SYSTEMTIME &locked_at, ULONGLONG elapsed_minutes,
                                                     bool lock_time_known) const
{
    return render_locked(locked_at, elapsed_minutes, lock_time_known, CodexUsage{});
}

std::unique_ptr<Bitmap> ClockRenderer::render_locked(const SYSTEMTIME &locked_at, ULONGLONG elapsed_minutes,
                                                     bool lock_time_known, const CodexUsage &usage) const
{
    return render_frame(locked_at, true, elapsed_minutes, lock_time_known, usage);
}

std::unique_ptr<Bitmap> ClockRenderer::render_frame(const SYSTEMTIME &time, bool locked,
                                                    ULONGLONG elapsed_minutes, bool lock_time_known,
                                                    const CodexUsage &usage) const
{
    if (_background == nullptr)
        throw std::runtime_error("Clock renderer has no background");
    // Draw at three times the panel resolution before filtering fine strokes and text down to pixels.
    auto image = std::make_unique<Bitmap>(960, 720, PixelFormat32bppPARGB);
    Graphics graphics(image.get());
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.ScaleTransform(3, 3);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const auto finish = [&]() {
        graphics.Flush(FlushIntentionSync);
        // Preserve native-resolution dithering while filtering only the transparent clock layer.
        std::unique_ptr<Bitmap> panel(_background->Clone(0, 0, 320, 240, PixelFormat24bppRGB));
        if (panel == nullptr)
            throw std::runtime_error("Could not composite the clock background");
        Graphics output(panel.get());
        output.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        output.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        ImageAttributes edges;
        edges.SetWrapMode(WrapModeTileFlipXY);
        output.DrawImage(image.get(), Rect(0, 0, 320, 240), 0, 0, 960, 720, UnitPixel, &edges);
        return panel;
    };
    const ClockText text = format_clock(time, _settings);
    const COLORREF color = _settings.text_color;
    if (locked)
    {
        Pen outline(text_color(_settings.accent_color, 200), 1.4f);
        graphics.DrawArc(&outline, 153.0f, 20.0f, 14.0f, 20.0f, 180.0f, 180.0f);
        graphics.DrawRectangle(&outline, 150.0f, 30.0f, 20.0f, 17.0f);
        SolidBrush key(text_color(_settings.accent_color, 220));
        graphics.FillEllipse(&key, 158.5f, 36.0f, 3.0f, 3.0f);
        graphics.FillRectangle(&key, 159.5f, 38.0f, 1.0f, 4.0f);
        draw_centered(graphics, lock_time_known ? L"LOCKED AT" : L"WINDOWS LOCKED", L"Segoe UI", 10, 55, 18,
                      color, 150, 0);
        std::wstring stamp = L"Before app started";
        if (lock_time_known)
        {
            Settings stamp_settings = _settings;
            stamp_settings.show_seconds = true;
            const ClockText lock_text = format_clock(time, stamp_settings);
            stamp = lock_text.time + lock_text.detail;
        }
        draw_centered(graphics, stamp, _settings.font.c_str(), lock_time_known ? 24.0f : 18.0f, 76, 34, color,
                      240, _settings.font_style);
        SolidBrush accent(text_color(_settings.accent_color, 150));
        graphics.FillRectangle(&accent, 146, 119, 28, 1);
        const std::wstring duration = format_locked_duration(elapsed_minutes);
        float size = static_cast<float>(std::clamp(_settings.font_size, 36, 56));
        StringFormat format(StringFormat::GenericTypographic());
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        for (;;)
        {
            Font font(_settings.font.c_str(), size, _settings.font_style, UnitPixel);
            RectF bounds;
            graphics.MeasureString(duration.c_str(), -1, &font, PointF(0, 0), &format, &bounds);
            if (((bounds.Width <= 280) && (bounds.Height <= 64)) || (size <= 16))
                break;
            size -= 2;
        }
        draw_centered(graphics, duration, _settings.font.c_str(), size, 129, 64, color, 255,
                      _settings.font_style);
        if (_settings.show_codex_usage)
        {
            SYSTEMTIME now = {};
            GetSystemTime(&now);
            const auto allowance = format_codex_usage(usage, utc_ticks(now), _settings.codex_refresh_seconds);
            if (allowance.remaining_percent >= 0)
                draw_centered(graphics, allowance.value, _settings.font.c_str(), 28, 199, 36,
                              _settings.accent_color, 255, _settings.font_style);
        }
        else
        {
            draw_centered(graphics, lock_time_known ? L"TIME LOCKED" : L"OBSERVED SINCE STARTUP", L"Segoe UI",
                          10, 195, 18, color, 145, 0);
            if (lock_time_known)
                draw_centered(graphics, text.date, L"Segoe UI", 10, 215, 16, color, 120, 0);
        }
        return finish();
    }
    if (_settings.layout == 2)
    {
        const PointF center(160, 120);
        constexpr float pi = 3.14159265358979323846f;
        // Intersect clock angles with the rectangular face so hands point at their perimeter ticks.
        const auto reach = [](float angle, float half_width, float half_height) {
            const float x = std::abs(std::sin(angle));
            const float y = std::abs(std::cos(angle));
            return std::min(half_width / std::max(x, 0.0001f), half_height / std::max(y, 0.0001f));
        };
        Pen minute_marks(text_color(color, 105), 0.7f);
        Pen hour_marks(text_color(color, 245), 1.5f);
        minute_marks.SetEndCap(LineCapRound);
        hour_marks.SetEndCap(LineCapRound);
        for (int tick = 0; tick < 60; ++tick)
        {
            const float angle = tick * pi / 30;
            const bool hour = (tick % 5) == 0;
            const float outer = reach(angle, 152, 112);
            const float inner = outer - (hour ? 10.0f : 3.5f);
            graphics.DrawLine(hour ? &hour_marks : &minute_marks, center.X + std::sin(angle) * inner,
                              center.Y - std::cos(angle) * inner, center.X + std::sin(angle) * outer,
                              center.Y - std::cos(angle) * outer);
        }

        Font numerals(_settings.font.c_str(), static_cast<float>(_settings.analog_font_size),
                      _settings.font_style, UnitPixel);
        SolidBrush numeral_color(text_color(color, 230));
        StringFormat numeral_format(StringFormat::GenericTypographic());
        numeral_format.SetAlignment(StringAlignmentCenter);
        numeral_format.SetLineAlignment(StringAlignmentCenter);
        numeral_format.SetFormatFlags(StringFormatFlagsNoWrap);
        float hand_half_width = 112;
        float hand_half_height = 78;
        for (int hour = 1; hour <= 12; ++hour)
        {
            const float angle = hour * pi / 6;
            const std::wstring number = std::to_wstring(hour);
            RectF bounds;
            graphics.MeasureString(number.c_str(), -1, &numerals, PointF(0, 0), &numeral_format, &bounds);
            const float width = bounds.Width + 2;
            const float height = bounds.Height + 2;
            if ((hour == 3) || (hour == 9))
                hand_half_width = std::min(hand_half_width, 134 - width - 6);
            if ((hour == 6) || (hour == 12))
                hand_half_height = std::min(hand_half_height, 94 - height - 6);
            // Keep the entire glyph box inside the dial, leaving a clear gap after the hour batons.
            const float distance = reach(angle, 134 - (width * 0.5f), 94 - (height * 0.5f));
            graphics.DrawString(number.c_str(), -1, &numerals,
                                RectF(center.X + (std::sin(angle) * distance) - (width * 0.5f),
                                      center.Y - (std::cos(angle) * distance) - (height * 0.5f), width,
                                      height),
                                &numeral_format, &numeral_color);
        }
        if (_settings.label.empty() == false)
            draw_centered(graphics, _settings.label, _settings.font.c_str(), 10, 72, 18, color, 145, 0);
        else if (_settings.show_weekday)
            draw_centered(graphics, text.weekday, _settings.font.c_str(), 10, 72, 18, color, 145, 0);
        if (_settings.show_codex_usage)
        {
            SYSTEMTIME utc = {};
            GetSystemTime(&utc);
            const CodexUsageText usage_text =
                format_codex_usage(usage, utc_ticks(utc), _settings.codex_refresh_seconds);
            if (usage_text.remaining_percent >= 0)
            {
                float size = std::clamp(_settings.analog_font_size * 1.2f, 22.0f, 30.0f);
                StringFormat format(StringFormat::GenericTypographic());
                format.SetAlignment(StringAlignmentCenter);
                format.SetLineAlignment(StringAlignmentCenter);
                format.SetFormatFlags(StringFormatFlagsNoWrap);
                for (;;)
                {
                    Font font(_settings.font.c_str(), size, _settings.font_style, UnitPixel);
                    RectF bounds;
                    graphics.MeasureString(usage_text.value.c_str(), -1, &font, PointF(0, 0), &format,
                                           &bounds);
                    if ((bounds.Width <= 80) || (size <= 20))
                    {
                        SolidBrush ink(text_color(color, 240));
                        graphics.DrawString(usage_text.value.c_str(), -1, &font, RectF(187, 102, 80, 36),
                                            &format, &ink);
                        break;
                    }
                    size -= 0.5f;
                }
                SolidBrush track(text_color(color, 40));
                SolidBrush fill(text_color(_settings.accent_color, 190));
                graphics.FillRectangle(&track, 204, 143, 46, 1);
                graphics.FillRectangle(&fill, 204.0f, 143.0f, 46 * usage_text.remaining_percent / 100.0f,
                                       1.0f);
            }
        }
        else if (_settings.show_date)
            draw_centered(graphics, text.date, _settings.font.c_str(), 10, 154, 20, color, 165, 0);

        const auto hand = [&](float angle, float length, float width, float tail, COLORREF hand_color) {
            Pen pen(text_color(hand_color, 240), width);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            graphics.DrawLine(&pen, center.X - (std::sin(angle) * tail), center.Y + (std::cos(angle) * tail),
                              center.X + (std::sin(angle) * length), center.Y - (std::cos(angle) * length));
        };
        const auto lance = [&](float angle, float length, float width, float tail) {
            const PointF direction(std::sin(angle), -std::cos(angle));
            const PointF normal(std::cos(angle), std::sin(angle));
            const PointF points[] = {
                PointF(center.X + (direction.X * length), center.Y + (direction.Y * length)),
                PointF(center.X + (normal.X * width), center.Y + (normal.Y * width)),
                PointF(center.X - (direction.X * tail), center.Y - (direction.Y * tail)),
                PointF(center.X - (normal.X * width), center.Y - (normal.Y * width))};
            SolidBrush metal(text_color(color, 245));
            graphics.FillPolygon(&metal, points, 4);
        };
        const float hour_angle =
            ((time.wHour % 12) + (time.wMinute / 60.0f) + (time.wSecond / 3600.0f)) * pi / 6;
        const float minute_angle = (time.wMinute + (time.wSecond / 60.0f)) * pi / 30;
        lance(hour_angle, reach(hour_angle, hand_half_width, hand_half_height) * 0.68f, 2.8f, 8);
        lance(minute_angle, reach(minute_angle, hand_half_width, hand_half_height), 1.7f, 10);
        if (_settings.show_seconds)
        {
            const float angle = time.wSecond * pi / 30;
            hand(angle, reach(angle, 152, 112) - 12, 0.75f, 19, _settings.accent_color);
            SolidBrush counterweight(text_color(_settings.accent_color, 255));
            graphics.FillEllipse(&counterweight, center.X - (std::sin(angle) * 17) - 2,
                                 center.Y + (std::cos(angle) * 17) - 2, 4.0f, 4.0f);
        }
        SolidBrush hub(text_color(color, 255));
        graphics.FillEllipse(&hub, center.X - 3, center.Y - 3, 6.0f, 6.0f);
        SolidBrush pin(text_color(_settings.accent_color, 255));
        graphics.FillEllipse(&pin, center.X - 1.25f, center.Y - 1.25f, 2.5f, 2.5f);
        return finish();
    }

    if (_settings.label.empty() == false)
        draw_centered(graphics, _settings.label, L"Segoe UI", 11, 27, 20, color, 165, 0);
    else if (_settings.show_weekday)
        draw_centered(graphics, text.weekday, L"Segoe UI", 11, 27, 20, color, 165, 0);

    std::wstring clock = text.time;
    std::wstring detail = text.detail;
    if ((_settings.layout == 1) && _settings.show_seconds)
    {
        wchar_t seconds[8] = {};
        swprintf_s(seconds, L":%02d", time.wSecond);
        clock += seconds;
        detail = _settings.use_24_hour ? L"" : (time.wHour < 12 ? L"AM" : L"PM");
    }

    const float detail_top = std::clamp(170.0f + _settings.vertical_offset, 142.0f, 168.0f);
    const float top = std::max(54.0f, (detail.empty() ? 77.0f : 65.0f) + _settings.vertical_offset);
    const float height = std::min(105.0f, (detail.empty() ? 190.0f : detail_top - 3) - top);
    float size = static_cast<float>(_settings.font_size);
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    for (;;)
    {
        Font font(_settings.font.c_str(), size, _settings.font_style, UnitPixel);
        RectF bounds;
        graphics.MeasureString(clock.c_str(), -1, &font, PointF(0, 0), &format, &bounds);
        if (((bounds.Width <= 286) && (bounds.Height <= height)) || (size <= 20))
            break;
        size -= 2;
    }
    draw_centered(graphics, clock, _settings.font.c_str(), size, top, height, color, 255,
                  _settings.font_style);
    if (detail.empty() == false)
        draw_centered(graphics, detail, L"Segoe UI", 17, detail_top, 25, _settings.accent_color, 220, 0);
    SolidBrush accent(text_color(_settings.accent_color, 140));
    graphics.FillRectangle(&accent, 149, 196, 22, 1);
    if (_settings.show_date)
        draw_centered(graphics, text.date, L"Segoe UI", 12, 208, 20, color, 180, 0);
    return finish();
}

std::vector<BYTE> ClockRenderer::encode(Bitmap &image) const
{
    using Microsoft::WRL::ComPtr;
    struct Pixels
    {
        Bitmap &image;
        BitmapData data = {};
        ~Pixels()
        {
            if (data.Scan0 != nullptr)
                image.UnlockBits(&data);
        }
    } pixels{image};
    Rect rectangle(0, 0, image.GetWidth(), image.GetHeight());
    if (image.LockBits(&rectangle, ImageLockModeRead, PixelFormat24bppRGB, &pixels.data) != Ok)
        throw std::runtime_error("Could not read clock pixels");
    for (const float quality : {1.0f, 0.98f, 0.95f, 0.90f, 0.85f, 0.75f, 0.60f, 0.45f, 0.30f})
    {
        ComPtr<IStream> stream;
        require_jpeg(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
        ComPtr<IWICBitmapEncoder> encoder;
        require_jpeg(_jpeg_factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
        require_jpeg(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        require_jpeg(encoder->CreateNewFrame(&frame, &options));
        PROPBAG2 properties[2] = {};
        properties[0].pstrName = const_cast<wchar_t *>(L"ImageQuality");
        properties[1].pstrName = const_cast<wchar_t *>(L"JpegYCrCbSubsampling");
        VARIANT values[2] = {};
        values[0].vt = VT_R4;
        values[0].fltVal = quality;
        values[1].vt = VT_UI1;
        values[1].bVal = WICJpegYCrCbSubsampling444;
        require_jpeg(options->Write(2, properties, values));
        require_jpeg(frame->Initialize(options.Get()));
        require_jpeg(frame->SetSize(image.GetWidth(), image.GetHeight()));
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        require_jpeg(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat24bppBGR)
            throw std::runtime_error("Windows JPEG encoder rejected clock colors");
        const UINT stride = static_cast<UINT>(std::abs(pixels.data.Stride));
        auto scan = static_cast<BYTE *>(pixels.data.Scan0);
        if (pixels.data.Stride > 0)
            require_jpeg(frame->WritePixels(image.GetHeight(), stride, stride * image.GetHeight(), scan));
        else
            for (UINT row = 0; row < image.GetHeight(); ++row)
                require_jpeg(frame->WritePixels(1, stride, stride,
                                                scan + (static_cast<ptrdiff_t>(row) * pixels.data.Stride)));
        require_jpeg(frame->Commit());
        require_jpeg(encoder->Commit());
        STATSTG stats = {};
        require_jpeg(stream->Stat(&stats, STATFLAG_NONAME));
        if (stats.cbSize.QuadPart > 65515)
            continue;
        std::vector<BYTE> result(static_cast<size_t>(stats.cbSize.QuadPart));
        LARGE_INTEGER start = {};
        require_jpeg(stream->Seek(start, STREAM_SEEK_SET, nullptr));
        ULONG read = 0;
        require_jpeg(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read));
        if (read != result.size())
            throw std::runtime_error("Windows could not encode the clock image");
        if ((result.empty() == false) && (result.size() <= 65515))
            return result;
    }
    throw std::runtime_error("The background is too complex for the hub's image size limit");
}
