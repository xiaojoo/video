#include "videohost.h"

#include <dwmapi.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

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
    m_curtain = nullptr;   // owned by the host, so it went with it
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

namespace {
QRect clientOf(HWND h)
{
    RECT c{};
    GetClientRect(h, &c);
    POINT tl{ c.left, c.top };
    ClientToScreen(h, &tl);
    return { int(tl.x), int(tl.y), int(c.right - c.left), int(c.bottom - c.top) };
}
}

void VideoHost::grabPicture()
{
    if (!m_hwnd)
        return;
    QScreen *sc = QGuiApplication::primaryScreen();
    const QRect r = clientOf(m_hwnd);
    m_snapObj.set(sc ? sc->grabWindow(0, r.x(), r.y(), r.width(), r.height()) : QPixmap());
}

void VideoHost::releasePicture()
{
    m_snapObj.set(QPixmap());      // 一张 4K 图 33 MB，溶解完就还掉
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

void VideoHost::showCurtain(const QRect &outer, const QRect &inner)
{
    static bool registered = false;
    const QColor c = dipColor();
    const COLORREF rgb = RGB(c.red(), c.green(), c.blue());
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = CreateSolidBrush(rgb);
        wc.lpszClassName = L"PlayeruiCurtain";
        registered = RegisterClassExW(&wc) != 0;
    }
    if (!registered || !m_hwnd)
        return;
    if (!m_curtain) {
        m_curtain = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                    L"PlayeruiCurtain", L"", WS_POPUP,
                                    outer.x(), outer.y(), outer.width(), outer.height(),
                                    m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_curtain)
            return;
        // Same strength as the QML veil. Without this the curtain's arrival would
        // step the screen from "dimmed" to fully opaque, which is the blackout
        // the veil exists to avoid.
        SetLayeredWindowAttributes(m_curtain, 0,
                                   static_cast<BYTE>(qRound(dipAlpha() * 255.0)), LWA_ALPHA);
    }
    SetWindowPos(m_curtain, HWND_TOP, outer.x(), outer.y(), outer.width(), outer.height(),
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // Cut out the part the chrome has already painted. Without the hole the
    // curtain's veil would sit on top of the chrome's and the two would stack:
    // measured, 0.72 over 0.72 leaves the picture at 15 % instead of 28 %.
    // SetWindowRgn takes ownership of the region on success.
    if (HRGN ring = CreateRectRgn(0, 0, outer.width(), outer.height())) {
        if (HRGN hole = CreateRectRgn(inner.x() - outer.x(), inner.y() - outer.y(),
                                      inner.x() - outer.x() + inner.width(),
                                      inner.y() - outer.y() + inner.height())) {
            CombineRgn(ring, ring, hole, RGN_DIFF);
            DeleteObject(hole);
            if (SetWindowRgn(m_curtain, ring, TRUE) == 0)
                DeleteObject(ring);   // rejected: we still own it
        } else {
            DeleteObject(ring);
        }
    }
    // Synchronous: the fill has to be in this surface before the batch move, or the
    // curtain is one message-loop iteration behind the thing it is there to hide.
    UpdateWindow(m_curtain);
    // And not just painted - composited. Without this the curtain's black lands
    // either one frame before or one frame after the resize, and the late case is
    // the flash: the end-size picture shows for one frame. Costs up to one vsync,
    // which the 40 ms trough already absorbs.
    DwmFlush();
    // 撤它的时机不在这里：见 main.cpp 里那次 frameSwapped。以前是固定 24 ms，
    // 界面窗偶尔画得比 24 ms 晚，那一圈就先透出终态画面（亮）再被压回去（暗）。
}

void VideoHost::noteSettled()
{
    if (!m_settledPending)
        return;
    m_settledPending = false;
    if (m_curtain && IsWindow(m_curtain) && IsWindowVisible(m_curtain))
        ShowWindow(m_curtain, SW_HIDE);
    emit settled();
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
    // The curtain covers the union minus whichever rect is smaller: the smaller one
    // is where the chrome's existing buffer still has valid pixels after the move,
    // so it needs no covering and must not be veiled twice.
    const QRect from = geometry();
    const auto area = [](const QRect &v) { return v.width() * v.height(); };
    // 只有进全屏要幕窗：新铺到的那一圈在界面窗画出来之前透的是没压暗的终态画面。
    // 退出时那一圈让给的是桌面，本身没有要盖的东西，盖上 72 % 反而把桌面压暗一块
    // （实测右下角从 41 掉到 25、停 28 ms）。退出要盖的是画面自己，交给快照。
    if (m_fs)
        showCurtain(from.united(to), area(from) <= area(to) ? from : to);
    m_settledPending = true;
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
