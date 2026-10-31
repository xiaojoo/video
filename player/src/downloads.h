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
    qint64 completed = 0;
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
    // Returns the row id, or -1 when the source is neither a magnet URI, an existing
    // .torrent file, nor an http(s) url the engine can pull.
    // `headers` is a block of "Name: value" lines sent with every request of an http
    // job; it is written beside the job's state, never into the catalogue or argv.
    Q_INVOKABLE qint64 add(const QString &source, const QString &dir = QString(),
                           const QString &poster = QString(),
                           const QString &headers = QString());
    Q_INVOKABLE void start(qint64 id);
    Q_INVOKABLE void stop(qint64 id);
    // Removes the row and its own state folder. The downloaded file is left exactly
    // where it is: this is the library's column, not a file manager.
    Q_INVOKABLE void remove(qint64 id);
    Q_INVOKABLE void setWant(qint64 id, const QString &mode);

    int count() const { return m_rows.size(); }
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
    // True while `path` is the file of a job that has not reached 100%: what is on
    // disk is a fragment. The library scans the download folder like any other root,
    // so such a fragment can be clicked, remembered as last played, and resumed on
    // the next launch - where it has no index to seek with and no picture to show.
    bool isUnfinished(const QString &path) const;

    // Fold one /status reply into the row. Public so the shell can reuse it later.
    void applyStatus(qint64 id, const QVariantMap &o);

signals:
    void countChanged();
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
    void spawn(DownloadRow &row);
    void poll(qint64 id);
    // One line of the child's stdout: LISTEN sets the view url, DONE carries the
    // engine's own final record.
    void ingestLine(qint64 id, const QString &line);
    QString stateDirFor(qint64 id) const;
    QString headersPathFor(qint64 id) const;
    void writeHeaders(qint64 id, const QString &headers);

    QString m_overrideDir;
    QSqlDatabase m_db;
    QList<DownloadRow> m_rows;
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
