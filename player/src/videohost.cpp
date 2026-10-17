#include "videohost.h"

#include <QCoreApplication>

#include <cmath>

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

void VideoHost::moveBoth(int x, int y, int w, int h, HWND after)
{
    const UINT hostFlags = SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW;
    HDWP dwp = m_chrome && IsWindow(m_chrome) ? BeginDeferWindowPos(2) : nullptr;
    if (!dwp) {
        SetWindowPos(m_hwnd, after, x, y, w, h, hostFlags);
        return;
    }
    // The chrome is an owned window, so Windows keeps it above its owner without a
    // z-order change here. Two separate SetWindowPos calls leave it on the previous
    // rectangle for ~8 ms - one frame at 144 Hz, and the whole UI reads as if it
    // had slid into the top-left corner.
    dwp = DeferWindowPos(dwp, m_hwnd, after, x, y, w, h, hostFlags);
    dwp = dwp ? DeferWindowPos(dwp, m_chrome, nullptr, x, y, w, h,
                               SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) : nullptr;
    if (dwp)
        EndDeferWindowPos(dwp);
    else
        SetWindowPos(m_hwnd, after, x, y, w, h, hostFlags);
}

void VideoHost::toggleFullscreen()
{
    if (!m_hwnd)
        return;
    if (!m_fs)
        m_restoreRect = geometry();
    const QRect to = m_fs ? m_restoreRect : monitorRect();
    m_fs = !m_fs;
    // One step, no transition: animating this means resizing a 4K scene and mpv's
    // swapchain ~20 times in 170 ms, and the steps land 7-26 ms apart - it reads as
    // stutter, which is worse than arriving at once.
    moveBoth(to.x(), to.y(), to.width(), to.height());
    emit resized();
    emit fullscreenChanged();
}

void VideoHost::commitRect(int x, int y, int w, int h)
{
    if (!m_hwnd)
        return;
    moveBoth(x, y, w, h);
    emit resized();
}

void VideoHost::startFsTransition()
{
    if (!m_hwnd || m_fsAnimating)
        return;
    const QRect now = geometry();
    const bool enter = !m_fs;
    const QRect to = enter ? monitorRect() : m_restoreRect;
    if (enter)
        m_restoreRect = now;
    m_fs = enter;
    emit fullscreenChanged();
    if (to.width() <= 0 || to.height() <= 0) {
        moveBoth(to.x(), to.y(), to.width(), to.height());
        emit resized();
        return;
    }
    m_fsFrom = now;
    m_fsTo = to;
    m_fsAnimating = true;
    m_fsCommitLate = !enter;
    fsProgress(0.0);
    if (enter)
        moveBoth(to.x(), to.y(), to.width(), to.height());
    emit fsTransitionChanged();
}

void VideoHost::fsProgress(double p)
{
    if (!m_fsAnimating)
        return;
    auto at = [&](int a, int b) { return int(std::lround(a + (b - a) * p)); };
    const QRect shown{at(m_fsFrom.x(), m_fsTo.x()), at(m_fsFrom.y(), m_fsTo.y()),
                      at(m_fsFrom.width(), m_fsTo.width()),
                      at(m_fsFrom.height(), m_fsTo.height())};
    // Relative to the window as it stands, not to the end rect: on the way out the
    // window is still the big one, so the scene is laid out for that size and must
    // start at identity.
    const QRect win = geometry();
    if (win.width() <= 0 || win.height() <= 0)
        return;
    m_fsScale = double(shown.width()) / win.width();
    m_fsOffX = shown.x() - win.x();
    m_fsOffY = shown.y() - win.y();
    emit fsProgressed();
}

void VideoHost::fsFinished()
{
    if (!m_fsAnimating)
        return;
    m_fsAnimating = false;
    m_fsScale = 1.0;
    m_fsOffX = m_fsOffY = 0;
    emit fsProgressed();
    if (m_fsCommitLate)
        moveBoth(m_fsTo.x(), m_fsTo.y(), m_fsTo.width(), m_fsTo.height());
    m_fsCommitLate = false;
    emit resized();
    emit fsTransitionChanged();
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
        moveBoth(x, y, w, h, HWND_TOPMOST);
        m_mini = true;
    } else {
        moveBoth(m_preMiniRect.left(), m_preMiniRect.top(),
                 m_preMiniRect.width(), m_preMiniRect.height(), HWND_NOTOPMOST);
        m_mini = false;
    }
    emit resized();
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
