#pragma once

#include <QColor>
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
    // Move both windows in one Win32 batch: the chrome and the host have to land on
    // the same frame, or the UI visibly trails the picture.
    Q_INVOKABLE void commitRect(int x, int y, int w, int h);
    Q_INVOKABLE void toggleMini();
    Q_INVOKABLE void minimize();
    Q_PROPERTY(bool mini READ mini NOTIFY miniChanged)
    bool mini() const { return m_mini; }

    // The frameless host is the window the user actually drags, so the chrome
    // needs its rect to start a move or resize from.
    Q_PROPERTY(QRect rect READ geometry NOTIFY resized)
    Q_INVOKABLE void setRect(int x, int y, int w, int h);

    // The dissolve's colour and strength, owned here because the Win32 curtain has
    // to be painted with exactly the same values as the QML layer - if they differ,
    // the curtain's arrival is a visible step. QML binds to these.
    Q_PROPERTY(QColor dipColor READ dipColor CONSTANT)
    QColor dipColor() const { return QColor(11, 14, 19); }
    Q_PROPERTY(double dipAlpha READ dipAlpha CONSTANT)
    double dipAlpha() const { return 0.72; }

signals:
    void resized();
    void fullscreenChanged();
    void miniChanged();
    void raised();
    void closeRequested();

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static VideoHost *s_self;

    void moveBoth(int x, int y, int w, int h, HWND after = HWND_TOP);
    // The dissolve's trough needs the curtain: after the batch move the chrome has
    // not painted the area it just grew into, and until it does (measured ~14 ms,
    // two frames at 144 Hz) the end-size picture shows through it. A plain Win32
    // window is filled by GDI inside the same call, so the ring is never exposed.
    // It covers outer minus inner so it never doubles up with the QML veil.
    void showCurtain(const QRect &outer, const QRect &inner);
    void endCurtain();

    HWND m_hwnd = nullptr;
    HWND m_chrome = nullptr;
    HWND m_curtain = nullptr;
    bool m_fs = false;
    bool m_mini = false;
    QRect m_preMiniRect;
    // Where fullscreen goes back to.
    QRect m_restoreRect;
};
