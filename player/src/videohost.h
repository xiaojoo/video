#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QTimer>


#include <windows.h>

class QWindow;

// Window management for the one and only window: the caption attributes, taking the
// frame off for fullscreen, the mini window's topmost band, and the cover that hides
// the swap. The fullscreen change is one step, and the step is hidden by a layered
// window holding a 1:1 copy of the monitor, handed to DWM in one UpdateLayeredWindow
// and taken back off when the window has really put three frames at the new size on the
// panel - the route the two-window build measured its way to (docs/最大化过渡.md).
class VideoHost final : public QObject
{
    Q_OBJECT
public:
    explicit VideoHost(QObject *parent = nullptr);
    ~VideoHost() override;

    // Take over the QML window's HWND. Everything here is measured off it at call
    // time, so a DPI change or a second monitor needs no extra handling.
    void adopt(QWindow *win);

    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    // True from the click that starts a transition until the window has been sitting
    // at its final size for a beat. The frame-dump showed the chrome drawn twice for
    // ~160 ms after the landing (two scrub lines, two icon rows, a doubled subtitle):
    // DWM stretches the previous bitmap while the bottom-anchored chrome has already
    // moved, so the two copies land on different rows. The chrome keeps out of the way
    // while this is true.
    Q_PROPERTY(bool transitioning READ transitioning NOTIFY transitioningChanged)
    bool transitioning() const { return m_trans; }
    bool fullscreen() const { return m_fs; }
    Q_INVOKABLE QRect monitorRect() const;
    Q_INVOKABLE void toggleFullscreen();
    // The middle button of the top row: the system's own maximise one way, and the way
    // out of it the other. No rect is written by hand, which is what keeps the shell's
    // animation on both legs.
    Q_INVOKABLE void toggleMaximized();
    // Fills the cover's surface with what is on the monitor at this instant.
    Q_INVOKABLE void grabPicture();
    // Start of a swap: grabs the frame, and `pictureReady()` is what lets QML ask for
    // the geometry change. The grab goes on the next turn of the event loop rather
    // than in this call so the signal does not arrive inside the caller's stack.
    Q_INVOKABLE void beginSwap();
    // The picture just went on screen at device size w x h, and this is the n-th frame
    // presented at that size. The third one ends the cover: the first only proves the
    // frame reached DWM, not that the panel has it.
    Q_INVOKABLE void notePresented(int w, int h, int n);
    Q_INVOKABLE void toggleMini();
    Q_INVOKABLE void minimize();
    // A second launch hands its file to this window and has to be able to bring it
    // back to the front.
    Q_INVOKABLE void activate();

    Q_PROPERTY(bool mini READ mini NOTIFY miniChanged)
    bool mini() const { return m_mini; }

    Q_PROPERTY(QRect rect READ geometry NOTIFY resized)
    Q_INVOKABLE void setRect(int x, int y, int w, int h);
    QRect geometry() const;
    // The picture area. With the caption on, the window rect is not the picture,
    // and everything that sizes the *video* (the hairline snap, mini) speaks in
    // client terms.
    QRect clientGeometry() const;
    void setClientSize(int w, int h);
    // The window fills the screen's work area. That is now our own state rather than
    // the system's zoomed one - see m_max - and it is what decides whether the top
    // strip may drag the window and whether the edge bands may resize it: a maximised
    // window that slides under the cursor instead of snapping back is a bug.
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY resized)
    bool zoomed() const { return m_max || (m_hwnd && IsZoomed(m_hwnd) != 0); }

signals:
    void pictureReady();
    void resized();
    void fullscreenChanged();
    void transitioningChanged();
    void settled();
    void miniChanged();

