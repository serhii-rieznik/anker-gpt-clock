#include "hub.h"
#include <setupapi.h>
#include <devguid.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace
{
constexpr wchar_t transfer_key[] = L"Software\\AnkerClock";
constexpr wchar_t transfer_value[] = L"PendingFrame";
void put_be32(BYTE *target, uint32_t value)
{
    target[0] = static_cast<BYTE>(value >> 24);
    target[1] = static_cast<BYTE>(value >> 16);
    target[2] = static_cast<BYTE>(value >> 8);
    target[3] = static_cast<BYTE>(value);
}

uint32_t read_be32(const BYTE *source)
{
    return (static_cast<uint32_t>(source[0]) << 24) | (static_cast<uint32_t>(source[1]) << 16) |
           (static_cast<uint32_t>(source[2]) << 8) | source[3];
}

uint16_t read_be16(const BYTE *source)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(source[0]) << 8) | source[1]);
}

struct DeviceList
{
    HDEVINFO value;
    ~DeviceList()
    {
        if (value != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(value);
    }
};
} // namespace

std::wstring find_hub_port()
{
    DeviceList devices{SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT)};
    if (devices.value == INVALID_HANDLE_VALUE)
        return L"";
    SP_DEVINFO_DATA info = {};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.value, index, &info) != FALSE; ++index)
    {
        wchar_t hardware[1024] = {};
        if (SetupDiGetDeviceRegistryPropertyW(devices.value, &info, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<BYTE *>(hardware), sizeof(hardware),
                                              nullptr) == FALSE)
            continue;
        CharUpperBuffW(hardware, static_cast<DWORD>(wcslen(hardware)));
        const std::wstring id = hardware;
        if ((id.find(L"VID_291A&PID_010C") == std::wstring::npos) ||
            (id.find(L"MI_00") == std::wstring::npos))
            continue;
        const HKEY key = SetupDiOpenDevRegKey(devices.value, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        wchar_t name[64] = {};
        DWORD bytes = sizeof(name);
        const LSTATUS status = RegGetValueW(key, nullptr, L"PortName", RRF_RT_REG_SZ, nullptr, name, &bytes);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS)
            return name;
    }
    return L"";
}

HubConnection::HubConnection(HANDLE stop_event) : HubConnection(stop_event, nullptr) {}

HubConnection::HubConnection(HANDLE stop_event, HANDLE change_event)
    : _stop_event(stop_event), _change_event(change_event)
{
    _packet_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (_packet_timer == nullptr)
        throw_windows_error("Create packet timer");
}

HubConnection::~HubConnection()
{
    if (_port != INVALID_HANDLE_VALUE)
        CloseHandle(_port);
    if (_packet_timer != nullptr)
        CloseHandle(_packet_timer);
}

