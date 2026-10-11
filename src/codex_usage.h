#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <string_view>

struct CodexUsageWindow
{
    int remaining_percent = 0;
    int duration_minutes = 0;
    LONGLONG resets_at = 0;
};

struct CodexUsage
{
    std::array<CodexUsageWindow, 2> windows = {};
    ULONGLONG fetched_utc_ticks = 0;
    int count = 0;
    bool refresh_failed = false;
};

struct CodexUsageText
{
    std::wstring value;
    int remaining_percent = -1;
};

CodexUsage parse_codex_usage(std::wstring_view response, ULONGLONG fetched_utc_ticks);
std::wstring find_codex_executable();
CodexUsage query_codex_usage(HANDLE stop_event);
CodexUsage load_codex_usage();
void save_codex_usage(const CodexUsage &usage);
CodexUsageText format_codex_usage(const CodexUsage &usage, ULONGLONG now_utc_ticks, int refresh_seconds);
