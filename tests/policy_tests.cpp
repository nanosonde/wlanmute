#include "network_policy.h"

#include <iostream>
#include <stdexcept>

namespace {

using namespace wlanmute;
const InterfaceId WifiId{1};
const InterfaceId LanId{2};
const auto Now = Clock::time_point{};

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

Subnet V4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint8_t prefix = 24) {
    return Subnet::FromAddress({a, b, c, d}, 4, prefix).value();
}

const Subnet Home = V4(192, 168, 1, 50);

WifiLink Wifi(WifiState state) {
    return {WifiId, state, state == WifiState::Connected ? L"Home" : L"",
        state == WifiState::Connected ? std::vector<Subnet>{Home} : std::vector<Subnet>{}, L"", L""};
}

Snapshot State(bool internet, WifiState state) {
    return {{{LanId, internet, {Home}, L"", L""}}, {Wifi(state)}};
}

class FakeWireless final : public WirelessActions {
public:
    std::vector<std::wstring> calls;
    bool fail = false;
    void Disconnect(const InterfaceId&, const std::wstring&) override { Record(L"disconnect"); }
    void Connect(const InterfaceId&, const std::wstring& profile) override { Record(L"connect:" + profile); }
private:
    void Record(const std::wstring& call) {
        if (fail)
            throw std::runtime_error("Simulated WLAN failure");
        calls.push_back(call);
    }
};

