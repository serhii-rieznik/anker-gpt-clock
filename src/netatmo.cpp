#include "netatmo.h"
#include "settings.h"
#include <windows.data.json.h>
#include <wincrypt.h>
#include <roapi.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::Data::Json;

namespace
{
constexpr wchar_t netatmo_key[] = L"Software\\AnkerClock\\Netatmo";
constexpr ULONGLONG unix_epoch_ticks = 116444736000000000;
constexpr size_t maximum_response = 1024 * 1024;

struct RegistryKey
{
    HKEY value = nullptr;
    ~RegistryKey()
    {
        if (value != nullptr)
            RegCloseKey(value);
    }
};

struct InternetHandle
{
    HINTERNET value = nullptr;
    ~InternetHandle()
    {
        if (value != nullptr)
            WinHttpCloseHandle(value);
    }
};

struct HttpError : std::runtime_error
{
    DWORD status;
    static std::string message(DWORD value, bool renewal)
    {
        std::string result =
            std::string(renewal ? "Netatmo sign-in renewal failed (HTTP " : "Netatmo request failed (HTTP ") +
            std::to_string(value) + ")";
        if (value >= 500)
            result += ". Netatmo service error; automatic refresh will try again.";
        else if (value == 429)
            result += ". Netatmo rate limit reached; increase the refresh interval.";
        else if (renewal && ((value == 400) || (value == 401) || (value == 403)))
            result += ". Reconnect by importing a newly generated access/refresh token pair.";
        return result;
    }
    HttpError(DWORD value, bool renewal) : std::runtime_error(message(value, renewal)), status(value) {}
};

ComPtr<IJsonObject> parse_object(std::wstring_view text)
{
    Microsoft::WRL::Wrappers::RoInitializeWrapper runtime(RO_INIT_MULTITHREADED);
    if (FAILED(runtime) && (runtime != RPC_E_CHANGED_MODE))
        throw std::runtime_error("Could not initialize the Netatmo reader");
    ComPtr<IJsonObjectStatics> factory;
    ComPtr<IJsonObject> result;
    HString input;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonObject).Get(),
                                      IID_PPV_ARGS(&factory))) ||
        FAILED(input.Set(text.data(), static_cast<UINT32>(text.size()))) ||
        FAILED(factory->Parse(input.Get(), &result)))
        throw std::runtime_error("Netatmo data is not valid JSON");
    return result;
}

ComPtr<IJsonValue> named(IJsonObject *object, const wchar_t *key)
{
    ComPtr<IJsonValue> value;
    if (object != nullptr)
        object->GetNamedValue(HStringReference(key).Get(), &value);
    return value;
}

ComPtr<IJsonObject> child(IJsonObject *object, const wchar_t *key)
{
    ComPtr<IJsonObject> result;
    const auto value = named(object, key);
    if (value != nullptr)
        value->GetObject(&result);
    return result;
}

ComPtr<IJsonArray> array(IJsonObject *object, const wchar_t *key)
{
    ComPtr<IJsonArray> result;
    const auto value = named(object, key);
    if (value != nullptr)
        value->GetArray(&result);
    return result;
}

UINT32 array_size(IJsonArray *values)
{
    if (values == nullptr)
        return 0;
    ComPtr<ABI::Windows::Foundation::Collections::IVector<IJsonValue *>> items;
    UINT32 size = 0;
    if (FAILED(values->QueryInterface(IID_PPV_ARGS(&items))) || FAILED(items->get_Size(&size)))
        throw std::runtime_error("Netatmo returned invalid array data");
    return size;
}

std::wstring text(IJsonObject *object, const wchar_t *key)
{
    HString result;
    const auto value = named(object, key);
    if ((value == nullptr) || FAILED(value->GetString(result.GetAddressOf())))
        return {};
    UINT32 size = 0;
    const wchar_t *characters = WindowsGetStringRawBuffer(result.Get(), &size);
    return {characters, size};
}

bool number(IJsonObject *object, const wchar_t *key, double &result)
{
    const auto value = named(object, key);
    return (value != nullptr) && SUCCEEDED(value->GetNumber(&result)) && std::isfinite(result);
}

std::wstring trim(std::wstring_view value)
{
    const size_t start = value.find_first_not_of(L" \t\r\n\ufeff");
    if (start == std::wstring_view::npos)
        return {};
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return std::wstring(value.substr(start, end - start + 1));
}

