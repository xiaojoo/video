#include "videohost.h"

#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>

#include <QWindow>

VideoHost *VideoHost::s_self = nullptr;

namespace {
// The cover for the geometry change, filled by GDI from a 1:1 copy of the screen
// taken just before it. A QML item cannot do this - it is drawn into the very surface
// being re-made - and Qt needs a few frames to redraw at the new size: measured, the
// old 1314x801 surface sat in the top-left corner of the 3840x2160 window for 19 ms
// (2 frames at 144 Hz), which is the "window jumps to the top-left" he saw.
//
// It is put up before the write and taken off by the third frame that goes over at a
// size other than the one the window had beforehand - the first only proves the frame
// reached DWM, and measured, the panel still showed the old surface in the top-left
// corner of the enlarged window for one frame after it. So the freeze lasts exactly as
// long as the surface re-allocation and the compositor catching up with it, not one
// millisecond more. A timer backs it up for the case where no frame is drawn at all.
// The last thing the cover does is fade out over the task bar's row alone: that strip
// goes from near white to letterbox black, and it is the one change a copy of the
// screen cannot hide.
HBITMAP g_coverBmp = nullptr;
uint8_t *g_coverBits = nullptr;
std::vector<uint8_t> g_bandSrc;      // the strip as grabbed, so each step is recomputed
int g_coverW = 0, g_coverH = 0, g_coverStride = 0;
int g_coverX = 0, g_coverY = 0;      // where the grab starts on the desktop
int g_bandH = 0, g_bandFade = 0;

constexpr wchar_t kCurtainClass[] = L"PlayeruiCurtain";

// The cover never paints. Its whole surface - and its rect - is handed to DWM in one
// UpdateLayeredWindow call, so there is no half-blitted frame to be caught: measured
// once in three entries, a 33 MB BitBlt from WM_PAINT was composited mid-copy and the
// screen showed the picture down to y=1360 with everything below it black for 7 ms.
LRESULT CALLBACK curtainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(h, msg, wp, lp);
}

