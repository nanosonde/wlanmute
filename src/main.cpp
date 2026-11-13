#include "resource.h"
#include "version.h"
#include "windows_network.h"

#include <objbase.h>
#include <shellapi.h>
#include <strsafe.h>

#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT_PTR PollTimer = 1;
constexpr UINT MenuAutomatic = 100;
constexpr UINT MenuCheck = 101;
constexpr UINT MenuActivity = 102;
constexpr UINT MenuAbout = 103;
constexpr UINT MenuExit = 104;
constexpr wchar_t WindowClass[] = L"wlanmute.TrayWindow";

struct HandleDeleter {
    void operator()(void* handle) const { if (handle) CloseHandle(handle); }
};

class ComApartment {
public:
    ComApartment() {
        const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr))
            throw std::runtime_error("Cannot initialize Windows COM.");
    }
    ~ComApartment() { CoUninitialize(); }
};

class TrayApp {
public:
    explicit TrayApp(HINSTANCE instance)
        : policy_(network_, [this](const auto& text) { Report(text); }), instance_(instance) {
        LoadIcons();
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = WindowProcedure;
        window_class.hInstance = instance;
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_MUTED));
        window_class.lpszClassName = WindowClass;
        if (!RegisterClassW(&window_class))
            throw std::runtime_error("Cannot register the tray window.");
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (!taskbar_created_)
            throw std::runtime_error("Cannot register taskbar recovery notification.");
        // Hidden top-level window receives Explorer's TaskbarCreated broadcast.
        window_ = CreateWindowExW(0, WindowClass, L"wlanmute", WS_OVERLAPPED,
            0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!window_)
            throw std::runtime_error("Cannot create the tray window.");
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = TrayMessage;
        tray_.hIcon = icons_[IconMonitoring];
        StringCchCopyW(tray_.szTip, std::size(tray_.szTip), L"wlanmute - monitoring");
        AddTrayIcon();
        if (!SetTimer(window_, PollTimer, 3000, nullptr))
            throw std::runtime_error("Cannot start the network monitor timer.");
        CheckConnections();
    }

    ~TrayApp() {
        if (window_) {
            KillTimer(window_, PollTimer);
            Shell_NotifyIconW(NIM_DELETE, &tray_);
            DestroyWindow(window_);
        }
        DestroyIcons();
    }

    int Run() {
        MSG message{};
        BOOL result = 0;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (result == -1)
            throw std::runtime_error("The Windows message loop failed.");
        return static_cast<int>(message.wParam);
    }

