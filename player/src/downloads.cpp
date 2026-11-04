#include "downloads.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QEventLoop>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

namespace {
// How long the shell waits for a child to say goodbye at exit. A SIGKILL-equivalent
// would lose the piece-completion flush, and the whole point of resume is that the
// state on disk is trustworthy, so the shell asks first and waits.
constexpr int kGoodbyeMs = 2000;

QString canonicalKey(int role)
{
    static const QHash<int, QByteArray> names{
        {DownloadsModel::IdRole, "jobId"}, {DownloadsModel::NameRole, "jobName"},
        {DownloadsModel::SrcRole, "jobSrc"}, {DownloadsModel::DirRole, "jobDir"},
        {DownloadsModel::TotalRole, "jobTotal"}, {DownloadsModel::CompletedRole, "jobCompleted"},
        {DownloadsModel::PctRole, "jobPct"}, {DownloadsModel::RateRole, "jobRate"},
        {DownloadsModel::PiecesRole, "jobPieces"}, {DownloadsModel::NumPiecesRole, "jobNumPieces"},
        {DownloadsModel::PeersRole, "jobPeers"}, {DownloadsModel::SeedersRole, "jobSeeders"},
        {DownloadsModel::WantRole, "jobWant"}, {DownloadsModel::ResumeRole, "jobResume"},
        {DownloadsModel::ResumeMsRole, "jobResumeMs"}, {DownloadsModel::CompleteRole, "jobComplete"},
        {DownloadsModel::RunningRole, "jobRunning"}, {DownloadsModel::PathRole, "jobPath"},
        {DownloadsModel::ErrorRole, "jobError"}, {DownloadsModel::KindRole, "jobKind"},
    };
    return QString::fromLatin1(names.value(role));
}

bool isHttpSource(const QString &src)
{
    return src.startsWith(u"http://"_qs, Qt::CaseInsensitive)
        || src.startsWith(u"https://"_qs, Qt::CaseInsensitive);
}

// A 百度 share's playback table: a `.m3u8` sitting on this machine whose segments are remote.
// It is pulled by the http engine (the engine reads the table off disk and stitches the
// segments into one MPEG-TS), and it is *playable as it is* - libmpv opens the same table and
// fetches the segment it needs, which is why the row's play mark does not wait on the pull.
bool isTableSource(const QString &src)
{
    const QFileInfo fi(src);
    return !src.isEmpty() && fi.isFile()
        && fi.suffix().compare(u"m3u8"_qs, Qt::CaseInsensitive) == 0;
}

// Whether the bytes that exist so far could be demuxed at all. An mp4 that keeps its
// index (the moov box) at the tail has no sample table anywhere in the prefix, so no
// fraction short of the last box plays - measured on a 夸克 row at 44.33%: the head is
// ftyp(28 B) + free(8 B) + mdat(1,625,722,230 B) and moov is the final 1,271,359 B, which
// the demuxer answers with "moov atom not found". A faststart file shows moov before mdat;
// matroska and mpeg-ts carry their index as they go and stream from the first megabyte.
bool partialPlayable(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(65536);
    f.close();
    if (head.size() < 8)
        return false;
    // Sniffed from the first box, not from the extension: the row this was written for
    // was called `4K.mkv.part` and opened with `ftyp isom` (measured).
    static const QStringList boxed{ u"ftyp"_qs, u"moov"_qs, u"mdat"_qs, u"free"_qs,
                                    u"skip"_qs, u"wide"_qs, u"uuid"_qs, u"pdin"_qs,
                                    u"pnot"_qs, u"junk"_qs, u"mfra"_qs, u"meta"_qs };
    if (!boxed.contains(QString::fromLatin1(head.mid(4, 4))))
        return true;
    auto be32 = [&head](qsizetype at) {
        return (quint32(quint8(head.at(at))) << 24) | (quint32(quint8(head.at(at + 1))) << 16)
             | (quint32(quint8(head.at(at + 2))) << 8) | quint32(quint8(head.at(at + 3)));
    };
    qsizetype off = 0;
    while (off + 8 <= head.size()) {
        const QString type = QString::fromLatin1(head.mid(off + 4, 4));
        bool fourcc = !type.isEmpty();
        for (const QChar &c : type)
            fourcc = fourcc && c.unicode() >= 0x20 && c.unicode() < 0x7f;
        // Zeros where a box name belongs: the first part has not landed yet, so there is
        // no header on disk and the view has nothing to seek with.
        if (!fourcc)
            return false;
        if (type == u"moov"_qs)
            return true;
        if (type == u"mdat"_qs)
            return false;
        quint64 size = be32(off);
        if (size == 1) {
            if (off + 16 > head.size())
                return true;
            size = (quint64(be32(off + 8)) << 32) | be32(off + 12);
        } else if (size == 0) {
            return true;    // this box runs to the end, so nothing indexes after it
        }
        if (size < 8)
            return true;    // a header this walk cannot read is no reason to block playback
        off += qsizetype(size);
    }
    return true;    // neither index nor media inside the window: unknown, so let it try
}

// The last segment of the url is what the file will be called; a url without one gets
// the host, because "download" as a name tells the row nothing it did not already say.
// A 网盘's own file name is usually just a quality label - the drive hands over `4k.mp4`
// for a film the poster names in full. Take the name the poster carries and keep the
// extension the drive reported, so the row, the file and the library say the same thing.
QString namedLike(const QString &title, const QString &file)
{
    QString t = title.trimmed();
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    t.replace(bad, QStringLiteral("_"));
    if (t.isEmpty())
        return file;
    const QString ext = QFileInfo(file).suffix();
    // The caller's name may already carry it: 迅雷's hand-off says `.mkv` or `.ts` itself, and
    // appending it a second time put 「蜘蛛侠：崭新之日.ts.ts」 on the row - measured on screen.
    if (!ext.isEmpty() && t.endsWith(QLatin1Char('.') + ext, Qt::CaseInsensitive))
        return t;
    return ext.isEmpty() ? t : t + QLatin1Char('.') + ext;
}

QString nameFromUrl(const QString &src)
{
    const QUrl u(src);
    // A drive's signed url carries the file's own name in the query: 夸克 hands over
    // `.../cVVG0w1e/9?filename=2026.1080p.mp4&auth_key=...`, and the last path segment there is
    // a 40-character object id. Without this the row is named after the id.
    static const QStringList nameKeys{ u"filename"_qs, u"fn"_qs, u"name"_qs };
    const QUrlQuery q(u);
    for (const QString &k : nameKeys) {
        const QString v = q.queryItemValue(k, QUrl::FullyDecoded);
        if (!v.isEmpty())
            return v;
    }
    const QString base = u.fileName();
    if (!base.isEmpty())
        return base;
    // Empty here means "this url names no file". The host is not a file name: 迅雷's address is
    // `https://vod…-vip-lixian.xunlei.com/?fid=…&mid=…`, and handing that to namedLike() put the
    // last label of the domain into the row's extension slot, which read 「片名.com」.
    return QString();
}
}