void HubConnection::connect()
{
    _port_name = find_hub_port();
    if (_port_name.empty())
        throw std::runtime_error("Connect your Anker hub to USB.");
    const std::wstring path = L"\\\\.\\" + _port_name;
    _port = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (_port == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_ACCESS_DENIED)
            throw std::runtime_error("Hub is in use. Close Dock Manager to connect.");
        throw_windows_error("Open Anker hub");
    }
    DCB state = {};
    state.DCBlength = sizeof(state);
    if (GetCommState(_port, &state) == FALSE)
        throw_windows_error("Read serial settings");
    state.BaudRate = 115200;
    state.ByteSize = 8;
    state.Parity = NOPARITY;
    state.StopBits = ONESTOPBIT;
    state.fBinary = TRUE;
    state.fParity = FALSE;
    state.fOutxCtsFlow = FALSE;
    state.fOutxDsrFlow = FALSE;
    state.fDtrControl = DTR_CONTROL_DISABLE;
    state.fRtsControl = RTS_CONTROL_DISABLE;
    state.fOutX = FALSE;
    state.fInX = FALSE;
    state.fAbortOnError = FALSE;
    if (SetCommState(_port, &state) == FALSE)
        throw_windows_error("Set serial settings");
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutConstant = 1500;
    if (SetCommTimeouts(_port, &timeouts) == FALSE)
        throw_windows_error("Set serial timeouts");
    PurgeComm(_port, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

void HubConnection::pause(DWORD milliseconds)
{
    const HANDLE handles[] = {_stop_event, _change_event};
    const DWORD result =
        WaitForMultipleObjects(_change_event != nullptr ? 2 : 1, handles, FALSE, milliseconds);
    if (result == WAIT_FAILED)
        throw_windows_error("Wait for hub operation");
    if (result != WAIT_TIMEOUT)
        throw std::runtime_error("Clock update interrupted");
}

void HubConnection::pace_packet()
{
    LARGE_INTEGER due;
    due.QuadPart = -20000;
    if (SetWaitableTimer(_packet_timer, &due, 0, nullptr, nullptr, FALSE) == FALSE)
        throw_windows_error("Set packet timer");
    const HANDLE handles[] = {_stop_event, _packet_timer, _change_event};
    const DWORD result = WaitForMultipleObjects(_change_event != nullptr ? 3 : 2, handles, FALSE, INFINITE);
    if ((result == WAIT_OBJECT_0) || (result == (WAIT_OBJECT_0 + 2)))
        throw std::runtime_error("Clock update interrupted");
    if (result != (WAIT_OBJECT_0 + 1))
        throw_windows_error("Wait for packet timer");
}

void HubConnection::wait_until(ULONGLONG utc_ticks)
{
    if (utc_ticks == 0)
        return;
    FILETIME now = {};
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER ticks;
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    if (ticks.QuadPart >= utc_ticks)
        return;
    // A clock or settings change interrupts this wait and reschedules the staged frame.
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(utc_ticks - ticks.QuadPart);
    if (SetWaitableTimer(_packet_timer, &due, 0, nullptr, nullptr, FALSE) == FALSE)
        throw_windows_error("Set image selection timer");
    const HANDLE handles[] = {_stop_event, _packet_timer, _change_event};
    const DWORD result = WaitForMultipleObjects(_change_event != nullptr ? 3 : 2, handles, FALSE, INFINITE);
    if ((result == WAIT_OBJECT_0) || (result == (WAIT_OBJECT_0 + 2)))
        throw std::runtime_error("Clock update interrupted");
    if (result != (WAIT_OBJECT_0 + 1))
        throw_windows_error("Wait for image selection timer");
}

void HubConnection::write(const BYTE *data, DWORD count)
{
    pause(0);
    DWORD written = 0;
    if (WriteFile(_port, data, count, &written, nullptr) == FALSE)
        throw_windows_error("Write to hub");
    if (written != count)
        throw std::runtime_error("The hub did not accept a complete packet");
    if (FlushFileBuffers(_port) == FALSE)
        throw_windows_error("Drain hub packet");
}

void HubConnection::read_bytes(BYTE *data, size_t count, ULONGLONG deadline)
{
    size_t received = 0;
    while (received < count)
    {
        pause(0);
        if (GetTickCount64() >= deadline)
            throw std::runtime_error("The hub did not respond. Retrying on the next update.");
        DWORD bytes = 0;
        if (ReadFile(_port, data + received, static_cast<DWORD>(count - received), &bytes, nullptr) == FALSE)
            throw_windows_error("Read from hub");
        received += bytes;
    }
}

void HubConnection::receive(BYTE first, BYTE command, BYTE *result, size_t count, DWORD timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    int previous = -1;
    for (;;)
    {
        BYTE value = 0;
        read_bytes(&value, 1, deadline);
        if ((previous == first) && (value == command))
        {
            result[0] = first;
            result[1] = command;
            read_bytes(result + 2, count - 2, deadline);
            return;
        }
        previous = value;
    }
}

std::array<BYTE, 8> HubConnection::gallery()
{
    const BYTE request[] = {2, 28};
    write(request, sizeof(request));
    std::array<BYTE, 8> result = {};
    receive(10, 28, result.data(), result.size(), 3000);
    return result;
}

std::array<BYTE, 5> HubConnection::current_image()
{
    const BYTE request[] = {2, 24};
    write(request, sizeof(request));
    std::array<BYTE, 5> result = {};
    receive(10, 24, result.data(), result.size(), 3000);
    return result;
}

void HubConnection::initialize(bool exclusive_gallery)
{
    const BYTE carousel[] = {2, 27, 0};
    write(carousel, sizeof(carousel));
    BYTE state[3] = {};
    receive(10, 27, state, sizeof(state), 3000);
    if (state[2] != 0)
        throw std::runtime_error("The hub did not disable its photo carousel");
    const BYTE screensaver[] = {2, 31, 1};
    write(screensaver, sizeof(screensaver));
    pause(20);
    const bool recovered = recover_pending_frame();
    const auto existing = gallery();
    if (existing[3] > 8)
        throw std::runtime_error("The hub returned an invalid gallery count");
    if ((existing[3] > 1) && exclusive_gallery)
    {
        const BYTE clear[] = {2, 41};
        bool erased = false;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            write(clear, sizeof(clear));
            BYTE response[4] = {};
            receive(10, 41, response, sizeof(response), 10000);
            if (response[2] == 0)
            {
                erased = true;
                break;
            }
            if (response[2] != 2)
                throw std::runtime_error("The hub could not prepare its image storage");
            pause(400);
        }
        if (erased == false)
            throw std::runtime_error("The hub is busy preparing its image storage");
        if (gallery()[3] != 0)
            throw std::runtime_error("The hub's image storage was not cleared");
        _has_current_image = false;
        _gallery_count = 1;
        clear_pending_frame();
    }
    else if (recovered == false)
    {
        _has_current_image = existing[3] > 0;
        _gallery_count = std::max(1, static_cast<int>(existing[3]));
        if (_has_current_image)
            _current_crc = read_be16(current_image().data() + 3);
    }
}

