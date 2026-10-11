#include "lock_session.h"

void LockSession::observe(bool now_locked, bool lock_event, ULONGLONG utc_time, ULONGLONG monotonic_ms)
{
    if (locked == now_locked)
        return;
    locked = now_locked;
    lock_time_known = now_locked && lock_event;
    locked_utc_ticks = now_locked ? utc_time : 0;
    started_ms = now_locked ? monotonic_ms : 0;
}

ULONGLONG LockSession::elapsed_minutes(ULONGLONG monotonic_ms) const
{
    return locked ? (monotonic_ms - started_ms) / 60000 : 0;
}

LockUpdateSchedule next_lock_update(const LockSession &session, ULONGLONG monotonic_ms, DWORD lead_ms)
{
    const ULONGLONG minute = ((monotonic_ms - session.started_ms + lead_ms) / 60000) + 1;
    const ULONGLONG target = session.started_ms + (minute * 60000);
    return {target, minute, static_cast<DWORD>(target - monotonic_ms - lead_ms)};
}

std::wstring format_locked_duration(ULONGLONG minutes)
{
    if (minutes < 60)
        return std::to_wstring(minutes) + L" min";
    const auto padded = [](ULONGLONG value) { return (value < 10 ? L"0" : L"") + std::to_wstring(value); };
    const std::wstring hours = std::to_wstring((minutes / 60) % 24);
    const std::wstring remainder = padded(minutes % 60) + L" min";
    if (minutes < 1440)
        return hours + L" h " + remainder;
    return std::to_wstring(minutes / 1440) + L" d " + padded((minutes / 60) % 24) + L" h " + remainder;
}
