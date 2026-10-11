#include "codex_usage.h"
#include "settings.h"
#include <windows.data.json.h>
#include <roapi.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <cstring>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::Data::Json;

namespace
{
constexpr wchar_t cache_key[] = L"Software\\AnkerClock\\CodexUsage";
constexpr ULONGLONG unix_epoch_ticks = 116444736000000000;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle()
    {
        if ((value != nullptr) && (value != INVALID_HANDLE_VALUE))
            CloseHandle(value);
    }
    void close()
    {
        if ((value != nullptr) && (value != INVALID_HANDLE_VALUE))
            CloseHandle(value);
        value = nullptr;
    }
};

ComPtr<IJsonObject> parse_object(std::wstring_view text)
{
    Microsoft::WRL::Wrappers::RoInitializeWrapper runtime(RO_INIT_MULTITHREADED);
    if (FAILED(runtime) && (runtime != RPC_E_CHANGED_MODE))
        throw std::runtime_error("Could not initialize the Codex usage reader");
    ComPtr<IJsonObjectStatics> factory;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonObject).Get(),
                                      IID_PPV_ARGS(&factory))))
        throw std::runtime_error("Windows JSON reader is unavailable");
    ComPtr<IJsonObject> object;
    Microsoft::WRL::Wrappers::HString input;
    if (FAILED(input.Set(text.data(), static_cast<UINT32>(text.size()))) ||
        FAILED(factory->Parse(input.Get(), &object)))
        throw std::runtime_error("Codex returned invalid usage data");
    return object;
}

ComPtr<IJsonValue> named(IJsonObject *object, const wchar_t *name)
{
    ComPtr<IJsonValue> value;
    if (object != nullptr)
        object->GetNamedValue(HStringReference(name).Get(), &value);
    return value;
}

ComPtr<IJsonObject> child(IJsonObject *object, const wchar_t *name)
{
    ComPtr<IJsonObject> result;
    const auto value = named(object, name);
    if (value != nullptr)
        value->GetObject(&result);
    return result;
}

double number(IJsonObject *object, const wchar_t *name)
{
    const auto value = named(object, name);
    double result = 0;
    if ((value == nullptr) || FAILED(value->GetNumber(&result)) || (std::isfinite(result) == false))
        throw std::runtime_error("Codex usage data is missing a number");
    return result;
}

double optional_number(IJsonObject *object, const wchar_t *name)
{
    const auto value = named(object, name);
    if (value == nullptr)
        return 0;
    JsonValueType type;
    if (FAILED(value->get_ValueType(&type)))
        throw std::runtime_error("Codex returned invalid usage data");
    return type == JsonValueType_Null ? 0 : number(object, name);
}

void send_json(HANDLE pipe, const char *json)
{
    const DWORD size = static_cast<DWORD>(strlen(json));
    DWORD written = 0;
    if ((WriteFile(pipe, json, size, &written, nullptr) == FALSE) || (written != size))
        throw std::runtime_error("Could not query Codex usage");
}

struct UsageCache
{
    DWORD version = 1;
    DWORD count = 0;
    ULONGLONG fetched_utc_ticks = 0;
    std::array<CodexUsageWindow, 2> windows = {};
    DWORD refresh_failed = 0;
};

} // namespace

std::wstring find_codex_executable()
{
    wchar_t buffer[32768] = {};
    const DWORD path_length = GetEnvironmentVariableW(L"PATH", buffer, 32768);
    std::error_code error;
    if ((path_length > 0) && (path_length < 32768))
    {
        const std::wstring_view paths(buffer, path_length);
        for (size_t start = 0; start < paths.size();)
        {
            const size_t end = paths.find(L';', start);
            auto entry = paths.substr(start, end == std::wstring_view::npos ? end : end - start);
            if ((entry.size() >= 2) && (entry.front() == L'"') && (entry.back() == L'"'))
                entry = entry.substr(1, entry.size() - 2);
            const std::filesystem::path directory(entry);
            const auto candidate = directory / L"codex.exe";
            // Relative and empty PATH entries would allow the working folder to supply executable code.
            if ((directory.is_absolute()) && (std::filesystem::is_regular_file(candidate, error)))
                return candidate.wstring();
            if (end == std::wstring_view::npos)
                break;
            start = end + 1;
        }
    }
    const DWORD local_length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if ((local_length == 0) || (local_length >= 32768))
        throw std::runtime_error("Codex is not installed");
    const std::filesystem::path local_directory(buffer);
    if (local_directory.is_absolute() == false)
        throw std::runtime_error("Codex is not installed");
    const auto directory = local_directory / L"OpenAI" / L"Codex" / L"bin";
    std::filesystem::path newest;
    auto modified = std::filesystem::file_time_type::min();
    std::filesystem::directory_iterator entries(directory, error), end;
    while ((error.value() == 0) && (entries != end))
    {
        const auto candidate = entries->path() / L"codex.exe";
        if (std::filesystem::is_regular_file(candidate, error))
        {
            const auto time = std::filesystem::last_write_time(candidate, error);
            if ((error.value() == 0) && (time > modified))
            {
                newest = candidate;
                modified = time;
            }
        }
        error.clear();
        entries.increment(error);
    }
    if (newest.empty())
        throw std::runtime_error("Codex is not installed");
    return newest.wstring();
}

