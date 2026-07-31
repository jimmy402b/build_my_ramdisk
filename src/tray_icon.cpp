#include "tray_icon.h"
#define WM_TRAYICON (WM_APP + 1)
#define IDM_OPEN  1001
#define IDM_SAVE  1002
#define IDM_EXIT  1003

TrayIcon::TrayIcon() : m_hWnd(nullptr), m_hMenu(nullptr) {}
TrayIcon::~TrayIcon() { if (m_nid.cbSize) Shell_NotifyIconA(NIM_DELETE, &m_nid); if (m_hWnd) DestroyWindow(m_hWnd); }

bool TrayIcon::Create(HINSTANCE hInst, const std::string& tooltip) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = "RamDiskTray"; RegisterClassA(&wc);
    m_hWnd = CreateWindowA("RamDiskTray", "", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, this);
    memset(&m_nid, 0, sizeof(m_nid)); m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hWnd; m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    strncpy(m_nid.szTip, tooltip.c_str(), sizeof(m_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &m_nid);
    BuildMenu(); return true;
}

void TrayIcon::BuildMenu() {
    m_hMenu = CreatePopupMenu();
    AppendMenuA(m_hMenu, MF_STRING, IDM_OPEN, "Open RAM Disk");
    AppendMenuA(m_hMenu, MF_STRING, IDM_SAVE, "Save Now");
    AppendMenuA(m_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(m_hMenu, MF_STRING, IDM_EXIT, "Exit");
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayIcon* self = (TrayIcon*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT*)lParam)->lpCreateParams);
        return 0;
    }
    if (!self) return DefWindowProc(hWnd, msg, wParam, lParam);
    if (msg == WM_TRAYICON && lParam == WM_RBUTTONUP) {
        POINT pt; GetCursorPos(&pt); SetForegroundWindow(hWnd);
        TrackPopupMenu(self->m_hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    } else if (msg == WM_TRAYICON && lParam == WM_LBUTTONDBLCLK) {
        if (self->m_onOpen) self->m_onOpen();
    } else if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDM_OPEN: if (self->m_onOpen) self->m_onOpen(); break;
        case IDM_SAVE: if (self->m_onSave) self->m_onSave(); break;
        case IDM_EXIT: if (self->m_onExit) self->m_onExit(); break;
        }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void TrayIcon::Run() { MSG msg; while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); } }
void TrayIcon::Quit() { PostMessage(m_hWnd, WM_DESTROY, 0, 0); }
void TrayIcon::SetTooltip(const std::string& text) { strncpy(m_nid.szTip, text.c_str(), sizeof(m_nid.szTip)-1); Shell_NotifyIconA(NIM_MODIFY, &m_nid); }
void TrayIcon::ShowBalloon(const std::string& title, const std::string& msg) {
    m_nid.uFlags |= NIF_INFO; m_nid.dwInfoFlags = NIIF_INFO;
    strncpy(m_nid.szInfoTitle, title.c_str(), sizeof(m_nid.szInfoTitle)-1);
    strncpy(m_nid.szInfo, msg.c_str(), sizeof(m_nid.szInfo)-1);
    Shell_NotifyIconA(NIM_MODIFY, &m_nid);
}
