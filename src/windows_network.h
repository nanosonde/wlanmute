#pragma once

#include <winsock2.h>
#include <windows.h>
#include <wlanapi.h>

#include <exception>

#include "network_policy.h"

namespace wlanmute {

class WindowsNetwork final : public WirelessActions {
public:
    WindowsNetwork() = default;
    ~WindowsNetwork() override;
    WindowsNetwork(const WindowsNetwork&) = delete;
    WindowsNetwork& operator=(const WindowsNetwork&) = delete;

    // require_wifi=false tolerates a missing WLAN AutoConfig service (used by the CI smoke test).
    Snapshot Read(bool require_wifi = true);
    void Disconnect(const InterfaceId& id, const std::wstring& expected_profile) override;
    void Connect(const InterfaceId& id, const std::wstring& profile) override;

private:
    void Open();
    HANDLE wlan_{};
};

std::wstring ErrorMessage(const std::exception& error);

}
