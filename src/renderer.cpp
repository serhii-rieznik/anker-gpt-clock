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

COLORREF metric_color(int reading, double value, COLORREF normal)
{
    if (reading == 2)
        return value < 1000 ? RGB(138, 172, 155) : (value < 2000 ? RGB(186, 164, 111) : RGB(186, 130, 132));
    if (reading == 3)
        return value < 55 ? RGB(138, 172, 155) : (value < 85 ? RGB(186, 164, 111) : RGB(186, 130, 132));
    if (reading == 4)
        return value >= 50 ? RGB(138, 172, 155) : (value >= 20 ? RGB(186, 164, 111) : RGB(186, 130, 132));
    return RGB((GetRValue(normal) * 4) / 5, (GetGValue(normal) * 4) / 5, (GetBValue(normal) * 4) / 5);
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

void rounded_path(GraphicsPath &path, const RectF &box, float radius)
{
    const float diameter = radius * 2;
    path.AddArc(box.X, box.Y, diameter, diameter, 180, 90);
    path.AddArc(box.GetRight() - diameter, box.Y, diameter, diameter, 270, 90);
    path.AddArc(box.GetRight() - diameter, box.GetBottom() - diameter, diameter, diameter, 0, 90);
    path.AddArc(box.X, box.GetBottom() - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
}

void draw_reading_icon(Graphics &graphics, int reading, float x, float y, COLORREF color)
{
    const GraphicsState saved = graphics.Save();
    graphics.TranslateTransform(x, y);
    graphics.ScaleTransform(0.86f, 0.86f);
    Pen pen(text_color(color, 255), 1.8f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush fill(text_color(color, 255));
    GraphicsPath path;
    if (reading == 0)
    {
        path.AddLine(4.5f, 19.0f, 4.5f, 4.0f);
        path.AddArc(4.5f, 0.5f, 7.0f, 7.0f, 180.0f, 180.0f);
        path.AddLine(11.5f, 4.0f, 11.5f, 19.0f);
        path.AddBezier(11.5f, 19.0f, 17.0f, 23.5f, 13.5f, 30.0f, 8.0f, 30.0f);
        path.AddBezier(8.0f, 30.0f, 2.5f, 30.0f, -1.0f, 23.5f, 4.5f, 19.0f);
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, 8.0f, 9.0f, 8.0f, 23.0f);
        graphics.FillEllipse(&fill, 5.7f, 22.7f, 4.6f, 4.6f);
    }
    else if (reading == 1)
    {
        path.AddBezier(8, 1, 6, 8, 0, 15, 0, 21);
        path.AddBezier(0, 21, 0, 32, 16, 32, 16, 21);
        path.AddBezier(16, 21, 16, 15, 10, 8, 8, 1);
        graphics.DrawPath(&pen, &path);
    }
    else if (reading == 2)
    {
        graphics.DrawLine(&pen, 8.0f, 7.5f, 8.0f, 11.5f);
        graphics.DrawLine(&pen, 8.0f, 19.0f, 8.0f, 23.0f);
        graphics.DrawEllipse(&pen, 4.5f, 0.5f, 7.0f, 7.0f);
        graphics.DrawEllipse(&pen, 3.5f, 10.5f, 9.0f, 9.0f);
        graphics.DrawEllipse(&pen, 4.5f, 23.5f, 7.0f, 7.0f);
    }
    else if (reading == 3)
    {
        const float heights[] = {8, 19, 28, 19, 8};
        for (int index = 0; index < 5; ++index)
            graphics.DrawLine(&pen, 1 + (index * 3.5f), 14 - (heights[index] * 0.5f), 1 + (index * 3.5f),
                              14 + (heights[index] * 0.5f));
    }
    else if (reading == 4)
    {
        rounded_path(path, RectF(0, 1, 16, 28), 3);
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, 4.0f, 9.0f, 8.0f, 13.0f);
        graphics.DrawLine(&pen, 8.0f, 13.0f, 4.0f, 17.0f);
        graphics.DrawLine(&pen, 8.0f, 21.0f, 12.0f, 21.0f);
    }
    graphics.Restore(saved);
}

void draw_reading(Graphics &graphics, int reading, const std::wstring &value, const wchar_t *unit, float top,
                  float size, COLORREF color, bool stale)
{
    GraphicsPath cell;
    rounded_path(cell, RectF(187, top, 130, 50), 8);
    SolidBrush background(Color(255, 16, 19, 17));
    graphics.FillPath(&background, &cell);
    if (reading >= 0)
        draw_reading_icon(graphics, reading, 194, top + 12, color);
    if (stale)
    {
        constexpr float x = 208;
        const float y = top + 37;
        graphics.FillEllipse(&background, x - 7, y - 7, 14.0f, 14.0f);
        Pen badge(Color(255, 186, 164, 111), 1.4f);
        badge.SetStartCap(LineCapRound);
        badge.SetEndCap(LineCapRound);
        graphics.DrawEllipse(&badge, x - 5, y - 5, 10.0f, 10.0f);
        graphics.DrawLine(&badge, x, y - 3, x, y);
        graphics.DrawLine(&badge, x, y, x + 2.5f, y + 1.5f);
    }
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    FontFamily family(L"Segoe UI");
    const float unit_size = reading == 0 ? 20.0f : 18.0f;
    Font unit_font(&family, unit_size, FontStyleRegular, UnitPixel);
    RectF unit_bounds;
    if (unit[0] != L'\0')
        graphics.MeasureString(unit, -1, &unit_font, PointF(0, 0), &format, &unit_bounds);
    const float gap = reading == 0 ? 0.0f : 3.0f;
    RectF bounds;
    for (;;)
    {
        Font font(&family, size, FontStyleRegular, UnitPixel);
        graphics.MeasureString(value.c_str(), -1, &font, PointF(0, 0), &format, &bounds);
        if (((bounds.Width + gap + unit_bounds.Width) <= 97) || (size <= 24))
            break;
        size -= 1;
    }
    const float ascent =
        static_cast<float>(family.GetCellAscent(FontStyleRegular)) / family.GetEmHeight(FontStyleRegular);
    const float baseline = top + 25 + (size * 0.34f);
    Font font(&family, size, FontStyleRegular, UnitPixel);
    SolidBrush ink(text_color(color, 255));
    graphics.DrawString(value.c_str(), -1, &font, PointF(217, baseline - (ascent * size)), &format, &ink);
    const float unit_baseline = reading == 0 ? baseline - (size * 0.38f) : baseline;
    if (unit[0] != L'\0')
        graphics.DrawString(unit, -1, &unit_font,
                            PointF(217 + bounds.Width + gap, unit_baseline - (ascent * unit_size)), &format,
                            &ink);
}

void draw_analog_date(Graphics &graphics, const SYSTEMTIME &time, const Settings &settings, float x, float y)
{
    constexpr const wchar_t *months[] = {L"JAN", L"FEB", L"MAR", L"APR", L"MAY", L"JUN",
                                         L"JUL", L"AUG", L"SEP", L"OCT", L"NOV", L"DEC"};
    const std::wstring day = std::to_wstring(time.wDay);
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    float size = 22;
    for (;;)
    {
        Font font(settings.font.c_str(), size, settings.font_style, UnitPixel);
        RectF bounds;
        graphics.MeasureString(day.c_str(), -1, &font, PointF(0, 0), &format, &bounds);
        if ((bounds.Width <= 40) || (size <= 18))
        {
            SolidBrush ink(text_color(settings.text_color, 230));
            graphics.DrawString(day.c_str(), -1, &font, RectF(x - 22, y - 26, 44, 29), &format, &ink);
            break;
        }
        size -= 1;
    }
    Font month(L"Segoe UI", 18, FontStyleRegular, UnitPixel);
    SolidBrush ink(text_color(settings.text_color, 180));
    graphics.DrawString(months[time.wMonth - 1], -1, &month, RectF(x - 22, y + 1, 44, 25), &format, &ink);
}

void draw_lock_overlay(Graphics &graphics, float width, float duration_width, ULONGLONG elapsed_minutes,
                       bool lock_time_known, COLORREF color)
{
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    Font heading(L"Segoe UI", 20, FontStyleRegular, UnitPixel);
    RectF bounds;
    graphics.MeasureString(L"locked", -1, &heading, PointF(0, 0), &format, &bounds);
    const float left = (width - bounds.Width - 23) * 0.5f;
    Pen outline(text_color(color, 210), 1.6f);
    outline.SetLineJoin(LineJoinRound);
    graphics.DrawArc(&outline, left + 3, 5.0f, 8.0f, 10.0f, 180.0f, 180.0f);
    graphics.DrawRectangle(&outline, left, 10.0f, 14.0f, 11.0f);
    SolidBrush ink(text_color(color, 210));
    graphics.FillEllipse(&ink, left + 6, 14.0f, 2.0f, 2.0f);
    format.SetLineAlignment(StringAlignmentCenter);
    graphics.DrawString(L"locked", -1, &heading, RectF(left + 23, 0, bounds.Width + 2, 29), &format, &ink);
    const std::wstring duration = (lock_time_known ? L"" : L"≥ ") + format_locked_duration(elapsed_minutes);
    float size = 18;
    for (;;)
    {
        Font font(L"Segoe UI", size, FontStyleRegular, UnitPixel);
        graphics.MeasureString(duration.c_str(), -1, &font, PointF(0, 0), &format, &bounds);
        if ((bounds.Width <= (duration_width - 8)) || (size <= 16))
        {
            format.SetAlignment(StringAlignmentCenter);
            graphics.DrawString(duration.c_str(), -1, &font, RectF(4, 214, duration_width - 8, 26), &format,
                                &ink);
            break;
        }
        size -= 1;
    }
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
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        ImageAttributes edges;
        edges.SetWrapMode(WrapModeTileFlipXY);
        graphics.Clear(Color(255, 9, 12, 19));
        if (settings.background_fit == 0)
        {
            const float scale = std::max(320.0f / source.GetWidth(), 240.0f / source.GetHeight());
            const float width = 320.0f / scale;
            const float height = 240.0f / scale;
            graphics.DrawImage(&source, RectF(0, 0, 320, 240), (source.GetWidth() - width) * 0.5f,
                               (source.GetHeight() - height) * 0.5f, width, height, UnitPixel, &edges);
        }
        else
        {
            const float scale = std::min(320.0f / source.GetWidth(), 240.0f / source.GetHeight());
            const float width = source.GetWidth() * scale;
            const float height = source.GetHeight() * scale;
            graphics.DrawImage(&source, RectF((320 - width) * 0.5f, (240 - height) * 0.5f, width, height), 0,
                               0, static_cast<float>(source.GetWidth()),
                               static_cast<float>(source.GetHeight()), UnitPixel, &edges);
        }
        SolidBrush dim(Color(static_cast<BYTE>((settings.image_dim * 255) / 100), 0, 0, 0));
        graphics.SetSmoothingMode(SmoothingModeNone);
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
    return render(time, usage, NetatmoData{});
}

std::unique_ptr<Bitmap> ClockRenderer::render(const SYSTEMTIME &time, const CodexUsage &usage,
                                              const NetatmoData &weather) const
{
    return render_frame(time, false, 0, false, usage, weather);
}

std::unique_ptr<Bitmap> ClockRenderer::render_locked(const SYSTEMTIME &time, ULONGLONG elapsed_minutes,
                                                     bool lock_time_known) const
{
    return render_locked(time, elapsed_minutes, lock_time_known, CodexUsage{});
}

std::unique_ptr<Bitmap> ClockRenderer::render_locked(const SYSTEMTIME &time, ULONGLONG elapsed_minutes,
                                                     bool lock_time_known, const CodexUsage &usage) const
{
    return render_locked(time, elapsed_minutes, lock_time_known, usage, NetatmoData{});
}

std::unique_ptr<Bitmap> ClockRenderer::render_locked(const SYSTEMTIME &time, ULONGLONG elapsed_minutes,
                                                     bool lock_time_known, const CodexUsage &usage,
                                                     const NetatmoData &weather) const
{
    return render_frame(time, true, elapsed_minutes, lock_time_known, usage, weather);
}

std::unique_ptr<Bitmap> ClockRenderer::render_frame(const SYSTEMTIME &time, bool locked,
                                                    ULONGLONG elapsed_minutes, bool lock_time_known,
                                                    const CodexUsage &usage, const NetatmoData &weather) const
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
    if (_settings.layout == 2)
    {
        const bool show_weather = _settings.show_netatmo;
        const bool precision = _settings.analog_face == 1;
        const bool framed = _settings.analog_face == 2;
        const PointF center(show_weather ? 92.0f : 160.0f, 120);
        const float outer_width =
            show_weather ? (framed ? 75.0f : (precision ? 86.0f : 88.0f)) : (framed ? 140.0f : 152.0f);
        const float outer_height =
            show_weather ? outer_width : (locked ? (framed ? 75.0f : 88.0f) : (framed ? 100.0f : 112.0f));
        const float numeral_width = show_weather ? 70.0f : 134.0f;
        const float numeral_height = (show_weather || locked) ? 70.0f : 94.0f;
        if (framed)
        {
            GraphicsPath border;
            rounded_path(border,
                         show_weather ? RectF(6, 34, 172, 172)
                                      : (locked ? RectF(8, 34, 304, 172) : RectF(8, 8, 304, 224)),
                         14);
            Pen outline(text_color(color, 70), 1);
            graphics.DrawPath(&outline, &border);
        }
        constexpr float pi = 3.14159265358979323846f;
        // Intersect clock angles with the rectangular face so hands point at their perimeter ticks.
        const auto reach = [](float angle, float half_width, float half_height) {
            const float x = std::abs(std::sin(angle));
            const float y = std::abs(std::cos(angle));
            return std::min(half_width / std::max(x, 0.0001f), half_height / std::max(y, 0.0001f));
        };
        Pen minute_marks(text_color(color, 105), 0.7f);
        Pen hour_marks(text_color(color, show_weather ? 210 : 245), precision ? 2.3f : 1.5f);
        minute_marks.SetEndCap(LineCapRound);
        hour_marks.SetEndCap(LineCapRound);
        for (int tick = 0; tick < 60; ++tick)
        {
            const float angle = tick * pi / 30;
            const bool hour = (tick % 5) == 0;
            if ((precision) && (hour == false))
                continue;
            const float outer = reach(angle, outer_width, outer_height);
            const float inner = outer - (hour ? (precision ? 11.0f : (show_weather ? 8.0f : 10.0f)) : 3.5f);
            graphics.DrawLine(hour ? &hour_marks : &minute_marks, center.X + std::sin(angle) * inner,
                              center.Y - std::cos(angle) * inner, center.X + std::sin(angle) * outer,
                              center.Y - std::cos(angle) * outer);
        }

        Font numerals(_settings.font.c_str(), _settings.analog_font_size * (precision ? 1.25f : 1.0f),
                      _settings.font_style, UnitPixel);
        SolidBrush numeral_color(text_color(color, 230));
        StringFormat numeral_format(StringFormat::GenericTypographic());
        numeral_format.SetAlignment(StringAlignmentCenter);
        numeral_format.SetLineAlignment(StringAlignmentCenter);
        numeral_format.SetFormatFlags(StringFormatFlagsNoWrap);
        float hand_half_width = show_weather ? 56.0f : 112.0f;
        float hand_half_height = (show_weather || locked) ? 56.0f : 78.0f;
        float date_x = center.X + (outer_width * 0.5f);
        for (int hour = 1; (framed == false) && (hour <= 12); ++hour)
        {
            if ((precision) && ((hour % 3) != 0))
                continue;
            const float angle = hour * pi / 6;
            const std::wstring number = std::to_wstring(hour);
            RectF bounds;
            graphics.MeasureString(number.c_str(), -1, &numerals, PointF(0, 0), &numeral_format, &bounds);
            const float width = bounds.Width + 2;
            const float height = bounds.Height + 2;
            if ((hour == 3) || (hour == 9))
                hand_half_width = std::min(hand_half_width, numeral_width - width - 6);
            if ((hour == 6) || (hour == 12))
                hand_half_height = std::min(hand_half_height, numeral_height - height - 6);
            if (hour == 3)
                date_x = center.X + ((numeral_width - (width * 0.5f)) * 0.5f);
            // Keep the entire glyph box inside the dial, leaving a clear gap after the hour batons.
            const float distance =
                reach(angle, numeral_width - (width * 0.5f), numeral_height - (height * 0.5f));
            graphics.DrawString(number.c_str(), -1, &numerals,
                                RectF(center.X + (std::sin(angle) * distance) - (width * 0.5f),
                                      center.Y - (std::cos(angle) * distance) - (height * 0.5f), width,
                                      height),
                                &numeral_format, &numeral_color);
        }
        if ((locked == false) && (show_weather == false) && (_settings.label.empty() == false))
            draw_centered(graphics, _settings.label, _settings.font.c_str(), 10, 72, 18, color, 145, 0);
        else if ((locked == false) && (show_weather == false) && _settings.show_weekday)
            draw_centered(graphics, text.weekday, _settings.font.c_str(), 10, 72, 18, color, 145, 0);
        if (_settings.show_codex_usage && (show_weather == false))
        {
            SYSTEMTIME utc = {};
            GetSystemTime(&utc);
            const CodexUsageText usage_text =
                format_codex_usage(usage, utc_ticks(utc), _settings.codex_refresh_seconds);
            if (usage_text.remaining_percent >= 0)
            {
                float size = std::clamp(_settings.analog_font_size * 1.2f, 22.0f, 30.0f);
                constexpr float usage_width = 80.0f;
                const float usage_x = _settings.show_date ? 53.0f : 187.0f;
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
                    if ((bounds.Width <= usage_width) || (size <= 20))
                    {
                        SolidBrush ink(text_color(color, 240));
                        graphics.DrawString(usage_text.value.c_str(), -1, &font,
                                            RectF(usage_x, 102, usage_width, 36), &format, &ink);
                        break;
                    }
                    size -= 0.5f;
                }
                SolidBrush track(text_color(color, 40));
                SolidBrush fill(text_color(_settings.accent_color, 190));
                const float track_x = usage_x + 17;
                constexpr float track_width = 46.0f;
                graphics.FillRectangle(&track, track_x, 143.0f, track_width, 1.0f);
                graphics.FillRectangle(&fill, track_x, 143.0f,
                                       track_width * usage_text.remaining_percent / 100.0f, 1.0f);
            }
        }
        if (_settings.show_date)
            draw_analog_date(graphics, time, _settings, date_x, center.Y);

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
        if (precision)
        {
            hand(hour_angle, reach(hour_angle, hand_half_width, hand_half_height) * 0.68f, 4, 6, color);
            hand(minute_angle, reach(minute_angle, hand_half_width, hand_half_height), 2.6f, 8, color);
        }
        else
        {
            lance(hour_angle, reach(hour_angle, hand_half_width, hand_half_height) * 0.68f,
                  show_weather ? 2.1f : 2.8f, 8);
            lance(minute_angle, reach(minute_angle, hand_half_width, hand_half_height),
                  show_weather ? 1.6f : 1.7f, show_weather ? 8.0f : 10.0f);
        }
        if (_settings.show_seconds)
        {
            const float angle = time.wSecond * pi / 30;
            hand(angle, reach(angle, outer_width, outer_height) - 12, 0.75f, 19, _settings.accent_color);
            if (show_weather == false)
            {
                SolidBrush counterweight(text_color(_settings.accent_color, 255));
                graphics.FillEllipse(&counterweight, center.X - (std::sin(angle) * 17) - 2,
                                     center.Y + (std::cos(angle) * 17) - 2, 4.0f, 4.0f);
            }
        }
        SolidBrush hub(text_color(color, 255));
        graphics.FillEllipse(&hub, center.X - 3, center.Y - 3, 6.0f, 6.0f);
        SolidBrush pin(text_color(_settings.accent_color, 255));
        graphics.FillEllipse(&pin, center.X - 1.25f, center.Y - 1.25f, 2.5f, 2.5f);
        if (show_weather)
        {
            SYSTEMTIME utc = {};
            GetSystemTime(&utc);
            const auto selection = netatmo_selection(weather, _settings.netatmo_module_id, utc_ticks(utc),
                                                     _settings.netatmo_refresh_seconds);
            const auto *module = ((selection.availability == NetatmoAvailability::ready) || selection.stale())
                                     ? selection.module
                                     : nullptr;
            const int readings =
                module == nullptr ? 0 : (_settings.netatmo_readings & module->available_readings);
            int count = 0;
            for (int index = 0; index < 4; ++index)
                if ((readings & (1 << index)) != 0)
                    ++count;
            const bool no_data = count == 0;
            if (no_data)
                count = 1;
            const CodexUsageText codex =
                format_codex_usage(usage, utc_ticks(utc), _settings.codex_refresh_seconds);
            if (_settings.show_codex_usage)
                ++count;
            const float row_height = count >= 4 ? 58.0f : 70.0f;
            int row = 0;
            const auto draw_cell = [&](int reading, const std::wstring &value, const wchar_t *unit,
                                       COLORREF ink, int position) {
                const float top = ((240 - (row_height * count)) * 0.5f) + (row_height * position) +
                                  ((row_height - 50) * 0.5f);
                draw_reading(graphics, reading, value, unit, top,
                             static_cast<float>(_settings.reading_font_size + (reading == 0 ? 2 : 0)), ink,
                             (reading >= 0) && (reading < 4) && selection.stale());
            };
            if (no_data)
            {
                draw_cell(-1, L"—", L"", metric_color(0, 0, color), row);
                ++row;
            }
            else
            {
                const wchar_t *units[] = {L"°", L"%", L"ppm", L"dB"};
                for (int index = 0; index < 4; ++index)
                {
                    if ((readings & (1 << index)) == 0)
                        continue;
                    wchar_t value[64] = {};
                    swprintf_s(value, index == 0 ? L"%.1f" : L"%.0f", module->values[index]);
                    draw_cell(index, value, units[index], metric_color(index, module->values[index], color),
                              row++);
                }
            }
            if (_settings.show_codex_usage && (codex.remaining_percent >= 0))
                draw_cell(4, std::to_wstring(codex.remaining_percent), L"%",
                          metric_color(4, codex.remaining_percent, color), row);
        }
        if (locked)
            draw_lock_overlay(graphics, show_weather ? 184.0f : 320.0f, show_weather ? 184.0f : 320.0f,
                              elapsed_minutes, lock_time_known, color);
        return finish();
    }

    if ((locked == false) && (_settings.label.empty() == false))
        draw_centered(graphics, _settings.label, L"Segoe UI", 11, 27, 20, color, 165, 0);
    else if ((locked == false) && _settings.show_weekday)
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
    if (locked)
    {
        SYSTEMTIME utc = {};
        GetSystemTime(&utc);
        const auto codex = format_codex_usage(usage, utc_ticks(utc), _settings.codex_refresh_seconds);
        const bool show_usage = _settings.show_codex_usage && (codex.remaining_percent >= 0);
        draw_lock_overlay(graphics, 320, show_usage ? 212.0f : 320.0f, elapsed_minutes, lock_time_known,
                          color);
        if (show_usage)
        {
            const COLORREF ink = metric_color(4, codex.remaining_percent, color);
            draw_reading_icon(graphics, 4, 221, 213, ink);
            Font font(L"Segoe UI", 24, FontStyleRegular, UnitPixel);
            SolidBrush brush(text_color(ink, 255));
            format.SetLineAlignment(StringAlignmentCenter);
            graphics.DrawString(codex.value.c_str(), -1, &font, RectF(242, 213, 76, 26), &format, &brush);
        }
    }
    else
    {
        SolidBrush accent(text_color(_settings.accent_color, 140));
        graphics.FillRectangle(&accent, 149, 196, 22, 1);
        if (_settings.show_date)
            draw_centered(graphics, text.date, L"Segoe UI", 12, 208, 20, color, 180, 0);
    }
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
