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
    QRect geometry() const;
    // Also called from QML: a second launch hands its file to this window and has
    // to be able to bring it back to the front.
    Q_INVOKABLE void activate();

    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    bool fullscreen() const { return m_fs; }
    QRect monitorRect() const;
    Q_INVOKABLE void toggleFullscreen();
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

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static VideoHost *s_self;

    HWND m_hwnd = nullptr;
    bool m_fs = false;
    bool m_mini = false;
    QRect m_preMiniRect;
    LONG m_savedStyle = 0;
    RECT m_savedRect{};
};
