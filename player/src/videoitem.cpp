#include "videoitem.h"

#include "playercore.h"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QtQuick/qsgtexture_platform.h>

#include <QDebug>

#include <cmath>
#include <cstdio>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_d3d11.h>

#include <d3d11.h>

VideoItem::VideoItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // The picture is the bottom of the scene: the scrim, the bar and the buttons
    // are drawn over it in the same pass.
    setZ(-2);
    // Bisect switches for the minimise/restore crash (0xc0000005 in Qt6Core).
    // Each one removes exactly one interaction; the first arm that survives names
    // the suspect, so nothing here is a fallback path - it is a probe.
    mNoRender = qEnvironmentVariableIsSet("PLAYERUI_NORENDER");
    mNoSwap = qEnvironmentVariableIsSet("PLAYERUI_NOSWAP");
    mNoRelease = qEnvironmentVariableIsSet("PLAYERUI_NORELEASE");
    mNoContext = qEnvironmentVariableIsSet("PLAYERUI_NORNCTX");
    mNoRelRc = qEnvironmentVariableIsSet("PLAYERUI_NOREL_RC");
    mNoRelQtex = qEnvironmentVariableIsSet("PLAYERUI_NOREL_QTEX");
    mNoRelTex = qEnvironmentVariableIsSet("PLAYERUI_NOREL_TEX");
    std::printf("ARMS norender=%d noswap=%d norelease=%d nornctx=%d\n", mNoRender, mNoSwap,
                mNoRelease, mNoContext);
    std::fflush(stdout);
    // The push needs a frame tick of its own: the window has already resized, so
    // nothing would repaint it between the first and the last step. 4 ms is under
    // the 6.94 ms frame period at 144 Hz.
    m_ramp = new QTimer(this);
    m_ramp->setTimerType(Qt::PreciseTimer);
    m_ramp->setInterval(4);
    // Stopping stays on this thread: the render thread only clears the flag, and
    // the next tick sees that and stands the timer down.
    connect(m_ramp, &QTimer::timeout, this, [this] {
        if (!m_transition.loadRelaxed()) {
            m_ramp->stop();
            return;
        }
        update();
    });
}

VideoItem::~VideoItem()
{
    // The one place the render context is freed. It has to happen before
    // PlayerCore destroys the mpv core (render.h:118), and the engine is destroyed
    // before main's locals, so the item always goes first.
    releaseContext();
}

void VideoItem::beginTransition(const QRect &oldClientOnScreen, int ms)
{
    // Nothing rendered yet, or no window: there is no frame to push, so let the
    // resize through rather than show an empty rect for the ramp.
    if (!m_tex || !window())
        return;
    m_from = oldClientOnScreen;
    m_rampMs = qMax(1, ms);
    m_clock.start();
    m_pushSized = false;
    m_pushFrames = 0;
    m_transition.storeRelaxed(1);
    m_ramp->start();
}

void VideoItem::setCore(PlayerCore *core)
{
    if (m_core == core)
        return;
    m_core = core;
    emit coreChanged();
}

void VideoItem::componentComplete()
{
    QQuickItem::componentComplete();
    hookWindow();
}

void VideoItem::hookWindow()
{
    QQuickWindow *w = window();
    if (!w)
        return;
    // DirectConnection: these are render-thread signals and everything they call
    // has to stay on that thread, including the mpv render calls.
    // Deliberately NOT released when the scene graph stops. Measured: minimising and
    // restoring crashed with 0xc0000005 in Qt6Core every time, while minimising on
    // its own (four times, release included, all four steps logged) survived - so the
    // fault is in building a *second* render context on the same core after the
    // restore. And the RHI pointer never changes across a minimise/restore, so there
    // is nothing here that actually has to be rebuilt. The context now lives as long
    // as the item, and is freed in the destructor.
    connect(w, &QQuickWindow::sceneGraphInitialized, this, &VideoItem::ensureContext,
            Qt::DirectConnection);
    connect(w, &QQuickWindow::beforeRenderPassRecording, this, &VideoItem::renderFrame,
            Qt::DirectConnection);
    connect(w, &QQuickWindow::afterFrameEnd, this, [this] {
        if (m_rc && !mNoSwap)
            mpv_render_context_report_swap(m_rc);
    }, Qt::DirectConnection);
    // Reported once per size change, on the swap of the very pass that first drew
    // into the new texture - so it names the frame that is now on screen, not one
    // still queued behind it. Nothing is emitted on an ordinary frame, which keeps
    // this off the hot path: 144 presents a second would otherwise mean 144 cross-
    // thread calls. m_texW is only ever written here and in updatePaintNode, both on
    // this thread.
    connect(w, &QQuickWindow::frameSwapped, this, [this] {
        if (m_texW <= 0)
            return;
        if (m_texW != m_repW || m_texH != m_repH) {
            m_repW = m_texW;
            m_repH = m_texH;
            m_repN = 0;
        }
        if (m_repN >= 2)
            return;                       // past the second frame this says nothing new
        ++m_repN;
        if (m_repN == 1 && window())
            // The host waits for the *second* frame at the new size, so make sure there
            // is one: a paused film presents nothing on its own after the resize.
            QMetaObject::invokeMethod(window(), &QQuickWindow::update, Qt::QueuedConnection);
        emit presented(m_texW, m_texH, m_repN);
    }, Qt::DirectConnection);

}

