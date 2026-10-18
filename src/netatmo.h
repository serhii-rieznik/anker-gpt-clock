#pragma once
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

constexpr int netatmo_temperature = 1;
constexpr int netatmo_humidity = 2;
constexpr int netatmo_co2 = 4;
constexpr int netatmo_noise = 8;

struct NetatmoCredentials
{
    std::wstring client_id, client_secret, access_token, refresh_token;
};

struct NetatmoModule
{
    std::wstring id, name;
    ULONGLONG measured_utc_ticks = 0;
    std::array<double, 4> values = {};
    int available_readings = 0;
    int supported_readings = 0;
    bool reachable = false;
};

struct NetatmoData
{
    std::vector<NetatmoModule> modules;
    std::wstring response, status;
    ULONGLONG fetched_utc_ticks = 0;
};

enum class NetatmoAvailability
{
    ready,
    no_cache,
    cache_expired,
    invalid_fetch_time,
    module_missing,
    module_offline,
    no_measurement,
    measurement_expired,
    invalid_measurement_time
};

struct NetatmoSelection
{
    const NetatmoModule *module;
    NetatmoAvailability availability;
    bool stale() const;
};

struct Settings;

NetatmoCredentials parse_netatmo_credentials(std::wstring_view text);
void save_netatmo_credentials(const NetatmoCredentials &credentials, const wchar_t *key_path);
NetatmoCredentials load_netatmo_credentials(const wchar_t *key_path);
void import_netatmo_credentials(const std::wstring &path);
NetatmoData parse_netatmo_data(std::wstring_view response, ULONGLONG fetched_utc_ticks);
NetatmoData load_netatmo_data();
void save_netatmo_data(const NetatmoData &data);
std::wstring netatmo_module_options(const NetatmoData &data);
NetatmoSelection netatmo_selection(const NetatmoData &data, const std::wstring &id, ULONGLONG now_utc_ticks,
                                   int refresh_seconds);
std::wstring netatmo_diagnostics(const NetatmoData &data, const Settings &settings, ULONGLONG now_utc_ticks);
const NetatmoModule *selected_netatmo_module(const NetatmoData &data, const std::wstring &id,
                                             ULONGLONG now_utc_ticks, int refresh_seconds);

class NetatmoClient
{
  public:
    NetatmoData query(HANDLE stop_event);
    void cancel();

  private:
    std::atomic<HINTERNET> _request = nullptr;
    std::wstring request(const wchar_t *path, const wchar_t *method, const std::string &body,
                         const std::wstring &access_token, HANDLE stop_event);
};