// Puts the sub-rect (sx, sy, w, h) of the cover DIB on screen at (x, y, w, h), as the
// whole content of the layered window, in one call. Opaque: the strip's fade is done by
// rewriting its pixels, not by blending, because per-pixel alpha does not blend with
// what is underneath it here (measured, see settle()).
bool presentCover(HWND h, int x, int y, int sx, int sy, int w, int hh)
{
    if (!g_coverBmp || !h)
        return false;
    HDC mem = CreateCompatibleDC(nullptr);
    if (!mem)
        return false;
    HGDIOBJ old = SelectObject(mem, g_coverBmp);
    POINT dst{ x, y };
    SIZE size{ w, hh };
    POINT src{ sx, sy };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, BYTE(0) };
    const bool ok = UpdateLayeredWindow(h, nullptr, &dst, &size, mem, &src, 0, &bf,
                                        ULW_OPAQUE) != FALSE;
    SelectObject(mem, old);
    DeleteDC(mem);
    return ok;
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
    m_band.setInterval(15);
    connect(&m_band, &QTimer::timeout, this, [this] {
        g_bandFade += 255 * m_band.interval() / 90;
        if (g_bandFade >= 255) {
            m_band.stop();
            g_bandFade = 0;
            hideCurtain();
            return;
        }
        bandStep();
    });
    m_curtainBeat.setInterval(15);
    connect(&m_curtainBeat, &QTimer::timeout, this, [this] {
        // Not stopped by settle(): the band's fade out runs after the cover comes down,
        // and on the way out of fullscreen the task bar is being re-shown in that same
        // window - Explorer puts the appbar at the head of the topmost band, i.e. over
        // the strip, which is what made the band light up in one step while the fade
        // ran underneath it.
        if (!m_settledPending && !m_band.isActive()) {
            m_curtainBeat.stop();
            return;
        }
        raiseCurtain();
    });
    m_settled.setSingleShot(true);
    m_settled.setInterval(200);
    connect(&m_settled, &QTimer::timeout, this, [this] {
        // Only the backstop. The reveal is normally driven by notePresented() long
        // before this: measured, a frame at the new size goes over 20-30 ms after the
        // write. This fires when nothing renders at all - a resize the scene graph
        // never took a pass for - and without it the cover would stay up and the
        // snapping guard wedged.
        reveal();
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
    qInfo("TRAY stash vis=%d", IsWindowVisible(tr) ? 1 : 0);
}

void VideoHost::restoreTaskbar()
{
    if (!m_taskbarHwnd)
        return;
    // SW_SHOWNOACTIVATE: SW_SHOW was measured to take the foreground away from the
    // player, and the next F then went to Explorer instead of leaving fullscreen.
    ShowWindow(m_taskbarHwnd, SW_SHOWNOACTIVATE);
    qInfo("TRAY restore vis=%d", IsWindowVisible(m_taskbarHwnd) ? 1 : 0);
    m_taskbarHwnd = nullptr;
}

void VideoHost::bandStep()
{
    if (!g_coverBmp || !g_coverBits || !m_curtain || !IsWindow(m_curtain))
        return;
    const int keep = 255 - g_bandFade;
    const size_t from = size_t(g_coverH - g_bandH) * g_coverStride;
    for (size_t i = 0; i < g_bandSrc.size(); i += 4) {
        // Recomputed from the grab every step, so a missed step cannot accumulate.
        g_coverBits[from + i] = static_cast<uint8_t>(g_bandSrc[i] * keep / 255);
        g_coverBits[from + i + 1] = static_cast<uint8_t>(g_bandSrc[i + 1] * keep / 255);
        g_coverBits[from + i + 2] = static_cast<uint8_t>(g_bandSrc[i + 2] * keep / 255);
    }
    presentCover(m_curtain, g_coverX, g_coverY + g_coverH - g_bandH,
                 0, g_coverH - g_bandH, g_coverW, g_bandH);
}
void VideoHost::showCover()
{
    if (!m_hwnd || !g_coverBmp || g_coverW <= 0)
        return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &curtainProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kCurtainClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    if (!registered)
        return;
    if (!m_curtain)
        // No owner: an owned window is demoted with its owner, and landOn takes our
        // window out of the topmost band on the way out of fullscreen - measured the
        // cover going down with it, so the desktop showed for 150 ms mid-transition.
        m_curtain = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    kCurtainClass, L"", WS_POPUP,
                                    g_coverX, g_coverY, g_coverW, g_coverH,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_curtain)
        return;
    // The surface goes in while the window is still hidden, so the first thing it is
    // ever composited with is a complete frame.
    presentCover(m_curtain, g_coverX, g_coverY, 0, 0, g_coverW, g_coverH);
    SetWindowPos(m_curtain, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_curtainBeat.start();
    // Put up is not enough - it has to be composited before anything moves. Without
    // the flush the cover lands one frame late and the re-allocation shows through it.
    DwmFlush();
}

void VideoHost::raiseCurtain()
{
    // landOn puts our own window at the top of the topmost band, i.e. back over the
    // strip. Re-asserting the order is what keeps it up until the fade finishes.
    if (m_curtain && IsWindow(m_curtain))
        SetWindowPos(m_curtain, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

    // Qt owns this window's procedure, so the only way to answer WM_NCCALCSIZE (and
    // WM_GETMINMAXINFO) is a subclass chained on top of it. It has to be installed
    // before the frame bits go on: the first WM_NCCALCSIZE is what decides the client
    // size, and answered by the default it shrinks the client by a caption - Qt then
    // takes that as the new truth and the window ends up 16x39 short of what QML asked
    // for. Measured with the subclass installed afterwards: 1282x723 for 1298x762.
    s_self = this;
    if (qEnvironmentVariableIsSet("PLAYERUI_NOSUB"))
        qInfo("VIDEOHOST subclass skipped (PLAYERUI_NOSUB)");
    else if (!SetWindowSubclass(m_hwnd, &VideoHost::subProc, 1, 0))
        qWarning("VIDEOHOST subclass failed (%lu)", GetLastError());

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

    // Qt was told the window is frameless, so it sizes the window as if the client were
    // the whole rect. The frame goes back on the Win32 side: the caption, the thick
    // border and the system menu are what make ShowWindow(SW_MAXIMIZE), the taskbar
    // entry and the edge hit-testing work at all. WM_NCCALCSIZE then hands the caption's
    // pixels back to the client, so nothing of it is ever drawn.
    constexpr DWORD kFrame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU
                           | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD style = DWORD(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, LONG_PTR((style & ~DWORD(WS_POPUP)) | kFrame));
    // With the caption gone out of the way the corners are the last thing DWM still
    // draws for a frame it no longer shows: rounded, it clips a wedge out of the
    // picture's top corners. DWMWCP_DONOTROUND is 1, and the attribute is from 1903.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
    int square = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &square, sizeof(square));
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    measureInsets();

    // A run that died with the tray stashed leaves it hidden, and nothing re-shows it:
    // stashTaskbar() will not adopt a tray it did not hide. With auto-hide off, a
    // hidden Shell_TrayWnd can only be that leftover.
    if (HWND tr = FindWindowW(L"Shell_TrayWnd", nullptr);
        tr && !IsWindowVisible(tr)) {
        APPBARDATA st{ uint32_t(sizeof(st)), tr };
        if (SHAppBarMessage(ABM_GETSTATE, &st) == 0) {   // 0 = no ABS_AUTOHIDE
            ShowWindow(tr, SW_SHOWNOACTIVATE);
            qInfo("TRAY recovered from a previous run");
        }
    }
    qInfo("ADOPT win=%d,%d %dx%d client=%d,%d %dx%d", geometry().x(), geometry().y(),
          geometry().width(), geometry().height(), clientGeometry().x(), clientGeometry().y(),
          clientGeometry().width(), clientGeometry().height());
}

LRESULT CALLBACK VideoHost::subProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    VideoHost *self = s_self;
    // The frame is never taken off this window any more, so the non-client rect is
    // answered here instead: the client is the whole window, which is also what Qt
    // believes (the window is frameless to it), so neither side compensates for margins
    // the other does not see. Only when zoomed the border is put back, because the
    // maximised window rect sticks out past the monitor by exactly that much on all
    // four sides - without it a maximised window loses a row to the screen edge and
    // covers the task bar. The 8 px the invisible border used to occupy is what our own
    // grab bands in QML are for.
    if (msg == WM_NCCALCSIZE && wp == TRUE) {
        if (IsZoomed(h)) {
            auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(lp);
            const int fx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            const int fy = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            RECT &r = p->rgrc[0];
            r.left += fx;
            r.top += fy;
            r.right -= fx;
            r.bottom -= fy;
        }
        return 0;
    }
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
    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
    // Qt answers our landing write with a deferred position change of its own, and
    // that raise puts the window back over the curtain - measured two frames of the
    // re-allocated surface (film at 56 % of its height, centred) leaking through the
    // cover. Re-asserting the cover on every position change while a transition is in
    // flight costs one SetWindowPos and closes that gap.
    if (msg == WM_WINDOWPOSCHANGED && self && self->m_settledPending)
        self->raiseCurtain();
    return r;
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

QRect VideoHost::workArea() const
{
    // The work area of the monitor the window is on, not `SPI_GETWORKAREA`: that one is
    // only ever the primary monitor's, so maximising a window on a second screen would
    // move it to the first. Same source fullscreen uses.
    if (!m_hwnd)
        return {};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return {};
    const RECT &w = mi.rcWork;
    return { int(w.left), int(w.top), int(w.right - w.left), int(w.bottom - w.top) };
}

void VideoHost::grabPicture()
{
    fillCover();
    emit pictureReady();
}

// The cover's surface, filled from what the monitor shows right now.
void VideoHost::fillCover()
{
    if (!m_hwnd)
        return;
    // The whole monitor, not the client rect. A cover that reproduces every pixel
    // already on screen is the one that does not read as a change: grabbing only the
    // client left the task bar band - 48 px of near-white in a light theme - to snap
    // to black when it was hidden, measured 213.97 -> 5.00 in a single 7 ms frame.
    const QRect mon = monitorRect();
    if (mon.isEmpty())
        return;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_bandH = qMax(0, int(mon.bottom() - LONG(wa.bottom) + 1));
    QElapsedTimer g;
    g.start();
    if (mon.width() != g_coverW || mon.height() != g_coverH) {
        if (g_coverBmp) {
            DeleteObject(g_coverBmp);
            g_coverBmp = nullptr;
            g_coverBits = nullptr;
        }
        // Plain BI_RGB 32 bpp. A BI_BITFIELDS header with bV5AlphaMask was tried for the
        // band's dissolve and the compositor still ignored the alpha byte, so the extra
        // declaration buys nothing here.
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = mon.width();
        bi.bmiHeader.biHeight = -mon.height();   // top-down: row 0 is the top of the screen
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        g_coverBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!g_coverBmp || !bits) {
            g_coverBits = nullptr;
            g_coverW = g_coverH = 0;
            qWarning("COVER dib %dx%d failed", mon.width(), mon.height());
            return;
        }
        g_coverBits = static_cast<uint8_t *>(bits);
        g_coverW = mon.width();
        g_coverH = mon.height();
        g_coverStride = mon.width() * 4;
    }
    g_coverX = mon.x();
    g_coverY = mon.y();
    // One BitBlt from the desktop straight into the surface the cover is shown from.
    // The Qt grab this replaces cost 47-70 ms at 4K - it copies the frame into a
    // QPixmap, converts it to a QImage, converts that again, and then we copied it a
    // third time into the DIB - and while it ran the GUI thread could not present, so
    // the screen stood still for that long before the cover even appeared.
    HDC sdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(sdc);
    HGDIOBJ old = (sdc && mem) ? SelectObject(mem, g_coverBmp) : nullptr;
    if (old)
        BitBlt(mem, 0, 0, g_coverW, g_coverH, sdc, g_coverX, g_coverY, SRCCOPY);
    if (old)
        SelectObject(mem, old);
    if (mem)
        DeleteDC(mem);
    if (sdc)
        ReleaseDC(nullptr, sdc);
    g_bandSrc.clear();
    if (g_bandH > 0) {
        const size_t from = size_t(g_coverH - g_bandH) * g_coverStride;
        g_bandSrc.assign(g_coverBits + from, g_coverBits + size_t(g_coverH) * g_coverStride);
    }
    g_bandFade = 0;
    qInfo("SWAP grab %d ms, %d ms after the press", int(g.elapsed()),
          int(m_press.isValid() ? m_press.elapsed() : -1));
}