void VideoItem::ensureContext()
{
    if (m_ctxTried || !m_core || !m_core->handle())
        return;
    m_ctxTried = true;
    if (mNoContext) {
        std::printf("ARM nornctx: render context not created\n");
        std::fflush(stdout);
        return;
    }
    QQuickWindow *w = window();
    QRhi *rhi = w ? w->rhi() : nullptr;
    if (!rhi || rhi->backend() != QRhi::D3D11) {
        qWarning("VIDEOITEM no D3D11 scene graph (backend=%d)", rhi ? int(rhi->backend()) : -1);
        return;
    }
    auto *nh = static_cast<QRhiD3D11NativeHandles *>(const_cast<QRhiNativeHandles *>(rhi->nativeHandles()));
    m_rhi = rhi;
    m_dev = static_cast<ID3D11Device *>(nh->dev);
    m_ctx = static_cast<ID3D11DeviceContext *>(nh->context);

    mpv_d3d11_init_params ip {};
    ip.device = m_dev;
    const char *api = MPV_RENDER_API_TYPE_D3D11;
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(api) },
        { MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &ip },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    const int r = mpv_render_context_create(&m_rc, m_core->handle(), p);
    if (r < 0 || !m_rc) {
        qWarning("VIDEOITEM render context failed: %s", mpv_error_string(r));
        m_rc = nullptr;
        return;
    }
    mpv_render_context_set_update_callback(m_rc, onUpdate, this);
    qInfo("VIDEOITEM ctx ok");
    // Nothing may be loaded before this: the libmpv VO fails with
    // "No render context set" and leaves the window black.
    QMetaObject::invokeMethod(this, &VideoItem::ready, Qt::QueuedConnection);
}

void VideoItem::onUpdate(void *ctx)
{
    auto *self = static_cast<VideoItem *>(ctx);
    self->m_pending.storeRelaxed(1);
    if (self->window())
        QMetaObject::invokeMethod(self->window(), &QQuickWindow::update, Qt::QueuedConnection);
}

void VideoItem::ensureTexture(int w, int h)
{
    if (m_tex && m_texW == w && m_texH == h)
        return;
    if (m_qtex) {
        delete m_qtex;
        m_qtex = nullptr;
    }
    if (m_tex) {
        m_tex->Release();
        m_tex = nullptr;
    }
    D3D11_TEXTURE2D_DESC d {};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &m_tex)) || !m_tex) {
        qWarning("VIDEOITEM texture create failed %dx%d", w, h);
        return;
    }
    m_texW = w;
    m_texH = h;
    m_qtex = QNativeInterface::QSGD3D11Texture::fromNative(
        m_tex, window(), QSize(w, h), QQuickWindow::TextureIsOpaque);
    if (m_qtex) {
        // A fresh texture is empty, and mpv only asks us to draw when there is a
        // *new* frame - while paused there is none, so the resized window stayed
        // black (measured: maximise and restore both leave the client area at
        // 0,0,0). Ask for the frame that is already there, and queue the extra
        // pass that draws it. While playing, this is the same hole - it just
        // closes itself within a frame, which is the flash.
        m_pending.storeRelaxed(1);
        QMetaObject::invokeMethod(window(), &QQuickWindow::update, Qt::QueuedConnection);
    }
    if (!m_qtex)
        qWarning("VIDEOITEM native import failed %dx%d", w, h);
}

