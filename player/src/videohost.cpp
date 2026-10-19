#include "videohost.h"

#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>

#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QWindow>

VideoHost *VideoHost::s_self = nullptr;

namespace {
// The curtain's bitmap, handed over by VideoHost::showCurtain. A QML item cannot
// cover a surface re-allocation - it is drawn into that same surface - so the two
// frames it takes Qt to bring up a new-size swap chain have to be hidden by a
// window whose pixels GDI puts there synchronously.
HBITMAP g_curtainBmp = nullptr;
int g_curtainW = 0, g_curtainH = 0;

constexpr wchar_t kCurtainClass[] = L"PlayeruiCurtain";

LRESULT CALLBACK curtainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        if (g_curtainBmp) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, g_curtainBmp);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, g_curtainW, g_curtainH,
                       SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        } else {
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}

// Defined from SDK 10.0.18985 up; the literal is what every build of Windows
// that can show a dark caption understands anyway.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
// From 24H2 the border the user actually sees is drawn per-monitor from this one;
// setting 34 alone leaves it in place.
#ifndef DWMWA_VISIBLE_BORDER_COLOR
#define DWMWA_VISIBLE_BORDER_COLOR 37
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0x02000000
#endif

namespace {
// Floor for the border drag. Below this the control row's 15 cells stop fitting.
constexpr int kMinWidth = 400;
constexpr int kMinHeight = 225;
constexpr int kMiniWidth = 480;
constexpr int kMiniHeight = 270;

QRect windowRectOf(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return { int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top) };
}

QRect clientOf(HWND h)
{
    RECT c{};
    GetClientRect(h, &c);
    POINT tl{ c.left, c.top };
    ClientToScreen(h, &tl);
    return { int(tl.x), int(tl.y), int(c.right - c.left), int(c.bottom - c.top) };
}
}

VideoHost::VideoHost(QObject *parent) : QObject(parent)
{
    // The shell's own hide of the tray runs from ~86 ms after the picture lands to
    // ~240 ms, so the flag is handed back behind that - and behind the dissolve.
    m_trayBeat.setSingleShot(true);
    m_trayBeat.setInterval(400);
    connect(&m_trayBeat, &QTimer::timeout, this, &VideoHost::restoreTaskbar);
    // Two frames at 144 Hz: enough for the chrome-less picture to be on screen
    // before the grab reads it off the desktop.
    m_snapArm.setSingleShot(true);
    m_snapArm.setInterval(16);
    connect(&m_snapArm, &QTimer::timeout, this, &VideoHost::grabPicture);
    m_settled.setSingleShot(true);
    m_settled.setInterval(200);
    connect(&m_settled, &QTimer::timeout, this, [this] {
        m_trans = false;
        emit transitioningChanged();
    });
    // Every geometry change re-arms the settle beat: the doubled chrome follows the
    // last size change, whichever beat produced it. Queued so it runs after the
    // emit, not in the middle of it.
    connect(this, &VideoHost::resized, this, [this] { m_settled.start(); },
            Qt::QueuedConnection);
}

VideoHost::~VideoHost()
{
    // Quitting from fullscreen must not leave the task bar switched off.
    restoreTaskbar();
}

void VideoHost::stashTaskbar()
{
    if (m_taskbarHwnd)
        return;
    HWND tr = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tr || !IsWindowVisible(tr))
        return;   // already out of the way - restoreTaskbar must not reveal it
    // ABM_SETSTATE was tried first and does nothing from outside Explorer: the
    // state read back 0 both before and during fullscreen, so the ask is ignored
    // for a window we do not own. Hiding the window is what moves those pixels.
    ShowWindow(tr, SW_HIDE);
    m_taskbarHwnd = tr;
}

void VideoHost::restoreTaskbar()
{
    if (!m_taskbarHwnd)
        return;
    // SW_SHOWNOACTIVATE: SW_SHOW was measured to take the foreground away from the
    // player, and the next F then went to Explorer instead of leaving fullscreen.
    ShowWindow(m_taskbarHwnd, SW_SHOWNOACTIVATE);
    m_taskbarHwnd = nullptr;
}