std::wstring quote(const std::wstring &value)
{
    std::wstring result = L"\"";
    for (wchar_t character : value)
    {
        if ((character == L'"') || (character == L'\\'))
            result += L'\\';
        if (character < 32)
            throw std::runtime_error("Netatmo credentials contain invalid characters");
        result += character;
    }
    return result + L'"';
}

void validate_credentials(const NetatmoCredentials &credentials)
{
    for (const auto *value : {&credentials.client_id, &credentials.client_secret, &credentials.access_token,
                              &credentials.refresh_token})
        if (value->empty() || (value->size() > 4096) ||
            (value->find_first_of(L"\r\n\t") != std::wstring::npos))
            throw std::runtime_error(
                "Import client_id, client_secret, access_token and refresh_token from Netatmo");
}

std::wstring credential_json(const NetatmoCredentials &credentials)
{
    validate_credentials(credentials);
    return L"{\"client_id\":" + quote(credentials.client_id) + L",\"client_secret\":" +
           quote(credentials.client_secret) + L",\"access_token\":" + quote(credentials.access_token) +
           L",\"refresh_token\":" + quote(credentials.refresh_token) + L"}";
}

std::wstring registry_text(HKEY key, const wchar_t *name)
{
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return {};
    if (size > (maximum_response * sizeof(wchar_t)))
        return {};
    std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1, 0);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
        return {};
    return buffer.data();
}

void write_text(HKEY key, const wchar_t *name, const std::wstring &value)
{
    if (RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
        throw std::runtime_error("Could not save Netatmo data");
}

std::string form_value(const std::wstring &value)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr,
                        nullptr);
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char character : utf8)
    {
        if (((character >= 'a') && (character <= 'z')) || ((character >= 'A') && (character <= 'Z')) ||
            ((character >= '0') && (character <= '9')) || (character == '-') || (character == '_') ||
            (character == '.') || (character == '~'))
            encoded += static_cast<char>(character);
        else
        {
            encoded += '%';
            encoded += hex[character >> 4];
            encoded += hex[character & 15];
        }
    }
    return encoded;
}

NetatmoModule read_module(IJsonObject *object)
{
    NetatmoModule module;
    module.id = text(object, L"_id");
    module.name = text(object, L"module_name");
    if (module.name.empty())
        module.name = L"Station";
    const auto reachable = named(object, L"reachable");
    boolean connected = false;
    module.reachable = (reachable != nullptr) && SUCCEEDED(reachable->GetBoolean(&connected)) && connected;
    const auto dashboard = child(object, L"dashboard_data");
    double stamp = 0;
    if (number(dashboard.Get(), L"time_utc", stamp) && (stamp > 0) && (stamp < 32503680000.0))
        module.measured_utc_ticks = unix_epoch_ticks + static_cast<ULONGLONG>(stamp * 10000000);
    const wchar_t *keys[] = {L"Temperature", L"Humidity", L"CO2", L"Noise"};
    for (int index = 0; index < 4; ++index)
        if (number(dashboard.Get(), keys[index], module.values[index]))
            module.available_readings |= 1 << index;
    const auto types = array(object, L"data_type");
    const UINT32 count = array_size(types.Get());
    for (UINT32 index = 0; index < count; ++index)
    {
        HString type;
        if (FAILED(types->GetStringAt(index, type.GetAddressOf())))
            continue;
        for (int reading = 0; reading < 4; ++reading)
            if (wcscmp(WindowsGetStringRawBuffer(type.Get(), nullptr), keys[reading]) == 0)
                module.supported_readings |= 1 << reading;
    }
    module.supported_readings |= module.available_readings;
    if (module.id.empty() || (module.id.size() >= 128))
        throw std::runtime_error("Netatmo returned an invalid module ID");
    return module;
}
} // namespace