void SubnetTests() {
    Require(Home == V4(192, 168, 1, 200), "Same IPv4 subnet must match");
    Require(!(Home == V4(192, 168, 2, 1)), "Different IPv4 subnets must not match");
    Require(!(Home == V4(192, 168, 1, 200, 25)), "Different prefix lengths must not match");
    Require(V4(192, 168, 1, 1, 23) == V4(192, 168, 0, 254, 23), "Non-byte-aligned prefix must match");
    Require(V4(10, 0, 0, 1, 32) != V4(10, 0, 0, 2, 32), "Host prefixes must retain address bits");
    auto first = Subnet::FromAddress({0x20, 1, 0xd, 0xb8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16, 64);
    auto second = Subnet::FromAddress({0x20, 1, 0xd, 0xb8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2}, 16, 64);
    Require(first == second, "Same IPv6 subnet must match");
    Require(!Subnet::FromAddress({169, 254, 1, 1}, 4, 16), "APIPA must be ignored");
    Require(!Subnet::FromAddress({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16, 64),
        "Link-local IPv6 must be ignored");
    Require(!Subnet::FromAddress({127, 0, 0, 1}, 4, 8), "Loopback must be ignored");
    Require(!Subnet::FromAddress({}, 4, 24), "Unspecified address must be ignored");
    Require(!Subnet::FromAddress({192, 168, 1, 1}, 4, 0), "Default route must be ignored");
    Require(!Subnet::FromAddress({224, 0, 0, 1}, 4, 24), "Multicast must be ignored");
    bool threw = false;
    try { (void)Subnet::FromAddress({1, 2, 3, 4}, 4, 33); }
    catch (const std::invalid_argument&) { threw = true; }
    Require(threw, "Invalid prefix must be reported");
}

void SwitchingTests() {
    FakeWireless actions;
    NetworkPolicy policy(actions, [](const auto&) {});
    policy.Update(State(true, WifiState::Connected), true, Now);
    Require(actions.calls == std::vector<std::wstring>{L"disconnect"}, "Matching Internet Ethernet must mute Wi-Fi");
    policy.Update(State(true, WifiState::Disconnected), true, Now + std::chrono::seconds(3));
    Require(actions.calls.size() == 1, "Saved subnet must remain usable after Wi-Fi loses IP");
    policy.Update({{}, {Wifi(WifiState::Disconnected)}}, true, Now + std::chrono::seconds(6));
    Require(actions.calls.back() == L"connect:Home", "Unplugging must reconnect original profile");
    Require(policy.MutedCount() == 1, "Unconfirmed reconnect must retain ownership");
    policy.Update({{}, {Wifi(WifiState::Connected)}}, true, Now + std::chrono::seconds(9));
    Require(policy.MutedCount() == 0, "Confirmed reconnect must release ownership");

    policy.Update(State(true, WifiState::Connected), true, Now + std::chrono::seconds(12));
    policy.Update(State(false, WifiState::Disconnected), true, Now + std::chrono::seconds(15));
    Require(actions.calls.back() == L"connect:Home", "Internet loss with LAN still attached must reconnect");
}

void SafeguardTests() {
    FakeWireless actions;
    NetworkPolicy policy(actions, [](const auto&) {});
    policy.Update(State(false, WifiState::Connected), true, Now);
    policy.Update({{{LanId, true, {V4(10, 0, 0, 1)}, L"", L""}}, {Wifi(WifiState::Connected)}}, true, Now);
    policy.Update(State(true, WifiState::Disconnected), true, Now);
    auto temporary = Wifi(WifiState::Connected);
    temporary.profile.clear();
    policy.Update({{{LanId, true, {Home}, L"", L""}}, {temporary}}, true, Now);
    Require(actions.calls.empty(), "Unrelated, disconnected, temporary or no-Internet Wi-Fi must be untouched");

    policy.Update(State(true, WifiState::Connected), true, Now);
    policy.Update(State(true, WifiState::Disconnected), false, Now + std::chrono::seconds(3));
    Require(actions.calls.back() == L"connect:Home", "Pausing must restore owned Wi-Fi");
    policy.Update(State(true, WifiState::Connected), false, Now + std::chrono::seconds(6));
    Require(policy.MutedCount() == 0 && actions.calls.size() == 2, "Paused app must not mute restored Wi-Fi");
}

void RetryAndManualOverrideTests() {
    FakeWireless actions;
    std::vector<std::wstring> messages;
    NetworkPolicy policy(actions, [&](const auto& text) { messages.push_back(text); });
    policy.Update(State(true, WifiState::Connected), true, Now);
    policy.Update(State(false, WifiState::Disconnected), true, Now + std::chrono::seconds(3));
    policy.Update(State(false, WifiState::Busy), true, Now + std::chrono::seconds(6));
    policy.Update(State(false, WifiState::Disconnected), true, Now + std::chrono::seconds(32));
    Require(actions.calls.size() == 2, "Must not spam reconnect while associating or before 30 seconds");
    policy.Update(State(false, WifiState::Disconnected), true, Now + std::chrono::seconds(33));
    Require(actions.calls.size() == 3, "Unconfirmed reconnect must retry at 30 seconds");
    bool timeout_reported = false;
    for (const auto& text : messages)
        timeout_reported |= text.find(L"not confirmed") != std::wstring::npos;
    Require(timeout_reported, "Reconnection timeout must be reported");
    auto other = Wifi(WifiState::Connected);
    other.profile = L"Other";
    policy.Update({{}, {other}}, true, Now + std::chrono::seconds(36));
    Require(policy.MutedCount() == 0 && actions.calls.size() == 3, "Must not override manual profile change");
}

void InterfaceNameTests() {
    FakeWireless actions;
    NetworkPolicy policy(actions, [](const auto&) {});
    auto connected = State(true, WifiState::Connected);
    connected.ethernet[0].name = L"USB Ethernet Adapter";
    connected.ethernet[0].network_name = L"Home Network";
    connected.wifi[0].name = L"Intel Wi-Fi 6E";
    connected.wifi[0].network_name = L"Home Wi-Fi";
    policy.Update(connected, true, Now);

    const auto details = policy.InterfaceDetails(connected);
    Require(details.size() == 4 &&
        details[0] == L"Ethernet: USB Ethernet Adapter" &&
        details[1] == L"  Network: Home Network" &&
        details[2] == L"Wi-Fi: Intel Wi-Fi 6E" &&
        details[3] == L"  Network: Home Wi-Fi",
        "Muted interface and network names must be shown");

    Snapshot unplugged{{}, {Wifi(WifiState::Disconnected)}};
    const auto restored_details = policy.InterfaceDetails(unplugged);
    Require(restored_details == details,
        "Interface and network names must remain visible while Wi-Fi is disconnected");

    Snapshot monitoring{
        {{LanId, true, {Home}, L"Dock Ethernet", L"Office LAN"}},
        {Wifi(WifiState::Connected)}};
    monitoring.wifi[0].name = L"Intel Wi-Fi";
    monitoring.wifi[0].network_name = L"SkyNet";
    FakeWireless monitoring_actions;
    NetworkPolicy monitoring_policy(monitoring_actions, [](const auto&) {});
    const auto active = monitoring_policy.InterfaceDetails(monitoring);
    Require(active.size() == 4 && active[0] == L"Ethernet: Dock Ethernet" &&
        active[1] == L"  Network: Office LAN" && active[2] == L"Wi-Fi: Intel Wi-Fi" &&
        active[3] == L"  Network: SkyNet",
        "Active interface and network names must be shown while monitoring");
}

void FailureTests() {
    FakeWireless actions;
    NetworkPolicy policy(actions, [](const auto&) {});
    actions.fail = true;
    bool threw = false;
    try { policy.Update(State(true, WifiState::Connected), true, Now); }
    catch (const std::runtime_error&) { threw = true; }
    Require(threw && policy.MutedCount() == 0, "Failed disconnect must report error without claiming ownership");
    actions.fail = false;
    policy.Update(State(true, WifiState::Connected), true, Now);
    actions.fail = true;
    threw = false;
    try { policy.Update(State(false, WifiState::Disconnected), true, Now); }
    catch (const std::runtime_error&) { threw = true; }
    Require(threw && policy.MutedCount() == 1, "Failed reconnect must report error without losing ownership");
}

void MultipleInterfaceTests() {
    FakeWireless actions;
    NetworkPolicy policy(actions, [](const auto&) {});
    auto other_wifi = Wifi(WifiState::Connected);
    other_wifi.id = InterfaceId{3};
    other_wifi.subnets = {V4(10, 0, 0, 1)};
    policy.Update({{{LanId, true, {Home}, L"", L""}}, {Wifi(WifiState::Connected), other_wifi}}, true, Now);
    Require(actions.calls.size() == 1 && policy.MutedCount() == 1, "Only matching Wi-Fi interface must be muted");
    policy.Update({{}, {}}, true, Now + std::chrono::seconds(3));
    Require(policy.MutedCount() == 1, "Missing Wi-Fi hardware must not discard saved restoration");
    policy.Update(State(false, WifiState::Disconnected), true, Now + std::chrono::seconds(6));
    Require(actions.calls.back() == L"connect:Home", "Reappearing Wi-Fi adapter must restore saved profile");
}

}

int main() {
    try {
        SubnetTests();
        SwitchingTests();
        SafeguardTests();
        RetryAndManualOverrideTests();
        InterfaceNameTests();
        FailureTests();
        MultipleInterfaceTests();
        std::cout << "All seven network policy test groups passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
