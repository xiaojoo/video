#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;
class QProcess;
class QTimer;

// One job as the shell sees it. The row outlives its process: the column is drawn
// from here, so `running` is a fact about a child process and nothing else.
struct DownloadRow {
    qint64 id = 0;
    QString src;        // magnet URI, a .torrent file, or an http(s) media url
    QString kind = u"torrent"_qs; // torrent | http - picks the engine's mode
    QString name;       // what the swarm called it, until the swarm says otherwise
    QString dir;        // where the finished file lands
    QString poster;     // the site's card picture, carried so the row can show it
    qint64 total = 0;
    // What the drive quoted for the length of this file, in seconds. 0 = it did not say,
    // and the player then shows what mpv measured off the bytes that exist.
    double duration = 0.0;
    qint64 completed = 0;
    // The contiguous prefix the engine's own view can serve, in bytes. On a free-tier drive
    // this is smaller than completed: the tail lands next to a middle that answers 416
    // forever, and a reader sent past the prefix meets nothing.
    qint64 prefix = 0;
    double rate = 0.0;
    int piecesDone = 0;
    int numPieces = 0;
    int peers = 0;
    int seeders = 0;
    QString want = u"stream"_qs;
    QString resume;     // the engine's own sentence about picking up where it left off
    qint64 resumeMs = 0;
    bool complete = false;
    bool running = false;
    QString path;       // the file, once it is finished
    // The loopback base its last child reported, kept after that child exits. The player page
    // only knows the url it was handed, and a free-tier job whose engine has nothing left to
    // fetch exits on its own - dropping the child alone made the row unmatchable from the url,
    // which turned the bar blind: no length, a full-width band and a seek into 416.
    QString base;
    QString error;
};

// Drives PlayerDown.exe: one child per job, because that is the shape the engine has
// (one torrent per process) and it keeps one job's crash off the player window.
//
// Every number the column shows is read back over loopback HTTP from the child's own
// /status - the same endpoint the measuring harness reads - so there is no second
// protocol between shell and engine to keep in step.
class Downloads final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Bumped whenever a row's numbers move. The player page asks for a length and a byte
    // fraction through functions, and a QML binding on a function result has nothing to
    // listen to: it is evaluated once and then sits there. Without this the bar kept the
    // answer from the moment the film was opened - which, on a job whose tail had not
    // landed yet, is "no length quoted", i.e. mpv's estimate and a full bar that lets him
    // seek into bytes that do not exist.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit Downloads(QObject *parent = nullptr);
    ~Downloads() override;

    void setDatabase(const QSqlDatabase &db);

    Q_INVOKABLE QString defaultDir() const;
    // Set from --dl-dir so a demo or a harness never writes into the user's own
    // Movies folder.
    void setOverrideDir(const QString &dir);
    // True when --dl-dir is in force. The window reads this before it trusts the
    // folder stored in the settings card: a probe run must never write into whatever
    // the user picked in the UI, and the card must not pretend otherwise.
    Q_INVOKABLE bool dirOverridden() const { return !m_overrideDir.isEmpty(); }
    // How many connections an http job pulls with. The settings card owns the number and
    // its persistence; this is the engine's copy, read once per spawn so a change applies
    // to the next job rather than interrupting one that is running.
    Q_INVOKABLE void setConns(int n);
    int conns() const { return m_conns; }
    // Returns the row id, or -1 when the source is neither a magnet URI, an existing
    // .torrent file, nor an http(s) url the engine can pull.
    // `headers` is a block of "Name: value" lines sent with every request of an http
    // job; it is written beside the job's state, never into the catalogue or argv.
    // `name` overrides the file name the url would have implied - a 网盘 share calls a
    // 2026 film `4k.mp4`, and the row is read from the poster, not from the drive.
    Q_INVOKABLE qint64 add(const QString &source, const QString &dir = QString(),
                           const QString &poster = QString(),
                           const QString &headers = QString(),
                           const QString &name = QString(),
                           double duration = 0.0);
    Q_INVOKABLE void start(qint64 id);
    Q_INVOKABLE void stop(qint64 id);
    // Removes the row and its own state folder. The downloaded file is left exactly
    // where it is: this is the library's column, not a file manager.
    Q_INVOKABLE void remove(qint64 id, bool withFile = true);
    Q_INVOKABLE void setWant(qint64 id, const QString &mode);

    int count() const { return m_rows.size(); }
    int revision() const { return m_revision; }
    // Q_INVOKABLE, not just a getter: the 在线媒体 page reads the rows straight from
    // here (downloads.jobs()) instead of through a list model, and QML answers a plain
    // C++ getter with "Property 'jobs' of object Downloads is not a function".
    Q_INVOKABLE QVariantList jobs() const;
    // The url the player loads to watch a job while it is still filling: the child's
    // seekable HTTP view of the first file. Empty unless the job is running.
    Q_INVOKABLE QString playUrl(qint64 id) const;
    // The name to show while playing that url; a loopback path has none of its own.
    Q_INVOKABLE QString playTitle(qint64 id) const;
    Q_INVOKABLE QString filePath(qint64 id) const;
    // A finished job has no child left to ask, so the shell needs to know that
    // before it decides between the http view and the file itself.
    Q_INVOKABLE bool isComplete(qint64 id) const;
    // The two numbers a file that is still arriving needs to be shown honestly: how long it
    // is going to be, and how far in its bytes have actually come. Both are asked with the
    // url the player is holding, because that is all the player page has.
    Q_INVOKABLE double durationForUrl(const QString &url) const;
    Q_INVOKABLE qreal bufferedForUrl(const QString &url) const;
    // True while `path` is the file of a job that has not reached 100%: what is on
    // disk is a fragment. The library scans the download folder like any other root,
    // so such a fragment can be clicked, remembered as last played, and resumed on
    // the next launch - where it has no index to seek with and no picture to show.
    bool isUnfinished(const QString &path) const;

    // Fold one /status reply into the row. Public so the shell can reuse it later.
    void applyStatus(qint64 id, const QVariantMap &o);