Downloads::Downloads(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, [this] {
        for (const DownloadRow &r : std::as_const(m_rows))
            if (m_children.contains(r.id))
                poll(r.id);
    });
}

Downloads::~Downloads()
{
    QList<QProcess *> live;
    for (auto &c : m_children) {
        if (c.proc->state() == QProcess::NotRunning)
            continue;
        if (!c.base.isEmpty())
            m_nam->post(QNetworkRequest(QUrl(c.base + u"/quit"_qs)), QByteArray());
        live << c.proc;
    }
    if (live.isEmpty())
        return;

    // m_nam is a child of this object, so it is still alive at this point and those
    // posts are on its queue: pump events until the children are gone or the budget
    // fires. A child that ignores /quit is then killed, and the next start re-hashes
    // - a cost that has been measured rather than guessed at.
    QEventLoop loop;
    QTimer budget;
    budget.setSingleShot(true);
    QObject::connect(&budget, &QTimer::timeout, &loop, &QEventLoop::quit);
    for (QProcess *p : std::as_const(live))
        QObject::connect(p, &QProcess::finished, &loop, [&loop, &live] {
            for (QProcess *q : std::as_const(live))
                if (q->state() != QProcess::NotRunning)
                    return;
            loop.quit();
        });
    budget.start(kGoodbyeMs);
    loop.exec();
    for (QProcess *p : std::as_const(live))
        if (p->state() != QProcess::NotRunning)
            p->kill();
}

void Downloads::ensureSchema()
{
    QSqlQuery q(m_db);
    if (!q.exec(u"CREATE TABLE IF NOT EXISTS downloads("
                " id INTEGER PRIMARY KEY, src TEXT UNIQUE, name TEXT, dir TEXT,"
                " total INTEGER, completed INTEGER, want TEXT, state TEXT,"
                " path TEXT, added INTEGER, done INTEGER, err TEXT, poster TEXT)"_qs))
        qWarning("downloads ddl failed: %s", qPrintable(q.lastError().text()));
    // A catalogue written before the row had a picture has no such column, and CREATE
    // TABLE IF NOT EXISTS will never add one. SQLite has no "if not exists" on ALTER,
    // so the duplicate is the expected answer on every later start and is dropped.
    if (!q.exec(u"ALTER TABLE downloads ADD COLUMN poster TEXT"_qs)
        && !q.lastError().text().contains(u"duplicate column name"_qs, Qt::CaseInsensitive))
        qWarning("downloads migration failed: %s", qPrintable(q.lastError().text()));
    // Same story for the engine kind: rows written before it existed are torrents.
    if (!q.exec(u"ALTER TABLE downloads ADD COLUMN kind TEXT"_qs)
        && !q.lastError().text().contains(u"duplicate column name"_qs, Qt::CaseInsensitive))
        qWarning("downloads kind migration failed: %s", qPrintable(q.lastError().text()));
    // The length the drive quoted. A row written before this column simply has no length to
    // offer, and the player then shows mpv's own number instead of guessing one.
    if (!q.exec(u"ALTER TABLE downloads ADD COLUMN dur REAL"_qs)
        && !q.lastError().text().contains(u"duplicate column name"_qs, Qt::CaseInsensitive))
        qWarning("downloads dur migration failed: %s", qPrintable(q.lastError().text()));
}

