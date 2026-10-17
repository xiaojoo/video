#pragma once

#include <QObject>
#include <QRect>

#include <windows.h>

// A bare Win32 window handed to mpv through the `wid` option. Deliberately not
// a QWindow: Qt would clear the client area between mpv's presents, which shows
// up as flicker while resizing.
class VideoHost final : public QObject
{
    Q_OBJECT
public:
    explicit VideoHost(QObject *parent = nullptr);
    ~VideoHost() override;

    bool create(const QRect &rect);
    void destroy();

    HWND hwnd() const { return m_hwnd; }
    // The chrome has to move in the same Win32 batch as the host, or it keeps the
    // previous frame's rectangle through the transition.
    void setChrome(HWND ch) { m_chrome = ch; }
    QRect geometry() const;
    // Also called from QML: a second launch hands its file to this window and has
    // to be able to bring it back to the front.
    Q_INVOKABLE void activate();

    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    bool fullscreen() const { return m_fs; }
    Q_INVOKABLE QRect monitorRect() const;
    Q_INVOKABLE void toggleFullscreen();
    // Move both windows in one Win32 batch. A transition spends exactly one of these.
    Q_INVOKABLE void commitRect(int x, int y, int w, int h);

    // The transition is one window resize plus GPU transforms: stepping the real
    // rect means reallocating a 4K scene ~20 times, which reads as stutter. QML owns
    // the clock (it animates at the panel's refresh rate) and calls fsProgress().
    Q_PROPERTY(bool fsAnimating READ fsAnimating NOTIFY fsTransitionChanged)
    bool fsAnimating() const { return m_fsAnimating; }
    Q_PROPERTY(double fsScale READ fsScale NOTIFY fsProgressed)
    double fsScale() const { return m_fsScale; }
    Q_PROPERTY(int fsOffX READ fsOffX NOTIFY fsProgressed)
    int fsOffX() const { return m_fsOffX; }
    Q_PROPERTY(int fsOffY READ fsOffY NOTIFY fsProgressed)
    int fsOffY() const { return m_fsOffY; }
    Q_INVOKABLE void startFsTransition();
    Q_INVOKABLE void fsProgress(double p);
    Q_INVOKABLE void fsFinished();
    Q_INVOKABLE void toggleMini();
    Q_INVOKABLE void minimize();
    Q_PROPERTY(bool mini READ mini NOTIFY miniChanged)
    bool mini() const { return m_mini; }

    // The frameless host is the window the user actually drags, so the chrome
    // needs its rect to start a move or resize from.
    Q_PROPERTY(QRect rect READ geometry NOTIFY resized)
    Q_INVOKABLE void setRect(int x, int y, int w, int h);

signals:
    void resized();
    void fullscreenChanged();
    void miniChanged();
    void raised();
    void closeRequested();
    void fsTransitionChanged();
    void fsProgressed();

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static VideoHost *s_self;

    void moveBoth(int x, int y, int w, int h, HWND after = HWND_TOP);

    HWND m_hwnd = nullptr;
    HWND m_chrome = nullptr;
    bool m_fs = false;
    bool m_mini = false;
    QRect m_preMiniRect;
    // Where fullscreen goes back to.
    QRect m_restoreRect;
    QRect m_fsFrom;
    QRect m_fsTo;
    bool m_fsAnimating = false;
    bool m_fsCommitLate = false;
    double m_fsScale = 1.0;
    int m_fsOffX = 0;
    int m_fsOffY = 0;
};