private:
    enum IconState { IconMonitoring, IconMuted, IconPaused, IconCount };

    void LoadIcons() {
        // Loaded at the current small-icon size so the matching .ico frame is used on high-DPI displays.
        const int width = GetSystemMetrics(SM_CXSMICON);
        const int height = GetSystemMetrics(SM_CYSMICON);
        const int ids[IconCount] = {IDI_MONITORING, IDI_APP_MUTED, IDI_PAUSED};
        for (int i = 0; i < IconCount; ++i) {
            const auto icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(ids[i]),
                IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
            if (!icon)
                throw std::runtime_error("Cannot load the tray icons.");
            if (icons_[i])
                DestroyIcon(icons_[i]);
            icons_[i] = icon;
        }
    }

    void DestroyIcons() {
        for (auto& icon : icons_) {
            if (icon)
                DestroyIcon(icon);
            icon = nullptr;
        }
    }

    IconState CurrentIconState() const {
        if (!enabled_ || !last_error_.empty())
            return IconPaused;
        return policy_.MutedCount() > 0 ? IconMuted : IconMonitoring;
    }

    void AddTrayIcon() {
        if (!Shell_NotifyIconW(NIM_ADD, &tray_))
            throw std::runtime_error("Cannot add the system tray icon.");
    }

    void Report(const std::wstring& text) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t stamp[16]{};
        StringCchPrintfW(stamp, std::size(stamp), L"%02u:%02u:%02u ",
            static_cast<unsigned>(time.wHour), static_cast<unsigned>(time.wMinute),
            static_cast<unsigned>(time.wSecond));
        events_.push_back(stamp + text);
        while (events_.size() > 30)
            events_.pop_front();
        OutputDebugStringW((events_.back() + L"\n").c_str());
    }

    void ShowError(const std::wstring& text) {
        status_ = L"Network error (see Recent activity)";
        if (last_error_ != text) {
            Report(L"ERROR: " + text);
            tray_.uFlags = NIF_INFO;
            StringCchCopyW(tray_.szInfoTitle, std::size(tray_.szInfoTitle), L"wlanmute: network operation failed");
            StringCchCopyNW(tray_.szInfo, std::size(tray_.szInfo), text.c_str(), std::size(tray_.szInfo) - 1);
            tray_.dwInfoFlags = NIIF_ERROR;
            if (!Shell_NotifyIconW(NIM_MODIFY, &tray_))
                OutputDebugStringW(L"wlanmute: could not show the error balloon.\n");
            last_error_ = text;
        }
    }

    void CheckConnections() {
        try {
            status_ = policy_.Update(network_.Read(), enabled_, wlanmute::Clock::now());
            last_error_.clear();
        } catch (const std::exception& error) {
            ShowError(wlanmute::ErrorMessage(error));
        }
        const auto tip = L"wlanmute - " + status_;
        tray_.uFlags = NIF_TIP | NIF_ICON;
        tray_.hIcon = icons_[CurrentIconState()];
        StringCchCopyNW(tray_.szTip, std::size(tray_.szTip), tip.c_str(), std::size(tray_.szTip) - 1);
        if (!Shell_NotifyIconW(NIM_MODIFY, &tray_))
            OutputDebugStringW(L"wlanmute: could not update tray status.\n");

        if (exiting_) {
            if (policy_.MutedCount() == 0) {
                PostQuitMessage(0);
            } else if (wlanmute::Clock::now() - *exiting_ >= std::chrono::seconds(35)) {
                exiting_.reset();
                MessageBoxW(window_, L"Wi-Fi restoration could not be confirmed. The app remains paused "
                    L"and will keep retrying. You can reconnect Wi-Fi manually in Windows, then exit again.",
                    L"wlanmute", MB_OK | MB_ICONWARNING);
            }
        }
    }

    void ShowMenu() {
        const auto menu = CreatePopupMenu();
        if (!menu)
            throw std::runtime_error("Cannot create the tray menu.");
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status_.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const auto automatic_flags = static_cast<UINT>(MF_STRING | (enabled_ ? MF_CHECKED : MF_UNCHECKED) |
            (exiting_ ? MF_GRAYED : MF_ENABLED));
        AppendMenuW(menu, automatic_flags, MenuAutomatic, L"Automatic switching");
        AppendMenuW(menu, MF_STRING, MenuCheck, L"Check now");
        AppendMenuW(menu, MF_STRING, MenuActivity, L"Recent activity");
        AppendMenuW(menu, MF_STRING, MenuAbout, L"About");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuExit, L"Exit (restore Wi-Fi)");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        const auto command = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0, window_, nullptr));
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        switch (command) {
        case MenuAutomatic:
            enabled_ = !enabled_;
            CheckConnections();
            break;
        case MenuCheck:
            CheckConnections();
            break;
        case MenuActivity: {
            std::wstring text;
            for (const auto& entry : events_)
                text += entry + L"\r\n";
            MessageBoxW(window_, text.empty() ? L"No switching activity yet." : text.c_str(),
                L"wlanmute: recent activity", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case MenuAbout:
            MessageBoxW(window_, L"wlanmute " WLANMUTE_VERSION L"\nNative Win32 Ethernet/Wi-Fi tray utility.\n"
                L"Prefers Internet-capable Ethernet on the same subnet.\n"
                L"Restores only Wi-Fi disconnected by this app.\n\nhttps://github.com/nanosonde/wlanmute",
                L"wlanmute", MB_OK | MB_ICONINFORMATION);
            break;
        case MenuExit:
            if (!exiting_)
                exiting_ = wlanmute::Clock::now();
            enabled_ = false;
            CheckConnections();
            break;
        default:
            break;
        }
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<TrayApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) {
            try {
                if (message == WM_TIMER && wparam == PollTimer) {
                    app->CheckConnections();
                    return 0;
                }
                if (message == TrayMessage && (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP)) {
                    app->ShowMenu();
                    return 0;
                }
                if (message == app->taskbar_created_) {
                    // Explorer restarted or the display scale changed: reload icons at the new size.
                    app->LoadIcons();
                    app->tray_.hIcon = app->icons_[app->CurrentIconState()];
                    app->tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
                    app->AddTrayIcon();
                    return 0;
                }
                if (message == WM_CLOSE) {
                    app->enabled_ = false;
                    app->exiting_ = wlanmute::Clock::now();
                    app->CheckConnections();
                    return 0;
                }
            } catch (const std::exception& error) {
                // Exceptions must never escape a Windows callback.
                app->ShowError(wlanmute::ErrorMessage(error));
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    wlanmute::WindowsNetwork network_;
    wlanmute::NetworkPolicy policy_;
    HINSTANCE instance_{};
    HICON icons_[IconCount]{};
    HWND window_{};
    UINT taskbar_created_{};
    NOTIFYICONDATAW tray_{};
    bool enabled_{true};
    std::optional<wlanmute::Clock::time_point> exiting_;
    std::wstring status_{L"Starting..."};
    std::wstring last_error_;
    std::deque<std::wstring> events_;
};

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    try {
        ComApartment apartment;
        if (std::wstring(command_line) == L"--smoke-test") {
            wlanmute::WindowsNetwork network;
            const auto snapshot = network.Read(false);
            const auto text = L"Read-only API smoke test passed. Ethernet interfaces: " +
                std::to_wstring(snapshot.ethernet.size()) + L", Wi-Fi interfaces: " +
                std::to_wstring(snapshot.wifi.size()) + L"\r\n";
            const auto file = CreateFileW(L"smoke-test-result.txt", GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
                CloseHandle(file);
            }
            return 0;
        }
        const auto raw_mutex = CreateMutexW(nullptr, FALSE, L"Local\\wlanmute");
        if (!raw_mutex)
            throw std::runtime_error("Cannot create the single-instance mutex.");
        const auto mutex_error = GetLastError();
        std::unique_ptr<void, HandleDeleter> mutex(raw_mutex);
        if (mutex_error == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"wlanmute is already running. Look in the system tray (including hidden icons).",
                L"wlanmute", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        TrayApp app(instance);
        return app.Run();
    } catch (const std::exception& error) {
        const auto text = wlanmute::ErrorMessage(error);
        if (std::wstring(command_line) == L"--smoke-test") {
            OutputDebugStringW((L"wlanmute smoke test failed: " + text + L"\n").c_str());
            const auto file = CreateFileW(L"smoke-test-error.txt", GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
                CloseHandle(file);
            }
        } else {
            MessageBoxW(nullptr, text.c_str(), L"wlanmute could not start", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
