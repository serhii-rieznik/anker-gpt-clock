#pragma once
#include "renderer.h"
#include <array>
#include <string>

std::wstring find_hub_port();

struct FrameResult
{
    size_t bytes = 0;
    uint16_t crc = 0;
    ULONGLONG elapsed_ms = 0;
    ULONGLONG ready_ms = 0;
    ULONGLONG selected_utc_ticks = 0;
};

class HubConnection
{
  public:
    explicit HubConnection(HANDLE stop_event);
    HubConnection(HANDLE stop_event, HANDLE change_event);
    ~HubConnection();
    HubConnection(const HubConnection &) = delete;
    HubConnection &operator=(const HubConnection &) = delete;
    void connect();
    void initialize(bool exclusive_gallery);
    void set_brightness(int brightness);
    FrameResult replace_image(std::vector<BYTE> image);
    FrameResult replace_image(std::vector<BYTE> image, ULONGLONG select_at_utc_ticks);
    std::array<BYTE, 8> gallery();
    std::array<BYTE, 5> current_image();
    std::vector<BYTE> read_image();
    const std::wstring &port_name() const;

  private:
    HANDLE _stop_event;
    HANDLE _change_event;
    HANDLE _port = INVALID_HANDLE_VALUE;
    HANDLE _packet_timer = nullptr;
    std::wstring _port_name;
    uint16_t _current_crc = 0;
    bool _has_current_image = false;
    int _gallery_count = 0;
    void write(const BYTE *data, DWORD count);
    void receive(BYTE first, BYTE command, BYTE *result, size_t count, DWORD timeout_ms);
    void read_bytes(BYTE *data, size_t count, ULONGLONG deadline);
    void pause(DWORD milliseconds);
    void pace_packet();
    void wait_until(ULONGLONG utc_ticks);
    void delete_image(uint16_t crc);
    bool select_image(uint16_t crc);
    bool crc_in_use(uint16_t crc);
    void save_pending_frame(uint16_t crc);
    void clear_pending_frame();
    bool recover_pending_frame();
};