void Downloads::setDatabase(const QSqlDatabase &db)
{
    m_db = db;
    ensureSchema();
    load();
    m_tick->start();
}

void Downloads::setOverrideDir(const QString &dir)
{
    m_overrideDir = dir;
}

// Clamped rather than rejected: the card's stepper can be clicked faster than the engine
// can be told, and a job that silently falls back to one connection is worse than a job
// that uses the nearest legal number.
void Downloads::setConns(int n)
{
    m_conns = qBound(1, n, 16);
}

QString Downloads::defaultDir() const
{
    if (!m_overrideDir.isEmpty())
        return m_overrideDir;

    QString d = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (d.isEmpty() || !QFileInfo(d).isDir())
        d = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return d;
}

QString Downloads::stateDirFor(qint64 id) const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(u"downloads/%1"_qs.arg(id));
}

QString Downloads::headersPathFor(qint64 id) const
{
    return QDir(stateDirFor(id)).filePath(u"headers.txt"_qs);
}

void Downloads::load()
{
    m_rows.clear();
    QSqlQuery q(m_db);
    if (!q.exec(u"SELECT id, src, name, dir, total, completed, want, state, path, err,"
                " poster, kind, dur FROM downloads ORDER BY added DESC, id DESC"_qs)) {
        qWarning("downloads select failed: %s", qPrintable(q.lastError().text()));
        return;
    }
    while (q.next()) {
        DownloadRow r;
        r.id = q.value(0).toLongLong();
        r.src = q.value(1).toString();
        r.name = q.value(2).toString();
        r.dir = q.value(3).toString();
        r.total = q.value(4).toLongLong();
        r.completed = q.value(5).toLongLong();
        r.want = q.value(6).toString();
        r.complete = q.value(7).toString() == u"done"_qs;
        r.path = q.value(8).toString();
        r.error = q.value(9).toString();
        r.poster = q.value(10).toString();
        const QString kind = q.value(11).toString();
        r.kind = kind.isEmpty() ? u"torrent"_qs : kind;
        r.duration = q.value(12).toDouble();
        m_rows.append(r);
    }
    emit countChanged();
}

void Downloads::persist(const DownloadRow &row)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE downloads SET name=?, dir=?, total=?, completed=?, want=?,"
              " state=?, path=?, err=?, poster=?, kind=?, dur=? WHERE id=?"_qs);
    q.addBindValue(row.name);
    q.addBindValue(row.dir);
    q.addBindValue(row.total);
    q.addBindValue(row.completed);
    q.addBindValue(row.want);
    q.addBindValue(row.complete ? u"done"_qs : (row.running ? u"running"_qs : u"stopped"_qs));
    q.addBindValue(row.path);
    q.addBindValue(row.error);
    q.addBindValue(row.poster);
    q.addBindValue(row.kind);
    q.addBindValue(row.duration);
    q.addBindValue(row.id);
    if (!q.exec())
        qWarning("downloads update failed: %s", qPrintable(q.lastError().text()));
}

DownloadRow *Downloads::find(qint64 id)
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].id == id)
            return &m_rows[i];
    return nullptr;
}

