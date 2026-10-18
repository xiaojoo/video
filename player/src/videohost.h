#pragma once

#include <QColor>
#include <QObject>
#include <QPixmap>
#include <QRect>

#include <windows.h>

// 切换那一刻抓下来的那帧画面，由 QML 盖在画面上溶解掉。抓的是桌面 DC 上宿主客户区
// 那一块：全屏时 grabWindow(宿主HWND) 拿回来的是全黑（实测退出那一拍黑 12 帧）。
class Snapshot final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int frame READ frame NOTIFY frameChanged)
public:
    int frame() const { return m_frame; }
    const QPixmap &pixmap() const { return m_px; }
    void set(QPixmap px)
    {
        m_px = std::move(px);
        ++m_frame;
        emit frameChanged();
    }

signals:
    void frameChanged();

private:
    QPixmap m_px;
    int m_frame = 0;
};

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
    // 切换前抓一帧；QML 等它真的进了纹理再动手换矩形。
    Q_PROPERTY(QObject *snapshot READ snapshotObj CONSTANT)
    QObject *snapshotObj() { return &m_snapObj; }
    Q_INVOKABLE void grabPicture();
    Q_INVOKABLE void releasePicture();
    Q_INVOKABLE void commitRect(int x, int y, int w, int h);
    // 移动之后界面窗真的按新尺寸画出了第一帧：撤幕窗、并告诉 QML 可以开始溶解。
    // 进和出都要走这里——退出没有幕窗，等幕窗信号会把 snapping 卡死。
    Q_INVOKABLE void noteSettled();
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
    // 界面窗已经按新尺寸画出第一帧（或 60 ms 兜底到点）。
    void settled();
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

    bool m_settledPending = false;
    Snapshot m_snapObj;
    HWND m_hwnd = nullptr;
    HWND m_chrome = nullptr;
    HWND m_curtain = nullptr;
    bool m_fs = false;
    bool m_mini = false;
    QRect m_preMiniRect;
    // Where fullscreen goes back to.
    QRect m_restoreRect;
};