void VideoHost::beginSwap()
{
    // The chrome stays up until the cover is on screen - the cover is a picture of the
    // screen as it is now, controls included, and it dissolves out over the new state.
    // Hiding the controls *before* the grab was what an aspect-stretched cover needed,
    // and it made the cover differ from the screen the instant it appeared.
    m_press.start();
    // Queued rather than called here, because pictureReady() is answered by QML writing
    // the window's geometry, and that should not happen inside this call's stack.
    QMetaObject::invokeMethod(this, &VideoHost::grabPicture, Qt::QueuedConnection);
}

void VideoHost::armReveal()
{
    // The size the frame on screen is drawn into right now, in device pixels: the one
    // size that must *not* end the cover. Before the first swap of a run nothing has
    // reported it, so fall back on what the window says it is.
    m_swapFrom = m_presented.isEmpty()
        ? m_win->size() * m_win->devicePixelRatio() : m_presented;
    m_swapClock.start();
}

void VideoHost::notePresented(int w, int h, int n)
{
    m_presented = QSize(w, h);
    if (!m_settledPending)
        return;
    qInfo("SWAP present %dx%d frame %d, %d ms after the write", w, h, n,
          m_swapClock.elapsed());
    // The item only reports when the size its frame was drawn into changed, so this
    // is the first frame at a new rect. If it is still the rect the swap started from,
    // the change was a resize Qt had already queued before the write.
    if (m_presented == m_swapFrom)
        return;
    // The third frame at the new size, not the first or the second: `Present()` only
    // hands the frame to DWM. Measured on the way in, the first present at the new size
    // came 6-11 ms after the write and the screen still showed the previous, smaller
    // surface for a frame after that; with the frame now covering the whole window the
    // gap is not even black, it is transparent, so what shows through is the desktop.
    // One more vsync on top of the second frame is what puts the composed picture under
    // the cover before it comes down.
    if (n < 3)
        return;
    reveal();
}

