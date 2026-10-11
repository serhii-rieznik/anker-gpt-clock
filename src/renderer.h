#pragma once
#include "settings.h"
#include "codex_usage.h"
#include <objidl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <cstdint>

struct ClockText
{
    std::wstring time;
    std::wstring detail;
    std::wstring weekday;
    std::wstring date;
};

ClockText format_clock(const SYSTEMTIME &time, const Settings &settings);
struct UpdateSchedule
{
    ULONGLONG target_utc_ticks;
    DWORD delay_ms;
};

ULONGLONG utc_ticks(const SYSTEMTIME &utc_time);
SYSTEMTIME utc_from_ticks(ULONGLONG ticks);
UpdateSchedule next_update_schedule(const SYSTEMTIME &utc_time, int interval_seconds, DWORD lead_ms);
uint16_t image_crc(const std::vector<BYTE> &image);
void distinguish_crc(std::vector<BYTE> &image, uint16_t previous_crc);

struct GraphicsRuntime
{
    ULONG_PTR token = 0;
    GraphicsRuntime();
    ~GraphicsRuntime();
};

class ClockRenderer
{
  public:
    ClockRenderer();
    void configure(const Settings &settings);
    SYSTEMTIME current_time() const;
    SYSTEMTIME time_at(const SYSTEMTIME &utc_time) const;
    std::unique_ptr<Gdiplus::Bitmap> render(const SYSTEMTIME &time) const;
    std::unique_ptr<Gdiplus::Bitmap> render(const SYSTEMTIME &time, const CodexUsage &usage) const;
    std::unique_ptr<Gdiplus::Bitmap> render_locked(const SYSTEMTIME &locked_at, ULONGLONG elapsed_minutes,
                                                   bool lock_time_known) const;
    std::unique_ptr<Gdiplus::Bitmap> render_locked(const SYSTEMTIME &locked_at, ULONGLONG elapsed_minutes,
                                                   bool lock_time_known, const CodexUsage &usage) const;
    std::vector<BYTE> encode(Gdiplus::Bitmap &image) const;

  private:
    std::unique_ptr<Gdiplus::Bitmap> render_frame(const SYSTEMTIME &time, bool locked,
                                                  ULONGLONG elapsed_minutes, bool lock_time_known,
                                                  const CodexUsage &usage) const;
    Settings _settings;
    std::unique_ptr<Gdiplus::Bitmap> _background;
    Microsoft::WRL::ComPtr<IWICImagingFactory> _jpeg_factory;
    DYNAMIC_TIME_ZONE_INFORMATION _zone = {};
    bool _use_zone = false;
};