void VideoItem::renderFrame()
{
    // During the push the texture keeps holding the frame under the finger: mpv is
    // not asked to draw, and the pending flag is left set so the live picture comes
    // straight back when the ramp ends.
    if (m_transition.loadRelaxed())
        return;
    if (mNoRender)
        return;
    if (!m_rc || !m_tex || !m_pending.testAndSetOrdered(true, false))
        return;
    mpv_d3d11_fbo fbo {};
    fbo.tex = m_tex;
    mpv_render_param p[] = {
        { MPV_RENDER_PARAM_D3D11_FBO, &fbo },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    mpv_render_context_render(m_rc, p);
}

QSGNode *VideoItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
    const int w = int(width() * dpr), h = int(height() * dpr);
    if (w <= 0 || h <= 0)
        return old;
    // Minimise + restore can hand back a different RHI. Everything cached below
    // belongs to the old one, so compare and rebuild rather than use a dangling
    // device: measured, the untouched path died with 0xc0000005 in Qt6Core on the
    // second restore.
    if (m_rc && window()->rhi() != m_rhi) {
        qInfo("VIDEOITEM rhi changed, rebuilding the context");
        releaseContext();
    }
    if (!m_rc)
        ensureContext();
    if (!m_dev)
        return old;
    const bool pushing = m_transition.loadRelaxed();
    // The push deliberately keeps drawing the *old* texture: that is the frame
    // under the finger. Recreating it at the new size would hand back a cleared
    // texture and the transition would show black.
    if (!pushing)
        ensureTexture(w, h);
    if (!m_qtex)
        return old;

    auto *node = static_cast<QSGSimpleTextureNode *>(old);
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setFiltering(QSGTexture::Linear);
        // The node owns the texture by default and deletes it in its destructor.
        // This class deletes it instead - in releaseContext and whenever the size
        // changes - so with the default left on, a minimise (which tears the scene
        // graph down) freed it once here and once there. That double free is the
        // 0xc0000005 in Qt6Core the bisect landed on: PLAYERUI_NORELEASE, the arm
        // that skips our delete, was the only one that survived the stress.
        node->setOwnsTexture(false);
        // No vertical flip: measured on the probe, mpv's D3D11 output is already
        // top-left origin like the scene graph expects, and the frame came out
        // upright with no transform.
    }
    node->setTexture(m_qtex);

    if (pushing) {
        const QPoint org = window()->position();
        const QRectF fromLocal(double(m_from.x() - org.x()), double(m_from.y() - org.y()),
                               double(m_from.width()), double(m_from.height()));
        if (!m_pushSized) {
            // The window has not taken the new size yet. Hold the frozen frame
            // exactly where it was: that way the first step of the push *is* the
            // old rect instead of 8 % into the ease, and the target is never
            // computed from a size Qt has not applied.
            if (int(width()) == m_from.width() && int(height()) == m_from.height()) {
                node->setRect(fromLocal);
                // A toggle that never lands would otherwise leave the picture
                // frozen and the ramp timer spinning for good.
                if (m_clock.elapsed() > 500) {
                    m_transition.storeRelaxed(0);
                    m_pending.storeRelaxed(1);
                }
                return node;
            }
            m_pushSized = true;
            m_rampClock.start();
        }
        const double t = qMin(1.0, m_rampClock.elapsed() / double(m_rampMs));
        const double e = 1.0 - std::pow(1.0 - t, 3.0);   // ease-out, like the system's
        const QRectF to(0.0, 0.0, width(), height());
        const QRectF r(fromLocal.x() + (to.x() - fromLocal.x()) * e,
                       fromLocal.y() + (to.y() - fromLocal.y()) * e,
                       fromLocal.width() + (to.width() - fromLocal.width()) * e,
                       fromLocal.height() + (to.height() - fromLocal.height()) * e);
        node->setRect(r);
        ++m_pushFrames;
        // Geometry, not brightness: the step count and the monotonic widening are
        // what say whether the push actually runs at the panel's rate.
        qInfo("PUSH %d t=%.3f rect=%d,%d %dx%d  win=%d,%d %dx%d", m_pushFrames, t,
              int(r.x()), int(r.y()), int(r.width()), int(r.height()),
              org.x(), org.y(), int(width()), int(height()));
        if (t >= 1.0) {
            m_transition.storeRelaxed(0);
            m_pending.storeRelaxed(1);
        }
        return node;
    }

    node->setRect(0, 0, width(), height());
    return node;
}

void VideoItem::releaseContext()
{
    if (mNoRelease) {
        std::printf("ARM norelease: skipped releasing the context\n");
        std::fflush(stdout);
        return;
    }
    // Breadcrumbs: the last one printed before the process vanishes is the step
    // that faulted. The three NOREL switches skip one step each.
    std::printf("REL 0 enter rc=%d qtex=%d tex=%d\n", m_rc ? 1 : 0, m_qtex ? 1 : 0,
                m_tex ? 1 : 0);
    std::fflush(stdout);
    if (m_rc && !mNoRelRc) {
        std::printf("REL 1 free render context\n");
        std::fflush(stdout);
        mpv_render_context_set_update_callback(m_rc, nullptr, nullptr);
        mpv_render_context_free(m_rc);
        m_rc = nullptr;
    }
    if (m_qtex && !mNoRelQtex) {
        std::printf("REL 2 delete qsg texture\n");
        std::fflush(stdout);
        delete m_qtex;
        m_qtex = nullptr;
    }
    if (m_tex && !mNoRelTex) {
        std::printf("REL 3 release d3d texture\n");
        std::fflush(stdout);
        m_tex->Release();
        m_tex = nullptr;
    }
    m_texW = m_texH = 0;
    m_ctxTried = false;
    // Nothing cached may outlive the RHI it came from.
    m_rhi = nullptr;
    m_dev = nullptr;
    m_ctx = nullptr;
}