void VideoHost::reveal()
{
    if (!m_settledPending)
        return;
    m_settled.stop();
    m_trans = false;
    emit transitioningChanged();
    settle();
}

void VideoHost::settle()
{
    if (!m_settledPending)
        return;
    m_settledPending = false;
    qInfo("SWAP settled %d ms after the write, %d ms after the press",
          m_swapClock.elapsed(), int(m_press.elapsed()));
    // Down to the task bar's strip in one step. For a layered window UpdateLayeredWindow
    // *is* the move, the resize and the repaint, all at once, so the film area goes back
    // to the live window in the same frame that leaves the strip covering the band. A
    // GDI cover could not do this: resizing it handed DWM a fresh empty surface, and
    // that empty frame went on screen first (measured 7 ms of black), which is why this
    // used to be a SetWindowRgn.
    if (m_bandIn && g_bandH > 0 && g_coverBmp) {
        // Down to the strip in one step, then the fade. bandStep() does both, so the
        // strip's first present and its last one are the same call.
        g_bandFade = 0;
        bandStep();
        m_band.start();
    } else {
        hideCurtain();
    }
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
        m_restoreMax = m_max;
        if (GetWindowPlacement(m_hwnd, &wp)) {
            m_restoreMax = m_restoreMax || wp.showCmd == SW_SHOWMAXIMIZED;
            const RECT &n = wp.rcNormalPosition;
            m_restoreRect = m_restoreMax
                ? QRect{ int(n.left), int(n.top), int(n.right - n.left), int(n.bottom - n.top) }
                : geometry();
        } else {
            m_restoreRect = geometry();
        }
        qInfo("SWAP enter from %d,%d %dx%d max=%d", m_restoreRect.x(), m_restoreRect.y(),
              m_restoreRect.width(), m_restoreRect.height(), m_restoreMax ? 1 : 0);
        m_fs = true;
        m_settledPending = true;
        m_bandIn = true;
        // The cover goes up first, and because it is a 1:1 copy of what is already on
        // screen, putting it there is not something you can see. Everything from here
        // to the reveal happens behind it.
        // One step, no animation. Growing the window in beats costs a 4K surface
        // re-allocation per step (measured 7-26 ms apart at 144 Hz - a stutter).
        // The style is not touched any more: WM_NCCALCSIZE keeps the client over the
        // caption in every state, so fullscreen is one landing write onto the monitor
        // rect, and the #A0A0A0 non-client repaint that used to follow a style flip
        // cannot happen because there is no flip.
        showCover();
        armReveal();
        // The tray goes behind the cover, before the landing: hiding it makes Explorer
        // recompute the work area, and doing that after the write costs a second
        // re-layout of a 4K surface - measured the first frame at the new size arriving
        // 60 ms after the write instead of 11.
        stashTaskbar();
        landOn(monitorRect());
        raiseCurtain();
        m_trans = true;
        emit transitioningChanged();
        // Only a backstop: reveal() is driven by the first new-size present. This
        // fires if the scene graph never takes a pass at the new size.
        m_settled.setInterval(120);
        emit resized();
        emit fullscreenChanged();
        // The tray flag is *not* handed back here. Restoring it 400 ms in put the
        // band over the bottom of the film while we were still fullscreen (measured
        // Shell_TrayWnd visible=True at 0,0 3840x2160), which reads as the desktop
        // showing through. It comes back with the way out, or with the destructor.
        return;
    }
    m_fs = false;
    m_settledPending = true;
    m_bandIn = false;
    qInfo("SWAP out to %d,%d %dx%d max=%d", m_restoreRect.x(), m_restoreRect.y(),
          m_restoreRect.width(), m_restoreRect.height(), m_restoreMax ? 1 : 0);
    m_trans = true;
    emit transitioningChanged();
    m_trayBeat.stop();
    if (m_restoreMax) {
        // The cover stays, but only for the writes. What it has to hide on this leg is
        // the task bar's band coming back; the caption no longer repaints because the
        // style never changes.
        // The band is not faded here, unlike the way in. Fading it needs the strip to
        // dissolve into the task bar underneath it, and per-pixel alpha does not do that
        // here: with the DIB declared ARGB (BI_BITFIELDS + bV5AlphaMask) and
        // AC_SRC_ALPHA, the strip still composited over black - measured the row going
        // 1.44, 1.15, 0.87, 0.58, 0.30, 0.01 and only then jumping to the task bar's
        // 214. Darkening it is the wrong direction on this leg because the strip holds
        // the film's own black and what has to come through is the bright band. So the
        // cover comes down in one piece and the band appears with it. See
        // presentCover().
        showCover();
        armReveal();
        // The order is the one the two-window build wrote down: the tray goes back
        // *before* the maximise, because the work area is whatever it is at the moment of
        // the call and the landing rect is read from it. Answering that twice is two
        // geometry steps.
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        restoreTaskbar();
        // Landed on the work area rather than handed to `ShowWindow(SW_MAXIMIZE)`: the
        // same reason the tile has, and it also means the rect Windows recorded as the
        // normal position while we were fullscreen never gets used, so the
        // SetWindowPlacement repair this replaced is gone with it.
        if (!m_maxFrom.isValid())
            m_maxFrom = m_restoreRect;
        m_max = true;
        m_swapClock.start();        // the trace's zero point is the write below
        landOn(workArea());
        m_settled.setInterval(200);
        emit resized();
        emit fullscreenChanged();
        return;
    }
    // A restore to a normal rect has no animation to borrow: the window would simply
    // appear at its old size with the previous surface parked in its top-left corner for
    // a frame or two (measured the content edge at 1280x732 of a 3840x2160 window), so
    // this leg keeps the cover and the render-driven reveal.
    showCover();
    armReveal();
    // Hide the tray for the way out too: left visible, its band lit up when the cover
    // came down and went under the shrinking window again - three events.
    stashTaskbar();
    landOn(m_restoreRect);
    m_settled.setInterval(200);
    emit resized();
    emit fullscreenChanged();
    m_trayBeat.start();
}

