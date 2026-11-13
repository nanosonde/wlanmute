#include "windows_network.h"

#include <iphlpapi.h>
#include <netlistmgr.h>
#include <ws2tcpip.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace wlanmute {
namespace {

// Defined locally so linking does not depend on these symbols being present in uuid.lib.
constexpr GUID NetworkListManagerClsid{0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr GUID NetworkListManagerIid{0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

class WindowsError final : public std::runtime_error {
public:
    WindowsError(const char* operation, DWORD error_code) : std::runtime_error(operation), code_(error_code) {}
    DWORD code() const { return code_; }
private:
    DWORD code_;
};

void Check(DWORD code, const char* operation) {
    if (code != ERROR_SUCCESS)
        throw WindowsError(operation, code);
}

void CheckHr(HRESULT hr, const char* operation) {
    if (FAILED(hr))
        throw WindowsError(operation, static_cast<DWORD>(hr));
}

struct WlanDeleter {
    void operator()(void* memory) const { WlanFreeMemory(memory); }
};

struct ComDeleter {
    void operator()(IUnknown* object) const { if (object) object->Release(); }
};

struct BstrDeleter {
    void operator()(wchar_t* value) const { SysFreeString(value); }
};

struct AdapterDetails {
    bool has_internet{};
    std::wstring network_name;
};

std::wstring SsidToWideString(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH)
        return {};

    const auto* bytes = reinterpret_cast<const char*>(ssid.ucSSID);
    const auto byte_count = static_cast<int>(ssid.uSSIDLength);
    const int character_count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes, byte_count, nullptr, 0);
    std::wstring result;
    if (character_count > 0) {
        result.resize(static_cast<std::size_t>(character_count));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, byte_count,
                result.data(), character_count) != character_count)
            result.clear();
    }
    if (result.empty()) {
        const int fallback_count = MultiByteToWideChar(CP_ACP, 0, bytes, byte_count, nullptr, 0);
        if (fallback_count <= 0)
            return {};
        result.resize(static_cast<std::size_t>(fallback_count));
        if (MultiByteToWideChar(CP_ACP, 0, bytes, byte_count,
                result.data(), fallback_count) != fallback_count)
            return {};
    }
    for (auto& character : result) {
        if (character < L' ' || character == 0x7f)
            character = L'?';
    }
    return result;
}

InterfaceId ToId(const GUID& guid) {
    InterfaceId result{};
    static_assert(sizeof(result) == sizeof(guid));
    std::memcpy(result.data(), &guid, sizeof(guid));
    return result;
}

GUID ToGuid(const InterfaceId& id) {
    GUID result{};
    std::memcpy(&result, id.data(), sizeof(result));
    return result;
}

