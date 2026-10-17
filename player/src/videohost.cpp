#include "videohost.h"

#include <QCoreApplication>

VideoHost *VideoHost::s_self = nullptr;

namespace {
// Floor for the border drag. Below this the control row's 15 cells stop fitting.
constexpr int kMinWidth = 400;
constexpr int kMinHeight = 225;
}


VideoHost::VideoHost(QObject *parent) : QObject(parent) {}

VideoHost::~VideoHost()
{
    destroy();
}

LRESULT CALLBACK VideoHost::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    VideoHost *self = s_self;
    switch (msg) {
    case WM_SIZE:
        if (self)
            emit self->resized();
        return 0;
    case WM_CLOSE:
        if (self)
            emit self->closeRequested();
        return 0;
    case WM_ERASEBKGND:
        // mpv owns the client area; painting a background here would flicker.
        return 1;
    case WM_WINDOWPOSCHANGED:
        // mpv raises the host when the VO comes up; the chrome has to follow.
        if (self)
            emit self->raised();
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool VideoHost::create(const QRect &rect)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &VideoHost::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PlayeruiVideoHost";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        registered = true;
    }
    s_self = this;

    const DWORD exStyle = qEnvironmentVariableIsSet("PLAYERUI_HOST_LAYERED") ? WS_EX_LAYERED : 0;
    m_hwnd = CreateWindowExW(exStyle, L"PlayeruiVideoHost", L"Playerui", WS_POPUP,
                             rect.x(), rect.y(), rect.width(), rect.height(),
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd)
        return false;
    ShowWindow(m_hwnd, SW_SHOW);
    return true;
}

void VideoHost::destroy()
{
    if (!m_hwnd)
        return;
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    if (s_self == this)
        s_self = nullptr;
}

void VideoHost::activate()
{
    if (!m_hwnd)
        return;
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
}

void VideoHost::toggleFullscreen()
{
    if (!m_hwnd)
        return;
    if (!m_fs) {
        m_savedStyle = GetWindowLongW(m_hwnd, GWL_STYLE);
        GetWindowRect(m_hwnd, &m_savedRect);
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
            return;
        SetWindowLongW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_fs = true;
    } else {
        SetWindowLongW(m_hwnd, GWL_STYLE, m_savedStyle);
        SetWindowPos(m_hwnd, HWND_TOP, m_savedRect.left, m_savedRect.top,
                     m_savedRect.right - m_savedRect.left, m_savedRect.bottom - m_savedRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_fs = false;
    }
    emit fullscreenChanged();
}

QRect VideoHost::monitorRect() const
{
    if (!m_hwnd)
        return {};
    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return {};
    return { int(mi.rcMonitor.left), int(mi.rcMonitor.top),
             int(mi.rcMonitor.right - mi.rcMonitor.left),
             int(mi.rcMonitor.bottom - mi.rcMonitor.top) };
}

void VideoHost::toggleMini()
{
    if (!m_hwnd)
        return;
    if (!m_mini) {
        RECT cur{};
        GetWindowRect(m_hwnd, &cur);
        // QRect's four-argument form is (x, y, width, height), not the RECT's
        // (left, top, right, bottom) — passing the corners makes the restore
        // window as big as its own screen offset.
        m_preMiniRect = { int(cur.left), int(cur.top),
                          int(cur.right - cur.left), int(cur.bottom - cur.top) };
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        const int w = 480, h = 270;
        const int x = mi.rcWork.right - w - 24;
        const int y = mi.rcWork.bottom - h - 24;
        SetWindowLongW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_mini = true;
    } else {
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, m_preMiniRect.left(), m_preMiniRect.top(),
                     m_preMiniRect.width(), m_preMiniRect.height(),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_mini = false;
    }
    emit miniChanged();
}

QRect VideoHost::geometry() const
{
    if (!m_hwnd)
        return {};
    RECT r{};
    GetWindowRect(m_hwnd, &r);
    return { int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top) };
}

void VideoHost::minimize()
{
    if (!m_hwnd)
        return;
    // Minimising the chrome alone leaves the video on screen: the host is a
    // separate top-level. Windows minimises owned windows with their owner, and
    // the chrome is owned by us through GWLP_HWNDPARENT.
    ShowWindow(m_hwnd, SW_MINIMIZE);
}

void VideoHost::setRect(int x, int y, int w, int h)
{
    if (!m_hwnd)
        return;
    // Clamped here rather than in QML so the border, the mini window and any
    // external resize all agree on one floor.
    if (w < kMinWidth)
        w = kMinWidth;
    if (h < kMinHeight)
        h = kMinHeight;
    MoveWindow(m_hwnd, x, y, w, h, TRUE);
    // mpv owns the host's window procedure, so our WM_SIZE never fires and
    // nothing else would tell the chrome to follow.
    emit resized();
}
