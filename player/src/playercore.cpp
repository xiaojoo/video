#include "playercore.h"

#include "bluray.h"
#include "library.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <mpv/client.h>

#include <cstring>

PlayerCore::PlayerCore(QObject *parent) : QObject(parent)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        return;

    auto set = [this](const char *k, const char *v) { mpv_set_option_string(m_mpv, k, v); };
    // The shell no longer hands mpv a window to draw into: VideoItem asks libmpv's
    // D3D11 render API to draw into a texture on the window's own device, and the
    // scene graph composites that texture under the chrome. So no `wid`, no
    // `force-window`, and the fullscreen state is purely the shell's.
    set("vo", "libmpv");
    set("gpu-api", "d3d11");
    set("hwdec", "d3d11va-copy");
    set("terminal", getenv("PLAYERUI_MPV_LOG") ? "yes" : "no");
    set("keep-open", "yes");
    // mpv paints subtitles into the picture, i.e. underneath our scrim, so
    // they have to be lifted above it. The margin is in OSD units referenced to
    // a 720-high window and mpv scales it with the window height, so a constant
    // lands the subtitle box at 0.743*h at any size -- just clear of the scrim's
    // top edge (measured: 19 px clear at 723, 14 at 485, 11 at 360).
    set("sub-margin-y", "185");
    // With the scrim no longer darkening them, mpv's own outline is all that
    // separates the glyphs from a bright picture; a 1 px shadow adds 1239 dark
    // pixels of separation around the same text.
    set("sub-shadow-offset", "1");
    set("input-default-bindings", "no");
    set("input-vo-keyboard", "no");

    if (mpv_initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE);
    // NONE = "tell me it changed, I'll read it myself". Requesting NODE_ARRAY
    // here is refused by mpv, and an unchecked return code made that silent.
    const int trobs = mpv_observe_property(m_mpv, 0, "track-list", MPV_FORMAT_NONE);
    if (trobs < 0)
        qWarning("observe track-list -> %s", mpv_error_string(trobs));
    mpv_observe_property(m_mpv, 0, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "video-codec", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "vo-driven", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "current-vo", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "fullscreen", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "video-params/aspect", MPV_FORMAT_DOUBLE);

    // The callback runs on an mpv thread, so it may only hand work to the GUI
    // thread. QPointer guards against a queued drain outliving the core.
    auto *guard = new QPointer<PlayerCore>(this);
    m_guard = guard;
    mpv_set_wakeup_callback(
        m_mpv,
        [](void *ctx) {
            auto *g = static_cast<QPointer<PlayerCore> *>(ctx);
            if (g && *g)
                QMetaObject::invokeMethod(*g, [g] { if (*g) g->data()->drainEvents(); }, Qt::QueuedConnection);
        },
        guard);
}

PlayerCore::~PlayerCore()
{
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    delete static_cast<QPointer<PlayerCore> *>(m_guard);
    m_guard = nullptr;
}

void PlayerCore::drainEvents()
{
    if (!m_mpv)
        return;
    while (true) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            onPropertyChange(ev->data);
            break;
        case MPV_EVENT_FILE_LOADED:
            m_hasMedia = true;
            emit hasMediaChanged();
            refreshChapters();
            refreshTracks();
            break;
        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(ev->data);
            if (m_blurayPending && end && end->error != 0 && !m_blurayFallback.isEmpty()) {
                const QString clip = m_blurayFallback;
                m_blurayPending = false;
                m_blurayFallback.clear();
                qInfo("BLURAY bd:// refused (error %d), falling back to %s",
                      end->error, qPrintable(clip));
                const QByteArray u = clip.toUtf8();
                const char *args[] = {"loadfile", u.constData(), "replace", nullptr};
                mpv_command(m_mpv, args);
                break;
            }
            m_blurayPending = false;
            m_hasMedia = false;
            m_chapterStarts.clear();
            m_chapterTitles.clear();
            m_audioTracks.clear();
            m_subTracks.clear();
            emit hasMediaChanged();
            emit chapterChanged();
            emit tracksChanged();
            break;
        }
        default:
            break;
        }
    }
}

