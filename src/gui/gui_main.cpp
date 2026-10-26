// The GUI's shell: a window with one list, and a tray icon that says when to look at it.
//
// Everything that decides *what a row says* is in `gui_view_model.cpp`, and everything that talks
// to the service is in `gui_client.cpp`.  This file is the part that cannot be tested without a
// desktop: create the window, fill the list, put an icon in the notification area, and repaint.
//
// It links `myrd_product_ipc` and `myrd_product_client` and nothing else -- T3-01's closure check
// refuses the build if that changes -- so this process cannot open a device, read a bundle or
// write a journal even if someone wanted it to.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "src/gui/gui_client.h"
#include "src/gui/gui_view_model.h"

#include <chrono>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"MyRamDiskGuiMainWindow";
constexpr wchar_t kWindowTitle[] = L"MyRamDisk";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRefreshTimerId = 1;
constexpr UINT kRefreshIntervalMs = 3000;
constexpr int kTrayCommandShow = 1001;
constexpr int kTrayCommandRefresh = 1002;
constexpr int kTrayCommandExit = 1003;
constexpr int kButtonRefresh = 2001;
constexpr int kListDisks = 2002;
constexpr int kStatusText = 2003;

HWND g_window{};
HWND g_list{};
HWND g_status{};
HINSTANCE g_instance{};
NOTIFYICONDATAW g_tray{};
std::vector<myrd::gui::DiskRow> g_rows;

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), needed);
    return wide;
}

void add_column(int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&column));
}

void add_columns() {
    add_column(0, L"Mount", 110);
    add_column(1, L"Verdict", 190);
    add_column(2, L"Runtime", 90);
    add_column(3, L"Persistence", 130);
    add_column(4, L"Generation", 100);
    add_column(5, L"Memory", 190);
    add_column(6, L"Profile", 290);
}

void set_cell(int row, int column, const std::wstring& text) {
    ListView_SetItemText(g_list, row, column, const_cast<wchar_t*>(text.c_str()));
}

void refresh() {
    const auto snapshot = myrd::gui::load_disks();
    const auto rows = myrd::gui::make_disk_rows(snapshot.disks);
    g_rows = rows;

    ListView_DeleteAllItems(g_list);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(widen(row.mount).c_str());
        const int inserted = static_cast<int>(
            SendMessageW(g_list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
        if (inserted < 0) continue;
        set_cell(inserted, 1, widen(row.verdict));
        set_cell(inserted, 2, widen(row.runtime));
        set_cell(inserted, 3, widen(row.persistence));
        set_cell(inserted, 4, widen(row.generation));
        set_cell(inserted, 5, widen(row.memory));
        set_cell(inserted, 6, widen(row.profile_id));
    }

    // The status line says why the list is empty rather than leaving the user to guess: "the
    // service is not running" and "you have no disks" are different facts.
    std::wstring status;
    if (!snapshot.unavailable_reason.empty()) {
        status = L"Service unavailable: " + widen(snapshot.unavailable_reason);
    } else {
        const auto summary = myrd::gui::tray_summary(rows);
        status = widen(summary);
    }
    SetWindowTextW(g_status, status.c_str());

    if (g_tray.cbSize != 0) {
        wcsncpy_s(g_tray.szTip, status.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    }
}

void add_tray_icon(HWND window) {
    g_tray = NOTIFYICONDATAW{};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = window;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_tray.szTip, L"MyRamDisk", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void show_tray_menu(HWND window) {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayCommandShow, L"Show MyRamDisk");
    AppendMenuW(menu, MF_STRING, kTrayCommandRefresh, L"Refresh");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCommandExit, L"Exit");
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window,
        nullptr);
    DestroyMenu(menu);
    if (command != 0) {
        PostMessageW(window, WM_COMMAND, command, 0);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            12, 12, 860, 380, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListDisks)), g_instance, nullptr);
        if (g_list == nullptr) return -1;
        ListView_SetExtendedListViewStyle(
            g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        add_columns();

        CreateWindowExW(0, L"BUTTON", L"Refresh",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        12, 404, 100, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRefresh)),
                        g_instance, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", L"",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   124, 412, 748, 22, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusText)), g_instance, nullptr);
        add_tray_icon(window);
        SetTimer(window, kRefreshTimerId, kRefreshIntervalMs, nullptr);
        refresh();
        return 0;
    }
    case WM_SIZE: {
        if (g_list != nullptr) {
            RECT client{};
            GetClientRect(window, &client);
            MoveWindow(g_list, 12, 12, client.right - 24, client.bottom - 76, TRUE);
            MoveWindow(g_status, 124, client.bottom - 46, client.right - 136, 22, TRUE);
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wparam)) {
        case kButtonRefresh:
        case kTrayCommandRefresh:
            refresh();
            return 0;
        case kTrayCommandShow:
            ShowWindow(window, SW_SHOWNORMAL);
            SetForegroundWindow(window);
            refresh();
            return 0;
        case kTrayCommandExit:
            DestroyWindow(window);
            return 0;
        default:
            return 0;
        }
    }
    case kTrayMessage: {
        if (lparam == WM_LBUTTONDBLCLK) {
            ShowWindow(window, SW_SHOWNORMAL);
            SetForegroundWindow(window);
        } else if (lparam == WM_RBUTTONUP) {
            show_tray_menu(window);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wparam == kRefreshTimerId) {
            refresh();
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_CLOSE:
        // Closing the window leaves the tray icon: a ramdisk's status is the kind of thing a user
        // keeps an eye on without keeping a window open.
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimerId);
        if (g_tray.cbSize != 0) {
            Shell_NotifyIconW(NIM_DELETE, &g_tray);
            g_tray.cbSize = 0;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_instance = instance;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.style = CS_HREDRAW | CS_VREDRAW;
    definition.lpfnWndProc = window_proc;
    definition.hInstance = instance;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    definition.lpszClassName = kWindowClass;
    if (RegisterClassExW(&definition) == 0) return 1;

    const HWND window = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 500, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    g_window = window;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
