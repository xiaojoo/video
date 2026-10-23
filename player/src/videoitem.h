#pragma once

#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QRect>
#include <QTimer>

class PlayerCore;
struct mpv_render_context;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
class QSGTexture;

// The picture, as one layer of the same window that draws the chrome. libmpv's
// D3D11 render API draws into a texture created on the window's own device, and
// the scene graph samples it, so video and UI land in one present. That is what
// removes the second window and with it the whole class of "the UI trails the
// picture" and "the new size shows one black frame" problems.
//
// Two measured constraints shape this file:
//   - the texture must be R8G8B8A8_UNORM. B8G8R8A8, its sRGB variant,
//     R10G10B10A2 and R16G16B16A16F all give a black sample here even though mpv
//     wrote them correctly (checked by copying the texture back), because this
//     Qt's native-texture import builds no usable view for them.
//   - the libmpv VO refuses to open without a render context, so nothing can be
//     loaded before ready() goes out.
// Qt instantiates a QML_ELEMENT type through QQmlPrivate::QQmlElement<T>, which
// cannot derive from a final class, so this one is not marked final.
class VideoItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PlayerCore *core READ core WRITE setCore NOTIFY coreChanged)
public:
    explicit VideoItem(QQuickItem *parent = nullptr);
    ~VideoItem() override;

    PlayerCore *core() const { return m_core; }
    void setCore(PlayerCore *core);

    // Freeze the frame that is on screen right now and push it from the rect it
    // occupied to the rect the window is about to take, over `ms` milliseconds.
    // Called from QML just before it asks the window to change state, with the
    // client rect in screen coordinates.
    //
    // This needs no screen capture and no second window: the frame is already a
    // texture we own, so the "grab came back black" and "two veils stacked into a
    // step" failures the earlier attempts died of cannot happen here. While the
    // push runs, mpv is not asked to render and the texture is not resized, so the
    // picture on screen is literally the frame under the finger at click time.
    Q_INVOKABLE void beginTransition(const QRect &oldClientOnScreen, int ms = 150);
    // True while the push owns the picture.
    bool transitioning() const { return m_transition.loadRelaxed(); }

    // Copy the frame that is on screen right now to `file`. The copy happens on the
    // render thread at the next present, so this returns before the file exists.
    // Used for the resume cover: the ~300 ms a film takes to open would otherwise
    // show as a black window.
    Q_INVOKABLE void requestSnapshot(const QString &file);
    // Hold the startup cover until the film's own picture is in the texture. Counting
    // render passes is not enough: measured on the 2160p film, the cover came off at
    // 320 ms and the first real frame landed at ~650 ms, so the black came back in the
    // middle. This samples a box in the middle of the frame once per pass.
    Q_INVOKABLE void watchForPicture();

signals:
    void coreChanged();
    void ready();
    // The size, in device pixels, of the first frame presented after the texture was
    // made at a new one, and how many frames have now gone over at that size. The
    // count is the point: a `Present()` only hands the frame to DWM, and measured on
    // the way into fullscreen the screen still showed the *old* surface parked in the
    // top-left corner of the enlarged window for one frame after the first present at
    // the new size (edge of the content at 1280x732 of a 3840x2160 window). Qt is
    // vsync-locked, so the third present means the first one has been on screen for a
    // frame - which is what the host waits for.
    void presented(int w, int h, int n);
    // Out of the render thread once the copy behind requestSnapshot() is on disk.
    void snapshotSaved(const QString &file, bool ok);
    // The sampled box has held a real picture on two passes in a row: the startup
    // cover can come off now.
    void pictureSeen();

protected:
    void componentComplete() override;
    QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;

private:
    void hookWindow();
    // All three run on the render thread.
    void ensureContext();
    void releaseContext();
    void renderFrame();
    void ensureTexture(int w, int h);
    void takeSnapshot();        // render thread
    void probePicture();        // render thread
    static void onUpdate(void *ctx);

    PlayerCore *m_core = nullptr;
    // The RHI we built the context against. Minimising and restoring the window
    // can hand back a different one, and every pointer cached below belongs to the
    // old one - measured, minimise + restore alone crashed the process with no key
    // press involved. Compared every frame, and the context rebuilt when it moved.
    class QRhi *m_rhi = nullptr;
    ID3D11Device *m_dev = nullptr;
    ID3D11DeviceContext *m_ctx = nullptr;
    ID3D11Texture2D *m_tex = nullptr;
    QSGTexture *m_qtex = nullptr;
    mpv_render_context *m_rc = nullptr;
    int m_texW = 0, m_texH = 0;
    // The size last reported through presented(), and how many frames have gone over
    // at it. Keeps the signal to two calls per size change instead of 144 per second.
    int m_repW = 0, m_repH = 0, m_repN = 0;
    bool m_ctxTried = false;
    // Bisect switches, read once from the environment (see the constructor).
    bool mNoRender = false;
    bool mNoSwap = false;
    bool mNoRelease = false;
    bool mNoContext = false;
    bool mNoRelRc = false;
    bool mNoRelQtex = false;
    bool mNoRelTex = false;
    QAtomicInteger<bool> m_pending { false };

    // The push. Written on the GUI thread just before the window changes state,
    // read on the render thread; the clock is started once per transition and only
    // ever read afterwards, so no lock is needed for it.
    QAtomicInteger<bool> m_transition { false };
    QRect m_from;
    int m_rampMs = 150;
    QElapsedTimer m_clock;        // from beginTransition: the resize deadline
    QElapsedTimer m_rampClock;    // from the first frame that sees the new size
    bool m_pushSized = false;
    QTimer *m_ramp = nullptr;
    int m_pushFrames = 0;

    // The resume cover. The flag is published after the path is written and cleared
    // before the copy starts, so the render thread never reads a half-set request.
    QAtomicInteger<bool> m_snapWanted { false };
    QString m_snapFile;
    ID3D11Texture2D *m_stage = nullptr;
    int m_stageW = 0, m_stageH = 0;
    // The picture probe: a small CPU-readable box off the middle of the frame, plus
    // how many passes in a row have looked non-empty.
    QAtomicInteger<bool> m_probeWanted { false };
    ID3D11Texture2D *m_probe = nullptr;
    int m_probeW = 0, m_probeH = 0;
    int m_probeOk = 0;
};