NetatmoCredentials parse_netatmo_credentials(std::wstring_view input)
{
    const std::wstring source = trim(input);
    NetatmoCredentials credentials;
    if (source.starts_with(L"{"))
    {
        const auto object = parse_object(source);
        credentials = {text(object.Get(), L"client_id"), text(object.Get(), L"client_secret"),
                       text(object.Get(), L"access_token"), text(object.Get(), L"refresh_token")};
    }
    else
    {
        for (size_t start = 0; start < source.size();)
        {
            const size_t end = source.find(L'\n', start);
            const auto line =
                std::wstring_view(source).substr(start, end == std::wstring::npos ? end : end - start);
            const size_t separator = line.find_first_of(L":=");
            if (separator != std::wstring_view::npos)
            {
                const auto key = trim(line.substr(0, separator));
                auto value = trim(line.substr(separator + 1));
                if ((value.size() >= 2) && (((value.front() == L'"') && (value.back() == L'"')) ||
                                            ((value.front() == L'\'') && (value.back() == L'\''))))
                    value = value.substr(1, value.size() - 2);
                if (key == L"client_id")
                    credentials.client_id = value;
                else if (key == L"client_secret")
                    credentials.client_secret = value;
                else if (key == L"access_token")
                    credentials.access_token = value;
                else if (key == L"refresh_token")
                    credentials.refresh_token = value;
            }
            if (end == std::wstring::npos)
                break;
            start = end + 1;
        }
    }
    validate_credentials(credentials);
    return credentials;
}

void save_netatmo_credentials(const NetatmoCredentials &credentials, const wchar_t *key_path)
{
    const auto json = credential_json(credentials);
    DATA_BLOB plain = {static_cast<DWORD>(json.size() * sizeof(wchar_t)),
                       reinterpret_cast<BYTE *>(const_cast<wchar_t *>(json.data()))};
    DATA_BLOB encrypted = {};
    if (CryptProtectData(&plain, L"Anker Clock Netatmo", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                         &encrypted) == FALSE)
        throw std::runtime_error("Windows could not protect the Netatmo credentials");
    RegistryKey key;
    const LSTATUS opened = RegCreateKeyExW(HKEY_CURRENT_USER, key_path, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                           &key.value, nullptr);
    const LSTATUS saved = opened == ERROR_SUCCESS ? RegSetValueExW(key.value, L"Credentials", 0, REG_BINARY,
                                                                   encrypted.pbData, encrypted.cbData)
                                                  : opened;
    LocalFree(encrypted.pbData);
    if (saved != ERROR_SUCCESS)
        throw std::runtime_error("Could not save the protected Netatmo credentials");
}

NetatmoCredentials load_netatmo_credentials(const wchar_t *key_path)
{
    DWORD size = 0;
    if ((RegGetValueW(HKEY_CURRENT_USER, key_path, L"Credentials", RRF_RT_REG_BINARY, nullptr, nullptr,
                      &size) != ERROR_SUCCESS) ||
        (size > 65536))
        throw std::runtime_error("Import your Netatmo credentials first");
    std::vector<BYTE> bytes(size);
    if (RegGetValueW(HKEY_CURRENT_USER, key_path, L"Credentials", RRF_RT_REG_BINARY, nullptr, bytes.data(),
                     &size) != ERROR_SUCCESS)
        throw std::runtime_error("Could not read the Netatmo credentials");
    DATA_BLOB encrypted = {size, bytes.data()}, plain = {};
    if (CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                           &plain) == FALSE)
        throw std::runtime_error("Windows could not unlock the Netatmo credentials");
    try
    {
        const auto credentials = parse_netatmo_credentials(std::wstring_view(
            reinterpret_cast<const wchar_t *>(plain.pbData), plain.cbData / sizeof(wchar_t)));
        SecureZeroMemory(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
        return credentials;
    }
    catch (...)
    {
        SecureZeroMemory(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
        throw;
    }
}

void import_netatmo_credentials(const std::wstring &path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (file.is_open() == false)
        throw std::runtime_error("Could not open the Netatmo credentials file");
    const auto size = file.tellg();
    if ((size <= 0) || (size > 65536))
        throw std::runtime_error("Netatmo credentials file has an invalid size");
    std::string contents(static_cast<size_t>(size), '\0');
    file.seekg(0);
    if (file.read(contents.data(), size).good() == false)
        throw std::runtime_error("Could not read the Netatmo credentials file");
    save_netatmo_credentials(parse_netatmo_credentials(widen(contents)), netatmo_key);
}

NetatmoData parse_netatmo_data(std::wstring_view response, ULONGLONG fetched_utc_ticks)
{
    const auto object = parse_object(response);
    const auto body = child(object.Get(), L"body");
    const auto devices = array(body.Get(), L"devices");
    if (devices == nullptr)
        throw std::runtime_error("Netatmo returned no station data");
    NetatmoData data;
    data.response = response;
    data.fetched_utc_ticks = fetched_utc_ticks;
    data.status = L"Connected to Netatmo";
    const UINT32 count = array_size(devices.Get());
    for (UINT32 index = 0; index < count; ++index)
    {
        ComPtr<IJsonObject> device;
        if (FAILED(devices->GetObjectAt(index, &device)))
            throw std::runtime_error("Netatmo returned invalid station data");
        data.modules.push_back(read_module(device.Get()));
        const auto modules = array(device.Get(), L"modules");
        const UINT32 module_count = array_size(modules.Get());
        for (UINT32 module_index = 0; module_index < module_count; ++module_index)
        {
            ComPtr<IJsonObject> module;
            if (FAILED(modules->GetObjectAt(module_index, &module)))
                throw std::runtime_error("Netatmo returned invalid module data");
            data.modules.push_back(read_module(module.Get()));
        }
    }
    return data;
}

NetatmoData load_netatmo_data()
{
    NetatmoData data;
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, netatmo_key, 0, KEY_QUERY_VALUE, &key.value) != ERROR_SUCCESS)
        return data;
    const auto response = registry_text(key.value, L"Data");
    ULONGLONG stamp = 0;
    DWORD size = sizeof(stamp);
    RegGetValueW(key.value, nullptr, L"FetchedAt", RRF_RT_REG_QWORD, nullptr, &stamp, &size);
    if (response.empty() == false)
    {
        try
        {
            data = parse_netatmo_data(response, stamp);
        }
        catch (const std::exception &)
        {
        }
    }
    data.status = registry_text(key.value, L"Status");
    return data;
}

