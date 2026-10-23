#include "library.h"

#include "bluray.h"
#include "duration.h"

#include <QDateTime>
#include <QCryptographicHash>
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
    m_dir = QFileInfo(dbPath).absolutePath();
    if (!m_db.open()) {
        qWarning("library db open failed: %s", qPrintable(m_db.lastError().text()));
        return false;
    }
    ensureSchema();
    // Where the resume covers go. QImage::save makes no directories, and the first
    // snapshot of a session is taken before anything else has written here.
    QDir().mkpath(m_dir + u"/resume"_qs);
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
        // The user's own lists. Deliberately not the catalogue: a playlist row is a path
        // and an order, so adding a film here never copies it, never scans it in, and
        // survives (and outlives) whatever the catalogue does with that file.
        "CREATE TABLE IF NOT EXISTS playlists("
        " id INTEGER PRIMARY KEY, name TEXT, ord INTEGER)",
        "CREATE TABLE IF NOT EXISTS playlist_items("
        " id INTEGER PRIMARY KEY, playlist_id INTEGER, path TEXT, ord INTEGER,"
        " UNIQUE(playlist_id, path))",
        // Where the user stopped each file. Its own table, not a column on files:
        // the walk rewrites those rows on every scan, and a path opened from disk
        // without ever being scanned in still has to come back to its minute.
        "CREATE TABLE IF NOT EXISTS resume("
        " path TEXT PRIMARY KEY, pos REAL, dur REAL, upd INTEGER, shot TEXT)",
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

    // Catalogues that already played something today have the resume table without
    // the cover column; the still only exists from here on.
    QSqlQuery rinfo(m_db);
    if (rinfo.exec(u"PRAGMA table_info(resume)"_qs)) {
        bool hasShot = false;
        while (rinfo.next()) {
            if (rinfo.value(1).toString().compare(u"shot"_qs, Qt::CaseInsensitive) == 0)
                hasShot = true;
        }
        if (!hasShot) {
            QSqlQuery alter(m_db);
            if (!alter.exec(u"ALTER TABLE resume ADD COLUMN shot TEXT"_qs))
                qWarning("ddl alter resume.shot failed: %s", qPrintable(alter.lastError().text()));
        }
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

QVariantList Library::itemsBeside(const QString &mediaUrl) const
{
    QVariantList out;
    // The url is what PlayerCore holds, so this is the one place that knows how to get
    // from it back to the file system: QUrl::toLocalFile has no QML equivalent.
    QString local = mediaUrl;
    if (local.startsWith(u"file:"_qs))
        local = QUrl(mediaUrl).toLocalFile();
    const QFileInfo fi(QDir::cleanPath(QDir::fromNativeSeparators(local)));
    // A disc path (…/Movie/BDMV) has no folder of siblings to list - its parent is part
    // of the disc - so it gets the empty list and the panel says so.
    if (!fi.exists() || !fi.isFile())
        return out;
    const QString dirKey = QDir::cleanPath(fi.absolutePath());
    if (dirKey.length() < 2)
        return out;
    QDir dir(dirKey);
    if (!dir.exists())
        return out;

    // What the catalogue already knows about this folder. The LIKE is only a coarse
    // filter - SQLite folds case for ASCII but not for the Chinese halves of these
    // paths, and a `_` in a folder name makes it match a little too much - so every
    // row it returns is checked again below against the entry we are about to list.
    // The scan is bounded by the folder instead of the whole library on purpose: the
    // playlist re-reads this every time a film changes.
    QHash<QString, QVariantMap> known;
    QSqlQuery q(m_db);
    q.prepare(u"SELECT f.path, f.duration, m.poster, m.title, COALESCE(m.custom, 0) "
              u"FROM files f JOIN media m ON m.id = f.media_id WHERE f.path LIKE ?"_qs);
    q.addBindValue(dirKey + u"/%"_qs);
    if (q.exec()) {
        while (q.next()) {
            QVariantMap m;
            m.insert(u"duration"_qs, q.value(1).toDouble());
            m.insert(u"poster"_qs, q.value(2).toString());
            // Only a name the user typed beats the file name here; the derived title is
            // the file name anyway, and the picture shows the file name.
            m.insert(u"title"_qs, q.value(4).toBool() ? q.value(3).toString() : QString());
            known.insert(QDir::cleanPath(QDir::fromNativeSeparators(q.value(0).toString())), m);
        }
    }

    const QStringList names = dir.entryList(QDir::Files, QDir::Name);
    for (const QString &name : names) {
        const QString full = QDir::cleanPath(dir.absolutePath() + QLatin1Char('/') + name);
        const QString ext = QFileInfo(full).suffix().toLower();
        const bool video = m_videoExt.contains(ext);
        if (!video && !m_audioExt.contains(ext))
            continue;
        QVariantMap m = known.value(full);
        m.insert(u"name"_qs, name);
        m.insert(u"path"_qs, full);
        m.insert(u"url"_qs, QUrl::fromLocalFile(full).toString());
        m.insert(u"kind"_qs, video ? u"video"_qs : u"audio"_qs);
        if (!m.contains(u"duration"_qs))
            m.insert(u"duration"_qs, 0.0);
        // A file the catalogue has never seen has no row to take a title or a cover
        // from, and an absent key is not the same as an empty one: the dock reads
        // `it.title.length` and threw "Cannot read property 'length' of undefined" for
        // every not-yet-scanned file in the folder.
        if (!m.contains(u"title"_qs))
            m.insert(u"title"_qs, QString());
        if (!m.contains(u"poster"_qs))
            m.insert(u"poster"_qs, QString());
        out << m;
    }
    return out;
}

// A playlist path is stored in exactly the form the rest of the shell compares on -
// forward slashes, cleaned - because the dock finds a file's duration and cover by
// matching this text against `files.path`, and the two come from different places
// (the catalogue's own walk, a file dialog, a folder listing). The dialog hands over a
// file:// url, and QML has no way to turn that into a path in this Qt, so it arrives
// here as a string and is unwrapped.
static QString plKey(const QString &raw)
{
    QString path = raw;
    if (path.startsWith(u"file:"))
        path = QUrl(path).toLocalFile();
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QVariantList Library::playlists() const
{
    QVariantList out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    q.prepare(u"SELECT p.id, p.name, (SELECT COUNT(*) FROM playlist_items i"
              u" WHERE i.playlist_id = p.id) FROM playlists p ORDER BY p.ord, p.id"_qs);
    if (!q.exec())
        return out;
    while (q.next()) {
        QVariantMap m;
        m.insert(u"id"_qs, q.value(0).toLongLong());
        m.insert(u"name"_qs, q.value(1).toString());
        m.insert(u"count"_qs, q.value(2).toInt());
        out << m;
    }
    return out;
}

QVariantList Library::playlistItems(qint64 playlistId) const
{
    QVariantList out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q(m_db);
    // LEFT JOIN, not JOIN: a playlist entry the catalogue has never seen still plays,
    // it just has no duration and no cover to show.
    q.prepare(u"SELECT i.path, i.ord, f.duration, m.poster, m.title, COALESCE(m.custom, 0)"
              u" FROM playlist_items i"
              u" LEFT JOIN files f ON f.path = i.path"
              u" LEFT JOIN media m ON m.id = f.media_id"
              u" WHERE i.playlist_id = ? ORDER BY i.ord, i.id"_qs);
    q.addBindValue(playlistId);
    if (!q.exec())
        return out;
    while (q.next()) {
        const QString path = q.value(0).toString();
        QVariantMap m;
        m.insert(u"path"_qs, path);
        m.insert(u"url"_qs, QUrl::fromLocalFile(path).toString());
        m.insert(u"name"_qs, QFileInfo(path).fileName());
        m.insert(u"title"_qs, q.value(5).toBool() ? q.value(4).toString() : QString());
        m.insert(u"duration"_qs, q.value(2).isNull() ? 0.0 : q.value(2).toDouble());
        m.insert(u"poster"_qs, q.value(3).toString());
        out << m;
    }
    return out;
}

qint64 Library::createPlaylist(const QString &name)
{
    if (!m_db.isOpen())
        return -1;
    const QString n = name.trimmed().isEmpty() ? u"未命名播单"_qs : name.trimmed();
    QSqlQuery q(m_db);
    q.prepare(u"SELECT COALESCE(MAX(ord), -1) + 1 FROM playlists"_qs);
    int ord = 0;
    if (q.exec() && q.next())
        ord = q.value(0).toInt();
    q.prepare(u"INSERT INTO playlists(name, ord) VALUES (?, ?)"_qs);
    q.addBindValue(n);
    q.addBindValue(ord);
    if (!q.exec()) {
        qWarning("playlist create failed: %s", qPrintable(q.lastError().text()));
        return -1;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    qInfo("LIBRARY playlist created id=%lld \"%s\"", long long(id), qPrintable(n));
    emit playlistsChanged();
    return id;
}

void Library::renamePlaylist(qint64 id, const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        qInfo("LIBRARY playlist rename refused (empty) id=%lld", long long(id));
        return;
    }
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE playlists SET name = ? WHERE id = ?"_qs);
    q.addBindValue(n);
    q.addBindValue(id);
    if (!q.exec()) {
        qWarning("playlist rename failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    qInfo("LIBRARY playlist id=%lld renamed to \"%s\"", long long(id), qPrintable(n));
    emit playlistsChanged();
}

void Library::deletePlaylist(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(u"DELETE FROM playlist_items WHERE playlist_id = ?"_qs);
    q.addBindValue(id);
    if (!q.exec())
        qWarning("playlist items delete failed: %s", qPrintable(q.lastError().text()));
    q.prepare(u"DELETE FROM playlists WHERE id = ?"_qs);
    q.addBindValue(id);
    if (!q.exec()) {
        qWarning("playlist delete failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    qInfo("LIBRARY playlist deleted id=%lld", long long(id));
    emit playlistsChanged();
}

void Library::addToPlaylist(qint64 id, const QStringList &paths)
{
    if (!m_db.isOpen() || id < 0)
        return;
    QSqlQuery ord(m_db);
    ord.prepare(u"SELECT COALESCE(MAX(ord), -1) + 1 FROM playlist_items WHERE playlist_id = ?"_qs);
    ord.addBindValue(id);
    int next = 0;
    if (ord.exec() && ord.next())
        next = ord.value(0).toInt();
    int added = 0;
    for (const QString &raw : paths) {
        const QString path = plKey(raw);
        if (path.isEmpty())
            continue;
        // UNIQUE(playlist_id, path) is the dedupe: adding the same film twice is a no-op
        // rather than a second row that would then play twice in the queue.
        QSqlQuery q(m_db);
        q.prepare(u"INSERT OR IGNORE INTO playlist_items(playlist_id, path, ord) VALUES (?,?,?)"_qs);
        q.addBindValue(id);
        q.addBindValue(path);
        q.addBindValue(next++);
        if (q.exec() && q.numRowsAffected() > 0)
            ++added;
    }
    qInfo("LIBRARY playlist id=%lld added %d of %d", long long(id), added, int(paths.size()));
    if (added > 0)
        emit playlistsChanged();
}

void Library::removeFromPlaylist(qint64 id, const QString &path)
{
    QSqlQuery q(m_db);
    q.prepare(u"DELETE FROM playlist_items WHERE playlist_id = ? AND path = ?"_qs);
    q.addBindValue(id);
    q.addBindValue(plKey(path));
    if (!q.exec()) {
        qWarning("playlist item delete failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    emit playlistsChanged();
}

// Manual order, one step at a time: the row trades its `ord` with its neighbour. A
// single UPDATE would collide with nothing here because ord is not unique, but swapping
// keeps every other row's number untouched, which is what makes an interrupted drag or a
// repeated click idempotent.
void Library::movePlaylistItem(qint64 id, const QString &path, int dir)
{
    const QString mine = plKey(path);
    QSqlQuery find(m_db);
    find.prepare(u"SELECT id, ord FROM playlist_items WHERE playlist_id = ? AND path = ?"_qs);
    find.addBindValue(id);
    find.addBindValue(mine);
    if (!find.exec() || !find.next())
        return;
    const qint64 mineId = find.value(0).toLongLong();
    const int ord = find.value(1).toInt();
    QSqlQuery other(m_db);
    other.prepare(u"SELECT id, ord FROM playlist_items WHERE playlist_id = ? AND ord "
                  + QString(dir < 0 ? "<" : ">") + u" ? ORDER BY ord "
                  + QString(dir < 0 ? "DESC" : "ASC") + u" LIMIT 1"_qs);
    other.addBindValue(id);
    other.addBindValue(ord);
    if (!other.exec() || !other.next())
        return;                       // already first / last: nothing to trade
    const qint64 otherId = other.value(0).toLongLong();
    const int otherOrd = other.value(1).toInt();
    // Trade the two numbers: the neighbour takes the slot this row is leaving and this
    // row takes the one the neighbour vacated. Written the other way round - each row
    // bound to its own `ord` - both UPDATEs succeed and nothing moves, which is what the
    // up and down marks did on screen: no error, no change.
    QSqlQuery up(m_db);
    up.prepare(u"UPDATE playlist_items SET ord = ? WHERE id = ?"_qs);
    up.addBindValue(ord);
    up.addBindValue(otherId);
    if (!up.exec()) {
        qWarning("playlist reorder failed: %s", qPrintable(up.lastError().text()));
        return;
    }
    QSqlQuery back(m_db);
    back.prepare(u"UPDATE playlist_items SET ord = ? WHERE id = ?"_qs);
    back.addBindValue(otherOrd);
    back.addBindValue(mineId);
    if (!back.exec())
        qWarning("playlist reorder failed: %s", qPrintable(back.lastError().text()));
    emit playlistsChanged();
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
              "       (SELECT f.duration FROM files f WHERE f.media_id=m.id LIMIT 1), "
              "       (SELECT rs.pos FROM files f3 JOIN resume rs ON rs.path = f3.path "
              "         WHERE f3.media_id=m.id LIMIT 1) "
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
        r.pos = q.value(9).toDouble();
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

void Library::saveProgress(const QString &path, double pos, double dur, const QString &shot)
{
    if (path.isEmpty() || dur <= 0.0 || pos <= 0.0)
        return;
    // The catalogue stores forward slashes; a path typed with backslashes is the same
    // file but a different string to SQLite, so both sides settle on the file system's
    // own absolute form.
    const QString key = QFileInfo::exists(path) ? QFileInfo(path).absoluteFilePath() : path;
    QSqlQuery q(m_db);
    // The last minute of a film is not a place to come back to: the row goes away
    // instead of sitting at 99.7%, which would put a bar on the card that is a lie
    // about where the user stopped. The cover still goes with it.
    if (pos >= dur - 1.0) {
        q.prepare(u"SELECT shot FROM resume WHERE path = ?"_qs);
        q.addBindValue(key);
        if (q.exec() && q.next()) {
            const QString old = resumeShotRead(q.value(0).toString());
            if (!old.isEmpty())
                QFile::remove(old);
        }
        q.prepare(u"DELETE FROM resume WHERE path = ?"_qs);
        q.addBindValue(key);
        if (!q.exec())
            qWarning("saveProgress clear failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    q.prepare(u"INSERT INTO resume(path, pos, dur, upd, shot) VALUES(?, ?, ?, ?, ?) "
              "ON CONFLICT(path) DO UPDATE SET pos = excluded.pos, "
              "dur = excluded.dur, upd = excluded.upd, shot = excluded.shot"_qs);
    q.addBindValue(key);
    q.addBindValue(pos);
    q.addBindValue(dur);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    // A row may only point at a still that is actually there: the copy is written by
    // the render thread, and a close can beat it. Only the name is stored - see
    // resumeShotRead().
    q.addBindValue(QFileInfo::exists(shot) ? QFileInfo(shot).fileName() : QString());
    if (!q.exec())
        qWarning("saveProgress failed: %s", qPrintable(q.lastError().text()));
}

QVariantMap Library::lastPlayed() const
{
    QVariantMap out;
    QSqlQuery q(m_db);
    if (!q.exec(u"SELECT path, pos, dur, COALESCE(shot, '') FROM resume "
                u"ORDER BY upd DESC LIMIT 1"_qs)
        || !q.next())
        return out;
    out.insert(u"path"_qs, q.value(0).toString());
    out.insert(u"pos"_qs, q.value(1).toDouble());
    out.insert(u"dur"_qs, q.value(2).toDouble());
    out.insert(u"shot"_qs, resumeShotRead(q.value(3).toString()));
    return out;
}

QString Library::resumeShotPath(const QString &path) const
{
    // Same normalisation the row is keyed by, so the file and the row cannot disagree.
    const QString key = QFileInfo::exists(path) ? QFileInfo(path).absoluteFilePath() : path;
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(),
                                                  QCryptographicHash::Md5).toHex().left(16);
    return m_dir + u"/resume/"_qs + QString::fromLatin1(h) + u".jpg"_qs;
}

QString Library::resumeShotRead(const QString &stored) const
{
    // Rows written before this stored a full path; new rows keep just the name, so
    // moving or copying the catalogue takes its covers along instead of turning them
    // into "the file does not exist, drop the cover".
    if (stored.isEmpty())
        return QString();
    if (stored.contains(QLatin1Char('/')) || stored.contains(QLatin1Char('\\')))
        return QFileInfo::exists(stored) ? stored : QString();
    const QString p = m_dir + u"/resume/"_qs + stored;
    return QFileInfo::exists(p) ? p : QString();
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
    case PosRole: return r.pos;
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
        { PosRole, "pos" },
        { IdRole, "mediaId" },
    };
}

void LibraryModel::reload()
{
    beginResetModel();
    m_rows = m_lib->rows();
    endResetModel();
}