bool HubConnection::select_image(uint16_t crc)
{
    const BYTE request[] = {2, 25, static_cast<BYTE>(crc >> 8), static_cast<BYTE>(crc)};
    write(request, sizeof(request));
    const auto selected = current_image();
    return (selected[2] < 8) && (read_be16(selected.data() + 3) == crc) &&
           ((gallery()[5] & (1 << selected[2])) != 0);
}

bool HubConnection::crc_in_use(uint16_t crc)
{
    if (gallery()[3] == 0)
        return false;
    const auto before = current_image();
    const uint16_t visible_crc = read_be16(before.data() + 3);
    const bool exists = select_image(crc);
    if (exists && (visible_crc != crc))
    {
        if (select_image(visible_crc) == false)
            throw std::runtime_error(
                "The hub could not restore the selected image after checking its identifier");
    }
    else if ((exists == false) && (read_be16(current_image().data() + 3) != visible_crc))
        throw std::runtime_error("The hub changed its selected image during an identifier check");
    return exists;
}

void HubConnection::save_pending_frame(uint16_t crc)
{
    const BYTE record[] = {'A',
                           'C',
                           1,
                           static_cast<BYTE>(_has_current_image),
                           static_cast<BYTE>(crc >> 8),
                           static_cast<BYTE>(crc),
                           static_cast<BYTE>(_current_crc >> 8),
                           static_cast<BYTE>(_current_crc),
                           static_cast<BYTE>(_gallery_count)};
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, transfer_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                     &key, nullptr);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not record the pending clock update");
    status = RegSetValueExW(key, transfer_value, 0, REG_BINARY, record, sizeof(record));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not record the pending clock update");
}