signals:
    void countChanged();
    void revisionChanged();
    void rowChanged(qint64 id);
    // A job reached 100%. The shell can now offer the file wherever films live.
    void finished(qint64 id, const QString &path);

private:
    struct Child {
        QProcess *proc = nullptr;
        QString base;      // http://127.0.0.1:port, read from the child's LISTEN line
    };

    void ensureSchema();
    void load();
    void persist(const DownloadRow &row);
    DownloadRow *find(qint64 id);
    // The row a player url was made from, or null. Private because it reads the child map,
    // which holds the loopback address only while that job's process is alive.
    const DownloadRow *rowForPlayUrl(const QString &url) const;
    // A name the row would write to that another row or the disk already has.
    bool nameTaken(const QString &dir, const QString &name) const;
    // The same question playable() answers, on a row already in hand.
    bool rowPlayable(const DownloadRow &row) const;
    void spawn(DownloadRow &row);
    void poll(qint64 id);
    // One line of the child's stdout: LISTEN sets the view url, DONE carries the
    // engine's own final record.
    void ingestLine(qint64 id, const QString &line);
    QString stateDirFor(qint64 id) const;
    QString headersPathFor(qint64 id) const;
    void writeHeaders(qint64 id, const QString &headers);

    QString m_overrideDir;
    int m_conns = 8;
    QSqlDatabase m_db;
    QList<DownloadRow> m_rows;
    int m_revision = 0;
    QHash<qint64, Child> m_children;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_tick = nullptr;
};

class DownloadsModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        SrcRole,
        DirRole,
        TotalRole,
        CompletedRole,
        PctRole,
        RateRole,
        PiecesRole,
        NumPiecesRole,
        PeersRole,
        SeedersRole,
        WantRole,
        ResumeRole,
        ResumeMsRole,
        CompleteRole,
        RunningRole,
        PathRole,
        ErrorRole,
        KindRole,
    };

    explicit DownloadsModel(Downloads *d, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void reload();

private:
    Downloads *m_downloads = nullptr;
    QVariantList m_rows;
};
