#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QProcess;
class QTimer;
class QWebSocket;

// The 网盘 session recorder, for the drives whose own api refuses a request that did not
// come from their page. Measured on 迅雷: bare, the share endpoint answers 400 "device_id is
// empty"; with the device id it answers 400 "captcha_token is empty". Copying the four headers
// the page puts on the wire makes the identical call answer 200 from outside the browser
// (measured 2026-11-02 22:52: three back-to-back share calls on one token, all 200) - so what
// has to be taken from the browser is the header set and the signed-in jar, not the request.
//
// This starts a Chrome of our own in its own profile directory - never the one he browses
// with - opens the drive's login page, and waits for either a new name in the cookie jar or
// one signed api call that came back 200. What it writes is a local file; the values are his
// session, so they are never logged, never put in argv, and never committed.
class PanLogin : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString note READ note NOTIFY stateChanged)
    // A property rather than a call: the settings card's line has to redraw itself when the
    // wait starts and when it ends, and a function call in a binding does not re-run.
    Q_PROPERTY(QString xunleiState READ xunleiState NOTIFY stateChanged)
    Q_PROPERTY(QString baiduState READ baiduState NOTIFY stateChanged)
    Q_PROPERTY(QString quarkState READ quarkState NOTIFY stateChanged)
    // Re-record a lapsed session by itself, without a window in front of him. Measured: 迅雷's
    // recorded headers answered 200 for hours and then every call - the checker's and the
    // ladder's alike - came back 401 at an age of 54,576 s, because its bearer runs 12.0 h.
    Q_PROPERTY(bool autoRelogin READ autoRelogin WRITE setAutoRelogin NOTIFY autoReloginChanged)

public:
    explicit PanLogin(QObject *parent = nullptr);

    // Where the browser lives. The registry entry first: this box has Chrome under
    // Program Files and Edge under Program Files (x86), and guessing the wrong one of the
    // two costs a silent failure the user cannot see.
    Q_INVOKABLE static QString browserExecutable();

    // One place defines the browser we drive: the login recorder starts it and Netdisk
    // attaches to it for the call a site will only answer from its own page.
    //
    // `visible` is the difference between those two. A login has to be a window he can read
    // and type into. A ladder only needs a page that behaves like a browser, and measured on
    // a 百度 share, a window parked at -32000,-32000 behaves exactly like a visible one: the
    // same 74 cookies, errno=0 on the listing, 962 segments on the streaming call.
    static int debugPort();
    static QString profileRoot();
    static QStringList browserArgs(const QString &startUrl, bool visible = true);

    // Open the drive's login page and start waiting. `provider` is "xunlei" for now.
    Q_INVOKABLE void login(const QString &provider);
    // Stop waiting. The browser window is left alone: he may have typed a password into it.
    Q_INVOKABLE void cancel();

    Q_INVOKABLE bool hasSession(const QString &provider) const;
    // 退出登录: the recorded key material goes. What is NOT touched is the browser profile's
    // own cookies - those live in a window he may have open, and clearing them would log him
    // out of something he did not ask to be logged out of.
    Q_INVOKABLE void logout(const QString &provider);
    // The line the settings card shows.
    Q_INVOKABLE QString stateText(const QString &provider) const;
    // Where the flag lives: the same store the download row count uses, so it survives a
    // rebuild and does not need a second settings object to agree with.
    bool autoRelogin() const;
    void setAutoRelogin(bool on);
    // How old the recorded headers are, in hours, for the card's own line. -1 when the drive
    // never wrote a timestamp.
    Q_INVOKABLE double headersAgeH(const QString &provider) const;
    // The clock this drive has been measured to have, in hours; 0 = no measured clock, so
    // the auto path leaves it alone rather than guessing at a refresh.
    Q_INVOKABLE static double ageLimitH(const QString &provider);
    QString xunleiState() const { return stateText(QStringLiteral("xunlei")); }
    QString baiduState() const { return stateText(QStringLiteral("baidu")); }
    QString quarkState() const { return stateText(QStringLiteral("quark")); }
    // The recorded jar, for the ladder that has to replay a request. Empty when he has not
    // logged in yet. Values stay inside the process and the file.
    Q_INVOKABLE QByteArray session(const QString &provider) const;

    // A flag, not `m_poll != nullptr`: the timer is created once and kept, so testing for it
    // made every click look like it was still waiting forever - the card's line never came
    // back from 正在等你登录 and the button never went back to 去登录.
    bool busy() const { return m_waiting; }
    QString note() const { return m_note; }