void save_netatmo_data(const NetatmoData &data)
{
    RegistryKey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, netatmo_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Could not save Netatmo data");
    write_text(key.value, L"Data", data.response);
    if (RegSetValueExW(key.value, L"FetchedAt", 0, REG_QWORD,
                       reinterpret_cast<const BYTE *>(&data.fetched_utc_ticks),
                       sizeof(data.fetched_utc_ticks)) != ERROR_SUCCESS)
        throw std::runtime_error("Could not save the Netatmo timestamp");
    write_text(key.value, L"Status", data.status);
}

std::wstring netatmo_module_options(const NetatmoData &data)
{
    std::wstring result = data.status;
    for (const auto &module : data.modules)
    {
        auto name = module.name;
        for (wchar_t &character : name)
            if ((character == L'\t') || (character == L'\n') || (character == L'\r'))
                character = L' ';
        result += L"\n" + module.id + L"\t" + name + L"\t" + std::to_wstring(module.supported_readings) +
                  L"\t" + std::to_wstring(module.measured_utc_ticks) + L"\t" +
                  (module.reachable ? L"1" : L"0");
    }
    return result;
}

NetatmoSelection netatmo_selection(const NetatmoData &data, const std::wstring &id, ULONGLONG now_utc_ticks,
                                   int refresh_seconds)
{
    const auto found =
        std::find_if(data.modules.begin(), data.modules.end(),
                     [&](const NetatmoModule &module) { return id.empty() || (module.id == id); });
    const auto module = found == data.modules.end() ? nullptr : &*found;
    const ULONGLONG maximum_age = static_cast<ULONGLONG>(std::max(1200, refresh_seconds * 2)) * 10000000;
    if (data.fetched_utc_ticks == 0)
        return {module, NetatmoAvailability::no_cache};
    if (module == nullptr)
        return {nullptr, NetatmoAvailability::module_missing};
    if (module->reachable == false)
        return {module, NetatmoAvailability::module_offline};
    if (module->measured_utc_ticks == 0)
        return {module, NetatmoAvailability::no_measurement};
    if (now_utc_ticks < module->measured_utc_ticks)
        return {module, NetatmoAvailability::invalid_measurement_time};
    if (now_utc_ticks < data.fetched_utc_ticks)
        return {module, NetatmoAvailability::invalid_fetch_time};
    if ((now_utc_ticks - data.fetched_utc_ticks) >= maximum_age)
        return {module, NetatmoAvailability::cache_expired};
    if ((now_utc_ticks - module->measured_utc_ticks) >= std::max(maximum_age, 36000000000ULL))
        return {module, NetatmoAvailability::measurement_expired};
    return {module, NetatmoAvailability::ready};
}