void VideoHost::showCurtain(const QRect &rect)
{
    if (!m_hwnd)
        return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &curtainProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kCurtainClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    if (!registered)
        return;
    // The grabbed frame as a top-down 32 bpp DIB; QImage's ARGB32 is already BGRA in
    // memory, which is what a DIB wants on little endian.
    const QImage img = m_snapObj.pixmap().toImage().convertToFormat(QImage::Format_ARGB32);
    if (g_curtainBmp) {
        DeleteObject(g_curtainBmp);
        g_curtainBmp = nullptr;
    }
    if (!img.isNull()) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = img.width();
        bi.bmiHeader.biHeight = -img.height();
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        g_curtainBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (g_curtainBmp && bits) {
            memcpy(bits, img.constBits(), size_t(img.height()) * img.bytesPerLine());
            g_curtainW = img.width();
            g_curtainH = img.height();
        }
    }
    if (!m_curtain)
        m_curtain = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCurtainClass, L"",
                                    WS_POPUP, rect.x(), rect.y(), rect.width(), rect.height(),
                                    m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_curtain)
        return;
    SetWindowPos(m_curtain, HWND_TOPMOST, rect.x(), rect.y(), rect.width(), rect.height(),
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // Painted is not enough - it has to be composited before the move. Without the
    // flush the curtain lands one frame late and the re-allocation shows through it.
    UpdateWindow(m_curtain);
    DwmFlush();
}

void VideoHost::hideCurtain()
{
    if (m_curtain && IsWindow(m_curtain))
        ShowWindow(m_curtain, SW_HIDE);
}

void VideoHost::adopt(QWindow *win)
{
    m_win = win;
    m_hwnd = reinterpret_cast<HWND>(win->winId());
    if (!m_hwnd)
        return;
    m_framed = true;

    // A light caption over a black player is the first thing on screen.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // The caption text is otherwise state-dependent: measured resting #797979, and
    // Windows repaints it near-white the moment the window is activated - which a
    // click on the title bar does, so the film name flashed white on every click.
    // Pinning the attribute overrides both states, so they become one colour.
    int captionText = RGB(0x79, 0x79, 0x79);
    DwmSetWindowAttribute(m_hwnd, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
    // Pinning the caption to the picture's black: the default dark caption is
    // #202020, which reads as a grey band sitting on top of a black player.
    int captionBg = RGB(0, 0, 0);
    DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionBg, sizeof(captionBg));
    // And the 1 px frame line DWM draws around the window - measured #303030 against
    // that #202020 caption - which brightens when the window is activated. COLOR_NONE
    // drops the line entirely, so there is nothing left to change state.
    int noBorder = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
    DwmSetWindowAttribute(m_hwnd, DWMWA_VISIBLE_BORDER_COLOR, &noBorder, sizeof(noBorder));

    // WS_EX_DLGMODALFRAME is what takes the icon out of the caption: Qt gives the
    // window none of its own, so without this Windows paints its generic
    // application glyph to the left of the film name. It has to leave with the
    // frame - left on a caption-less popup it insets the client by 3 px on all four
    // edges (measured: client 3,3 3834x2154 inside a 0,0 3840x2160 window).
    DWORD ex = DWORD(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, LONG_PTR(ex | WS_EX_DLGMODALFRAME));
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    measureInsets();

    // Qt owns this window's procedure, so the only way to answer WM_GETMINMAXINFO
    // is a subclass chained on top of it. That is what lets our fullscreen borrow
    // Windows' own maximise animation and still land on the whole monitor.
    s_self = this;
    if (qEnvironmentVariableIsSet("PLAYERUI_NOSUB"))
        qInfo("VIDEOHOST subclass skipped (PLAYERUI_NOSUB)");
    else if (!SetWindowSubclass(m_hwnd, &VideoHost::subProc, 1, 0))
        qWarning("VIDEOHOST subclass failed (%lu)", GetLastError());
}