std::map<InterfaceId, AdapterDetails> ReadNetworkDetails() {
    INetworkListManager* raw_manager = nullptr;
    CheckHr(CoCreateInstance(NetworkListManagerClsid, nullptr, CLSCTX_ALL,
        NetworkListManagerIid, reinterpret_cast<void**>(&raw_manager)), "Open Network List Manager");
    std::unique_ptr<INetworkListManager, ComDeleter> manager(raw_manager);
    IEnumNetworkConnections* raw_connections = nullptr;
    CheckHr(manager->GetNetworkConnections(&raw_connections), "Enumerate network connections");
    std::unique_ptr<IEnumNetworkConnections, ComDeleter> connections(raw_connections);
    std::map<InterfaceId, AdapterDetails> result;
    while (true) {
        INetworkConnection* raw_connection = nullptr;
        ULONG fetched = 0;
        const auto hr = connections->Next(1, &raw_connection, &fetched);
        std::unique_ptr<INetworkConnection, ComDeleter> connection(raw_connection);
        CheckHr(hr, "Read network connection");
        if (fetched == 0)
            break;
        NLM_CONNECTIVITY connectivity{};
        GUID id{};
        CheckHr(connection->GetConnectivity(&connectivity), "Read adapter Internet status");
        CheckHr(connection->GetAdapterId(&id), "Read adapter ID");
        INetwork* raw_network = nullptr;
        CheckHr(connection->GetNetwork(&raw_network), "Read network profile");
        std::unique_ptr<INetwork, ComDeleter> network(raw_network);
        BSTR raw_name = nullptr;
        CheckHr(network->GetName(&raw_name), "Read network name");
        std::unique_ptr<wchar_t, BstrDeleter> name(raw_name);

        auto& details = result[ToId(id)];
        details.has_internet |= (connectivity &
            (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
        if (details.network_name.empty() && name)
            details.network_name = name.get();
    }
    return result;
}

std::vector<Subnet> ReadSubnets(const IP_ADAPTER_ADDRESSES& adapter) {
    std::vector<Subnet> result;
    for (auto address = adapter.FirstUnicastAddress; address; address = address->Next) {
        if (address->DadState != IpDadStatePreferred || !address->Address.lpSockaddr)
            continue;
        const auto socket = address->Address.lpSockaddr;
        std::array<std::uint8_t, 16> bytes{};
        std::uint8_t size = 0;
        if (socket->sa_family == AF_INET) {
            const auto ipv4 = reinterpret_cast<const sockaddr_in*>(socket);
            std::memcpy(bytes.data(), &ipv4->sin_addr, 4);
            size = 4;
        } else if (socket->sa_family == AF_INET6) {
            const auto ipv6 = reinterpret_cast<const sockaddr_in6*>(socket);
            std::memcpy(bytes.data(), &ipv6->sin6_addr, 16);
            size = 16;
        } else {
            continue;
        }
        if (auto subnet = Subnet::FromAddress(bytes, size, address->OnLinkPrefixLength))
            result.push_back(*subnet);
    }
    return result;
}

}

WindowsNetwork::~WindowsNetwork() {
    if (wlan_)
        WlanCloseHandle(wlan_, nullptr);
}

void WindowsNetwork::Open() {
    if (!wlan_) {
        DWORD version = 0;
        Check(WlanOpenHandle(2, nullptr, &version, &wlan_), "Open WLAN client (check WLAN AutoConfig service)");
    }
}

Snapshot WindowsNetwork::Read(bool require_wifi) {
    const auto network_details = ReadNetworkDetails();
    ULONG size = 16384;
    std::vector<std::uint8_t> buffer(size);
    DWORD status = ERROR_BUFFER_OVERFLOW;
    // Adapter changes can require a larger buffer between calls.
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != ERROR_NO_DATA)
        Check(status, "Read adapter addresses");

    Snapshot snapshot;
    std::map<InterfaceId, std::vector<Subnet>> networks;
    if (status == ERROR_SUCCESS) {
        for (auto adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
             adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp)
                continue;
            GUID guid{};
            Check(ConvertInterfaceLuidToGuid(&adapter->Luid, &guid), "Read interface GUID");
            const auto id = ToId(guid);
            auto subnets = ReadSubnets(*adapter);
            networks.emplace(id, subnets);
            if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
                const auto profile = network_details.find(id);
                snapshot.ethernet.push_back({
                    id,
                    profile != network_details.end() && profile->second.has_internet,
                    std::move(subnets),
                    adapter->FriendlyName ? adapter->FriendlyName : L"Ethernet",
                    profile != network_details.end() ? profile->second.network_name : L""});
            }
        }
    }

    if (!wlan_) {
        DWORD version = 0;
        status = WlanOpenHandle(2, nullptr, &version, &wlan_);
        if (!require_wifi && status == ERROR_SERVICE_NOT_ACTIVE) {
            wlan_ = nullptr;
            return snapshot;
        }
        Check(status, "Open WLAN client (check WLAN AutoConfig service)");
    }
    WLAN_INTERFACE_INFO_LIST* raw_interfaces = nullptr;
    Check(WlanEnumInterfaces(wlan_, nullptr, &raw_interfaces), "Enumerate Wi-Fi interfaces");
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, WlanDeleter> interfaces(raw_interfaces);
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        auto& info = interfaces->InterfaceInfo[i];
        WifiLink wifi;
        wifi.id = ToId(info.InterfaceGuid);
        wifi.name = info.strInterfaceDescription;
        if (const auto profile = network_details.find(wifi.id); profile != network_details.end())
            wifi.network_name = profile->second.network_name;
        if (const auto found = networks.find(wifi.id); found != networks.end())
            wifi.subnets = found->second;
        switch (info.isState) {
        case wlan_interface_state_connected: {
            DWORD data_size = 0;
            void* raw_attributes = nullptr;
            WLAN_OPCODE_VALUE_TYPE type{};
            status = WlanQueryInterface(wlan_, &info.InterfaceGuid, wlan_intf_opcode_current_connection,
                nullptr, &data_size, &raw_attributes, &type);
            std::unique_ptr<void, WlanDeleter> attributes(raw_attributes);
            if (status == ERROR_INVALID_STATE) {
                wifi.state = WifiState::Busy;
                break;
            }
            Check(status, "Read Wi-Fi connection (check Windows Location permission)");
            if (data_size < sizeof(WLAN_CONNECTION_ATTRIBUTES))
                throw std::runtime_error("WLAN returned truncated connection attributes.");
            const auto connection = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(attributes.get());
            wifi.state = WifiState::Connected;
            const std::wstring connection_profile = connection->strProfileName;
            if (connection->wlanConnectionMode == wlan_connection_mode_profile)
                wifi.profile = connection_profile;
            const auto ssid = SsidToWideString(connection->wlanAssociationAttributes.dot11Ssid);
            if (!ssid.empty())
                wifi.network_name = ssid;
            else if (!connection_profile.empty())
                wifi.network_name = connection_profile;
            break;
        }
        case wlan_interface_state_disconnected:
            wifi.state = WifiState::Disconnected;
            break;
        case wlan_interface_state_not_ready:
            wifi.state = WifiState::Unavailable;
            break;
        default:
            wifi.state = WifiState::Busy;
            break;
        }
        snapshot.wifi.push_back(std::move(wifi));
    }
    return snapshot;
}