CodexUsage parse_codex_usage(std::wstring_view response, ULONGLONG fetched_utc_ticks)
{
    const auto root = parse_object(response);
    const auto result = child(root.Get(), L"result");
    if (result == nullptr)
        throw std::runtime_error("Codex usage is unavailable");
    const auto buckets = child(result.Get(), L"rateLimitsByLimitId");
    const auto limits =
        buckets != nullptr ? child(buckets.Get(), L"codex") : child(result.Get(), L"rateLimits");
    if (limits == nullptr)
        throw std::runtime_error("Codex usage is unavailable");
    CodexUsage usage;
    usage.fetched_utc_ticks = fetched_utc_ticks;
    for (const wchar_t *name : {L"primary", L"secondary"})
    {
        const auto window = child(limits.Get(), name);
        if (window == nullptr)
            continue;
        const double used = number(window.Get(), L"usedPercent");
        const double duration = optional_number(window.Get(), L"windowDurationMins");
        const double reset = optional_number(window.Get(), L"resetsAt");
        if ((duration < 0) || (duration > INT_MAX) || (std::floor(duration) != duration) || (reset < 0) ||
            (reset > 253402300799.0) || (std::floor(reset) != reset))
            throw std::runtime_error("Codex returned an invalid quota window");
        usage.windows[usage.count++] = {static_cast<int>(std::floor(100 - std::clamp(used, 0.0, 100.0))),
                                        static_cast<int>(duration), static_cast<LONGLONG>(reset)};
    }
    return usage;
}

