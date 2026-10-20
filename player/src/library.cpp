#include "library.h"

#include "bluray.h"
#include "duration.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <QElapsedTimer>
#include <QSet>

#include <windows.h>
#include <shellapi.h>

Library::Library(QObject *parent) : QObject(parent)
{
    m_videoExt = { "mp4", "m4v", "mkv", "webm", "avi", "mov", "flv", "wmv", "ts", "m2ts",
                   "mts", "mpg", "mpeg", "vob", "ogv", "3gp", "divx", "strm" };
    m_audioExt = { "flac", "alac", "m4a", "mp3", "opus", "ogg", "oga", "wav", "ape",
                   "wv", "aac", "mka", "dsf", "dff", "aiff", "aif" };
    m_skipDirs = { "$recycle.bin", "system volume information", "node_modules",
                   ".git", ".svn", "__pycache__", "build", "venv", ".venv" };
}

bool Library::open(const QString &dbPath)
{
    m_db = QSqlDatabase::addDatabase(u"QSQLITE"_qs, u"library"_qs);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        qWarning("library db open failed: %s", qPrintable(m_db.lastError().text()));
        return false;
    }
    ensureSchema();
    return true;
}

void Library::ensureSchema()
{
    static const char *ddl[] = {
        "CREATE TABLE IF NOT EXISTS roots("
        " id INTEGER PRIMARY KEY, path TEXT UNIQUE, last_scan INTEGER)",
        "CREATE TABLE IF NOT EXISTS media("
        " id INTEGER PRIMARY KEY, kind TEXT, title TEXT, year INTEGER,"
        " parsed INTEGER, poster TEXT, UNIQUE(kind, title, year))",
        "CREATE TABLE IF NOT EXISTS files("
        " id INTEGER PRIMARY KEY, media_id INTEGER, path TEXT UNIQUE,"
        " size INTEGER, mtime INTEGER, duration REAL, probed INTEGER, scanned INTEGER)",
        "CREATE INDEX IF NOT EXISTS files_media_idx ON files(media_id)",
    };
    for (const char *sql : ddl) {
        QSqlQuery q(m_db);
        if (!q.exec(QLatin1StringView(sql)))
            qWarning("ddl failed: %s", qPrintable(q.lastError().text()));
    }

    // Catalogues written before the duration probe have no column for its result.
    QSqlQuery info(m_db);
    if (info.exec(u"PRAGMA table_info(files)"_qs)) {
        bool hasProbed = false;
        while (info.next()) {
            if (info.value(1).toString().compare(u"probed"_qs, Qt::CaseInsensitive) == 0)
                hasProbed = true;
        }
        if (!hasProbed) {
            QSqlQuery alter(m_db);
            if (!alter.exec(u"ALTER TABLE files ADD COLUMN probed INTEGER"_qs))
                qWarning("ddl alter failed: %s", qPrintable(alter.lastError().text()));
        }
    }

    // Two more marks on the media row, both about the card rather than the file:
    // `hidden` is the "remove from the library" the card menu offers (the file stays
    // exactly where it is on disk), and `custom` says the title was typed by the user,
    // so the next scan must not derive it from the file name again.
    QSqlQuery minfo(m_db);
    if (minfo.exec(u"PRAGMA table_info(media)"_qs)) {
        QStringList have;
        while (minfo.next())
            have << minfo.value(1).toString().toLower();
        QSqlQuery alter(m_db);
        if (!have.contains(u"hidden"_qs)
            && !alter.exec(u"ALTER TABLE media ADD COLUMN hidden INTEGER NOT NULL DEFAULT 0"_qs))
            qWarning("ddl alter media.hidden failed: %s", qPrintable(alter.lastError().text()));
        if (!have.contains(u"custom"_qs)
            && !alter.exec(u"ALTER TABLE media ADD COLUMN custom INTEGER NOT NULL DEFAULT 0"_qs))
            qWarning("ddl alter media.custom failed: %s", qPrintable(alter.lastError().text()));
    }
}

