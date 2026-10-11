#include "hub.h"
#include "ui_contract.h"
#include "lock_session.h"
#include <shellapi.h>
#include <wtsapi32.h>
#include <dbt.h>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <cmath>

namespace
{
constexpr wchar_t preferences_key[] = L"Software\\AnkerClock";
constexpr wchar_t controller_class[] = L"AnkerClock.Controller";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT status_message = WM_APP + 2;
constexpr UINT open_message = WM_APP + 3;
constexpr int open_command = 4001;
constexpr int update_command = 4002;
constexpr int pause_command = 4003;
constexpr int quit_command = 4004;

struct SharedState
{
    std::mutex mutex;
    Settings settings;
    std::wstring status = L"Connecting to your Anker hub…";
    bool connected = false;
    bool paused = false;
    LockSession lock_session;
    CodexUsage usage;
    bool update_requested = false;
};

class App
{
  public:
    explicit App(HINSTANCE instance) : _instance(instance)
    {
        _state.settings = load_settings(preferences_key);
        _state.usage = load_codex_usage();
        _stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        _wake_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        _usage_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if ((_stop_event == nullptr) || (_wake_event == nullptr) || (_usage_wake_event == nullptr))
            throw_windows_error("Create update events");
        _icon = static_cast<HICON>(LoadImageW(_instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_SHARED));
        _taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    }

    ~App()
    {
        stop();
        if (IsWindow(_settings_window) != FALSE)
            PostMessageW(_settings_window, WM_CLOSE, 0, 0);
        if (_settings_process != nullptr)
            CloseHandle(_settings_process);
        if (_tray_added)
        {
            NOTIFYICONDATAW icon = tray_data();
            Shell_NotifyIconW(NIM_DELETE, &icon);
        }
        if (_controller != nullptr)
        {
            WTSUnRegisterSessionNotification(_controller);
            DestroyWindow(_controller);
        }
        if (_stop_event != nullptr)
            CloseHandle(_stop_event);
        if (_wake_event != nullptr)
            CloseHandle(_wake_event);
        if (_usage_wake_event != nullptr)
            CloseHandle(_usage_wake_event);
    }

