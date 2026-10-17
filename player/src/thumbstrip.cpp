#include "thumbstrip.h"

#include "bluray.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>

#include <mpv/client.h>

#include <windows.h>

namespace {

constexpr int kSettlePolls = 5;         // 250 ms after the load before grabbing
constexpr int kPresentBudgetPolls = 40;   // 40 x 50 ms for a frame to reach the VO
constexpr int kFileBudgetPolls = 60;      // 60 x 50 ms for the jpg to land

QString slotPath(const QString &dir, int index)
{
    return dir + QLatin1Char('/') + QString::number(index).rightJustified(2, QLatin1Char('0'))
        + QLatin1String(".jpg");
}

// Keyed on the path plus the file's own identity, so a re-encode or a rename
// gets a fresh strip instead of last week's frames. `kind` keeps the scrub strip
// and the cover still of the same movie in separate directories.
QString cacheDirFor(const QString &path, const char *kind)
{
    const QFileInfo fi(path);
    const QByteArray key = (path + QLatin1Char('|') + QByteArray::number(fi.size())
                            + QLatin1Char('|')
                            + QByteArray::number(fi.lastModified().toSecsSinceEpoch())).toUtf8();
    QDir base(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    return base.absoluteFilePath(QLatin1String(kind) + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16)));
}

// A bare path, a file:// url, or a disc folder - all three arrive here, and the
// core can only load a real file.
QString resolveSource(const QString &url)
{
    QString path = QFileInfo::exists(url) ? url : QUrl(url).toLocalFile();
    if (QFileInfo(path).isDir())
        path = bluray::longestClip(path);
    return path;
}

// A film that opens on a black leader gives a card that reads as a broken
// thumbnail, so a cover still with no light in it is replaced by the next
// candidate time. Measured: the Soul Land opener is max 0 over every pixel,
// while a genuinely dark scene still carries a few hundred lit ones.
bool looksBlack(const QString &file)
{
    QImage im(file);
    if (im.isNull())
        return false;
    if (im.format() != QImage::Format_Grayscale8)
        im = im.convertToFormat(QImage::Format_Grayscale8);
    int max = 0;
    for (int y = 0; y < im.height() && max < 8; y += 2) {
        const uchar *line = im.constBits() + y * size_t(im.bytesPerLine());
        for (int x = 0; x < im.width(); ++x)
            max = qMax(max, int(line[x]));
    }
    return max < 8;
}

LRESULT CALLBACK thumbProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, msg, w, l);
}

HWND createOffscreenHost()
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &thumbProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PlayeruiThumbHost";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        registered = true;
    }
    // Fully outside the virtual desktop, but shown: a window that was never made
    // visible gives the video output no surface to screenshot.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"PlayeruiThumbHost", L"thumb", WS_POPUP,
                             -32000, -32000, 480, 270, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (h)
        ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

} // namespace

ThumbStrip::ThumbStrip(QObject *parent) : QObject(parent)
{
    m_poll.setInterval(50);
    connect(&m_poll, &QTimer::timeout, this, &ThumbStrip::pollTick);
}

ThumbStrip::~ThumbStrip()
{
    m_poll.stop();
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    delete static_cast<QPointer<ThumbStrip> *>(m_guard);
    m_guard = nullptr;
    if (m_wid)
        DestroyWindow(reinterpret_cast<HWND>(m_wid));
}