void Library::addRoot(const QUrl &folder)
{
    const QString path = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (path.isEmpty()) {
        qWarning("PICK got nothing usable from %s", qPrintable(folder.toString()));
        return;
    }
    QSqlQuery q(m_db);
    q.prepare(u"INSERT OR IGNORE INTO roots(path) VALUES (?)"_qs);
    q.addBindValue(path);
    q.exec();
    if (!m_roots.contains(path))
        m_roots << path;
}

QStringList Library::roots() const
{
    // Both sources are live: roots added from the UI are persisted, roots passed
    // on the command line are session-only.
    QSet<QString> seen;
    QStringList out;
    auto addUnique = [&seen, &out](const QString &raw) {
        QString p = QDir::cleanPath(raw);
        p = QFileInfo(p).exists() ? QFileInfo(p).absoluteFilePath() : p;
        if (p.isEmpty() || seen.contains(p.toLower()))
            return;
        seen.insert(p.toLower());
        out << p;
    };
    for (const QString &r : m_roots)
        addUnique(r);
    QSqlQuery q(m_db);
    if (q.exec(u"SELECT path FROM roots ORDER BY id"_qs))
        while (q.next())
            addUnique(q.value(0).toString());
    return out;
}

void Library::walk(const QString &dirPath, int depth)
{
    if (depth > 8)
        return;
    // A disc folder is one title. Record it and do not descend, or every clip in
    // BDMV/STREAM becomes its own entry. Testing this on entry rather than on the
    // children also covers the case where the disc folder itself was added as a
    // root - the walk would otherwise start inside it and never ask.
    if (bluray::isDisc(dirPath)) {
        const QFileInfo self(dirPath);
        record(dirPath, u"video"_qs, bluray::streamBytes(dirPath),
               self.lastModified().toSecsSinceEpoch());
        return;
    }
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : entries) {
        if (fi.isDir()) {
            const QString name = fi.fileName();
            if (m_skipDirs.contains(name.toLower()) || name.startsWith(u'.') || name.startsWith(u'_'))
                continue;
            walk(fi.absoluteFilePath(), depth + 1);
            continue;
        }
        const QString ext = fi.suffix().toLower();
        const QString kind = m_videoExt.contains(ext) ? u"video"_qs
                             : m_audioExt.contains(ext) ? u"audio"_qs
                                                        : QString();
        if (kind.isEmpty())
            continue;

        record(fi.absoluteFilePath(), kind, fi.size(), fi.lastModified().toSecsSinceEpoch());
    }
}

