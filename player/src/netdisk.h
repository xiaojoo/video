#pragma once

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QWebSocket;
class QTimer;

// The 夸克 side of the magnet board: a share link in, a running download out.
//
// What this class does is only the part a third-party downloader cannot do on its own: walk
// the share down to a file, save a copy to the account (the only way the api hands out a url
// at all) and get that url plus the headers that make it readable. Measured on a SUPER_VIP
// session: the signed url answers 412 with a 262-byte Tengine page to any request that does
// not carry the account's cookies, and 206 with a correct Content-Range - at interior offsets
// too - as soon as they are attached. That is what LinkSwift's 增强下载 amounts to; its own
// words are "向服务器提供正确的请求头".
//
// The bytes are then pulled by our own engine. Measured on one 5.6 GB file: the client's
// downloader holds ~100 KB/s per task (4 tasks = 399 KB/s), while range-reading with the
// cookie gives 1.1 MB/s on one connection and a stable 2.78 MB/s at four or eight. So Netdisk
// stops being a downloader and hands a readable url to Downloads, which is what buys the
// progress bar, the resume and watch-while-downloading for free. The copy that made the url
// exists only for the length of that job and is taken back afterwards, byte-guarded, so a
// stale fid can never point at one of his own files.
class Netdisk final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The share a ladder is working on, so the board can say "正在转存" on the row he clicked
    // instead of lighting a control somewhere else.
    Q_PROPERTY(QString busyKey READ busyKey NOTIFY busyChanged)

public:
    explicit Netdisk(QObject *parent = nullptr);

    // Where the 夸克 client's executable is. The web api's own registry entry
    // (StartMenuInternet) carries no command on this machine, so the scheme handler is the
    // one that actually answers.
    static QString clientExecutable();

    bool busy() const { return m_stage != Idle; }
    QString busyKey() const { return m_stage == Idle ? QString() : m_job.share; }

    // One share link -> one handed-over download. `name` and `poster` are the board's own.
    // Where it lands is Downloads' business (its own folder, or the harness's override), so
    // there is no directory in this call.
    Q_INVOKABLE void download(const QString &shareUrl, const QString &name,
                              const QString &poster);
    // Is the client up and signed in, and does the account have a paid tier.
    Q_INVOKABLE void checkSession();
    // Brings the client to the 夸克 sign-in page. The login itself is his, not ours.
    Q_INVOKABLE void openLogin();
    // Drop a job that has not been handed over yet. Once it has, the row is Downloads' and
    // its own stop is what stops it.
    Q_INVOKABLE void cancel(const QString &key);

    // The copy in his drive is there only to make the url readable: take it back as soon as
    // the bytes are on our disk. Keyed by the handed-over url, because that is what the row
    // knows.
    Q_INVOKABLE void dropCopy(const QString &url);
    // Copies left behind by a job that died with the app. Anything whose url is not in
    // `liveUrls` is stale and goes.
    Q_INVOKABLE void reconcile(const QStringList &liveUrls);

signals:
    void busyChanged();
    void sessionReady(bool signedIn, bool vip, const QString &note);
    // A ladder has begun. `key` is the share url, stable for the whole job, so the board can
    // mark the row he clicked while there is nothing yet to mark in the download list.
    void queued(const QString &key, const QString &name, const QString &poster);
    // The url is readable and its headers are attached: the shell turns this into an ordinary
    // http job. `name` is what the poster calls the film - the drive's own name for it is
    // usually a quality label like `4k.mp4`, which is not a name a row should wear.
    void ready(const QString &key, const QString &url, const QString &headers,
               const QString &name, const QString &poster);
    void failed(const QString &key, const QString &note);

private:
    enum Stage { Idle, Connecting, Laddering };

    struct Job
    {
        QString share;
        QString name;
        QString poster;
        QString dir;
        QString pwdId;
        QString stoken;
        QString fid;        // the share's own file id
        QString fidToken;
        qint64 size = 0;
        QString taskId;     // the save is an async task on the account
        QString copyFid;    // our saved copy
    };

    // A copy we owe the account back, kept across the download and across restarts.
    struct Copy
    {
        QString fid;
        qint64 size = 0;
    };

    void runNext();
    void setStage(Stage s);

    // ---- the DevTools side ----
    void ensurePage(std::function<void(bool, const QString &)> done, bool launch = true);
    // The client's own DevTools port is not the one we would ask for: find it by asking.
    void findPort(std::function<void(int)> done, int index = 0);
    void pageOnPort(int port, std::function<void(bool, const QString &)> done, bool launch);
    void launchClient(std::function<void(bool, const QString &)> done);
    void attach(const QJsonObject &target);
    void send(const QString &method, const QJsonObject &params,
              const std::function<void(const QJsonObject &)> &then);
    void eval(const QString &expression, const std::function<void(bool, const QJsonValue &)> &then);
    // An in-page call to one of the drive's own json endpoints, with the session attached.
    void api(const QString &method, const QString &url, const QJsonValue &body,
             const std::function<void(bool, const QJsonObject &)> &then);
    void clientVersion();
    // The account's cookies for the host serving the file, as "Name: value" lines. Read out of
    // the cookie jar and not off whichever request the network domain reported first: a miss
    // there yields an empty Cookie, and every range then 412s in a way that looks like the cdn
    // refusing us.
    void grabHeaders(const QString &url, std::function<void(const QString &)> done);

    // ---- the ladder ----
    void stepToken();
    void stepDetail();
    void stepSave();
    void stepTask(int round);
    void stepDownload(int round);
    void stepHandoff(const QString &url, const QString &serverName);
    void deleteCopy(const QString &fid, qint64 size, std::function<void()> after = {});

    void fail(const QString &note);
    // Which step a timeout tripped on: "夸克没有回应" alone cannot tell a dead client from
    // a call the page is sitting on.
    QString stageText() const;

    QString copiesPath() const;
    void loadCopies();
    void saveCopies() const;

    QNetworkAccessManager *m_nam = nullptr;
    QWebSocket *m_ws = nullptr;
    QTimer *m_deadline = nullptr;
    int m_rpcId = 0;
    QHash<int, std::function<void(const QJsonObject &)>> m_rpc;
    Stage m_stage = Idle;
    int m_port = 0;
    QList<Job> m_queue;
    Job m_job;
    QHash<QString, Copy> m_copies;   // handed-over url -> the copy that made it readable
    QString m_ve = QStringLiteral("7.4.2.1020");
    QString m_lastAnswer;   // the drive's own answer to the last call that refused us
    bool m_askLogin = false;   // the last failure was "no session": bring the login page
};