signals:
    void busyChanged();
    void stateChanged();
    void autoReloginChanged();

private:
    QString profileDir() const;
    QString sessionPath(const QString &provider) const;
    // The watch that asks, every few minutes, whether a drive's recorded headers have run
    // past the clock it was measured with. Quiet runs never raise the browser: a window that
    // walks itself into the foreground is the thing he would notice first.
    void watchSessions();
    void loginQuiet(const QString &provider);
    void startLogin(const QString &provider, bool quiet);
    void setBusy(bool on);
    void startPoll();
    void stopPoll();
    // One step of the wait: list the tabs, attach to the drive's, read its jar.
    void stepList();
    void readCookies();
    // The page's own localStorage: where a single-page drive keeps its bearer token, which
    // is not necessarily in a cookie at all.
    void askStore();
    // The drive's own answer to "is this session signed in", asked from inside the page.
    // Used by the providers whose session cookie cannot be told from an anonymous one until
    // he has actually logged in.
    void askProbe();
    void decide();
    void save(const QStringList &marks);
    // The api answers 400 "captcha_token is empty" to a request that only carries the
    // cookies; the page adds four more headers. Those are not derivable from the jar, so
    // they are taken from one live request the page makes itself.
    void watchForApiHeaders(const QJsonObject &o);
    void captureHeaders();
    void finishCapture();
    void finish(const QString &note);
    // One CDP call over the tab's socket. Returns the id its answer comes back under.
    int send2(const QString &method, const QJsonObject &params = {});
    // Reload the tab once and let it make its own api calls. Needed because a page that is
    // already signed in is quiet: nothing crosses the wire until it is asked.
    void requestReload();

    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_poll = nullptr;
    QWebSocket *m_ws = nullptr;
    // The tab we read the jar from, and the id its answer comes back under.
    QString m_wsUrl;
    // The url the live socket is actually attached to. `m_wsUrl` is what this poll picked, so
    // the two differ the moment he opens the share in a new tab.
    QString m_attached;
    int m_cookieId = 0;
    int m_evalId = 0;
    // The probe call's id and its last answer. `m_probeIn` is only trusted when
    // m_probeDone is true; before that the poll has no opinion.
    int m_probeId = 0;
    bool m_probeDone = false;
    bool m_probeIn = false;
    int m_rpcId = 0;
    // What the last poll read, kept until both halves are in and `decide` can weigh them.
    QJsonArray m_jar;
    QJsonObject m_store;
    bool m_haveStore = false;
    // requestId -> url, and the headers of the first api call we saw them on.
    QMap<QString, QString> m_reqUrl;
    // requestId -> the headers the browser actually put on the wire, kept so a 200 answer can
    // be traced back to the signed request that earned it.
    QMap<QString, QJsonObject> m_reqHdr;
    QJsonObject m_apiHeaders;
    bool m_capturing = false;
    bool m_waiting = false;
    // One recording per click: while the signed headers are being captured the poll is
    // still running, and every pass would otherwise rewrite the session file.
    bool m_done = false;
    QString m_provider;
    QString m_note;
    // The names present when the page first settled. Anything new after that is the login.
    QSet<QString> m_before;
    bool m_baselined = false;
    // The jar's own cookie names, from the last pass. The signed-traffic path saves them,
    // because it fires between two polls and must not write a jar of nothing.
    QStringList m_jarNames;
    bool m_reloaded = false;
    // The tab has been raised once for this click: every reconnect of the poll would
    // otherwise push the window forward again.
    bool m_raised = false;
    // Set when a signed call of this page's came back 200, so the header set copied from it
    // is not thrown away to go hunting for a fresher one.
    bool m_savedFromTraffic = false;
    int m_ticks = 0;
    // This wait was started by the watch, not by a click: shorter leash, no window raised,
    // and a failure that says so on the row instead of a second attempt.
    bool m_quiet = false;
    QTimer *m_watch = nullptr;
    // Not before this epoch, so a drive whose profile really has logged him out is not asked
    // again every ten minutes.
    qint64 m_retryAfter = 0;
    QString m_autoFail;
};