void PlayerCore::onPropertyChange(void *propPtr)
{
    auto *p = static_cast<mpv_event_property *>(propPtr);
    if (!p || !p->name)
        return;

    const char *n = p->name;
    if (!std::strcmp(n, "track-list")) {
        // Read back through the property rather than the event payload: the shape
        // is the same one refreshTracks() already knows how to walk.
        refreshTracks();
        return;
    }
    if (p->format == MPV_FORMAT_NONE) {
        // Property currently unavailable (e.g. time-pos before the first frame).
        return;
    }
    if (!std::strcmp(n, "time-pos") && p->format == MPV_FORMAT_DOUBLE) {
        m_position = *static_cast<double *>(p->data);
        emit positionChanged();
    } else if (!std::strcmp(n, "duration") && p->format == MPV_FORMAT_DOUBLE) {
        m_duration = *static_cast<double *>(p->data);
        emit durationChanged();
    } else if (!std::strcmp(n, "fullscreen") && p->format == MPV_FORMAT_FLAG) {
        m_fsActive = *static_cast<int *>(p->data) != 0;
        qInfo("fsActive -> %d", m_fsActive ? 1 : 0);
        emit fullscreenActiveChanged();
    } else if (!std::strcmp(n, "pause") && p->format == MPV_FORMAT_FLAG) {
        m_paused = *static_cast<int *>(p->data) != 0;
        qInfo("pause = %d", m_paused ? 1 : 0);
        emit pausedChanged();
    } else if (!std::strcmp(n, "volume") && p->format == MPV_FORMAT_DOUBLE) {
        m_volume = *static_cast<double *>(p->data);
        emit volumeChanged();
    } else if (!std::strcmp(n, "speed") && p->format == MPV_FORMAT_DOUBLE) {
        m_speed = *static_cast<double *>(p->data);
        emit speedChanged();
    } else if (!std::strcmp(n, "video-params/aspect") && p->format == MPV_FORMAT_DOUBLE) {
        const double a = *static_cast<double *>(p->data);
        if (qAbs(a - m_videoAspect) > 1e-6) {
            m_videoAspect = a;
            qInfo("ASPECT %.6f", a);
            emit videoAspectChanged();
        }
    } else if (!std::strcmp(n, "vo-driven") && p->format == MPV_FORMAT_FLAG) {
        m_voDriven = *static_cast<int *>(p->data) ? "yes" : "no";
        emit voDrivenChanged();
    } else if (!std::strcmp(n, "chapter") && p->format == MPV_FORMAT_INT64) {
        m_chapter = int(*static_cast<int64_t *>(p->data));
        m_chapterTitle = m_chapterTitles.value(m_chapter);
        emit chapterChanged();
    } else if (p->format == MPV_FORMAT_STRING) {
        const QString v = *static_cast<char **>(p->data) ? QString::fromUtf8(*static_cast<char **>(p->data)) : QString();
        if (!std::strcmp(n, "media-title")) {
            m_mediaTitle = v;
            emit mediaTitleChanged();
        } else if (!std::strcmp(n, "hwdec-current")) {
            m_hwdecCurrent = v;
            emit hwdecCurrentChanged();
        } else if (!std::strcmp(n, "video-codec")) {
            m_videoCodec = v;
            emit videoCodecChanged();
        } else if (!std::strcmp(n, "current-vo")) {
            m_voDriven = v;
            emit voDrivenChanged();
        }
    }
}

