#pragma once
#include <windows.h>
#include <string>

struct LockSession
{
    bool locked = false;
    bool lock_time_known = false;
    ULONGLONG locked_utc_ticks = 0;
    ULONGLONG started_ms = 0;

    void observe(bool now_locked, bool lock_event, ULONGLONG utc_time, ULONGLONG monotonic_ms);
    ULONGLONG elapsed_minutes(ULONGLONG monotonic_ms) const;
};

struct LockUpdateSchedule
{
    ULONGLONG target_ms;
    ULONGLONG elapsed_minutes;
    DWORD delay_ms;
};

LockUpdateSchedule next_lock_update(const LockSession &session, ULONGLONG monotonic_ms, DWORD lead_ms);
std::wstring format_locked_duration(ULONGLONG minutes);
