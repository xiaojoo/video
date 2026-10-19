#pragma once

#include <QObject>
#include <QPixmap>
#include <QRect>
#include <QTimer>

#include <windows.h>

class QWindow;

// The frame `grabPicture()` takes, handed to QML's Image through the `image://snap`
// provider. The frame number is the cache buster: the URL has to change or QML
// treats a repeat grab as a cache hit and never re-reads it.
class Snapshot final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int frame READ frame NOTIFY frameChanged)
public:
    int frame() const { return m_frame; }
    const QPixmap &pixmap() const { return m_px; }
    void set(QPixmap px)
    {
        const bool filled = !px.isNull();
        m_px = std::move(px);
        // Only a real grab moves the frame number: the release at the end of the
        // dissolve would otherwise make every Image reload onto an empty one and log
        // a "failed to get image from provider" for each swap.
        if (filled) {
            ++m_frame;
            emit frameChanged();
        }
    }

signals:
    void frameChanged();

private:
    QPixmap m_px;
    int m_frame = 0;
};

// Window management for the one and only window: the caption attributes, taking the
// frame off for fullscreen, the mini window's topmost band, and the frame that covers
// the swap. The fullscreen change is one step (no system animation) and the step is
// hidden by a snapshot of the picture laid at the end rect, dissolved away when the
// window has really presented a frame at that size - the route the two-window build
// measured its way to (docs/最大化过渡.md).
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
    // The frame the transition covers the swap with. QML grabs, waits until the
    // Image really has the texture, and only then calls toggleFullscreen - grabbing
    // and moving in the same tick would put the old frame's black underneath.
    Q_PROPERTY(QObject *snapshot READ snapshotObj CONSTANT)
    QObject *snapshotObj() { return &m_snapObj; }
    Q_INVOKABLE void grabPicture();
    // Start of a swap: puts the chrome out of the way and grabs two frames later.
    // QML waits for the Image to report Ready and then calls toggleFullscreen().
    Q_INVOKABLE void beginSwap();
    Q_INVOKABLE void releasePicture();
    // Called from the window's frameSwapped: the first frame at the new size is on
    // screen, so the snapshot may start dissolving.
    Q_INVOKABLE void noteSettled();
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
    bool zoomed() const { return m_hwnd && IsZoomed(m_hwnd) != 0; }

signals:
    void resized();
    void fullscreenChanged();
    void transitioningChanged();
    void settled();
    void miniChanged();

private:
    // Put the native caption on or take it off. The frame change has to be
    // committed (SWP_FRAMECHANGED) before the insets can be read back.
    void setFramed(bool on);
    void measureInsets();
    // One Win32 step onto `to`: leave the zoomed state, land on the rect, and take or
    // give back the topmost band in the same call.
    void landOn(const QRect &to);
    // Take the task bar out of the way for the duration of a transition, so its
    // bright band is not left to appear or disappear on the shell's own clock.
    void stashTaskbar();
    void restoreTaskbar();
    // The frame that covers the surface re-allocation. A QML item cannot do this -
    // it is drawn into the very surface being re-made - so it is a plain Win32 window
    // filled by GDI, composited (UpdateWindow + DwmFlush) before the move and taken
    // away by noteSettled, the same signal that starts the dissolve.
    void showCurtain(const QRect &rect);
    void hideCurtain();

    // Only one window now, so the subclass is the only way to answer
    // WM_GETMINMAXINFO: Qt owns the window procedure.
    static VideoHost *s_self;
    static LRESULT CALLBACK subProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // Single-shot, and each one only *completes* a state m_fs already holds, so a
    // missed timer leaves a usable window and the next F still works.
    // The tray goes back to "visible" a beat after the picture lands: the film is
    // topmost and covers the band, so it stays dark anyway, and nothing is left
    // hidden if the process dies later in the film.
    QTimer m_trayBeat;
    // Fires the grab once the chrome-less frame is on screen.
    QTimer m_snapArm;
    // Clears `transitioning` a beat after the geometry has stopped changing, so the
    // chrome comes back behind the snapshot dissolve instead of inside it.
    QTimer m_settled;
    bool m_trans = false;
    Snapshot m_snapObj;
    bool m_settledPending = false;

    QWindow *m_win = nullptr;
    HWND m_hwnd = nullptr;
    bool m_fs = false;
    bool m_mini = false;
    bool m_framed = false;
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
    // l, t, r, b of the native frame; all zero while frameless.
    RECT m_insets{};
};