private:
    // Read the invisible border back off the real client, for the code that sizes the
    // picture in client terms (mini, the hairline snap).
    void measureInsets();
    // One Win32 step onto `to`: leave the zoomed state, land on the rect, and take or
    // give back the topmost band in the same call.
    void landOn(const QRect &to);
    // The work area of the monitor this window is on - what a maximised window covers.
    // Read every time rather than cached: it moves when the task bar hides.
    QRect workArea() const;
    // Take the task bar out of the way for the duration of a transition, so its
    // bright band is not left to appear or disappear on the shell's own clock.
    void stashTaskbar();
    void restoreTaskbar();
    // The strip over the task bar's row. It has to be a plain Win32 window filled by
    // GDI and composited (UpdateWindow + DwmFlush) before anything moves, and it is
    // faded out by settle(), the same signal that ends the transition.
    void showCover();
    // Fills the cover's surface from the monitor as it is right now, without showing
    // it. `grabPicture()` is this plus the signal QML waits on.
    void fillCover();
    void bandStep();
    void raiseCurtain();
    void hideCurtain();
    void settle();
    // Remembers the size a presented frame currently has, so the first present at any
    // other size is the one that ends the cover. Call it right before the write.
    void armReveal();
    // Take the cover off: ends the transition and fades the band. Driven by the first
    // presented frame at the new size, with the timer above as the backstop.
    void reveal();

    // Only one window now, so the subclass is the only way to answer
    // WM_GETMINMAXINFO: Qt owns the window procedure.
    static VideoHost *s_self;
    static LRESULT CALLBACK subProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // The bottom band's 90 ms fade out on the way into fullscreen.
    QTimer m_band;
    bool m_bandIn = false;
    // Re-asserts the cover while a transition is in flight: Explorer re-shows the
    // task bar ~170 ms into one and puts it at the head of the topmost band, i.e.
    // *over* the cover (measured the bottom 48 px going 4.7 -> 213.9 for 416 ms).
    QTimer m_curtainBeat;
    // The tray goes back to "visible" a beat after the picture lands: the film is
    // topmost and covers the band, so it stays dark anyway, and nothing is left
    // hidden if the process dies later in the film.
    QTimer m_trayBeat;
    // The backstop: a resize that Qt never draws a frame for (the render loop can be
    // idle) must still hand the screen back. `transitioning` is cleared and the cover
    // taken off by reveal(), which the third new-size present normally calls long
    // before this fires.
    QTimer m_settled;
    bool m_trans = false;
    bool m_settledPending = false;
    // What size the last frame that reached the screen was drawn into, and what that
    // size was at the moment the geometry was written. Two frames at any other size is
    // the picture at its new rect on the panel, and that is when the cover comes down.
    QSize m_presented;
    QSize m_swapFrom;
    // From the write to the reveal, for the trace.
    QElapsedTimer m_swapClock;
    // From the press that started the swap, so the grab and the cover are told apart.
    QElapsedTimer m_press;

    QWindow *m_win = nullptr;
    HWND m_hwnd = nullptr;
    bool m_fs = false;
    bool m_mini = false;
    QRect m_preMiniRect;
    // Where fullscreen goes back to.
    QRect m_restoreRect;
    // The tray window we hid for the duration, or null when it was already out of
    // the way. Held as a handle so leaving shows back exactly what was taken.
    HWND m_taskbarHwnd = nullptr;
    // Owned by this window, so it goes with it; only the bitmap is freed by hand.
    HWND m_curtain = nullptr;
    // Fullscreen has to come back to the maximised state when the user was
    // maximised going in, because the maximised rect is not a normal position.
    bool m_restoreMax = false;
    // Our own maximised state. `ShowWindow(SW_MAXIMIZE)` hands the window to the shell,
    // which cross-fades it for ~210 ms with the desktop showing around the two images;
    // nothing we present marks the end of that, so hiding it cost a 240 ms hold. Landing
    // on the work area with one write has no animation to hide - the same route
    // fullscreen already takes - so the state is ours to remember instead.
    bool m_max = false;
    // The rect the tile comes back from.
    QRect m_maxFrom;
    // Mini replaces whatever rect the window had and puts it back on the way out, so the
    // maximised state has to survive that round trip rather than be read off the rect.
    bool m_maxBeforeMini = false;
    // l, t, r, b of the native frame; all zero while frameless.
    RECT m_insets{};
};
