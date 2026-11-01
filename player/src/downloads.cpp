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
    QString base = u.fileName();
    if (base.isEmpty())
        base = u.host();
    return base.isEmpty() ? u"下载"_qs : base;
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
                " poster, kind FROM downloads ORDER BY added DESC, id DESC"_qs)) {
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
        m_rows.append(r);
    }
    emit countChanged();
}

void Downloads::persist(const DownloadRow &row)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE downloads SET name=?, dir=?, total=?, completed=?, want=?,"
              " state=?, path=?, err=?, poster=?, kind=? WHERE id=?"_qs);
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
                      const QString &headers, const QString &name)
{
    const QString src = source.trimmed();
    const bool isMagnet = src.startsWith(u"magnet:"_qs);
    const bool isHttp = isHttpSource(src);
    const QFileInfo fi(src);
    if (src.isEmpty() || (!isMagnet && !isHttp && !(fi.exists() && fi.isFile())))
        return -1;

    const QString kind = isHttp ? u"http"_qs : u"torrent"_qs;
    const QString where = dir.isEmpty() ? defaultDir() : dir;
    QString initialName = isHttp ? nameFromUrl(src)
                             : (isMagnet ? tr("正在取种子信息") : fi.completeBaseName());
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
              " state, added, poster, kind) VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?,?)"_qs);
    q.addBindValue(src);
    q.addBindValue(initialName);
    q.addBindValue(where);
    q.addBindValue(0);
    q.addBindValue(0);
    q.addBindValue(u"all"_qs);
    q.addBindValue(u"stopped"_qs);
    q.addBindValue(poster.trimmed());
    q.addBindValue(kind);
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
             << u"-conns"_qs << QString::number(m_conns) << u"-chunk"_qs << u"8"_qs
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
        if (it != m_children.end())
            it.value().base = line.mid(7).trimmed();
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

void Downloads::remove(qint64 id)
{
    stop(id);
    if (m_rows.removeIf([id](const DownloadRow &r) { return r.id == id; })) {
        QDir d(stateDirFor(id));
        if (d.exists())
            d.removeRecursively();
        QSqlQuery q(m_db);
        q.prepare(u"DELETE FROM downloads WHERE id=?"_qs);
        q.addBindValue(id);
        q.exec();
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
    auto it = m_children.constFind(id);
    return it == m_children.constEnd() || it.value().base.isEmpty()
        ? QString() : it.value().base + u"/file/0"_qs;
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
    const QString name = o.value(u"name"_qs).toString();
    if (!name.isEmpty() && name != row->name)
        row->name = name;
    row->total = o.value(u"total"_qs).toLongLong();
    row->completed = o.value(u"completed"_qs).toLongLong();
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