void PlayerCore::refreshChapters()
{
    m_chapterStarts.clear();
    m_chapterTitles.clear();
    if (!m_mpv)
        return;

    mpv_node node;
    if (mpv_get_property(m_mpv, "chapter-list", MPV_FORMAT_NODE, &node) < 0)
        return;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num; ++i) {
            mpv_node &item = node.u.list->values[i];
            double t = 0.0;
            QString title;
            if (item.format == MPV_FORMAT_NODE_MAP) {
                for (int k = 0; k < item.u.list->num; ++k) {
                    const char *key = item.u.list->keys[k];
                    mpv_node &val = item.u.list->values[k];
                    if (!std::strcmp(key, "time") && val.format == MPV_FORMAT_DOUBLE)
                        t = val.u.double_;
                    else if (!std::strcmp(key, "title") && val.format == MPV_FORMAT_STRING)
                        title = QString::fromUtf8(val.u.string);
                }
            }
            m_chapterStarts.append(t);
            m_chapterTitles.append(title);
        }
    }
    mpv_free_node_contents(&node);
    QString dbg;
    for (int i = 0; i < m_chapterStarts.size(); ++i)
        dbg += QStringLiteral(" [%1:%2]").arg(m_chapterStarts[i].toDouble(), 0, 'f', 3)
                                          .arg(m_chapterTitles.value(i));
    qInfo("chapters n=%d dur=%.3f%s", m_chapterStarts.size(), m_duration, qPrintable(dbg));
    emit chapterChanged();
}

void PlayerCore::refreshTracks()
{
    m_audioTracks.clear();
    m_subTracks.clear();
    if (m_mpv) {
        mpv_node node;
        if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &node) == 0) {
            if (node.format == MPV_FORMAT_NODE_ARRAY) {
                for (int i = 0; i < node.u.list->num; ++i) {
                    mpv_node &item = node.u.list->values[i];
                    if (item.format != MPV_FORMAT_NODE_MAP)
                        continue;
                    QVariantMap t;
                    for (int k = 0; k < item.u.list->num; ++k) {
                        const char *key = item.u.list->keys[k];
                        mpv_node &val = item.u.list->values[k];
                        if (!std::strcmp(key, "id") && val.format == MPV_FORMAT_INT64)
                            t.insert(u"id"_qs, double(val.u.int64));
                        else if (!std::strcmp(key, "type") && val.format == MPV_FORMAT_STRING)
                            t.insert(u"type"_qs, QString::fromUtf8(val.u.string));
                        else if (!std::strcmp(key, "title") && val.format == MPV_FORMAT_STRING)
                            t.insert(u"title"_qs, QString::fromUtf8(val.u.string));
                        else if (!std::strcmp(key, "lang") && val.format == MPV_FORMAT_STRING)
                            t.insert(u"lang"_qs, QString::fromUtf8(val.u.string));
                        else if (!std::strcmp(key, "codec") && val.format == MPV_FORMAT_STRING)
                            t.insert(u"codec"_qs, QString::fromUtf8(val.u.string));
                        else if (!std::strcmp(key, "selected") && val.format == MPV_FORMAT_FLAG)
                            t.insert(u"selected"_qs, val.u.flag != 0);
                    }
                    const QString type = t.value(u"type"_qs).toString();
                    // Video is not what either of these two menus is for, and an
                    // embedded cover art track arrives as a video one.
                    if (type != u"audio"_qs && type != u"sub"_qs)
                        continue;
                    const bool sub = type == u"sub"_qs;
                    QString label = t.value(u"title"_qs).toString();
                    if (label.isEmpty())
                        label = t.value(u"lang"_qs).toString();
                    if (label.isEmpty()) {
                        // Most rips carry neither a title nor a language, and
                        // "音轨 1 / 音轨 2" tells you nothing to choose on. The
                        // codec does: that pair is usually the difference between
                        // a stereo track and the lossless one.
                        const QString codec = t.value(u"codec"_qs).toString();
                        label = QStringLiteral("%1 %2%3").arg(sub ? u"字幕"_qs : u"音轨"_qs)
                                                          .arg(t.value(u"id"_qs).toInt())
                                                          .arg(codec.isEmpty() ? QString()
                                                                               : u" · " + codec);
                    }
                    QVariantMap row;
                    row.insert(u"id"_qs, t.value(u"id"_qs));
                    row.insert(u"label"_qs, label);
                    row.insert(u"selected"_qs, t.value(u"selected"_qs).toBool());
                    (sub ? m_subTracks : m_audioTracks).append(row);
                }
            }
            mpv_free_node_contents(&node);
        }
    }
    // Only worth offering once there is something to switch off to.
    bool subOn = false;
    for (const QVariant &v : std::as_const(m_subTracks))
        subOn = subOn || v.toMap().value(u"selected"_qs).toBool();
    if (!m_subTracks.isEmpty())
        m_subTracks.prepend(QVariantMap { { u"id"_qs, 0.0 }, { u"label"_qs, u"关闭"_qs },
                                          { u"selected"_qs, !subOn } });
    QString dbg;
    for (const QVariant &v : std::as_const(m_audioTracks))
        dbg += QStringLiteral(" a[%1]").arg(v.toMap().value(u"label"_qs).toString());
    for (const QVariant &v : std::as_const(m_subTracks))
        dbg += QStringLiteral(" s[%1%2]").arg(v.toMap().value(u"label"_qs).toString(),
                                              v.toMap().value(u"selected"_qs).toBool() ? u"*"_qs : u""_qs);
    qInfo("TRACKS audio=%d sub=%d%s", m_audioTracks.size(), m_subTracks.size(), qPrintable(dbg));
    emit tracksChanged();
}

