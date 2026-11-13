#include "resource.h"
#include "version.h"
#include "windows_network.h"

#include <commctrl.h>
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
constexpr wchar_t ActivityWindowClass[] = L"wlanmute.ActivityWindow";
constexpr int ActivityListId = 200;
constexpr int ActivityCloseId = 201;

struct ActivityEvent {
    std::wstring time;
    std::wstring message;
};

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
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        if (!InitCommonControlsEx(&controls))
            throw std::runtime_error("Cannot initialize the recent-activity table.");
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
        if (activity_window_)
            DestroyWindow(activity_window_);
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
        events_.push_back({stamp, text});
        while (events_.size() > 30)
            events_.pop_front();
        OutputDebugStringW((stamp + text + L"\n").c_str());
        RefreshActivityWindow();
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

    void RefreshActivityWindow() {
        if (!activity_list_)
            return;

        ListView_DeleteAllItems(activity_list_);
        int row = 0;
        if (events_.empty()) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(L"--");
            ListView_InsertItem(activity_list_, &item);
            ListView_SetItemText(activity_list_, row, 1, const_cast<wchar_t*>(L"No recent activity."));
            return;
        }

        for (const auto& event : events_) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(event.time.c_str());
            if (ListView_InsertItem(activity_list_, &item) >= 0)
                ListView_SetItemText(activity_list_, row, 1, const_cast<wchar_t*>(event.message.c_str()));
            ++row;
        }
        ListView_EnsureVisible(activity_list_, row - 1, FALSE);
    }

    void ShowActivityWindow() {
        if (activity_window_) {
            RefreshActivityWindow();
            ShowWindow(activity_window_, SW_SHOWNORMAL);
            SetForegroundWindow(activity_window_);
            return;
        }

        WNDCLASSEXW activity_class{};
        activity_class.cbSize = sizeof(activity_class);
        activity_class.lpfnWndProc = ActivityWindowProcedure;
        activity_class.hInstance = instance_;
        activity_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        activity_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_MUTED));
        activity_class.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
        activity_class.lpszClassName = ActivityWindowClass;
        if (!RegisterClassExW(&activity_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create the recent-activity window.");

        const auto dpi = GetDpiForWindow(window_);
        RECT bounds{0, 0, MulDiv(640, static_cast<int>(dpi), 96), MulDiv(360, static_cast<int>(dpi), 96)};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
        constexpr DWORD extended_style = WS_EX_TOOLWINDOW;
        if (!AdjustWindowRectExForDpi(&bounds, style, FALSE, extended_style, dpi))
            throw std::runtime_error("Cannot size the recent-activity window.");

        activity_window_ = CreateWindowExW(extended_style, ActivityWindowClass,
            L"wlanmute - Recent activity", style, CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top, window_, nullptr, instance_, this);
        if (!activity_window_)
            throw std::runtime_error("Cannot open the recent-activity window.");
        ShowWindow(activity_window_, SW_SHOWNORMAL);
        SetForegroundWindow(activity_window_);
    }

    static LRESULT CALLBACK ActivityWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<TrayApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->activity_window_ = window;
        }

        if (app) {
            if (message == WM_CREATE) {
                const auto dpi = GetDpiForWindow(window);
                const int margin = MulDiv(12, static_cast<int>(dpi), 96);
                app->activity_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
                    margin, margin, 1, 1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ActivityListId)),
                    app->instance_, nullptr);
                app->activity_close_ = CreateWindowExW(0, L"BUTTON", L"Close",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ActivityCloseId)),
                    app->instance_, nullptr);
                if (!app->activity_list_ || !app->activity_close_)
                    return -1;

                ListView_SetExtendedListViewStyle(app->activity_list_,
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                column.pszText = const_cast<wchar_t*>(L"Time");
                column.iSubItem = 0;
                column.cx = MulDiv(100, static_cast<int>(dpi), 96);
                if (ListView_InsertColumn(app->activity_list_, 0, &column) < 0)
                    return -1;
                column.pszText = const_cast<wchar_t*>(L"Activity");
                column.iSubItem = 1;
                column.cx = MulDiv(500, static_cast<int>(dpi), 96);
                if (ListView_InsertColumn(app->activity_list_, 1, &column) < 0)
                    return -1;
                app->RefreshActivityWindow();
                return 0;
            }
            if (message == WM_SIZE && app->activity_list_ && app->activity_close_) {
                RECT client{};
                GetClientRect(window, &client);
                const auto dpi = GetDpiForWindow(window);
                const int margin = MulDiv(12, static_cast<int>(dpi), 96);
                const int button_width = MulDiv(90, static_cast<int>(dpi), 96);
                const int button_height = MulDiv(28, static_cast<int>(dpi), 96);
                const int width = client.right - client.left;
                const int height = client.bottom - client.top;
                MoveWindow(app->activity_list_, margin, margin,
                    width - 2 * margin, height - 3 * margin - button_height, TRUE);
                const int time_column = ListView_GetColumnWidth(app->activity_list_, 0);
                const int activity_width = width - 2 * margin - time_column - MulDiv(24, static_cast<int>(dpi), 96);
                ListView_SetColumnWidth(app->activity_list_, 1, activity_width > 0 ? activity_width : 0);
                MoveWindow(app->activity_close_, width - margin - button_width,
                    height - margin - button_height, button_width, button_height, TRUE);
                return 0;
            }
            if (message == WM_GETMINMAXINFO) {
                auto info = reinterpret_cast<MINMAXINFO*>(lparam);
                const auto dpi = GetDpiForWindow(window);
                info->ptMinTrackSize.x = MulDiv(480, static_cast<int>(dpi), 96);
                info->ptMinTrackSize.y = MulDiv(280, static_cast<int>(dpi), 96);
                return 0;
            }
            if (message == WM_DPICHANGED) {
                const auto suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (message == WM_COMMAND && LOWORD(wparam) == ActivityCloseId) {
                DestroyWindow(window);
                return 0;
            }
            if (message == WM_CLOSE) {
                DestroyWindow(window);
                return 0;
            }
            if (message == WM_DESTROY) {
                app->activity_window_ = nullptr;
                app->activity_list_ = nullptr;
                app->activity_close_ = nullptr;
                return 0;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
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
        case MenuActivity:
            ShowActivityWindow();
            break;
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
    HWND activity_window_{};
    HWND activity_list_{};
    HWND activity_close_{};
    UINT taskbar_created_{};
    NOTIFYICONDATAW tray_{};
    bool enabled_{true};
    std::optional<wlanmute::Clock::time_point> exiting_;
    std::wstring status_{L"Starting..."};
    std::wstring last_error_;
    std::deque<ActivityEvent> events_;
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