CodexUsage query_codex_usage(HANDLE stop_event)
{
    if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
        throw std::runtime_error("Codex usage query was cancelled");
    const std::wstring executable = find_codex_executable();
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    Handle input_read, input_write, output_read, output_write, errors, job, process, thread;
    if ((CreatePipe(&input_read.value, &input_write.value, &security, 0) == FALSE) ||
        (CreatePipe(&output_read.value, &output_write.value, &security, 0) == FALSE))
        throw_windows_error("Create Codex pipes");
    if ((SetHandleInformation(input_write.value, HANDLE_FLAG_INHERIT, 0) == FALSE) ||
        (SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0) == FALSE))
        throw_windows_error("Protect Codex pipes");
    errors.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, 0, nullptr);
    if (errors.value == INVALID_HANDLE_VALUE)
        throw_windows_error("Create Codex error sink");
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits = {};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if ((job.value == nullptr) || (SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                                           &job_limits, sizeof(job_limits)) == FALSE))
        throw_windows_error("Create Codex process job");
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attributes(attribute_size);
    auto attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size) == FALSE)
        throw_windows_error("Initialize Codex process attributes");
    struct Attributes
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes()
        {
            DeleteProcThreadAttributeList(value);
        }
    } cleanup{attribute_list};
    HANDLE inherited[] = {input_read.value, output_write.value, errors.value};
    if (UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                  sizeof(inherited), nullptr, nullptr) == FALSE)
        throw_windows_error("Set Codex process handles");
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_read.value;
    startup.StartupInfo.hStdOutput = output_write.value;
    startup.StartupInfo.hStdError = errors.value;
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION info = {};
    std::wstring command = L"\"" + executable + L"\" app-server --listen stdio://";
    if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                       &startup.StartupInfo, &info) == FALSE)
        throw_windows_error("Start Codex usage reader");
    process.value = info.hProcess;
    thread.value = info.hThread;
    if (AssignProcessToJobObject(job.value, process.value) == FALSE)
    {
        TerminateProcess(process.value, 1);
        throw_windows_error("Track Codex usage reader");
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
        throw_windows_error("Resume Codex usage reader");
    thread.close();
    input_read.close();
    output_write.close();
    send_json(input_write.value,
              "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"anker_clock\","
              "\"title\":\"Anker Clock\",\"version\":\"1.0.0\"}}}\n");
    std::string pending;
    size_t received = 0;
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while ((GetTickCount64() < deadline) && (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0))
    {
        DWORD available = 0;
        if (PeekNamedPipe(output_read.value, nullptr, 0, nullptr, &available, nullptr) == FALSE)
            break;
        if (available == 0)
        {
            if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0)
                break;
            WaitForSingleObject(stop_event, 25);
            continue;
        }
        char buffer[8192];
        DWORD read = 0;
        if (ReadFile(output_read.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) ==
            FALSE)
            break;
        received += read;
        if (received > 1024 * 1024)
            throw std::runtime_error("Codex usage response is too large");
        pending.append(buffer, read);
        for (size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n'))
        {
            const std::wstring line = widen(pending.substr(0, end));
            pending.erase(0, end + 1);
            const auto message = parse_object(line);
            if (named(message.Get(), L"id") == nullptr)
                continue;
            const double id = number(message.Get(), L"id");
            if (id == 1)
            {
                if (child(message.Get(), L"result") == nullptr)
                    throw std::runtime_error("Could not initialize Codex usage reader");
                send_json(input_write.value, "{\"method\":\"initialized\",\"params\":{}}\n"
                                             "{\"id\":2,\"method\":\"account/rateLimits/read\"}\n");
            }
            else if (id == 2)
            {
                FILETIME now = {};
                GetSystemTimeAsFileTime(&now);
                const ULONGLONG ticks =
                    (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
                const CodexUsage usage = parse_codex_usage(line, ticks);
                input_write.close();
                WaitForSingleObject(process.value, 500);
                return usage;
            }
        }
    }
    throw std::runtime_error("Codex usage query did not complete");
}

CodexUsage load_codex_usage()
{
    UsageCache cache;
    DWORD bytes = sizeof(cache);
    if ((RegGetValueW(HKEY_CURRENT_USER, cache_key, L"Snapshot", RRF_RT_REG_BINARY, nullptr, &cache,
                      &bytes) != ERROR_SUCCESS) ||
        (bytes != sizeof(cache)) || (cache.version != 1) || (cache.count > 2))
        return {};
    for (DWORD index = 0; index < cache.count; ++index)
        if ((cache.windows[index].remaining_percent < 0) || (cache.windows[index].remaining_percent > 100) ||
            (cache.windows[index].duration_minutes < 0) || (cache.windows[index].resets_at < 0))
            return {};
    return {cache.windows, cache.fetched_utc_ticks, static_cast<int>(cache.count), cache.refresh_failed != 0};
}

void save_codex_usage(const CodexUsage &usage)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, cache_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    const UsageCache cache{1, static_cast<DWORD>(usage.count), usage.fetched_utc_ticks, usage.windows,
                           usage.refresh_failed ? 1u : 0u};
    RegSetValueExW(key, L"Snapshot", 0, REG_BINARY, reinterpret_cast<const BYTE *>(&cache), sizeof(cache));
    RegCloseKey(key);
}

CodexUsageText format_codex_usage(const CodexUsage &usage, ULONGLONG now_utc_ticks, int refresh_seconds)
{
    if ((usage.count == 0) || (usage.refresh_failed) || (usage.fetched_utc_ticks == 0) ||
        (now_utc_ticks < usage.fetched_utc_ticks) ||
        ((now_utc_ticks - usage.fetched_utc_ticks) >= (static_cast<ULONGLONG>(refresh_seconds) * 20000000)))
        return {};
    CodexUsageText text;
    text.remaining_percent = 100;
    const LONGLONG unix_now = now_utc_ticks >= unix_epoch_ticks
                                  ? static_cast<LONGLONG>((now_utc_ticks - unix_epoch_ticks) / 10000000)
                                  : 0;
    for (int index = 0; index < usage.count; ++index)
    {
        text.remaining_percent = std::min(text.remaining_percent, usage.windows[index].remaining_percent);
        if ((usage.windows[index].resets_at > 0) && (unix_now >= usage.windows[index].resets_at))
            return {};
    }
    text.value = std::to_wstring(text.remaining_percent) + L"%";
    return text;
}