bool ThumbStrip::ensureCore()
{
    if (m_mpv)
        return true;
    if (!m_wid) {
        HWND h = createOffscreenHost();
        if (!h)
            return false;
        m_wid = reinterpret_cast<qint64>(h);
    }
    m_mpv = mpv_create();
    if (!m_mpv)
        return false;

    auto set = [this](const char *k, const char *v) { mpv_set_option_string(m_mpv, k, v); };
    qint64 wid = m_wid;
    set("vo", "gpu-next");
    set("gpu-api", "d3d11");
    set("hwdec", "d3d11va-copy");
    set("terminal", "no");
    set("msg-level", "all=no");
    set("config", "no");
    set("keep-open", "yes");
    set("pause", "yes");
    set("hr-seek", "absolute");
    set("sub", "no");
    set("osd-level", "0");
    set("ao", "null");
    set("screenshot-format", "jpg");
    set("screenshot-jpeg-quality", "82");
    // A native-resolution grab of a 2160p file is 3840x1610 and ~600 KB per
    // still; the card is 163 px wide, so scale the strip down at the filter.
    set("vf", "scale=w=480:h=-2");
    set("input-default-bindings", "no");
    set("input-vo-keyboard", "no");
    mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid);

    if (mpv_initialize(m_mpv) < 0) {
        qWarning("THUMB mpv_initialize failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    auto *guard = new QPointer<ThumbStrip>(this);
    m_guard = guard;
    mpv_set_wakeup_callback(
        m_mpv,
        [](void *ctx) {
            auto *g = static_cast<QPointer<ThumbStrip> *>(ctx);
            if (g && *g)
                QMetaObject::invokeMethod(*g, [g] { if (*g) (*g)->drain(); }, Qt::QueuedConnection);
        },
        guard);
    return true;
}

void ThumbStrip::build(const QString &url, double duration, int count)
{
    if (url.isEmpty() || duration <= 0.0 || count <= 0)
        return;
    // A second call with a different duration is not a repeat: the first one can
    // arrive while the shell still holds the previous movie's length.
    if (url == m_stripUrl && qFuzzyCompare(duration, m_stripDuration))
        return;

    const QString source = resolveSource(url);
    if (source.isEmpty() || !QFileInfo::exists(source)) {
        qInfo("THUMB skip, not a local file: %s", qPrintable(url));
        return;
    }

    m_stripUrl = url;
    m_stripDuration = duration;

    Job job;
    job.source = source;
    job.dir = cacheDirFor(source, "thumbs");
    for (int i = 0; i < count; ++i)
        job.times << duration * (i + 0.5) / count;
    QDir().mkpath(job.dir);

    int cached = 0;
    for (int i = 0; i < count; ++i) {
        if (QFileInfo::exists(slotPath(job.dir, i)))
            ++cached;
    }
    if (cached == count) {
        QStringList urls;
        for (int i = 0; i < count; ++i)
            urls << QUrl::fromLocalFile(slotPath(job.dir, i)).toString();
        m_urls = urls;
        emit framesChanged();
        qInfo("THUMB cache hit %d/%d", cached, count);
        return;
    }

    // The viewer is waiting on this one, so it takes the core off the cover queue.
    if (!m_stripRunning) {
        m_stripRunning = true;
        emit buildingChanged();
    }
    qInfo("THUMB build start cached=%d of %d dir=%s", cached, count, qPrintable(job.dir));
    startStrip(job);
}

void ThumbStrip::covers(const QStringList &paths)
{
    int queued = 0, fromCache = 0, unreadable = 0, duplicate = 0;
    for (const QString &p : paths) {
        if (m_coverKeys.contains(p)) {
            ++duplicate;
            continue;
        }
        const QString source = resolveSource(p);
        if (source.isEmpty() || !QFileInfo::exists(source)) {
            ++unreadable;
            continue;
        }
        m_coverKeys.insert(p);

        Job job;
        job.key = p;
        job.source = source;
        job.times = { 0.0, 1.0, 3.0, 6.0, 10.0, 15.0 };   // 首帧, then past the leader
        job.dir = cacheDirFor(source, "covers");
        job.cover = true;

        const QString file = slotPath(job.dir, 0);
        if (QFileInfo(file).size() > 0) {
            ++fromCache;
            const QString url = QUrl::fromLocalFile(file).toString();
            // Queued so the caller's pump loop is not re-entered through the
            // poster write.
            QMetaObject::invokeMethod(this, [this, p, url] { emit coverReady(p, url); },
                                      Qt::QueuedConnection);
            continue;
        }
        QDir().mkpath(job.dir);
        m_queue.enqueue(job);
        ++queued;
    }
    qInfo("COVER asked=%d queued=%d cache=%d unreadable=%d duplicate=%d pending=%d",
          paths.size(), queued, fromCache, unreadable, duplicate, m_queue.size());
    if (queued > 0)
        startNext();
}

void ThumbStrip::clear()
{
    // Only the movie's own strip: cover stills still belong to the library after
    // the player has moved on.
    const bool hadStrip = m_activeLive && !m_active.cover;
    if (hadStrip)
        abortActive();
    m_stripUrl.clear();
    m_stripDuration = 0.0;
    m_urls.clear();
    emit framesChanged();
    if (m_stripRunning) {
        m_stripRunning = false;
        emit buildingChanged();
    }
    if (hadStrip)
        startNext();
}

void ThumbStrip::startStrip(const Job &job)
{
    if (m_activeLive)
        abortActive();
    // startNext() creates the core for queued jobs; a strip goes straight onto
    // the core, so it has to raise one itself or startFrame() would hand a null
    // handle to mpv_set_property_string and take the process down with it.
    if (!ensureCore()) {
        qWarning("THUMB no core");
        m_stripRunning = false;
        emit buildingChanged();
        return;
    }
    m_active = job;
    m_activeLive = true;
    m_index = 0;
    m_clock.start();
    startFrame();
}

void ThumbStrip::abortActive()
{
    m_poll.stop();
    m_stage = Idle;
    if (m_active.cover) {
        // The strip took the core off this still. Put it back at the end of the
        // queue rather than leave the card blank until the next scan; its key
        // stays held so a scan in the meantime cannot queue it twice.
        m_queue.enqueue(m_active);
    } else {
        m_stripRunning = false;
    }
    m_active = Job();
    m_activeLive = false;
    m_index = 0;
    if (m_mpv) {
        const char *stop[] = { "stop", nullptr };
        mpv_command_async(m_mpv, 0, stop);
    }
}

void ThumbStrip::startNext()
{
    if (m_activeLive)
        return;
    if (m_queue.isEmpty()) {
        if (m_mpv) {
            // Nothing left to grab: release the decoder rather than hold a frame.
            const char *stop[] = { "stop", nullptr };
            mpv_command_async(m_mpv, 0, stop);
        }
        return;
    }
    if (!ensureCore()) {
        qWarning("THUMB no core");
        m_queue.clear();
        return;
    }
    m_active = m_queue.dequeue();
    m_activeLive = true;
    m_index = 0;
    m_clock.start();
    startFrame();
}

QString ThumbStrip::target() const
{
    // A cover keeps one slot while it walks its candidate times; the strip keeps
    // one slot per frame.
    return slotPath(m_active.dir, m_active.cover ? 0 : m_index);
}

void ThumbStrip::startFrame()
{
    if (!m_activeLive)
        return;
    if (m_index >= m_active.times.size()) {
        finish();
        return;
    }
    const QString file = target();
    QFile::remove(file);
    mpv_set_property_string(m_mpv, "start",
                            QByteArray::number(m_active.times.at(m_index), 'f', 3).constData());
    // Events from the previous load can still be sitting in the queue. Dropping
    // them here is what makes the FILE_LOADED below belong to this loadfile.
    while (mpv_wait_event(m_mpv, 0)->event_id != MPV_EVENT_NONE) {}
    const QByteArray u = m_active.source.toUtf8();
    const char *args[] = { "loadfile", u.constData(), "replace", nullptr };
    if (mpv_command_async(m_mpv, 0, args) < 0) {
        qWarning("THUMB loadfile refused at %d: %s", m_index, qPrintable(m_active.source));
        ++m_index;
        startFrame();
        return;
    }
    m_stage = WaitingLoad;
    m_polls = 0;
    m_poll.start();
}

void ThumbStrip::takeShot()
{
    const QString file = target();
    m_poll.stop();
    m_stage = WaitingFile;
    m_polls = 0;
    const QByteArray p = file.toUtf8();
    const QByteArray d = m_active.dir.toUtf8();
    mpv_set_property_string(m_mpv, "screenshot-directory", d.constData());
    const char *args[] = { "screenshot-to-file", p.constData(), "video", nullptr };
    // Synchronous: the reply carries the failure we would otherwise only see as
    // a missing file three seconds later.
    const int rc = mpv_command(m_mpv, args);
    if (rc < 0) {
        qWarning("THUMB screenshot rc=%d (%s) at %d -> %s", rc, mpv_error_string(rc),
                 m_index, qPrintable(file));
        ++m_index;
        startFrame();
        return;
    }
    m_poll.start();
}

void ThumbStrip::pollTick()
{
    const QString file = target();
    const int budget = m_stage == WaitingPresent ? kPresentBudgetPolls : kFileBudgetPolls;
    if (++m_polls > budget) {
        qWarning("THUMB stage %d timed out at frame %d", int(m_stage), m_index);
        ++m_index;
        startFrame();
        return;
    }
    if (m_stage == WaitingPresent) {
        // This libmpv reports frame-counter as unavailable (-8), so the settle
        // time is what stands in for "a frame reached the VO"; the jpg landing
        // (or not) is the real verdict.
        if (m_polls >= kSettlePolls)
            takeShot();
        return;
    }
    if (m_stage != WaitingFile || !QFileInfo::exists(file))
        return;
    m_poll.stop();
    if (m_active.cover && m_index + 1 < m_active.times.size() && looksBlack(file)) {
        qInfo("COVER black leader at %.1f s, next candidate", m_active.times.at(m_index));
        ++m_index;
        startFrame();
        return;
    }
    ++m_index;
    startFrame();
}

void ThumbStrip::drain()
{
    if (!m_mpv)
        return;
    while (true) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;
        if (ev->event_id == MPV_EVENT_FILE_LOADED && m_stage == WaitingLoad) {
            m_stage = WaitingPresent;
            m_polls = 0;
            m_poll.start();
        }
    }
}

