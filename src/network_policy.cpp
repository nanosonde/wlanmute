#include "network_policy.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace wlanmute {

std::optional<Subnet> Subnet::FromAddress(
    std::array<std::uint8_t, 16> bytes, std::uint8_t size, std::uint8_t prefix) {
    if ((size != 4 && size != 16) || prefix > size * 8)
        throw std::invalid_argument("Invalid network address or prefix length.");

    const bool all_zero = std::all_of(bytes.begin(), bytes.begin() + size,
        [](auto byte) { return byte == 0; });
    const bool ipv6_loopback = size == 16 && bytes[15] == 1 &&
        std::all_of(bytes.begin(), bytes.begin() + 15, [](auto byte) { return byte == 0; });
    // Link-local/APIPA addresses do not identify a shared routed LAN.
    if (prefix == 0 || all_zero || ipv6_loopback ||
        (size == 4 && (bytes[0] == 127 || bytes[0] >= 224 ||
                      (bytes[0] == 169 && bytes[1] == 254))) ||
        (size == 16 && (bytes[0] == 0xff ||
                       (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80))))
        return std::nullopt;

    for (int i = 0; i < size; ++i) {
        const int bits = std::clamp(static_cast<int>(prefix) - i * 8, 0, 8);
        bytes[static_cast<std::size_t>(i)] &= static_cast<std::uint8_t>(0xff << (8 - bits));
    }
    std::fill(bytes.begin() + size, bytes.end(), std::uint8_t{0});
    return Subnet{bytes, size, prefix};
}

NetworkPolicy::NetworkPolicy(WirelessActions& wireless, std::function<void(const std::wstring&)> report)
    : wireless_(wireless), report_(std::move(report)) {}

bool NetworkPolicy::HasMatchingEthernet(const Snapshot& snapshot, const std::vector<Subnet>& subnets) {
    return std::any_of(snapshot.ethernet.begin(), snapshot.ethernet.end(), [&](const auto& lan) {
        return lan.has_internet && std::any_of(lan.subnets.begin(), lan.subnets.end(), [&](const auto& subnet) {
            return std::find(subnets.begin(), subnets.end(), subnet) != subnets.end();
        });
    });
}

std::wstring NetworkPolicy::Update(const Snapshot& snapshot, bool enabled, Clock::time_point now) {
    for (const auto& wifi : snapshot.wifi) {
        auto owned = muted_.find(wifi.id);
        if (owned != muted_.end()) {
            auto& saved = owned->second;
            if (wifi.state == WifiState::Connected && wifi.profile != saved.profile) {
                // Do not overwrite a profile the user connected to manually.
                muted_.erase(owned);
                report_(L"Manual Wi-Fi profile change detected; cancelled pending restoration.");
                continue;
            }

            const bool prefer_ethernet = enabled && HasMatchingEthernet(snapshot, saved.subnets);
            if (prefer_ethernet) {
                if (wifi.state == WifiState::Connected) {
                    wireless_.Disconnect(wifi.id, saved.profile);
                    saved.reconnect_requested.reset();
                    report_(L"Disconnected Wi-Fi again: matching Ethernet has Internet access.");
                }
            } else if (wifi.state == WifiState::Connected) {
                muted_.erase(owned);
                report_(L"Wi-Fi connection restored.");
            } else if (wifi.state == WifiState::Disconnected &&
                       (!saved.reconnect_requested || now - *saved.reconnect_requested >= std::chrono::seconds(30))) {
                if (saved.reconnect_requested)
                    report_(L"Wi-Fi reconnection was not confirmed within 30 seconds; retrying.");
                wireless_.Connect(wifi.id, saved.profile);
                saved.reconnect_requested = now;
                report_(L"Requested reconnection of the saved Wi-Fi profile.");
            }
            continue;
        }

        if (enabled && wifi.state == WifiState::Connected && !wifi.profile.empty() &&
            HasMatchingEthernet(snapshot, wifi.subnets)) {
            // Allocate tracking state before disconnecting; roll it back if the API rejects the request.
            const auto matching_ethernet = std::find_if(snapshot.ethernet.begin(), snapshot.ethernet.end(),
                [&](const auto& ethernet) {
                    return ethernet.has_internet && std::any_of(ethernet.subnets.begin(), ethernet.subnets.end(),
                        [&](const auto& subnet) {
                            return std::find(wifi.subnets.begin(), wifi.subnets.end(), subnet) != wifi.subnets.end();
                        });
                });
            const auto ethernet_name = matching_ethernet == snapshot.ethernet.end() || matching_ethernet->name.empty()
                ? L"Ethernet" : matching_ethernet->name;
            const auto ethernet_network_name = matching_ethernet == snapshot.ethernet.end()
                ? L"" : matching_ethernet->network_name;
            muted_.emplace(wifi.id, MutedConnection{
                wifi.profile, wifi.subnets, wifi.name, wifi.network_name,
                ethernet_name, ethernet_network_name, std::nullopt});
            try {
                wireless_.Disconnect(wifi.id, wifi.profile);
            } catch (...) {
                muted_.erase(wifi.id);
                throw;
            }
            report_(L"Disconnected Wi-Fi: matching Ethernet has Internet access.");
        }
    }

    if (muted_.empty())
        return enabled ? L"Monitoring connections" : L"Paused";
    const bool restoring = !enabled || std::any_of(muted_.begin(), muted_.end(), [&](const auto& entry) {
        return !HasMatchingEthernet(snapshot, entry.second.subnets);
    });
    return restoring ? L"Restoring Wi-Fi" : L"Ethernet preferred / Wi-Fi managed";
}

std::vector<std::wstring> NetworkPolicy::InterfaceDetails(const Snapshot& snapshot) const {
    std::vector<std::wstring> details;
    const auto add_interface = [&](const std::wstring& type, const std::wstring& interface_name,
                                   const std::wstring& network_name) {
        const auto label = interface_name.empty() ? type : interface_name;
        const auto interface_detail = type + L": " + label;
        if (std::find(details.begin(), details.end(), interface_detail) == details.end())
            details.push_back(interface_detail);
        if (!network_name.empty()) {
            const auto network_detail = L"  Network: " + network_name;
            if (std::find(details.begin(), details.end(), network_detail) == details.end())
                details.push_back(network_detail);
        }
    };

    for (const auto& [id, saved] : muted_) {
        (void)id;
        for (const auto& ethernet : snapshot.ethernet) {
            if (!ethernet.has_internet ||
                !std::any_of(ethernet.subnets.begin(), ethernet.subnets.end(), [&](const auto& subnet) {
                    return std::find(saved.subnets.begin(), saved.subnets.end(), subnet) != saved.subnets.end();
                }))
                continue;

            add_interface(L"Ethernet", ethernet.name, ethernet.network_name);
            add_interface(L"Wi-Fi", saved.interface_name, saved.network_name);
        }
    }

    if (!details.empty())
        return details;

    for (const auto& [id, saved] : muted_) {
        (void)id;
        add_interface(L"Ethernet", saved.ethernet_name, saved.ethernet_network_name);
        add_interface(L"Wi-Fi", saved.interface_name, saved.network_name);
    }
    if (!details.empty())
        return details;

    for (const auto& ethernet : snapshot.ethernet) {
        if (!ethernet.has_internet)
            continue;
        add_interface(L"Ethernet", ethernet.name, ethernet.network_name);
    }
    for (const auto& wifi : snapshot.wifi) {
        if (wifi.state != WifiState::Connected)
            continue;
        add_interface(L"Wi-Fi", wifi.name, wifi.network_name);
    }
    return details;
}

}
