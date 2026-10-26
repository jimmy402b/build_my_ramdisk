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

#include "src/control/command.h"
#include "src/gui/gui_client.h"
#include "src/gui/gui_view_model.h"

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
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
constexpr int kButtonStart = 2010;
constexpr int kButtonStop = 2011;
constexpr int kButtonCheckpoint = 2012;
constexpr int kButtonForget = 2013;
constexpr int kProgressText = 2014;
constexpr UINT kCommandDone = WM_APP + 2;
constexpr UINT kProgressTimerId = 2;
constexpr UINT kProgressIntervalMs = 600;

HWND g_window{};
HWND g_list{};
HWND g_status{};
HINSTANCE g_instance{};
NOTIFYICONDATAW g_tray{};
std::vector<myrd::gui::DiskRow> g_rows;
HWND g_progress{};
// The operation the progress line is following, if any.
std::optional<myrd::product::Uuid> g_followed_operation;

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

std::optional<std::size_t> selected_row() {
    const int index = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (index < 0 || static_cast<std::size_t>(index) >= g_rows.size()) return std::nullopt;
    return static_cast<std::size_t>(index);
}

void set_progress(const std::string& text) {
    SetWindowTextW(g_progress, widen(text).c_str());
}

// **The exchange happens on another thread.**  A command is admitted quickly, but the exchange
// still crosses a pipe and can wait on a service that is busy; doing it on the UI thread is how a
// window stops repainting while an operation is "in progress".
void run_command(myrd::control::Command command) {
    set_progress("sending...");
    std::thread([command = std::move(command)] {
        auto outcome = myrd::gui::send_command(command);
        auto* heap = new myrd::gui::CommandOutcome(std::move(outcome));
        PostMessageW(g_window, kCommandDone, 0, reinterpret_cast<LPARAM>(heap));
    }).detach();
}

void follow_operation(const myrd::product::Uuid& operation_id) {
    g_followed_operation = operation_id;
    SetTimer(g_window, kProgressTimerId, kProgressIntervalMs, nullptr);
}

void on_command_done(myrd::gui::CommandOutcome outcome) {
    if (!outcome.accepted) {
        set_progress(outcome.message);
        MessageBoxW(g_window, widen(outcome.message).c_str(), L"MyRamDisk", MB_ICONWARNING);
        return;
    }
    if (outcome.response.has_value() && outcome.response->operation_id.has_value()) {
        follow_operation(outcome.response->operation_id.value());
        set_progress("queued");
    } else {
        set_progress("done");
    }
    refresh();
}

void tick_progress() {
    if (!g_followed_operation.has_value()) {
        KillTimer(g_window, kProgressTimerId);
        return;
    }
    const auto progress = myrd::gui::poll_operation(g_followed_operation.value());
    if (!progress.observed) {
        set_progress(progress.message);
        return;
    }
    set_progress(myrd::gui::progress_text(progress.state, progress.phase));
    if (myrd::gui::operation_finished(progress.state)) {
        // The terminal state is the last word: the prompt stops following and the list is rebuilt
        // from the service's answer rather than from what the window expected to happen.
        g_followed_operation.reset();
        KillTimer(g_window, kProgressTimerId);
        refresh();
    }
}

void command_for_start() {
    const auto row = selected_row();
    if (!row.has_value()) return;
    run_command(myrd::control::DiskStartCommand{widen(g_rows[row.value()].profile_id)});
}

void command_for_checkpoint() {
    const auto row = selected_row();
    if (!row.has_value()) return;
    run_command(myrd::control::DiskCheckpointCommand{widen(g_rows[row.value()].mount)});
}

void command_for_stop() {
    const auto row = selected_row();
    if (!row.has_value()) return;
    // `checkpoint_required` is the profile default, so a plain stop asks the service to checkpoint
    // first; the confirmation for the data-losing variant is what T3-04 adds, and it is shown only
    // when the caller has to use `force`.
    run_command(myrd::control::DiskStopCommand{widen(g_rows[row.value()].mount), false, false});
}

void command_for_forget() {
    const auto row = selected_row();
    if (!row.has_value()) return;
    const auto& disk = g_rows[row.value()];
    const auto confirmation =
        myrd::gui::forget_confirmation(disk.mount, disk.profile_id);
    const int answer = MessageBoxW(g_window, widen(confirmation.text).c_str(), L"Forget disk",
                                   MB_ICONWARNING | MB_OKCANCEL);
    if (answer != IDOK) return;
    const auto profile = myrd::product::Uuid::parse(disk.profile_id);
    if (!profile.ok()) {
        set_progress("the profile identifier could not be read");
        return;
    }
    // The identifier is echoed as the confirmation, which is what makes this command impossible to
    // run by accident: the caller has to have read the profile it is about to forget.
    run_command(myrd::control::DiskForgetCommand{profile.value(), profile.value()});
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
        const struct { int id; const wchar_t* text; int x; } buttons[] = {
            {kButtonStart, L"Start", 12}, {kButtonStop, L"Stop", 116},
            {kButtonCheckpoint, L"Checkpoint", 220}, {kButtonForget, L"Forget", 324}};
        for (const auto& button : buttons) {
            CreateWindowExW(0, L"BUTTON", button.text,
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, button.x, 444, 100, 28,
                            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)),
                            g_instance, nullptr);
        }
        g_progress = CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     436, 450, 436, 22, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgressText)),
                                     g_instance, nullptr);
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
        case kButtonStart: command_for_start(); return 0;
        case kButtonStop: command_for_stop(); return 0;
        case kButtonCheckpoint: command_for_checkpoint(); return 0;
        case kButtonForget: command_for_forget(); return 0;
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
    case kCommandDone: {
        std::unique_ptr<myrd::gui::CommandOutcome> outcome(
            reinterpret_cast<myrd::gui::CommandOutcome*>(lparam));
        if (outcome != nullptr) on_command_done(std::move(*outcome));
        return 0;
    }
    case WM_TIMER: {
        if (wparam == kRefreshTimerId) {
            refresh();
            return 0;
        }
        if (wparam == kProgressTimerId) {
            tick_progress();
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

// **`--self-check`: the same client and the same view model, without a window.**
//
// T3-05 asks the guest to accept the GUI as a thin client.  A desktop test cannot read a ListView
// from another process, so the readable half is this mode: it asks the service for its disks
// through `gui_client` and formats them through `gui_view_model`, exactly as the window does, and
// prints what it got.  The window itself is then accepted by existing, being alive, and owning a
// window with the product's title -- facts this mode cannot fake.
int run_self_check() {
    const auto snapshot = myrd::gui::load_disks(std::chrono::milliseconds{5000});
    if (!snapshot.unavailable_reason.empty()) {
        std::printf("self-check: unavailable: %s\n", snapshot.unavailable_reason.c_str());
        std::fflush(stdout);
        return 3;
    }
    const auto rows = myrd::gui::make_disk_rows(snapshot.disks);
    std::printf("self-check: %zu disk(s)\n", rows.size());
    for (const auto& row : rows) {
        std::printf("  %s  %s  %s  %s  %s\n", row.mount.c_str(), row.verdict.c_str(),
                    row.runtime.c_str(), row.generation.c_str(), row.memory.c_str());
    }
    std::printf("tray: %s\n", myrd::gui::tray_summary(rows).c_str());
    std::fflush(stdout);
    return 0;
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_instance = instance;
    // A GUI process started with `--self-check` prints and exits; everything else is the window.
    if (std::wcsstr(GetCommandLineW(), L"--self-check") != nullptr) {
        return run_self_check();
    }
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