void ThumbStrip::finish()
{
    m_poll.stop();
    m_stage = Idle;
    m_activeLive = false;
    const Job done = m_active;
    m_active = Job();

    if (done.cover) {
        const QString file = slotPath(done.dir, 0);
        if (QFileInfo(file).size() > 0) {
            qInfo("COVER built %s <- %s in %lld ms", qPrintable(file), qPrintable(done.key),
                  m_clock.isValid() ? m_clock.elapsed() : 0);
            emit coverReady(done.key, QUrl::fromLocalFile(file).toString());
        } else {
            m_coverKeys.remove(done.key);
            qWarning("COVER no frame from %s in %lld ms", qPrintable(done.source),
                     m_clock.isValid() ? m_clock.elapsed() : 0);
        }
        startNext();
        return;
    }

    QStringList urls;
    for (int i = 0; i < done.times.size(); ++i) {
        const QString f = slotPath(done.dir, i);
        if (QFileInfo(f).size() > 0)
            urls << QUrl::fromLocalFile(f).toString();
    }
    m_urls = urls;
    m_stripRunning = false;
    emit framesChanged();
    emit buildingChanged();
    qInfo("THUMB built %d/%d in %lld ms", urls.size(), done.times.size(),
          m_clock.isValid() ? m_clock.elapsed() : 0);
    startNext();
}