void VideoHost::toggleMaximized()
{
    if (!m_hwnd)
        return;
    if (m_fs) {
        toggleFullscreen();       // out of fullscreen goes back where it came from
        return;
    }
    // One landing write onto the work area, which is the route fullscreen already takes.
    // Going through `ShowWindow(SW_MAXIMIZE)` instead hands the window to the shell, and
    // the shell cross-fades it for ~210 ms with the desktop showing around both images;
    // no present of ours marks the end of that, so hiding it meant holding the cover for
    // 240 ms after the press - measured 325 ms from press to picture, 266 ms of which was
    // a frozen screen and one hard step. Landed this way the third frame at the new size
    // comes 12-29 ms after the write and the cover comes down on it: 96-108 ms.
    //
    // The cover still has to be up for the write itself: the client now covers what used
    // to be the frame, so the area the window grows into holds no painted pixels until Qt
    // presents at the new size, and what shows through them is the desktop.
    const bool maxed = m_max || IsZoomed(m_hwnd) != 0;
    m_settledPending = true;
    m_bandIn = false;
    m_press.start();
    fillCover();
    showCover();
    armReveal();
    m_trans = true;
    emit transitioningChanged();
    if (maxed) {
        // Win+Up and snap assist still put the window into the system's zoomed state, and
        // then the only copy of the rect to come back from is the placement's.
        QRect back = m_maxFrom;
        if (!back.isValid()) {
            WINDOWPLACEMENT wp{ uint32_t(sizeof(wp)) };
            if (GetWindowPlacement(m_hwnd, &wp))
                back = QRect{ wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                              wp.rcNormalPosition.right - wp.rcNormalPosition.left,
                              wp.rcNormalPosition.bottom - wp.rcNormalPosition.top };
        }
        m_max = false;
        landOn(back.isValid() ? back : geometry());
    } else {
        m_maxFrom = geometry();
        m_max = true;
        landOn(workArea());
    }
    qInfo("MAX maxed=%d zoom=%d rect=%d,%d %dx%d", maxed ? 1 : 0,
          IsZoomed(m_hwnd) != 0 ? 1 : 0, geometry().x(), geometry().y(),
          geometry().width(), geometry().height());
    // Only a backstop, like the fullscreen legs: a resize Qt never draws a frame for must
    // still hand the screen back.
    m_settled.setInterval(200);
    emit resized();
}

