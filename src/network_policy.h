#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wlanmute {

using InterfaceId = std::array<std::uint8_t, 16>;
using Clock = std::chrono::steady_clock;

struct Subnet {
    std::array<std::uint8_t, 16> network{};
    std::uint8_t address_size{};
    std::uint8_t prefix_length{};
    bool operator==(const Subnet&) const = default;

    static std::optional<Subnet> FromAddress(
        std::array<std::uint8_t, 16> bytes, std::uint8_t size, std::uint8_t prefix);
};

enum class WifiState { Unavailable, Connected, Disconnected, Busy };

struct EthernetLink {
    InterfaceId id{};
    bool has_internet{};
    std::vector<Subnet> subnets;
    std::wstring name;
};

struct WifiLink {
    InterfaceId id{};
    WifiState state{WifiState::Unavailable};
    std::wstring profile;
    std::vector<Subnet> subnets;
    std::wstring name;
};

struct Snapshot {
    std::vector<EthernetLink> ethernet;
    std::vector<WifiLink> wifi;
};

class WirelessActions {
public:
    virtual ~WirelessActions() = default;
    virtual void Disconnect(const InterfaceId& id, const std::wstring& expected_profile) = 0;
    virtual void Connect(const InterfaceId& id, const std::wstring& profile) = 0;
};

class NetworkPolicy {
public:
    NetworkPolicy(WirelessActions& wireless, std::function<void(const std::wstring&)> report);
    std::wstring Update(const Snapshot& snapshot, bool enabled, Clock::time_point now);
    std::vector<std::wstring> InterfaceDetails(const Snapshot& snapshot) const;
    std::size_t MutedCount() const { return muted_.size(); }

private:
    struct MutedConnection {
        std::wstring profile;
        std::vector<Subnet> subnets;
        std::wstring interface_name;
        std::wstring ethernet_name;
        std::optional<Clock::time_point> reconnect_requested;
    };
    static bool HasMatchingEthernet(const Snapshot& snapshot, const std::vector<Subnet>& subnets);
    WirelessActions& wireless_;
    std::function<void(const std::wstring&)> report_;
    std::map<InterfaceId, MutedConnection> muted_;
};

}