bool NetatmoSelection::stale() const
{
    return (availability == NetatmoAvailability::cache_expired) ||
           (availability == NetatmoAvailability::measurement_expired);
}

const NetatmoModule *selected_netatmo_module(const NetatmoData &data, const std::wstring &id,
                                             ULONGLONG now_utc_ticks, int refresh_seconds)
{
    const auto selection = netatmo_selection(data, id, now_utc_ticks, refresh_seconds);
    return ((selection.availability == NetatmoAvailability::ready) || selection.stale()) ? selection.module
                                                                                         : nullptr;
}

std::wstring netatmo_diagnostics(const NetatmoData &data, const Settings &settings, ULONGLONG now_utc_ticks)
{
    const auto selection =
        netatmo_selection(data, settings.netatmo_module_id, now_utc_ticks, settings.netatmo_refresh_seconds);
    const int cache_minutes = std::max(1200, settings.netatmo_refresh_seconds * 2) / 60;
    std::wstring reason, action;
    if (settings.layout != 2)
    {
        reason = L"Weather hidden: this clock face does not show readings.";
        action = L"Switch to Analog in Clock settings.";
    }
    else if (settings.show_netatmo == false)
    {
        reason = L"Weather hidden: readings are turned off.";
        action = L"Enable Show readings beside the analog clock.";
    }
    else
    {
        switch (selection.availability)
        {
        case NetatmoAvailability::no_cache:
            reason = L"Weather hidden: no successful weather fetch yet.";
            action = L"Import credentials if needed, then refresh readings.";
            break;
        case NetatmoAvailability::cache_expired:
            reason = L"Weather stale: showing the last readings with clock badges.";
            action = L"The last successful fetch is too old. Refresh readings. Fetches become stale after " +
                     std::to_wstring(cache_minutes) + L" minutes.";
            break;
        case NetatmoAvailability::invalid_fetch_time:
        case NetatmoAvailability::invalid_measurement_time:
            reason = L"Weather hidden: a timestamp is ahead of the computer's clock.";
            action = L"Check Windows date and time, then refresh readings.";
            break;
        case NetatmoAvailability::module_missing:
            reason = L"Weather hidden: the selected module was not returned by Netatmo.";
            action = L"Refresh readings or choose another module.";
            break;
        case NetatmoAvailability::module_offline:
            reason = L"Weather hidden: Netatmo reports this module offline.";
            action = L"Check its power, batteries and connection in the Netatmo app.";
            break;
        case NetatmoAvailability::no_measurement:
            reason = L"Weather hidden: this module has no timestamped measurements.";
            action = L"Check the module in the Netatmo app and wait for its next measurement.";
            break;
        case NetatmoAvailability::measurement_expired:
            reason = L"Weather stale: showing the last readings with clock badges.";
            action = L"Netatmo is returning old measurements. Check the station's power and Wi-Fi in the "
                     L"Netatmo app. "
                     L"Measurements become stale after " +
                     std::to_wstring(std::max(cache_minutes, 60)) + L" minutes.";
            break;
        case NetatmoAvailability::ready: {
            const int requested = settings.netatmo_readings & selection.module->supported_readings;
            const int available = requested & selection.module->available_readings;
            if (available == 0)
            {
                reason = L"Weather hidden: none of the selected readings are available.";
                action = L"Choose a reading supported by this module, or check it in the Netatmo app.";
            }
            else if (available != requested)
            {
                reason = L"Weather available: some selected readings are missing.";
                action = L"Netatmo did not return every selected reading. Available values are still shown.";
            }
            else
                reason = L"Weather available for display.";
            break;
        }
        }
    }
    const auto timestamp = [&](ULONGLONG ticks) -> std::wstring {
        if (ticks == 0)
            return L"Never";
        const FILETIME utc = {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        FILETIME local = {};
        SYSTEMTIME time = {};
        if ((FileTimeToLocalFileTime(&utc, &local) == FALSE) ||
            (FileTimeToSystemTime(&local, &time) == FALSE))
            return L"Invalid timestamp";
        wchar_t formatted[64];
        swprintf_s(formatted, L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay,
                   time.wHour, time.wMinute, time.wSecond);
        std::wstring result = formatted;
        if (now_utc_ticks >= ticks)
            result += L" (" + std::to_wstring((now_utc_ticks - ticks) / 600000000) + L" min ago)";
        return result;
    };
    std::wstring result =
        reason + L"\n" + action + L"\nLast successful fetch: " + timestamp(data.fetched_utc_ticks);
    if (selection.module != nullptr)
        result += L"\n" + selection.module->name + L" measurement: " +
                  timestamp(selection.module->measured_utc_ticks);
    return result;
}

void NetatmoClient::cancel()
{
    const auto request = _request.exchange(nullptr);
    if (request != nullptr)
        WinHttpCloseHandle(request);
}

std::wstring NetatmoClient::request(const wchar_t *path, const wchar_t *method, const std::string &body,
                                    const std::wstring &access_token, HANDLE stop_event)
{
    if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
        throw std::runtime_error("Netatmo request cancelled");
    InternetHandle session{WinHttpOpen(L"AnkerClock/1.1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.value == nullptr)
        throw std::runtime_error("Could not open a Netatmo connection");
    WinHttpSetTimeouts(session.value, 3000, 5000, 5000, 5000);
    InternetHandle connection{
        WinHttpConnect(session.value, L"api.netatmo.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (connection.value == nullptr)
        throw std::runtime_error("Could not connect to Netatmo");
    const auto request_handle =
        WinHttpOpenRequest(connection.value, method, path, nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request_handle == nullptr)
        throw std::runtime_error("Could not create a Netatmo request");
    _request.store(request_handle);
    struct Cleanup
    {
        NetatmoClient &client;
        ~Cleanup()
        {
            client.cancel();
        }
    } cleanup{*this};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request_handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    if (access_token.empty() == false)
        headers += L"Authorization: Bearer " + access_token + L"\r\n";
    if ((WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) ||
        (WinHttpSendRequest(request_handle, headers.c_str(), static_cast<DWORD>(headers.size()),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) == FALSE) ||
        (WinHttpReceiveResponse(request_handle, nullptr) == FALSE))
        throw std::runtime_error("Netatmo connection failed (Windows error " +
                                 std::to_string(GetLastError()) +
                                 "). Check internet access, VPN or proxy settings.");
    DWORD status = 0, size = sizeof(status);
    if (WinHttpQueryHeaders(request_handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) == FALSE)
        throw std::runtime_error("Could not read the Netatmo response");
    if (status != 200)
        throw HttpError(status, wcscmp(path, L"/oauth2/token") == 0);
    std::string response;
    char buffer[4096];
    for (;;)
    {
        DWORD read = 0;
        if ((WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) ||
            (WinHttpReadData(request_handle, buffer, sizeof(buffer), &read) == FALSE))
            throw std::runtime_error("Netatmo response interrupted");
        if (read == 0)
            break;
        if ((response.size() + read) > maximum_response)
            throw std::runtime_error("Netatmo response is too large");
        response.append(buffer, read);
    }
    return widen(response);
}

NetatmoData NetatmoClient::query(HANDLE stop_event)
{
    auto credentials = load_netatmo_credentials(netatmo_key);
    std::wstring response;
    try
    {
        response = request(L"/api/getstationsdata", L"GET", {}, credentials.access_token, stop_event);
    }
    catch (const HttpError &error)
    {
        if ((error.status != 401) && (error.status != 403))
            throw;
        const std::string form = "grant_type=refresh_token&client_id=" + form_value(credentials.client_id) +
                                 "&client_secret=" + form_value(credentials.client_secret) +
                                 "&refresh_token=" + form_value(credentials.refresh_token);
        const auto tokens = parse_object(request(L"/oauth2/token", L"POST", form, {}, stop_event));
        credentials.access_token = text(tokens.Get(), L"access_token");
        credentials.refresh_token = text(tokens.Get(), L"refresh_token");
        // Persist rotated refresh tokens before the next request or a subsequent restart.
        save_netatmo_credentials(credentials, netatmo_key);
        response = request(L"/api/getstationsdata", L"GET", {}, credentials.access_token, stop_event);
    }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG stamp = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return parse_netatmo_data(response, stamp);
}