LRESULT CALLBACK VideoHost::subProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    VideoHost *self = s_self;
    // PLAYERUI_NOMMI turns the answer off but leaves the subclass installed, so the
    // two candidate causes of the 0xc0000005 can be told apart without a rebuild.
    static const bool off = qEnvironmentVariableIsSet("PLAYERUI_NOMMI");
    if (msg == WM_GETMINMAXINFO && self && self->m_fs && !off) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(mon, &mi)) {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
            // Both fields matter. Maximise normally lands on the work area with the
            // invisible border added back as the position (-8,-8), so asking for the
            // monitor size alone would still leave the task-bar band uncovered.
            // Asked for the whole monitor. Measured: this is discarded whenever the
            // size exceeds the work area (both 0,0 3840x2160 and the border-
            // compensating -8,-8 3856x2176 landed on the default -8,-8 3856x2128),
            // while a rect inside the work area is taken verbatim (100,100
            // 3640x1900). So the answer cannot reach the task-bar band; the landing
            // beat is what covers it.
            mmi->ptMaxPosition.x = 0;
            mmi->ptMaxPosition.y = 0;
            mmi->ptMaxSize.x = mi.rcMonitor.right - mi.rcMonitor.left;
            mmi->ptMaxSize.y = mi.rcMonitor.bottom - mi.rcMonitor.top;
            mmi->ptMaxTrackSize = mmi->ptMaxSize;
        }
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void VideoHost::measureInsets()
{
    if (!m_hwnd)
        return;
    const QRect w = windowRectOf(m_hwnd), c = clientOf(m_hwnd);
    m_insets.left = c.x() - w.x();
    m_insets.top = c.y() - w.y();
    m_insets.right = w.right() - c.right();
    m_insets.bottom = w.bottom() - c.bottom();
    qInfo("FRAMEINSETS L%d T%d R%d B%d", int(m_insets.left), int(m_insets.top),
          int(m_insets.right), int(m_insets.bottom));
}

QRect VideoHost::geometry() const
{
    return m_hwnd ? windowRectOf(m_hwnd) : QRect();
}

QRect VideoHost::clientGeometry() const
{
    return m_hwnd ? clientOf(m_hwnd) : QRect();
}

void VideoHost::setClientSize(int w, int h)
{
    if (!m_hwnd)
        return;
    const QRect r = geometry();
    MoveWindow(m_hwnd, r.x(), r.y(), w + int(m_insets.left + m_insets.right),
               h + int(m_insets.top + m_insets.bottom), TRUE);
    emit resized();
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
    emit resized();
}