    void start(bool startup, bool paused)
    {
        _state.paused = paused;
        WNDCLASSEXW controller = {sizeof(controller)};
        controller.lpfnWndProc = controller_proc;
        controller.hInstance = _instance;
        controller.lpszClassName = controller_class;
        RegisterClassExW(&controller);
        _controller = CreateWindowExW(0, controller_class, L"Anker Clock controller", WS_OVERLAPPED, 0, 0, 0,
                                      0, nullptr, nullptr, _instance, this);
        if (_controller == nullptr)
            throw_windows_error("Create tray controller");
        if (WTSRegisterSessionNotification(_controller, NOTIFY_FOR_THIS_SESSION) == FALSE)
            throw_windows_error("Register Windows lock notifications");
        wchar_t *session = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx,
                                        &session, &bytes) != FALSE)
        {
            const auto info = reinterpret_cast<const WTSINFOEXW *>(session);
            if ((bytes >= sizeof(WTSINFOEXW)) && (info->Level == 1))
            {
                SYSTEMTIME utc = {};
                GetSystemTime(&utc);
                _state.lock_session.observe(info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK,
                                            false, utc_ticks(utc), GetTickCount64());
            }
            WTSFreeMemory(session);
        }
        add_tray();
        _worker = std::thread([this] { run_worker(); });
        _usage_worker = std::thread([this] { run_usage_worker(); });
        if ((startup == false) && _state.settings.show_settings_on_launch)
            show_settings();
    }

    int run()
    {
        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

  private:
    HINSTANCE _instance;
    HWND _controller = nullptr;
    HWND _settings_window = nullptr;
    HANDLE _stop_event = nullptr;
    HANDLE _wake_event = nullptr;
    HANDLE _usage_wake_event = nullptr;
    HICON _icon = nullptr;
    UINT _taskbar_created = 0;
    bool _tray_added = false;
    std::thread _worker;
    std::thread _usage_worker;
    SharedState _state;
    HANDLE _settings_process = nullptr;
    DWORD _settings_process_id = 0;

    void stop()
    {
        if (_stop_event != nullptr)
            SetEvent(_stop_event);
        if (_worker.joinable())
        {
            CancelSynchronousIo(_worker.native_handle());
            _worker.join();
        }
        if (_usage_worker.joinable())
            _usage_worker.join();
    }

    NOTIFYICONDATAW tray_data() const
    {
        NOTIFYICONDATAW icon = {};
        icon.cbSize = sizeof(icon);
        icon.hWnd = _controller;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        icon.uCallbackMessage = tray_message;
        icon.hIcon = _icon;
        return icon;
    }

    void add_tray()
    {
        NOTIFYICONDATAW icon = tray_data();
        wcscpy_s(icon.szTip, L"Anker Clock");
        _tray_added = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
        update_status();
    }

    void publish(const std::wstring &status, bool connected)
    {
        {
            std::lock_guard lock(_state.mutex);
            _state.status = status;
            _state.connected = connected;
        }
        PostMessageW(_controller, status_message, 0, 0);
    }

    void run_worker()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        THREAD_POWER_THROTTLING_STATE qos = {};
        qos.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        qos.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        qos.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &qos, sizeof(qos));
        std::unique_ptr<HubConnection> hub;
        std::unique_ptr<ClockRenderer> renderer;
        Settings rendered_settings;
        LockSession rendered_lock;
        ULONGLONG rendered_lock_minutes = 0;
        bool configured = false;
        int brightness = 0;
        std::vector<BYTE> last_image;
        ULONGLONG pending_target = 0;
        ULONGLONG pending_lock_minutes = 0;
        bool scheduled_update = false;
        DWORD preparation_ms = 2000;
        const HANDLE events[] = {_stop_event, _wake_event};
        while (WaitForSingleObject(_stop_event, 0) != WAIT_OBJECT_0)
        {
            ULONGLONG scheduled_target = 0;
            ULONGLONG scheduled_lock_minutes = 0;
            Settings settings;
            bool paused;
            LockSession lock_session;
            CodexUsage usage;
            bool forced;
            {
                std::lock_guard lock(_state.mutex);
                if (scheduled_update && (WaitForSingleObject(_wake_event, 0) != WAIT_OBJECT_0))
                {
                    scheduled_target = pending_target;
                    scheduled_lock_minutes = pending_lock_minutes;
                }
                // Reset with the snapshot so changes cancel transfers without losing wake notifications.
                ResetEvent(_wake_event);
                settings = _state.settings;
                paused = _state.paused;
                lock_session = _state.lock_session;
                usage = _state.usage;
                forced = _state.update_requested;
                _state.update_requested = false;
            }
            pending_target = 0;
            SYSTEM_POWER_STATUS power = {};
            const bool battery = settings.pause_on_battery && (GetSystemPowerStatus(&power) != FALSE) &&
                                 (power.ACLineStatus == 0);
            DWORD delay = 10000;
            try
            {
                const bool suspended = paused || battery;
                if (suspended && (forced == false))
                {
                    hub.reset();
                    publish(paused ? L"Updates paused" : L"Paused while on battery", false);
                    delay = INFINITE;
                }
                else
                {
                    if (renderer == nullptr)
                        renderer = std::make_unique<ClockRenderer>();
                    const bool settings_changed = (configured == false) || (settings != rendered_settings);
                    const bool lock_changed = (lock_session.locked != rendered_lock.locked) ||
                                              (lock_session.started_ms != rendered_lock.started_ms);
                    const bool lock_elapsed_changed =
                        lock_session.locked &&
                        (lock_session.elapsed_minutes(GetTickCount64()) > rendered_lock_minutes);
                    if (settings_changed)
                    {
                        renderer->configure(settings);
                        rendered_settings = settings;
                        configured = true;
                    }
                    const bool reconnecting = hub == nullptr;
                    if (reconnecting)
                    {
                        auto connection = std::make_unique<HubConnection>(_stop_event, _wake_event);
                        connection->connect();
                        connection->initialize(settings.exclusive_gallery);
                        hub = std::move(connection);
                        brightness = 0;
                        last_image.clear();
                    }
                    if (brightness != settings.hub_brightness)
                    {
                        hub->set_brightness(settings.hub_brightness);
                        brightness = settings.hub_brightness;
                    }
                    SYSTEMTIME utc = {};
                    GetSystemTime(&utc);
                    if ((scheduled_target == 0) && (forced == false) && (settings_changed == false) &&
                        (reconnecting == false) && (lock_changed == false) && (lock_elapsed_changed == false))
                    {
                        if (lock_session.locked == false)
                            publish(L"Connected · waiting for next interval", true);
                    }
                    else
                    {
                        const ULONGLONG now = utc_ticks(utc);
                        const ULONGLONG interval =
                            static_cast<ULONGLONG>(lock_session.locked ? 60 : settings.interval_seconds) *
                            10000000;
                        const bool timed = (forced == false) && (settings_changed == false) &&
                                           (reconnecting == false) && (lock_changed == false) &&
                                           (lock_elapsed_changed == false) && (scheduled_target != 0) &&
                                           ((scheduled_target + interval) > now);
                        const ULONGLONG target = timed ? scheduled_target : now;
                        const SYSTEMTIME time = renderer->time_at(
                            utc_from_ticks(lock_session.locked ? lock_session.locked_utc_ticks : target));
                        const ULONGLONG elapsed_minutes =
                            timed ? scheduled_lock_minutes : lock_session.elapsed_minutes(GetTickCount64());
                        const ULONGLONG render_start = GetTickCount64();
                        auto bitmap = lock_session.locked
                                          ? renderer->render_locked(time, elapsed_minutes,
                                                                    lock_session.lock_time_known, usage)
                                          : renderer->render(time, usage);
                        auto image = renderer->encode(*bitmap);
                        const ULONGLONG render_ms = GetTickCount64() - render_start;
                        if (image != last_image)
                        {
                            const FrameResult result =
                                hub->replace_image(image, timed ? target - 5000000 : 0);
                            preparation_ms = static_cast<DWORD>(result.ready_ms + render_ms);
                            last_image = std::move(image);
                            wchar_t stamp[32] = {};
                            swprintf_s(stamp, L"%02d:%02d:%02d", time.wHour, time.wMinute, time.wSecond);
                            std::wstring status;
                            if (lock_session.locked)
                                status = (suspended ? L"Paused · " : L"") +
                                         (lock_session.lock_time_known
                                              ? L"Locked at " + std::wstring(stamp) + L" · "
                                              : std::wstring(L"Windows locked · observed for ")) +
                                         format_locked_duration(elapsed_minutes);
                            else
                                status = (suspended ? L"Paused · updated " : L"Connected · updated ") +
                                         std::wstring(stamp);
                            if (timed)
                            {
                                const LONGLONG ahead = static_cast<LONGLONG>(target) -
                                                       static_cast<LONGLONG>(result.selected_utc_ticks);
                                wchar_t timing[80] = {};
                                swprintf_s(timing, L"\nSent %.2f s %s boundary", std::abs(ahead / 10000000.0),
                                           ahead >= 0 ? L"before" : L"after");
                                status += timing;
                            }
                            publish(status, suspended == false);
                        }
                        rendered_lock = lock_session;
                        rendered_lock_minutes = elapsed_minutes;
                    }
                    GetSystemTime(&utc);
                    if (lock_session.locked)
                    {
                        const ULONGLONG monotonic_ms = GetTickCount64();
                        const LockUpdateSchedule next =
                            next_lock_update(lock_session, monotonic_ms, preparation_ms + 750);
                        pending_target = utc_ticks(utc) + ((next.target_ms - monotonic_ms) * 10000);
                        pending_lock_minutes = next.elapsed_minutes;
                        delay = next.delay_ms;
                    }
                    else
                    {
                        const UpdateSchedule next =
                            next_update_schedule(utc, settings.interval_seconds, preparation_ms + 750);
                        pending_target = next.target_utc_ticks;
                        delay = next.delay_ms;
                    }
                    if (suspended)
                    {
                        hub.reset();
                        pending_target = 0;
                        delay = INFINITE;
                    }
                }
            }
            catch (const std::exception &error)
            {
                hub.reset();
                if (WaitForSingleObject(_stop_event, 0) == WAIT_OBJECT_0)
                    break;
                if (WaitForSingleObject(_wake_event, 0) != WAIT_OBJECT_0)
                    publish(widen(error.what()), false);
            }
            const DWORD wake = WaitForMultipleObjects(2, events, FALSE, delay);
            if (wake == WAIT_OBJECT_0)
                break;
            scheduled_update = (wake == WAIT_TIMEOUT) && (pending_target != 0);
        }
        hub.reset();
        renderer.reset();
        CoUninitialize();
    }

    void run_usage_worker()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HANDLE events[] = {_stop_event, _usage_wake_event};
        SYSTEMTIME utc = {};
        GetSystemTime(&utc);
        ULONGLONG next_query = utc_ticks(utc) + 10000000;
        int refresh_seconds = 0;
        while (WaitForSingleObject(_stop_event, 0) != WAIT_OBJECT_0)
        {
            Settings settings;
            bool paused;
            bool locked;
            CodexUsage usage;
            {
                std::lock_guard lock(_state.mutex);
                settings = _state.settings;
                paused = _state.paused;
                locked = _state.lock_session.locked;
                usage = _state.usage;
            }
            SYSTEM_POWER_STATUS power = {};
            const bool battery = settings.pause_on_battery && (GetSystemPowerStatus(&power) != FALSE) &&
                                 (power.ACLineStatus == 0);
            const bool enabled = settings.show_codex_usage && ((settings.layout == 2) || locked) &&
                                 (paused == false) && (battery == false);
            DWORD delay = INFINITE;
            if (enabled)
            {
                GetSystemTime(&utc);
                if ((refresh_seconds != 0) && (refresh_seconds != settings.codex_refresh_seconds))
                    next_query =
                        next_update_schedule(utc, settings.codex_refresh_seconds, 2500).target_utc_ticks -
                        25000000;
                refresh_seconds = settings.codex_refresh_seconds;
                const ULONGLONG now = utc_ticks(utc);
                if (next_query > (now + (static_cast<ULONGLONG>(refresh_seconds) * 10000000)))
                    next_query = next_update_schedule(utc, refresh_seconds, 2500).target_utc_ticks - 25000000;
                if (now >= next_query)
                {
                    try
                    {
                        usage = query_codex_usage(_stop_event);
                    }
                    catch (const std::exception &)
                    {
                        usage.refresh_failed = true;
                    }
                    if (WaitForSingleObject(_stop_event, 0) == WAIT_OBJECT_0)
                        break;
                    save_codex_usage(usage);
                    {
                        std::lock_guard lock(_state.mutex);
                        _state.usage = usage;
                    }
                    GetSystemTime(&utc);
                    next_query =
                        next_update_schedule(utc, settings.codex_refresh_seconds, 2500).target_utc_ticks -
                        25000000;
                }
                GetSystemTime(&utc);
                const ULONGLONG current = utc_ticks(utc);
                delay = next_query > current ? static_cast<DWORD>((next_query - current + 9999) / 10000) : 0;
            }
            if (WaitForMultipleObjects(2, events, FALSE, delay) == WAIT_OBJECT_0)
                break;
        }
        CoUninitialize();
    }

    void update_status()
    {
        std::wstring status;
        bool paused;
        {
            std::lock_guard lock(_state.mutex);
            status = _state.status;
            paused = _state.paused;
        }
        NOTIFYICONDATAW icon = tray_data();
        const std::wstring tooltip = L"Anker Clock\n" + status;
        wcsncpy_s(icon.szTip, tooltip.c_str(), _TRUNCATE);
        if (_tray_added)
            Shell_NotifyIconW(NIM_MODIFY, &icon);
        SetWindowTextW(_controller, status.c_str());
    }

    void tray_menu()
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, open_command, L"Clock settings…");
        AppendMenuW(menu, MF_STRING, update_command, L"Update now");
        bool paused;
        {
            std::lock_guard lock(_state.mutex);
            paused = _state.paused;
        }
        AppendMenuW(menu, MF_STRING | (paused ? MF_CHECKED : 0), pause_command, L"Pause updates");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, quit_command, L"Quit Anker Clock");
        POINT cursor = {};
        GetCursorPos(&cursor);
        SetForegroundWindow(_controller);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0,
                                            _controller, nullptr);
        DestroyMenu(menu);
        if (command != 0)
            PostMessageW(_controller, WM_COMMAND, command, 0);
        PostMessageW(_controller, WM_NULL, 0, 0);
    }

    void toggle_pause()
    {
        {
            std::lock_guard lock(_state.mutex);
            _state.paused = _state.paused == false;
        }
        SetEvent(_wake_event);
        SetEvent(_usage_wake_event);
        update_status();
    }

    void request_update()
    {
        {
            std::lock_guard lock(_state.mutex);
            _state.update_requested = true;
        }
        SetEvent(_wake_event);
    }

    static App *get_app(HWND window, UINT message, LPARAM parameter)
    {
        if (message == WM_NCCREATE)
        {
            const auto create = reinterpret_cast<CREATESTRUCTW *>(parameter);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        return reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    static LRESULT CALLBACK controller_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        App *app = get_app(window, message, lparam);
        if (app == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);
        try
        {
            if (message == app->_taskbar_created)
            {
                app->add_tray();
                return 0;
            }
            switch (message)
            {
            case tray_message:
                if (LOWORD(lparam) == WM_CONTEXTMENU)
                    app->tray_menu();
                else if ((LOWORD(lparam) == NIN_SELECT) || (LOWORD(lparam) == NIN_KEYSELECT) ||
                         (LOWORD(lparam) == WM_LBUTTONDBLCLK))
                    app->show_settings();
                return 0;
            case status_message:
                app->update_status();
                return 0;
            case open_message:
                app->show_settings();
                return 0;
            case settings_query_message: {
                std::lock_guard lock(app->_state.mutex);
                return (app->_state.paused ? 1 : 0) | (app->_state.connected ? 2 : 0);
            }
            case settings_register_message: {
                const HWND sender = reinterpret_cast<HWND>(wparam);
                DWORD process = 0;
                GetWindowThreadProcessId(sender, &process);
                if ((process == 0) || (process != app->_settings_process_id))
                    return 0;
                app->_settings_window = sender;
                return 1;
            }
            case WM_COPYDATA: {
                const auto data = reinterpret_cast<const COPYDATASTRUCT *>(lparam);
                if ((app->_settings_window == nullptr) ||
                    (reinterpret_cast<HWND>(wparam) != app->_settings_window) || (data == nullptr) ||
                    (data->lpData == nullptr) || (data->dwData != settings_apply_data) ||
                    (data->cbData != sizeof(SettingsWire)))
                    return 0;
                SettingsWire wire = {};
                memcpy(&wire, data->lpData, sizeof(wire));
                const Settings settings = settings_from_wire(wire);
                ClockRenderer validation;
                validation.configure(settings);
                set_start_with_windows(wire.start_windows != 0, L"AnkerClock", executable_path());
                save_settings(settings, preferences_key);
                {
                    std::lock_guard lock(app->_state.mutex);
                    app->_state.settings = settings;
                }
                SetEvent(app->_wake_event);
                SetEvent(app->_usage_wake_event);
                return 1;
            }
            case WM_COMMAND:
                switch (LOWORD(wparam))
                {
                case open_command:
                    app->show_settings();
                    break;
                case update_command:
                    app->request_update();
                    break;
                case pause_command:
                    app->toggle_pause();
                    break;
                case quit_command:
                    PostQuitMessage(0);
                    break;
                }
                return 0;
            case WM_WTSSESSION_CHANGE:
                if ((wparam == WTS_SESSION_LOCK) || (wparam == WTS_SESSION_UNLOCK))
                {
                    {
                        std::lock_guard lock(app->_state.mutex);
                        const bool locked = wparam == WTS_SESSION_LOCK;
                        if (app->_state.lock_session.locked == locked)
                            return 0;
                        SYSTEMTIME utc = {};
                        GetSystemTime(&utc);
                        app->_state.lock_session.observe(locked, true, utc_ticks(utc), GetTickCount64());
                        SetEvent(app->_wake_event);
                        SetEvent(app->_usage_wake_event);
                    }
                }
                return 0;
            case WM_TIMECHANGE:
            case WM_DEVICECHANGE:
            case WM_POWERBROADCAST: {
                std::lock_guard lock(app->_state.mutex);
                SetEvent(app->_wake_event);
                SetEvent(app->_usage_wake_event);
                return TRUE;
            }
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wparam != FALSE)
                    PostQuitMessage(0);
                return 0;
            }
        }
        catch (const std::exception &error)
        {
            MessageBoxW(app->_settings_window, widen(error.what()).c_str(), L"Anker Clock",
                        MB_OK | MB_ICONERROR);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void show_settings()
    {
        DWORD exit_code = 0;
        if ((_settings_process != nullptr) && (GetExitCodeProcess(_settings_process, &exit_code) != FALSE) &&
            (exit_code == STILL_ACTIVE))
        {
            if (IsWindow(_settings_window) != FALSE)
            {
                ShowWindow(_settings_window, SW_RESTORE);
                SetForegroundWindow(_settings_window);
            }
            return;
        }
        if (_settings_process != nullptr)
        {
            CloseHandle(_settings_process);
            _settings_process = nullptr;
        }
        _settings_window = nullptr;
        const std::wstring path = executable_path();
        const std::wstring directory = path.substr(0, path.find_last_of(L"\\"));
        const std::wstring ui = directory + L"\\AnkerClock.Settings.exe";
        std::wstring command = L"\"" + ui + L"\"";
        STARTUPINFOW startup = {sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(ui.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(),
                           &startup, &process) == FALSE)
            throw_windows_error("Open clock settings");
        CloseHandle(process.hThread);
        _settings_process = process.hProcess;
        _settings_process_id = process.dwProcessId;
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\AnkerClock.Singleton");
    const bool already_running = GetLastError() == ERROR_ALREADY_EXISTS;
    int argc = 0;
    wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool startup = false;
    bool paused = false;
    for (int index = 1; index < argc; ++index)
    {
        startup = startup || (wcscmp(args[index], L"--startup") == 0);
        paused = paused || (wcscmp(args[index], L"--paused") == 0);
    }
    LocalFree(args);
    if (already_running)
    {
        if (startup == false)
            PostMessageW(FindWindowW(controller_class, nullptr), open_message, 0, 0);
        CloseHandle(mutex);
        return 0;
    }
    int result = 0;
    try
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        GraphicsRuntime graphics;
        {
            App app(instance);
            app.start(startup, paused);
            result = app.run();
        }
        CoUninitialize();
    }
    catch (const std::exception &error)
    {
        MessageBoxW(nullptr, widen(error.what()).c_str(), L"Anker Clock", MB_OK | MB_ICONERROR);
        result = 1;
    }
    if (mutex != nullptr)
        CloseHandle(mutex);
    return result;
}