void HubConnection::clear_pending_frame()
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, transfer_key, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND)
        return;
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Could not complete the clock update record");
    const LSTATUS removed = RegDeleteValueW(key, transfer_value);
    RegCloseKey(key);
    if ((removed != ERROR_SUCCESS) && (removed != ERROR_FILE_NOT_FOUND))
        throw std::runtime_error("Could not complete the clock update record");
}

bool HubConnection::recover_pending_frame()
{
    BYTE record[9] = {};
    DWORD bytes = sizeof(record);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, transfer_key, transfer_value, RRF_RT_REG_BINARY,
                                        nullptr, record, &bytes);
    if (status == ERROR_FILE_NOT_FOUND)
        return false;
    if ((status != ERROR_SUCCESS) || (bytes != sizeof(record)) || (record[0] != 'A') || (record[1] != 'C') ||
        (record[2] != 1) || (record[3] > 1) || (record[8] == 0) || (record[8] > 8))
        throw std::runtime_error("The pending clock update record is invalid");
    const uint16_t pending_crc = read_be16(record + 4);
    const uint16_t previous_crc = read_be16(record + 6);
    _gallery_count = record[8];
    if (select_image(pending_crc))
    {
        _current_crc = pending_crc;
        _has_current_image = true;
        if (record[3] != 0)
            delete_image(previous_crc);
        if (gallery()[3] != _gallery_count)
            throw std::runtime_error("The hub could not finish its interrupted clock update");
        clear_pending_frame();
    }
    else
    {
        _has_current_image = (record[3] != 0) && select_image(previous_crc);
        _current_crc = previous_crc;
        delete_image(pending_crc);
        if (_has_current_image)
            clear_pending_frame();
        // Keep the record when a full gallery lost its replaced slot, so retry fills that slot.
    }
    return true;
}

void HubConnection::delete_image(uint16_t crc)
{
    const BYTE request[] = {2, 26, static_cast<BYTE>(crc >> 8), static_cast<BYTE>(crc)};
    write(request, sizeof(request));
    BYTE response[3] = {};
    receive(10, 26, response, sizeof(response), 5000);
    if ((response[2] != 0) && (response[2] != 1))
        throw std::runtime_error("The hub could not remove the preceding clock image");
}

void HubConnection::set_brightness(int brightness)
{
    const BYTE request[] = {2, 18, static_cast<BYTE>(brightness)};
    write(request, sizeof(request));
    const BYTE query[] = {2, 22};
    write(query, sizeof(query));
    BYTE response[7] = {};
    receive(10, 22, response, sizeof(response), 3000);
    if (response[2] != brightness)
        throw std::runtime_error("The hub did not accept the brightness setting");
}

FrameResult HubConnection::replace_image(std::vector<BYTE> image)
{
    return replace_image(std::move(image), 0);
}