// One media file - or one disc folder - into the catalogue. Unchanged rows are
// skipped so a warm scan costs a directory listing and nothing else.
void Library::record(const QString &absPath, const QString &kind, qint64 size, qint64 mtime)
{
    ++m_seenFiles;
    QSqlQuery chk(m_db);
    chk.prepare(u"SELECT id, size, mtime FROM files WHERE path = ?"_qs);
    chk.addBindValue(absPath);
    chk.exec();
    if (chk.next() && chk.value(1).toLongLong() == size && chk.value(2).toLongLong() == mtime) {
        ++m_skipped;
        // A card the user removed from the view comes back when its folder is scanned
        // again - the mark was only ever about the display, and this is the way back.
        unhidePath(absPath);
        return;
    }

    const QFileInfo fi(absPath);
    QString title;
    int year = 0;
    const bool parsed = parseName(fi.fileName(), fi.dir().dirName(), &title, &year);
    if (!parsed)
        ++m_unparsed;
    if (title.isEmpty())
        title = fi.isDir() ? fi.fileName() : fi.completeBaseName();

    QElapsedTimer dt;
    dt.start();
    const double duration = readDuration(absPath);
    m_durationMs += dt.nsecsElapsed() / 1000000.0;
    if (duration > 0)
        ++m_withDuration;

    QSqlQuery mid(m_db);
    mid.prepare(u"INSERT OR IGNORE INTO media(kind, title, year, parsed) VALUES (?,?,?,?)"_qs);
    mid.addBindValue(kind);
    mid.addBindValue(title);
    mid.addBindValue(year);
    mid.addBindValue(parsed ? 1 : 0);
    mid.exec();

    QSqlQuery look(m_db);
    look.prepare(u"SELECT id FROM media WHERE kind=? AND title=? AND year=?"_qs);
    look.addBindValue(kind);
    look.addBindValue(title);
    look.addBindValue(year);
    look.exec();
    // A row the user renamed keeps the name they typed: re-deriving the title from the
    // file name here would undo the rename and leave a second card for the same file.
    qint64 mediaId = customMediaIdOfPath(absPath);
    if (mediaId == 0)
        mediaId = look.next() ? look.value(0).toLongLong() : 0;

    QSqlQuery up(m_db);
    up.prepare(u"INSERT INTO files(media_id, path, size, mtime, duration, probed, scanned) "
               "VALUES (?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET "
               "size=excluded.size, mtime=excluded.mtime, scanned=excluded.scanned, "
               "duration=excluded.duration, probed=excluded.probed"_qs);
    up.addBindValue(mediaId);
    up.addBindValue(absPath);
    up.addBindValue(size);
    up.addBindValue(mtime);
    up.addBindValue(duration > 0 ? QVariant(duration) : QVariant());
    up.addBindValue(1);
    up.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!up.exec())
        qWarning("file upsert failed: %s", qPrintable(up.lastError().text()));
    // The same rule as the unchanged branch: scanning the folder the file lives in
    // brings its card back.
    unhidePath(absPath);
}

// The media row this path belongs to, but only when the user named it: a scan must
// not re-derive a title that was typed in.
qint64 Library::customMediaIdOfPath(const QString &absPath) const
{
    QSqlQuery q(m_db);
    q.prepare(u"SELECT f.media_id FROM files f JOIN media m ON m.id = f.media_id "
              "WHERE f.path = ? AND COALESCE(m.custom, 0) = 1"_qs);
    q.addBindValue(absPath);
    return (q.exec() && q.next()) ? q.value(0).toLongLong() : 0;
}

void Library::unhidePath(const QString &absPath)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE media SET hidden = 0 WHERE hidden = 1 "
              "AND id = (SELECT media_id FROM files WHERE path = ?)"_qs);
    q.addBindValue(absPath);
    q.exec();
}