void PlayerCore::selectTrack(const QString &kind, int id)
{
    if (!m_mpv)
        return;
    const QByteArray n = (kind == u"sub"_qs ? "sid" : "aid");
    int64_t v = id;
    const int rc = mpv_set_property(m_mpv, n.constData(), MPV_FORMAT_INT64, &v);
    qInfo("TRACK %s=%d rc=%d", n.constData(), id, rc);
    // The selection itself comes back through the track-list observation, so the
    // menu's dot is always what the engine says, not what we asked for.
}

void PlayerCore::load(const QString &url)
{
    if (!m_mpv)
        return;
    // The libmpv VO cannot come up without a render context - it answers
    // "No render context set" and leaves the window black - so a load issued
    // before VideoItem is ready is remembered and replayed instead.
    if (!m_renderReady) {
        m_pendingUrl = url;
        return;
    }
    // The scrub preview keys its cache on this, so it has to be the exact url
    // mpv is being asked for.
    // The shell hands over a bare path from the catalogue or a file:// url from
    // the command line. Normalise first: the catalogue, the strip cache and the
    // duration write-back all key on the same string.
    const QString local = QFileInfo::exists(url) ? url : QUrl(url).toLocalFile();
    const QString key = local.isEmpty() ? url : local;
    if (m_mediaUrl != key) {
        m_mediaUrl = key;
        emit mediaUrlChanged();
    }
    const QFileInfo fi(key);
    if (fi.isDir() && bluray::isDisc(fi.absoluteFilePath())) {
        loadBluray(fi.absoluteFilePath());
        return;
    }
    m_blurayPending = false;
    m_blurayFallback.clear();
    if (!m_discTitle.isEmpty()) {
        m_discTitle.clear();
        emit mediaTitleChanged();
    }
    const QByteArray u = url.toUtf8();
    const char *args[] = {"loadfile", u.constData(), "replace", nullptr};
    const int rc = mpv_command(m_mpv, args);
    if (rc < 0)
        emit loadFailed(QString::fromUtf8(mpv_error_string(rc)));
}