bool Downloads::nameTaken(const QString &dir, const QString &name) const
{
    for (const DownloadRow &r : m_rows)
        if (r.dir == dir && r.name.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return QFile::exists(QDir(dir).filePath(name));
}

qint64 Downloads::add(const QString &source, const QString &dir, const QString &poster,
                      const QString &headers, const QString &name, double duration)
{
    const QString src = source.trimmed();
    const bool isMagnet = src.startsWith(u"magnet:"_qs);
    const bool isTable = isTableSource(src);
    const bool isHttp = isHttpSource(src) || isTable;
    const QFileInfo fi(src);
    if (src.isEmpty() || (!isMagnet && !isHttp && !(fi.exists() && fi.isFile())))
        return -1;

    const QString kind = isHttp ? u"http"_qs : u"torrent"_qs;
    const QString where = dir.isEmpty() ? defaultDir() : dir;
    // A table's payload is MPEG-TS no matter what the share called the original, so the row
    // says `.ts`: naming the stitched stream `4k.mp4` would be the second time this week a
    // row's extension lied about the container inside it.
    QString initialName = isTable ? fi.completeBaseName() + u".ts"_qs
                            : isHttp ? nameFromUrl(src)
                            : (isMagnet ? tr("正在取种子信息") : fi.completeBaseName());
    if (initialName.isEmpty())
        // The url named no file, so the board's title is the name. With no title either - a
        // paste in 新建下载 - the host is the only thing left to point at, with its dots out so
        // nothing downstream can read the last label of a domain as a file extension.
        initialName = name.trimmed().isEmpty()
            ? QStringLiteral("%1 的下载").arg(QUrl(src).host().replace(QLatin1Char('.'),
                                                                      QLatin1Char('_')))
            : name.trimmed();
    // A 网盘 share hands the same film in a dozen encodes, so the board's title arrives more
    // than once; two rows writing one path would make the second read like a finished copy of
    // the first. Only the named path is touched - a magnet's name is the torrent's own.
    if (!name.trimmed().isEmpty()) {
        const QString wanted = namedLike(name, initialName);
        const QFileInfo wf(wanted);
        const QString tail = wf.suffix().isEmpty() ? QString() : QLatin1Char('.') + wf.suffix();
        initialName = wanted;
        for (int n = 2; nameTaken(where, initialName); ++n)
            initialName = wf.completeBaseName() + u" (%1)"_qs.arg(n) + tail;
    }

    QSqlQuery q(m_db);
    q.prepare(u"INSERT OR IGNORE INTO downloads(src, name, dir, total, completed, want,"
              " state, added, poster, kind, dur)"
              " VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?,?,?)"_qs);
    q.addBindValue(src);
    q.addBindValue(initialName);
    q.addBindValue(where);
    q.addBindValue(0);
    q.addBindValue(0);
    q.addBindValue(u"all"_qs);
    q.addBindValue(u"stopped"_qs);
    q.addBindValue(poster.trimmed());
    q.addBindValue(kind);
    q.addBindValue(duration);
    if (!q.exec()) {
        qWarning("downloads insert failed: %s", qPrintable(q.lastError().text()));
        return -1;
    }
    if (q.numRowsAffected() == 0) {
        // The same magnet twice is the same job: no second process for it. But the click
        // still has to do something - a stopped row that stays stopped reads as "the
        // download does not work", which is what it looked like when re-adding a finished
        // or cancelled job returned an id and left it idle.
        QSqlQuery dup(m_db);
        dup.prepare(u"SELECT id, poster FROM downloads WHERE src=?"_qs);
        dup.addBindValue(src);
        if (dup.exec() && dup.next()) {
            const qint64 again = dup.value(0).toLongLong();
            // A row added from the paste box has no picture; picking the same seed off
            // the wall a second time should still light the card up.
            if (dup.value(1).toString().isEmpty() && !poster.trimmed().isEmpty())
                if (DownloadRow *r = find(again)) {
                    r->poster = poster.trimmed();
                    persist(*r);
                    emit rowChanged(again);
                }
            // A retried url may arrive with a fresh signature, so the headers that came
            // with it replace the ones the first attempt stored.
            if (!headers.trimmed().isEmpty())
                writeHeaders(again, headers);
            start(again);
            return again;
        }
        return -1;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    DownloadRow r;
    r.id = id;
    r.src = src;
    r.kind = kind;
    r.name = initialName;
    r.dir = where;
    r.poster = poster.trimmed();
    if (!headers.trimmed().isEmpty())
        writeHeaders(id, headers);
    // The action is called 加入下载, so the job has to download. "stream" pulls nothing
    // until a reader asks, which made a freshly added row read "0 KiB / 2.78 GiB" forever
    // and look exactly like a broken download. The row's own chip flips it to 边看 before
    // playing, and that is the mode the fast first frame belongs to.
    r.want = u"all"_qs;
    m_rows.prepend(r);
    emit countChanged();
    emit rowChanged(id);
    start(id);
    return id;
}

void Downloads::writeHeaders(qint64 id, const QString &headers)
{
    if (!QDir().mkpath(stateDirFor(id)))
        return;
    QFile f(headersPathFor(id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("downloads headers write failed: %s", qPrintable(f.errorString()));
        return;
    }
    f.write(headers.toUtf8());
}

void Downloads::start(qint64 id)
{
    DownloadRow *row = find(id);
    if (!row || row->complete || m_children.contains(id))
        return;
    spawn(*row);
}

void Downloads::spawn(DownloadRow &row)
{
    const QString exe = QCoreApplication::applicationDirPath() + u"/PlayerDown.exe"_qs;
    if (!QFileInfo::exists(exe)) {
        row.error = tr("找不到下载进程 PlayerDown.exe");
        persist(row);
        emit rowChanged(row.id);
        return;
    }
    QDir().mkpath(stateDirFor(row.id));

    Child c;
    c.proc = new QProcess(this);
    c.proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(c.proc, &QProcess::readyReadStandardOutput, this, [this, id = row.id, proc = c.proc] {
        while (proc->canReadLine())
            ingestLine(id, QString::fromUtf8(proc->readLine()).trimmed());
    });
    connect(c.proc, &QProcess::finished, this, [this, id = row.id, proc = c.proc](int code) {
        // Whatever the pipe still holds is the end of the story, and it arrives here
        // before `finished` does not: a small file can be complete long before the
        // first /status poll, and only the DONE line says so.
        for (const QString &line : QString::fromUtf8(proc->readAllStandardOutput()).split(u'\n'))
            ingestLine(id, line.trimmed());
        // An http job that dies before LISTEN leaves no row state at all, so the child's
        // own sentence is the only thing that says why: 403, an expired link, a host that
        // never answered. "下载进程退出（1）" is the message that makes people retry blindly.
        // Only lines the engine marks as errors count; it also logs progress there.
        QString lastErr;
        for (const QString &line : QString::fromUtf8(proc->readAllStandardError()).split(u'\n')) {
            const QString t = line.trimmed();
            if (t.startsWith(u"ERROR "_qs))
                lastErr = t.mid(6);
        }
        DownloadRow *r = find(id);
        if (r) {
            r->running = false;
            if (!r->complete) {
                if (!lastErr.isEmpty())
                    r->error = lastErr;
                // Two different failures read the same exit code. A job that never got
                // its piece list died on the metadata wait, and "进程退出（1）" tells
                // nobody that the answer is to point it at a seed that answers.
                else if (code != 0)
                    r->error = r->kind == u"http"_qs
                        ? tr("下载进程退出（%1）").arg(code)
                        : (r->total <= 0
                               ? tr("没连上做种：种子信息还没拿到，点播放重试")
                               : tr("下载进程退出（%1）").arg(code));
            }
            persist(*r);
            emit rowChanged(id);
        }
        m_children.remove(id);
    });

    // Port 0 lets the engine choose and report it on its stdout line; hunting for a
    // free port here would only race whatever else on the box is binding, and this
    // machine has ports reserved in ways netstat does not show.
    // Extra engine flags, environment-only: a demo or a harness needs to point the
    // job at a swarm the user's machine can actually reach, and on this box the
    // public one answers 0 peers. Nothing in the UI reads or writes this.
    QStringList args;
    if (row.kind == u"http"_qs) {
        args << u"-mode"_qs << u"http"_qs << u"-data"_qs << row.dir
             << u"-url"_qs << row.src << u"-out-name"_qs << row.name
             << u"-conns"_qs << QString::number(m_conns) << u"-chunk"_qs << u"2"_qs
             << u"-http"_qs << u"127.0.0.1:0"_qs << u"-interval"_qs << u"1s"_qs;
        // The path, not the text: a process list is readable by anything on the box.
        if (QFileInfo::exists(headersPathFor(row.id)))
            args << u"-headers"_qs << headersPathFor(row.id);
    } else {
        args << u"-mode"_qs << u"download"_qs << u"-data"_qs << row.dir
             << u"-state"_qs << stateDirFor(row.id) << u"-src"_qs << row.src
             << u"-http"_qs << u"127.0.0.1:0"_qs << u"-want"_qs << row.want
             << u"-interval"_qs << u"1s"_qs;
    }
    const QString extra = qEnvironmentVariable("PLAYERUI_DL_ARGS");
    if (!extra.isEmpty())
        args += extra.split(u' ', Qt::SkipEmptyParts);
    // One line per job, carrying the number the settings card just set: argv is the only
    // place that number lands, so "did the change take effect" is answerable from the log
    // instead of from a stopwatch.
    qInfo("downloads spawn id=%lld kind=%s conns=%d name=%s",
          static_cast<long long>(row.id), qPrintable(row.kind), m_conns,
          qPrintable(QString::fromUtf8(row.name.toUtf8().toPercentEncoding().left(60))));
    c.proc->start(exe, args);
    row.running = true;
    row.error.clear();
    m_children.insert(row.id, c);
    persist(row);
    emit rowChanged(row.id);
}

void Downloads::ingestLine(qint64 id, const QString &line)
{
    if (line.startsWith(u"LISTEN "_qs)) {
        auto it = m_children.find(id);
        if (it == m_children.end())
            return;
        it.value().base = line.mid(7).trimmed();
        // And on the row, where it survives the child. A free-tier job runs out of fetchable
        // blocks and exits; the film it handed to mpv keeps playing, and the bar still has to
        // know which row that url belongs to.
        if (DownloadRow *r = find(id))
            r->base = it.value().base;
        return;
    }
    if (!line.startsWith(u"DONE "_qs))
        return;
    const QVariantMap o = QJsonDocument::fromJson(line.mid(5).toUtf8()).object().toVariantMap();
    if (o.value(u"complete"_qs).toBool())
        applyStatus(id, o);
}

void Downloads::stop(qint64 id)
{
    auto it = m_children.find(id);
    if (it == m_children.end() || it.value().base.isEmpty())
        return;
    // The engine exposes /quit for exactly this: a GUI process cannot deliver a Unix
    // signal to a child on Windows, and an unclean exit is what resume must not eat.
    m_nam->post(QNetworkRequest(QUrl(it.value().base + u"/quit"_qs)), QByteArray());
}

void Downloads::remove(qint64 id, bool withFile)
{
    stop(id);
    // The row is read before it goes, because the file it names is what he is asking for:
    // a finished film at `path`, and while a job is still filling the bytes live at
    // `<dir>/<name>.part` with the engine's own `<file>.part.json` beside it.
    const DownloadRow *r = find(id);
    const QString finalPath = r ? r->path : QString();
    const QString dir = r ? r->dir : QString();
    const QString name = r ? r->name : QString();
    if (m_rows.removeIf([id](const DownloadRow &r) { return r.id == id; })) {
        QDir d(stateDirFor(id));
        if (d.exists())
            d.removeRecursively();
        int gone = 0;
        qint64 bytes = 0;
        if (withFile) {
            QStringList victims;
            if (!finalPath.isEmpty())
                victims << finalPath;
            if (!dir.isEmpty() && !name.isEmpty()) {
                const QString part = QDir(dir).filePath(name + u".part"_qs);
                victims << part << (part + u".json"_qs);
                // A finished job's `.part.json` is left beside the file it renamed.
                victims << QDir(dir).filePath(name + u".part.json"_qs);
            }
            for (const QString &p : victims) {
                QFileInfo fi(p);
                if (!fi.exists())
                    continue;
                const qint64 sz = fi.size();
                if (QFile::remove(p)) {
                    ++gone;
                    bytes += sz;
                } else {
                    qWarning("dl file kept id=%lld (remove refused)", static_cast<long long>(id));
                }
            }
        }
        QSqlQuery q(m_db);
        q.prepare(u"DELETE FROM downloads WHERE id=?"_qs);
        q.addBindValue(id);
        q.exec();
        qInfo("dl removed id=%lld files=%d bytes=%lld withFile=%d",
              static_cast<long long>(id), gone, static_cast<long long>(bytes), int(withFile));
        emit countChanged();
    }
}

void Downloads::setWant(qint64 id, const QString &mode)
{
    DownloadRow *row = find(id);
    if (!row || (mode != u"stream"_qs && mode != u"all"_qs))
        return;
    row->want = mode;
    persist(*row);
    emit rowChanged(id);
    auto it = m_children.constFind(id);
    if (it != m_children.constEnd() && !it.value().base.isEmpty())
        m_nam->post(QNetworkRequest(QUrl(it.value().base + u"/want/"_qs + mode)), QByteArray());
}

QString Downloads::playUrl(qint64 id) const
{
    // A table row plays the table: the segments are absolute https that need no cookie, so
    // libmpv pulls the one it needs and the row never has to wait on the pull. Once the file
    // is complete the caller loads that instead, and it is seekable from the first byte.
    for (const DownloadRow &r : m_rows) {
        if (r.id == id && isTableSource(r.src))
            return QUrl::fromLocalFile(r.src).toString();
    }
    auto it = m_children.constFind(id);
    return it == m_children.constEnd() || it.value().base.isEmpty()
        ? QString() : it.value().base + u"/file/0"_qs;
}

// The row a player url belongs to, by the only two shapes the shell ever loads: the child's
// loopback view (http://127.0.0.1:port/file/0) while a job is running, and the finished file
// once it is not. Everything else answers "1.0 / 0.0" - a file the shell cannot place is a
// file it has all of, as far as a progress bar is concerned.
const DownloadRow *Downloads::rowForPlayUrl(const QString &url) const
{
    for (const DownloadRow &r : m_rows) {
        const auto it = m_children.constFind(r.id);
        if (it != m_children.constEnd() && !it.value().base.isEmpty()
            && url.startsWith(it.value().base))
            return &r;
        // The child is gone but its url is still the one mpv is holding.
        if (!r.base.isEmpty() && url.startsWith(r.base))
            return &r;
        if (!r.path.isEmpty() && url == r.path)
            return &r;
    }
    return nullptr;
}

double Downloads::durationForUrl(const QString &url) const
{
    const DownloadRow *r = rowForPlayUrl(url);
    return r ? r->duration : 0.0;
}

qreal Downloads::bufferedForUrl(const QString &url) const
{
    const DownloadRow *r = rowForPlayUrl(url);
    if (!r || r->complete)
        return 1.0;
    // Nothing is known to be here yet: the child has not answered its first /status. A full
    // bar here would be a promise that every point is playable, which is the one thing this
    // bar must not promise.
    if (r->total <= 0)
        return 0.0;
    // The byte fraction is only honest on a timeline that is the whole film. Without a
    // The byte fraction is honest on either axis. mpv maps a seek to a byte offset by the same
    // linear rule it uses to estimate its own length, so a point at fraction f of the bar lands
    // at f of the declared size whether the axis is the drive's number or mpv's guess - and the
    // clamp has to be able to say "not past here" even in the first minute of a job, when nobody
    // has quoted anything. This used to return 1.0 whenever the drive had not spoken, which is
    // exactly how a fresh row let a click land in bytes that do not exist and paint a black
    // screen.
    // completed counts the tail the drive does hand over, and a reader cannot reach across
    // the middle that it refuses. The prefix is where the bytes stop being contiguous, and
    // that is the edge the bar and the clamp have to speak of. A job that is simply whole
    // (a torrent, or a drive that gives everything) has no such hole and lands on completed.
    const qint64 reachable = r->prefix > 0 ? r->prefix : r->completed;
    return qreal(reachable) / qreal(r->total);
}

QString Downloads::playTitle(qint64 id) const
{
    // The header for a streamed job: the row's own file name, which is what playing
    // the finished file gives too. Without it libmpv titles the loopback url after
    // its last segment and the page reads "0".
    for (const DownloadRow &r : m_rows) {
        if (r.id == id)
            return r.name;
    }
    return {};
}

QString Downloads::filePath(qint64 id) const
{
    for (const DownloadRow &r : m_rows) {
        if (r.id != id)
            continue;
        if (!r.path.isEmpty())
            return r.path;
        return r.name.isEmpty() ? QString() : QDir(r.dir).filePath(r.name);
    }
    return {};
}

bool Downloads::isUnfinished(const QString &path) const
{
    if (path.isEmpty())
        return false;
    const QString want = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const DownloadRow &r : m_rows) {
        if (r.complete || r.name.isEmpty() || r.dir.isEmpty())
            continue;
        const QString at = QDir::cleanPath(QDir(r.dir).filePath(r.name));
        if (at.compare(want, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void Downloads::applyStatus(qint64 id, const QVariantMap &o)
{
    DownloadRow *row = find(id);
    if (!row || o.isEmpty())
        return;
    const qint64 wasTotal = row->total;
    const qint64 wasCompleted = row->completed;
    const qint64 wasPrefix = row->prefix;
    const double wasDuration = row->duration;
    const QString name = o.value(u"name"_qs).toString();
    if (!name.isEmpty() && name != row->name)
        row->name = name;
    row->total = o.value(u"total"_qs).toLongLong();
    row->completed = o.value(u"completed"_qs).toLongLong();
    row->prefix = o.value(u"prefix"_qs).toLongLong();
    // The length read off the file's own timestamps. The drive's number wins when it gave
    // one; 迅雷's field is there and says "0", so for those jobs this is all there is.
    const double measured = o.value(u"duration"_qs).toDouble();
    if (measured > 0.0 && row->duration <= 0.0)
        row->duration = measured;
    row->rate = o.value(u"rate"_qs).toDouble();
    row->piecesDone = o.value(u"piecesDone"_qs).toInt();
    row->numPieces = o.value(u"numPieces"_qs).toInt();
    row->peers = o.value(u"peers"_qs).toInt();
    row->seeders = o.value(u"seeders"_qs).toInt();
    row->resume = o.value(u"resume"_qs).toString();
    row->resumeMs = o.value(u"resumeMs"_qs).toLongLong();
    const QString want = o.value(u"want"_qs).toString();
    if (!want.isEmpty())
        row->want = want;
    const bool done = o.value(u"complete"_qs).toBool();
    const bool justDone = done && !row->complete;
    if (justDone) {
        row->complete = true;
        row->error.clear();
        row->path = QDir(row->dir).filePath(row->name);
    }
    if (wasTotal != row->total || wasCompleted != row->completed || wasPrefix != row->prefix
        || wasDuration != row->duration) {
        ++m_revision;
        emit revisionChanged();
    }
    persist(*row);
    emit rowChanged(id);
    if (justDone)
        emit finished(id, row->path);
}

void Downloads::poll(qint64 id)
{
    auto it = m_children.constFind(id);
    if (it == m_children.constEnd() || it.value().base.isEmpty())
        return;
    QNetworkReply *rep = m_nam->get(QNetworkRequest(QUrl(it.value().base + u"/status"_qs)));
    connect(rep, &QNetworkReply::finished, this, [this, id, rep] {
        const QByteArray body = rep->readAll();
        const bool ok = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        if (!ok)
            return;
        applyStatus(id, QJsonDocument::fromJson(body).object().toVariantMap());
    });
}

bool Downloads::isComplete(qint64 id) const
{
    for (const DownloadRow &r : m_rows)
        if (r.id == id)
            return r.complete;
    return false;
}

// Whether the bytes that exist right now could be demuxed at all, which is what the row's
// play mark answers before it offers to play. A finished row always can; a filling one
// depends on where its container keeps its index, not on how much has landed.
bool Downloads::rowPlayable(const DownloadRow &row) const
{
    if (row.complete)
        return true;
    // The table itself is the stream: nothing about the pull has to land first.
    if (isTableSource(row.src))
        return true;
    if (row.name.isEmpty() || row.dir.isEmpty())
        return false;
    // An http row is a film only once its own bar has moved. The engine counts the block
    // still being written into `prefix`, so `prefix > 0` was already true while the row read
    // 0.0% - and pressing it opened a page that sat black until a block landed. `completed`
    // is the number the row's bar is drawn from, so the mark and the bar answer to one fact:
    // bytes have arrived. `prefix > 0` is kept beside it because a holed job counts its tail
    // in `completed` too, and a tail with no head has nothing to start from.
    // Torrent rows do not answer the prefix field at all, so they are left to the sniff below.
    if (row.kind == u"http"_qs && (row.completed <= 0 || row.prefix <= 0))
        return false;
    // While a job fills, the http engine writes beside the final name with a `.part`
    // suffix; the name the row carries only exists after the rename.
    const QString part = QDir(row.dir).filePath(row.name + u".part"_qs);
    if (QFile::exists(part))
        return partialPlayable(part);
    const QString at = QDir(row.dir).filePath(row.name);
    return QFile::exists(at) && partialPlayable(at);
}

QVariantList Downloads::jobs() const
{
    QVariantList out;
    for (const DownloadRow &r : m_rows) {
        QVariantMap m;
        m.insert(u"jobId"_qs, r.id);
        m.insert(u"jobName"_qs, r.name);
        m.insert(u"jobKind"_qs, r.kind);
        m.insert(u"jobSrc"_qs, r.src);
        m.insert(u"jobDir"_qs, r.dir);
        m.insert(u"jobPoster"_qs, r.poster);
        m.insert(u"jobTotal"_qs, r.total);
        m.insert(u"jobDuration"_qs, r.duration);
        m.insert(u"jobCompleted"_qs, r.completed);
        m.insert(u"jobPct"_qs, r.total > 0 ? 100.0 * double(r.completed) / double(r.total) : 0.0);
        m.insert(u"jobRate"_qs, r.rate);
        m.insert(u"jobPieces"_qs, r.piecesDone);
        m.insert(u"jobNumPieces"_qs, r.numPieces);
        m.insert(u"jobPeers"_qs, r.peers);
        m.insert(u"jobSeeders"_qs, r.seeders);
        m.insert(u"jobWant"_qs, r.want);
        m.insert(u"jobResume"_qs, r.resume);
        m.insert(u"jobResumeMs"_qs, r.resumeMs);
        m.insert(u"jobComplete"_qs, r.complete);
        m.insert(u"jobPlayable"_qs, rowPlayable(r));
        m.insert(u"jobRunning"_qs, r.running);
        m.insert(u"jobPath"_qs, r.path);
        m.insert(u"jobError"_qs, r.error);
        out.append(m);
    }
    return out;
}

DownloadsModel::DownloadsModel(Downloads *d, QObject *parent)
    : QAbstractListModel(parent), m_downloads(d)
{
    reload();
    connect(d, &Downloads::countChanged, this, &DownloadsModel::reload);
    connect(d, &Downloads::rowChanged, this, [this](qint64 id) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].toMap().value(u"jobId"_qs).toLongLong() != id)
                continue;
            m_rows[i] = m_downloads->jobs().at(i);
            emit dataChanged(index(i), index(i));
            return;
        }
    });
}

int DownloadsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> DownloadsModel::roleNames() const
{
    return {
        {IdRole, "jobId"}, {NameRole, "jobName"}, {SrcRole, "jobSrc"}, {DirRole, "jobDir"},
        {TotalRole, "jobTotal"}, {CompletedRole, "jobCompleted"}, {PctRole, "jobPct"},
        {RateRole, "jobRate"}, {PiecesRole, "jobPieces"}, {NumPiecesRole, "jobNumPieces"},
        {PeersRole, "jobPeers"}, {SeedersRole, "jobSeeders"}, {WantRole, "jobWant"},
        {ResumeRole, "jobResume"}, {ResumeMsRole, "jobResumeMs"}, {CompleteRole, "jobComplete"},
        {RunningRole, "jobRunning"}, {PathRole, "jobPath"}, {ErrorRole, "jobError"},
        {KindRole, "jobKind"},
    };
}

QVariant DownloadsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    return m_rows.at(index.row()).toMap().value(canonicalKey(role));
}

void DownloadsModel::reload()
{
    beginResetModel();
    m_rows = m_downloads->jobs();
    endResetModel();
}
