#include "bluray.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QUrl>

namespace {

// mainFeatureClips() fills this while it compares playlists; it is the
// allocation-free way to hand the winner back without re-reading the MPLS.
static QStringList g_mainClips;

QString bdmvRoot(const QString &path)
{
    return QDir(path).absolutePath() + QLatin1String("/BDMV");
}

} // namespace

namespace bluray {

bool isDisc(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isDir())
        return false;
    const QDir bdmv(bdmvRoot(path));
    if (!bdmv.exists())
        return false;
    if (bdmv.exists(QLatin1String("index.bdmv")) || bdmv.exists(QLatin1String("MovieObject.bdmv")))
        return true;
    // Some rips ship only the playlist and stream tree; a non-empty PLAYLIST
    // alongside BDMV/ is still a disc layout.
    return QDir(bdmv.filePath(QLatin1String("PLAYLIST")))
               .entryList({QLatin1String("*.mpls"), QLatin1String("*.MPLS")}, QDir::Files).size() > 0;
}

QString longestClip(const QString &path)
{
    QDir st(bdmvRoot(path) + QLatin1String("/STREAM"));
    const QFileInfoList entries = st.entryInfoList(
        {QLatin1String("*.m2ts"), QLatin1String("*.M2TS"), QLatin1String("*.mts")}, QDir::Files);
    QString best;
    qint64 bestSize = 0;
    for (const QFileInfo &fi : entries) {
        if (fi.size() > bestSize) {
            bestSize = fi.size();
            best = fi.absoluteFilePath();
        }
    }
    return best;
}

// The clip references inside a playlist, in the order they appear - which is the
// playback order. Read as a pattern rather than by spec offsets: the fallback
// runs precisely when the metadata cannot be trusted to be laid out correctly,
// and a real disc's ClipInformationFileName is the 9 bytes "dddddM2TS".
QStringList clipsOfPlaylist(const QString &mpls)
{
    QFile f(mpls);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return {};
    const QByteArray blob = f.readAll();
    static const QRegularExpression clipRe(QLatin1String("([0-9]{5})M2TS"));
    QStringList out;
    QSet<QString> seen;
    QRegularExpressionMatchIterator it = clipRe.globalMatch(blob);
    while (it.hasNext()) {
        const QString id = it.next().captured(1);
        if (seen.contains(id))
            continue;                       // the same clip can be referenced twice
        seen.insert(id);
        out << id;
    }
    return out;
}

QStringList bluray::mainFeatureClips(const QString &path)
{
    QDir pl(bdmvRoot(path) + QLatin1String("/PLAYLIST"));
    const QFileInfoList lists = pl.entryInfoList(
        {QLatin1String("*.mpls"), QLatin1String("*.MPLS")}, QDir::Files, QDir::Name);
    const QDir stream(bdmvRoot(path) + QLatin1String("/STREAM"));
    QString best;
    qint64 bestBytes = 0;
    g_mainClips.clear();
    for (const QFileInfo &l : lists) {
        const QStringList ids = clipsOfPlaylist(l.absoluteFilePath());
        qint64 bytes = 0;
        QStringList files;
        for (const QString &id : ids) {
            const QFileInfo c(stream.filePath(id + QLatin1String(".m2ts")));
            if (!c.exists())
                break;                      // a playlist we cannot resolve end to end
            bytes += c.size();
            files << c.absoluteFilePath();
        }
        if (files.size() != ids.size() || ids.isEmpty())
            continue;
        if (bytes > bestBytes) {
            bestBytes = bytes;
            best = l.fileName();
            QStringList abs;
            for (const QString &f : std::as_const(files))
                abs << QDir::toNativeSeparators(f);
            g_mainClips = abs;
        }
    }
    if (bestBytes > 0) {
        qInfo("BLURAY main title %s clips=%d bytes=%lld", qPrintable(best),
              int(g_mainClips.size()), bestBytes);
        return g_mainClips;
    }
    return {};
}

QString clipPlaylist(const QString &path, const QString &cacheDir)
{
    QStringList clips = mainFeatureClips(path);
    if (clips.isEmpty()) {
        // No usable playlist (a stripped or damaged disc): fall back to every
        // clip in name order, which is the best guess at the film.
        QDir st(bdmvRoot(path) + QLatin1String("/STREAM"));
        const QFileInfoList all = st.entryInfoList(
            {QLatin1String("*.m2ts"), QLatin1String("*.M2TS"),
             QLatin1String("*.mts"), QLatin1String("*.MTS")}, QDir::Files, QDir::Name);
        for (const QFileInfo &fi : all)
            clips << QDir::toNativeSeparators(fi.absoluteFilePath());
        qInfo("BLURAY no readable playlist, concatenating %d clips by name", int(clips.size()));
    }
    if (clips.isEmpty())
        return QString();
    QDir().mkpath(cacheDir);
    const QString file = cacheDir + QLatin1String("/disc.m3u");
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("BLURAY cannot write %s", qPrintable(file));
        return QString();
    }
    QTextStream ts(&out);
    ts << "#EXTM3U" << Qt::endl;
    for (const QString &clip : std::as_const(clips))
        ts << clip << Qt::endl;
    ts.flush();
    const qint64 bytes = out.size();
    out.close();
    qInfo("BLURAY playlist %s clips=%d bytes=%lld", qPrintable(file), int(clips.size()), bytes);
    return QUrl::fromLocalFile(file).toString();
}

qint64 streamBytes(const QString &path)
{
    QDir st(bdmvRoot(path) + QLatin1String("/STREAM"));
    qint64 total = 0;
    const QFileInfoList entries = st.entryInfoList(QDir::Files);
    for (const QFileInfo &fi : entries)
        total += fi.size();
    return total;
}

} // namespace bluray