void PlayerCore::loadBluray(const QString &discDir)
{
    // Same estimator the catalogue card uses, so the header and the library
    // agree on what the disc is called.
    QString title;
    int year = 0;
    const QFileInfo di(discDir);
    if (!Library::parseName(di.fileName(), di.dir().dirName(), &title, &year) || title.isEmpty())
        title = di.fileName();
    m_discTitle = title;
    emit mediaTitleChanged();

    m_blurayFallback = bluray::clipPlaylist(discDir,
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1String("/disc"));
    const QString dev = QDir::toNativeSeparators(discDir);
    const int prc = mpv_set_property_string(m_mpv, "bluray-device", dev.toUtf8().constData());
    const char *args[] = {"loadfile", "bd://", "replace", nullptr};
    const int rc = mpv_command(m_mpv, args);
    // loadfile only reports that the request was accepted; whether libbluray
    // could read the disc shows up later as END_FILE with an error.
    m_blurayPending = (prc >= 0 && rc >= 0);
    qInfo("BLURAY bd:// device=%s prop_rc=%d load_rc=%d fallback=%s",
          qPrintable(dev), prc, rc, qPrintable(m_blurayFallback));
    if (!m_blurayPending) {
        if (m_blurayFallback.isEmpty())
            emit loadFailed(QString::fromUtf8(mpv_error_string(rc)));
    }
}

void PlayerCore::markRenderReady()
{
    m_renderReady = true;
    if (m_pendingUrl.isEmpty())
        return;
    const QString u = m_pendingUrl;
    m_pendingUrl.clear();
    load(u);
}

void PlayerCore::setStartAt(double seconds)
{
    if (!m_mpv)
        return;
    mpv_set_property_string(m_mpv, "start", QByteArray::number(seconds, 'f', 3).constData());
}

void PlayerCore::togglePause()
{
    if (!m_mpv)
        return;
    int v = m_paused ? 0 : 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &v);
}

void PlayerCore::seekTo(double seconds)
{
    if (!m_mpv)
        return;
    const QByteArray t = QByteArray::number(seconds, 'f', 3);
    qInfo("seek -> %.3f s", seconds);
    const char *args[] = {"seek", t.constData(), "absolute", nullptr};
    mpv_command(m_mpv, args);
}

// Second instrument: the buttons only count as wired if mpv reports the value
// back, so every write path reads the property again and logs it.
static void logBack(mpv_handle *mpv, const QByteArray &name)
{
    char *v = mpv_get_property_string(mpv, name.constData());
    qInfo("%s = %s", name.constData(), v ? v : "-");
    mpv_free(v);
}

void PlayerCore::cycleProperty(const QString &name)
{
    if (!m_mpv)
        return;
    const QByteArray n = name.toUtf8();
    const char *args[] = {"cycle", n.constData(), nullptr};
    const int rc = mpv_command(m_mpv, args);
    if (rc < 0)
        qInfo("cycle %s -> %s", n.constData(), mpv_error_string(rc));
    else
        logBack(m_mpv, n);
}

void PlayerCore::setPropertyString(const QString &name, const QString &value)
{
    if (!m_mpv)
        return;
    const QByteArray n = name.toUtf8();
    const int rc = mpv_set_property_string(m_mpv, n.constData(), value.toUtf8().constData());
    if (rc < 0)
        qInfo("set %s=%s -> %s", n.constData(), value.toUtf8().constData(), mpv_error_string(rc));
    else
        logBack(m_mpv, n);
}


void PlayerCore::setPropertyInt(const QString &name, qint64 value)
{
    if (!m_mpv)
        return;
    const QByteArray n = name.toUtf8();
    const int rc = mpv_set_property(m_mpv, n.constData(), MPV_FORMAT_INT64, &value);
    if (rc < 0)
        qInfo("set %s=%lld -> %s", n.constData(), value, mpv_error_string(rc));
    else
        logBack(m_mpv, n);
}

void PlayerCore::setFullscreen(bool on)
{
    if (!m_mpv)
        return;
    int v = on ? 1 : 0;
    qInfo("fullscreen -> %d", v);
    mpv_set_property(m_mpv, "fullscreen", MPV_FORMAT_FLAG, &v);
    m_fullscreen = on;
    emit fullscreenChanged();
}

void PlayerCore::setVolume(double percent)
{
    if (!m_mpv)
        return;
    double v = percent;
    qInfo("volume -> %.1f", v);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

void PlayerCore::setSpeed(double rate)
{
    if (!m_mpv)
        return;
    double v = rate;
    qInfo("speed -> %.2f", v);
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &v);
}
