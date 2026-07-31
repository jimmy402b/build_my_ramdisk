#pragma once
#include <windows.h>
#include <functional>
#include <string>

class TrayIcon {
public:
    using Callback = std::function<void()>;
    TrayIcon();
    ~TrayIcon();
    bool Create(HINSTANCE hInst, const std::string& tooltip);
    void OnOpenDisk(Callback cb) { m_onOpen = cb; }
    void OnSave(Callback cb) { m_onSave = cb; }
    void OnExit(Callback cb) { m_onExit = cb; }
    void SetTooltip(const std::string& text);
    void ShowBalloon(const std::string& title, const std::string& msg);
    void Run();
    void Quit();
private:
    HWND m_hWnd;
    NOTIFYICONDATAA m_nid;
    HMENU m_hMenu;
    Callback m_onOpen, m_onSave, m_onExit;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void BuildMenu();
};