void Library::hideMedia(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE media SET hidden = 1 WHERE id = ?"_qs);
    q.addBindValue(id);
    if (!q.exec()) {
        qWarning("hide failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    qInfo("LIBRARY hide media=%lld", long long(id));
    emit rowsChanged();
}

void Library::setTitle(qint64 id, const QString &title)
{
    const QString t = title.trimmed();
    if (t.isEmpty()) {
        qInfo("LIBRARY rename refused (empty) media=%lld", long long(id));
        return;
    }
    QSqlQuery q(m_db);
    // `custom` is what tells the next scan to leave this title alone.
    q.prepare(u"UPDATE media SET title = ?, custom = 1 WHERE id = ?"_qs);
    q.addBindValue(t);
    q.addBindValue(id);
    if (!q.exec()) {
        qWarning("rename failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    qInfo("LIBRARY rename media=%lld to \"%s\"", long long(id), qPrintable(t));
    emit rowsChanged();
}

bool Library::revealInFolder(const QString &path)
{
    // The shell's own select-and-open. The argument has to reach explorer as
    // `/select,"C:\dir with space\file.mkv"` - quotes around the path only. QProcess is
    // not usable here: it quotes the whole argument when it contains a space, and that
    // form makes explorer open its default folder with nothing selected (measured - the
    // window came up titled "文档" for a path with a space in it). ShellExecuteW takes
    // the parameter string as it is given.
    const QString native = QDir::toNativeSeparators(path);
    if (native.isEmpty())
        return false;
    const std::wstring params = L"/select,\"" + native.toStdWString() + L"\"";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns an HINSTANCE; values above 32 are success.
    const bool ok = reinterpret_cast<INT_PTR>(r) > 32;
    qInfo("LIBRARY reveal %s ok=%d", qPrintable(path), ok ? 1 : 0);
    return ok;
}

QString Library::titleForPath(const QString &path) const
{
    if (path.isEmpty())
        return {};
    const QString key = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QSqlQuery q(m_db);
    // Only rows the user named are candidates. The compare happens here rather than in
    // the WHERE clause because SQLite's LIKE ignores case only for ASCII, and these
    // paths are Chinese as often as not.
    q.prepare(u"SELECT m.title, f.path FROM media m JOIN files f ON f.media_id = m.id "
              u"WHERE COALESCE(m.custom, 0) = 1"_qs);
    if (!q.exec())
        return {};
    while (q.next()) {
        if (QDir::cleanPath(QDir::fromNativeSeparators(q.value(1).toString()))
                .compare(key, Qt::CaseInsensitive) == 0)
            return q.value(0).toString();
    }
    return {};
}

void Library::scan()
{
    if (!m_db.isOpen())
        return;
    m_mediaCount = m_fileCount = m_skipped = m_unparsed = m_withDuration = 0;
    m_durationMs = 0.0;
    m_seenFiles = 0;
    QElapsedTimer t;
    t.start();
    const QStringList rs = roots();
    // One transaction for the whole walk: per-row autocommit costs a disk flush
    // each, which measured 7.7 ms/file (3.9 s for 500 files) against 0.07 ms/file
    // for the header reads themselves.
    m_db.transaction();
    for (const QString &r : rs)
        walk(r, 0);
    if (!m_db.commit())
        qWarning("scan commit failed: %s", qPrintable(m_db.lastError().text()));
    m_scanMs = t.elapsed();
    m_fileCount = int(m_seenFiles);

    QSqlQuery q(m_db);
    if (q.exec(u"SELECT COUNT(*) FROM media WHERE COALESCE(hidden, 0) = 0"_qs) && q.next())
        m_mediaCount = q.value(0).toInt();
    if (q.exec(u"SELECT COUNT(*) FROM media WHERE parsed=0 AND COALESCE(hidden, 0) = 0"_qs) && q.next())
        m_unparsed = q.value(0).toInt();
    countDurations();

    qInfo("SCAN roots=%d files=%d media=%d skipped_unchanged=%d unparsed=%d "
          "durations=%d read_now=%d read_ms=%.0f ms=%lld",
          rs.size(), m_fileCount, m_mediaCount, m_skipped, m_unparsed,
          m_durationCount, m_withDuration, m_durationMs, m_scanMs);
    emit statsChanged();
    emit scanned();
}

void Library::countDurations()
{
    QSqlQuery q(m_db);
    if (q.exec(u"SELECT COUNT(*) FROM files WHERE duration > 0"_qs) && q.next())
        m_durationCount = q.value(0).toInt();
}

bool Library::parseName(const QString &fileName, const QString &parentDir,
                        QString *title, int *year)
{
    static const QRegularExpression yearRe(u"(?<![0-9])(19[0-9]{2}|20[0-9]{2})(?![0-9])"_qs);
    static const QRegularExpression markerRe(
        u"(?i)\\b(720p|1080p|1440p|2160p|4k|uhd|hd|sd|blu-?ray|bdrip|brrip|web-?dl|webrip|"
        u"hdtv|x264|x265|h\\.?264|h\\.?265|hevc|av1|h265|10-?bit|8-?bit|xvid|divx|"
        u"remux|proper|repack|extended|uncut|imax|hdr10\\+?|dolby ?vision|dv|aac\\d?|"
        u"ac-?3|dts(?:-?hd)?|truehd|atmos|flac|2ch|5\\.1|7\\.1)\\b"_qs);

    *title = QString();
    *year = 0;

    // An organised library often names the folder "Title (2008)".
    static const QRegularExpression folderRe(u"^(.*?)\\s*\\((19[0-9]{2}|20[0-9]{2})\\)"_qs);
    const QFileInfo fi(fileName);
    QString stem = fi.completeBaseName();
    const QRegularExpressionMatch fm = folderRe.match(parentDir);
    if (fm.hasMatch()) {
        *title = fm.captured(1).trimmed();
        *year = fm.captured(2).toInt();
        if (!title->isEmpty())
            return true;
    }

    stem.replace(u'.', u' ').replace(u'_', u' ');
    QRegularExpressionMatchIterator it = yearRe.globalMatch(stem);
    int lastYearPos = -1;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        *year = m.captured(1).toInt();
        lastYearPos = int(m.capturedStart());
    }

    QString head = lastYearPos >= 0 ? stem.left(lastYearPos) : stem;
    const QRegularExpressionMatch mm = markerRe.match(head);
    if (mm.hasMatch())
        head = head.left(mm.capturedStart());

    head = head.remove(QRegularExpression(u"[\\[\\](){}]+\\s*$"_qs)).trimmed();
    head = head.remove(QRegularExpression(u"^[\\s._-]+"_qs));
    while (head.endsWith(u' ') || head.endsWith(u'-') || head.endsWith(u'.'))
        head.chop(1);

    *title = head;
    const bool credible = !head.isEmpty() && (head.size() > 1)
            && (*year != 0 || mm.hasMatch() || head.contains(u' '));
    return credible;
}

QList<MediaRow> Library::rows(int limit) const
{
    QList<MediaRow> out;
    QSqlQuery q(m_db);
    q.prepare(u"SELECT m.id, m.kind, m.title, m.year, m.parsed, m.poster, "
              "       (SELECT f.path FROM files f WHERE f.media_id=m.id LIMIT 1), "
              "       (SELECT f.size FROM files f WHERE f.media_id=m.id LIMIT 1), "
              "       (SELECT f.duration FROM files f WHERE f.media_id=m.id LIMIT 1) "
              "FROM media m WHERE COALESCE(m.hidden, 0) = 0 "
              "ORDER BY m.title COLLATE NOCASE LIMIT ?"_qs);
    q.addBindValue(limit);
    q.exec();
    while (q.next()) {
        MediaRow r;
        r.id = q.value(0).toLongLong();
        r.kind = q.value(1).toString();
        r.title = q.value(2).toString();
        r.year = q.value(3).toInt();
        r.parsed = q.value(4).toInt() != 0;
        r.poster = q.value(5).toString();
        r.path = q.value(6).toString();
        r.size = q.value(7).toLongLong();
        r.duration = q.value(8).toDouble();
        out << r;
    }
    return out;
}

QStringList Library::pathsWithoutDuration(int limit)
{
    QStringList out;
    QSqlQuery q(m_db);
    q.prepare(u"SELECT path FROM files WHERE COALESCE(probed, 0) = 0 "
              "AND (duration IS NULL OR duration <= 0) ORDER BY id LIMIT ?"_qs);
    q.addBindValue(limit);
    if (!q.exec()) {
        qWarning("duration query failed: %s", qPrintable(q.lastError().text()));
        return out;
    }
    while (q.next())
        out << q.value(0).toString();
    return out;
}

void Library::setDuration(const QString &path, double seconds)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE files SET duration = ?, probed = 1 WHERE path = ?"_qs);
    q.addBindValue(seconds > 0 ? QVariant(seconds) : QVariant());
    q.addBindValue(path);
    if (!q.exec())
        qWarning("duration write failed: %s", qPrintable(q.lastError().text()));
    else if (q.numRowsAffected() == 0)
        qWarning("duration write hit no row: %s", qPrintable(path));
}

void Library::noteDuration(const QString &path, double seconds)
{
    if (path.isEmpty() || seconds <= 0.0)
        return;
    QSqlQuery q(m_db);
    // The 0.5 s band keeps a running player from re-writing the same row on every
    // duration tick, while still correcting a value that was never right.
    q.prepare(u"UPDATE files SET duration = ?, probed = 1 WHERE path = ? "
              "AND (duration IS NULL OR abs(duration - ?) > 0.5)"_qs);
    q.addBindValue(seconds);
    q.addBindValue(path);
    q.addBindValue(seconds);
    if (!q.exec())
        qWarning("noteDuration failed: %s", qPrintable(q.lastError().text()));
    else if (q.numRowsAffected() > 0)
        emit rowChanged();
}

QStringList Library::pathsNeedingCover(int limit)
{
    QStringList out;
    QSqlQuery q(m_db);
    q.prepare(u"SELECT f.path FROM files f JOIN media m ON m.id = f.media_id "
              "WHERE m.kind = ? AND (m.poster IS NULL OR m.poster = '') "
              "ORDER BY f.id LIMIT ?"_qs);
    q.addBindValue(u"video"_qs);
    q.addBindValue(limit);
    if (!q.exec()) {
        qWarning("cover query failed: %s", qPrintable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        const QString p = q.value(0).toString();
        if (QFileInfo::exists(p))
            out << p;
    }
    return out;
}

void Library::setPoster(const QString &path, const QString &file)
{
    if (path.isEmpty())
        return;
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE media SET poster = ? WHERE id = "
              "(SELECT media_id FROM files WHERE path = ?)"_qs);
    q.addBindValue(file.isEmpty() ? QVariant() : QVariant(file));
    q.addBindValue(path);
    if (!q.exec())
        qWarning("poster write failed: %s", qPrintable(q.lastError().text()));
    else if (q.numRowsAffected() > 0 && !file.isEmpty())
        emit rowChanged();
}

int Library::fillMissingDurations(int limit)
{
    const QStringList paths = pathsWithoutDuration(limit);
    int filled = 0;
    QElapsedTimer t;
    t.start();
    m_db.transaction();
    for (const QString &p : paths) {
        const double seconds = readDuration(p);
        setDuration(p, seconds);
        if (seconds > 0)
            ++filled;
    }
    if (!m_db.commit())
        qWarning("duration commit failed: %s", qPrintable(m_db.lastError().text()));
    if (!paths.isEmpty()) {
        countDurations();
        emit statsChanged();
        emit rowChanged();
    }
    qInfo("DUR backfilled=%d tried=%d ms=%.0f", filled, paths.size(),
          double(t.nsecsElapsed()) / 1e6);
    return filled;
}

LibraryModel::LibraryModel(Library *lib, QObject *parent)
    : QAbstractListModel(parent), m_lib(lib)
{
    connect(lib, &Library::scanned, this, &LibraryModel::reload);
    // A card hidden or renamed by the menu changes the grid at once; the rename also
    // has to repaint the label, which a reload of the rows does.
    connect(lib, &Library::rowsChanged, this, &LibraryModel::reload);
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(250);
    connect(&m_coalesce, &QTimer::timeout, this, &LibraryModel::reload);
    connect(lib, &Library::rowChanged, &m_coalesce, qOverload<>(&QTimer::start));
}

int LibraryModel::rowCount(const QModelIndex &) const
{
    return m_rows.size();
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const MediaRow &r = m_rows.at(index.row());
    switch (role) {
    case TitleRole: return r.title;
    case YearRole: return r.year;
    case KindRole: return r.kind;
    case PathRole: return r.path;
    case DurationRole: return r.duration;
    case ParsedRole: return r.parsed;
    case PosterRole: return r.poster;
    case IdRole: return QVariant::fromValue(r.id);
    }
    return {};
}

QHash<int, QByteArray> LibraryModel::roleNames() const
{
    return {
        { TitleRole, "title" },
        { YearRole, "year" },
        { KindRole, "kind" },
        { PathRole, "path" },
        { DurationRole, "duration" },
        { ParsedRole, "parsed" },
        { PosterRole, "poster" },
        { IdRole, "mediaId" },
    };
}

void LibraryModel::reload()
{
    beginResetModel();
    m_rows = m_lib->rows();
    endResetModel();
}