void VideoHost::setFramed(bool on)
{
    if (!m_hwnd || m_framed == on)
        return;
    constexpr DWORD kFrame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU
                           | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD style = DWORD(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
    style &= ~(kFrame | WS_POPUP);
    style |= on ? kFrame : WS_POPUP;
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, LONG_PTR(style));
    DWORD ex = DWORD(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    ex &= ~DWORD(WS_EX_DLGMODALFRAME);
    if (on)
        ex |= WS_EX_DLGMODALFRAME;
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, LONG_PTR(ex));
    m_framed = on;
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (on)
        measureInsets();
    else
        SetRectEmpty(&m_insets);
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

void VideoHost::grabPicture()
{
    if (!m_hwnd)
        return;
    QScreen *sc = QGuiApplication::primaryScreen();
    const QRect r = clientGeometry();
    m_snapObj.set(sc ? sc->grabWindow(0, r.x(), r.y(), r.width(), r.height()) : QPixmap());
}

void VideoHost::beginSwap()
{
    // The chrome is part of this window, so it has to be off the screen *before* the
    // grab: measured with the controls still in the frame, the stretched copy of them
    // landed 71 px off the real row for ~150 ms and read as a double image.
    m_trans = true;
    emit transitioningChanged();
    m_snapArm.start();
}

void VideoHost::releasePicture()
{
    m_snapObj.set(QPixmap());      // one 4K frame is 33 MB; hand it back after the dissolve
}

void VideoHost::noteSettled()
{
    if (!m_settledPending)
        return;
    m_settledPending = false;
    // One signal, both halves: the curtain comes off exactly when the snapshot may
    // start dissolving, so the two cannot land a frame apart and flash a hard cut.
    hideCurtain();
    emit settled();
}

void VideoHost::toggleFullscreen()
{
    if (!m_hwnd)
        return;
    if (!m_fs) {
        // Where to come back to. rcNormalPosition is only trustworthy while the
        // window is actually zoomed; otherwise the live rect is exact and the
        // placement's copy is two pixels short (1298x760 for a 1298x762 window).
        WINDOWPLACEMENT wp{ uint32_t(sizeof(wp)) };
        m_restoreMax = false;
        if (GetWindowPlacement(m_hwnd, &wp)) {
            m_restoreMax = wp.showCmd == SW_SHOWMAXIMIZED;
            const RECT &n = wp.rcNormalPosition;
            m_restoreRect = m_restoreMax
                ? QRect{ int(n.left), int(n.top), int(n.right - n.left), int(n.bottom - n.top) }
                : geometry();
        } else {
            m_restoreRect = geometry();
        }
        m_trans = true;
        emit transitioningChanged();
        stashTaskbar();
        m_fs = true;
        m_settledPending = true;
        // One step, no animation. Growing the window in beats costs a 4K surface
        // re-allocation per step (measured 7-26 ms apart at 144 Hz - a stutter), and
        // the system maximise cannot be answered past the work area, so it always
        // left the task-bar band to be covered by a second move. The snapshot laid at
        // the end rect is what covers the black first frame instead.
        // Land first, then take the caption off. Measured the other way round, Qt
        // recomputes its own geometry from the client area the moment the style
        // changes and overwrites the rect two calls later: the window ended up
        // frameless at 1282x721 in the middle of the screen instead of on the monitor.
        showCurtain(monitorRect());
        landOn(monitorRect());
        setFramed(false);
        emit resized();
        emit fullscreenChanged();
        m_trayBeat.start();
        return;
    }
    m_trans = true;
    emit transitioningChanged();
    // Hide the tray for the way out too: left visible, its band lit up when beat one
    // uncovered it and went under the shrinking window again - three events.
    stashTaskbar();
    m_trayBeat.stop();
    m_fs = false;
    m_settledPending = true;
    showCurtain(m_restoreRect);
    landOn(m_restoreRect);
    setFramed(true);
    emit resized();
    emit fullscreenChanged();
    if (m_restoreMax)
        ShowWindow(m_hwnd, SW_SHOWMAXIMIZED);
    m_trayBeat.start();
}

void VideoHost::landOn(const QRect &to)
{
    if (!m_hwnd)
        return;
    // Leave the zoomed state and land on the target in one write: stripping the
    // frame while the window is still zoomed insets the client by 3 px on every edge
    // (measured: client 3,3 3834x2154 inside a 0,0 3840x2160 popup).
    WINDOWPLACEMENT np{ uint32_t(sizeof(np)) };
    if (GetWindowPlacement(m_hwnd, &np)) {
        np.showCmd = SW_RESTORE;
        np.rcNormalPosition = { to.x(), to.y(), to.x() + to.width(), to.y() + to.height() };
        SetWindowPlacement(m_hwnd, &np);
    }
    // Above the shell while fullscreen so nothing slides over the film; the band
    // comes back with the window. One SetWindowPos, so no half-committed rect can
    // reach the screen between the placement write and the z-order.
    SetWindowPos(m_hwnd, m_fs ? HWND_TOPMOST : HWND_NOTOPMOST, to.x(), to.y(),
                 to.width(), to.height(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void VideoHost::toggleMini()
{
    if (!m_hwnd)
        return;
    if (!m_mini) {
        // QRect's four-argument form is (x, y, width, height), not the RECT's
        // (left, top, right, bottom) - passing the corners makes the restore window
        // as big as its own screen offset.
        m_preMiniRect = geometry();
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        // The picture is 480x270, so the window carries the frame on top of it.
        const int w = kMiniWidth + int(m_insets.left + m_insets.right);
        const int h = kMiniHeight + int(m_insets.top + m_insets.bottom);
        const int x = mi.rcWork.right - w - 24;
        const int y = mi.rcWork.bottom - h - 24;
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_mini = true;
    } else {
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, m_preMiniRect.x(), m_preMiniRect.y(),
                     m_preMiniRect.width(), m_preMiniRect.height(),
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        m_mini = false;
    }
    emit resized();
    emit miniChanged();
}

void VideoHost::minimize()
{
    if (m_hwnd)
        ShowWindow(m_hwnd, SW_MINIMIZE);
}

void VideoHost::activate()
{
    if (!m_hwnd)
        return;
    if (IsIconic(m_hwnd))
        ShowWindow(m_hwnd, SW_RESTORE);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
}