void WindowsNetwork::Disconnect(const InterfaceId& id, const std::wstring& expected_profile) {
    Open();
    auto guid = ToGuid(id);
    DWORD data_size = 0;
    void* raw_attributes = nullptr;
    WLAN_OPCODE_VALUE_TYPE type{};
    Check(WlanQueryInterface(wlan_, &guid, wlan_intf_opcode_current_connection, nullptr,
        &data_size, &raw_attributes, &type), "Recheck Wi-Fi before disconnect");
    std::unique_ptr<void, WlanDeleter> attributes(raw_attributes);
    if (data_size < sizeof(WLAN_CONNECTION_ATTRIBUTES))
        throw std::runtime_error("WLAN returned truncated connection attributes.");
    const auto connection = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(attributes.get());
    if (connection->isState != wlan_interface_state_connected ||
        connection->wlanConnectionMode != wlan_connection_mode_profile ||
        expected_profile != connection->strProfileName)
        throw std::runtime_error("Wi-Fi changed during the check; leaving the new connection alone.");
    Check(WlanDisconnect(wlan_, &guid, nullptr), "Disconnect Wi-Fi");
}

void WindowsNetwork::Connect(const InterfaceId& id, const std::wstring& profile) {
    Open();
    auto guid = ToGuid(id);
    WLAN_CONNECTION_PARAMETERS parameters{};
    parameters.wlanConnectionMode = wlan_connection_mode_profile;
    parameters.strProfile = profile.c_str();
    parameters.dot11BssType = dot11_BSS_type_any;
    Check(WlanConnect(wlan_, &guid, &parameters, nullptr), "Reconnect saved Wi-Fi profile");
}

std::wstring ErrorMessage(const std::exception& error) {
    const std::string operation = error.what();
    std::wstring result;
    for (const char character : operation)
        result += static_cast<wchar_t>(static_cast<unsigned char>(character));
    if (const auto windows_error = dynamic_cast<const WindowsError*>(&error)) {
        wchar_t text[1024]{};
        const auto length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, windows_error->code(), 0, text, static_cast<DWORD>(std::size(text)), nullptr);
        result += L" (code " + std::to_wstring(windows_error->code()) + L")";
        if (length != 0)
            result += L": " + std::wstring(text, length);
    }
    return result;
}

}