void VideoHost::landOn(const QRect &to)
{
    if (!m_hwnd)
        return;
    // SetWindowPos, not SetWindowPlacement. The placement write also *stores*
    // rcNormalPosition, so landing on the monitor through it left the window's normal
    // rect at 0,0 3840x2160 - measured, the restore button then brought the window back
    // at full screen. Dropping the zoom flag is part of this same write, and the frame
    // commit re-asks WM_NCCALCSIZE, whose answer depends on the zoomed state.
    // While the window is zoomed Windows ignores the rect of a SetWindowPos - measured,
    // the write went through and the window stayed at -8,-8 3856x2128 with only the band
    // changed. The zoom flag has to come off first, and it is taken off *without* moving
    // the window: ShowWindow(SW_RESTORE) lands it on the normal rect first, Qt draws that
    // frame too, and the reveal then waits for a second pass at the final size - measured
    // 71 ms after the write instead of 12.
    if (IsZoomed(m_hwnd)) {
        DWORD st = DWORD(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, LONG_PTR(st & ~DWORD(WS_MAXIMIZE)));
    }
    HWND after = m_fs ? (m_curtain ? m_curtain : HWND_TOPMOST) : HWND_NOTOPMOST;
    SetWindowPos(m_hwnd, after, to.x(), to.y(), to.width(), to.height(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    measureInsets();
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
        m_maxBeforeMini = m_max;
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
        m_max = m_maxBeforeMini;   // the rect coming back is the one mini replaced
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