FrameResult HubConnection::replace_image(std::vector<BYTE> image, ULONGLONG select_at_utc_ticks)
{
    if ((image.empty()) || (image.size() > 65515))
        throw std::runtime_error("Clock image exceeds the hub's size limit");
    const ULONGLONG start = GetTickCount64();
    uint16_t crc = image_crc(image);
    if (crc_in_use(crc))
    {
        distinguish_crc(image, crc);
        for (int attempt = 0;; ++attempt)
        {
            crc = image_crc(image);
            if (crc_in_use(crc) == false)
                break;
            if (attempt == 8)
                throw std::runtime_error("Could not choose a free clock image identifier");
            ++image[image.size() - 3];
        }
    }
    const uint32_t packets = static_cast<uint32_t>((image.size() + 54) / 55);
    BYTE header[] = {2,   32, 161, 1, 255, 162, 5, 3, 0, 0, 0, 0, 163, 2, 1, 0,
                     164, 3,  2,   0, 55,  165, 5, 3, 0, 0, 0, 0, 166, 2, 0, 0};
    put_be32(header + 8, static_cast<uint32_t>(image.size()));
    put_be32(header + 24, packets);
    header[30] = static_cast<BYTE>(crc >> 8);
    header[31] = static_cast<BYTE>(crc);
    save_pending_frame(crc);
    if ((_gallery_count == 8) && _has_current_image)
        delete_image(_current_crc);
    pause(300);
    BYTE ready[3] = {};
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (attempt > 0)
            pause(400);
        write(header, sizeof(header));
        receive(10, 32, ready, sizeof(ready), 5000);
        if (ready[2] != 1)
            break;
    }
    if (ready[2] != 0)
        throw std::runtime_error("The hub is not ready to receive the clock image");
    BYTE packet[61] = {2, 33};
    for (uint32_t sequence = 0; sequence < packets; ++sequence)
    {
        const size_t offset = static_cast<size_t>(sequence) * 55;
        const size_t count = std::min<size_t>(55, image.size() - offset);
        put_be32(packet + 2, sequence);
        std::memcpy(packet + 6, image.data() + offset, count);
        write(packet, static_cast<DWORD>(count + 6));
        pace_packet();
    }
    BYTE finished[13] = {};
    receive(10, 33, finished, sizeof(finished), 5000);
    if (finished[2] != 16)
        throw std::runtime_error("The hub could not store this clock image. Retrying on the next update.");
    pause(300);
    const ULONGLONG ready_ms = GetTickCount64() - start;
    wait_until(select_at_utc_ticks);
    FILETIME selected_time = {};
    GetSystemTimePreciseAsFileTime(&selected_time);
    if (select_image(crc) == false)
        throw std::runtime_error("The hub did not select the new clock image");
    const uint16_t previous = _current_crc;
    const bool had_previous = _has_current_image;
    _current_crc = crc;
    _has_current_image = true;
    if (had_previous)
        delete_image(previous);
    if (gallery()[3] != _gallery_count)
        throw std::runtime_error("The hub's clock image storage is inconsistent");
    clear_pending_frame();
    ULARGE_INTEGER selected;
    selected.LowPart = selected_time.dwLowDateTime;
    selected.HighPart = selected_time.dwHighDateTime;
    return {image.size(), crc, GetTickCount64() - start, ready_ms, selected.QuadPart};
}

std::vector<BYTE> HubConnection::read_image()
{
    BYTE request[64] = {3, 101};
    write(request, sizeof(request));
    BYTE ready[3] = {};
    receive(11, 101, ready, sizeof(ready), 5000);
    if (ready[2] != 0)
        throw std::runtime_error("The hub's image reader is busy");
    request[0] = 2;
    write(request, sizeof(request));
    BYTE info[11] = {};
    receive(10, 101, info, sizeof(info), 5000);
    if (info[2] != 0)
        throw std::runtime_error("The hub's image information is unavailable");
    const uint32_t size = read_be32(info + 3);
    const uint32_t packets = read_be32(info + 7);
    if ((size == 0) || (size > 65520) || (packets != ((size + 56) / 57)))
        throw std::runtime_error("The hub returned an invalid image size");
    std::vector<BYTE> image(size);
    request[2] = 1;
    for (uint32_t sequence = 0; sequence < packets; ++sequence)
    {
        put_be32(request + 3, sequence);
        write(request, sizeof(request));
        BYTE response[7] = {};
        receive(10, 101, response, sizeof(response), 5000);
        if ((response[2] != 0) || (read_be32(response + 3) != sequence))
            throw std::runtime_error("The hub's image reader is busy or returned an invalid packet");
        const size_t offset = static_cast<size_t>(sequence) * 57;
        const size_t count = std::min<size_t>(57, image.size() - offset);
        read_bytes(image.data() + offset, count, GetTickCount64() + 5000);
    }
    return image;
}

const std::wstring &HubConnection::port_name() const
{
    return _port_name;
}
