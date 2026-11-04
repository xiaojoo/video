#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QSet>
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

    bool busy() const { return m_stage != Idle; }
    QString busyKey() const { return m_stage == Idle ? QString() : m_job.share; }

    // One share link -> one handed-over download. `name` and `poster` are the board's own.
    // Where it lands is Downloads' business (its own folder, or the harness's override), so
    // there is no directory in this call.
    // Which drive a pasted address belongs to, or "" when it is not a share page of one
    // this app can walk. 新建下载 asks this before it decides whether a line goes to the
    // ladder or straight to the byte engine - an http(s) share page pulled as a direct
    // link would save the HTML of a login wall and call it a film.
    Q_INVOKABLE QString providerOf(const QString &url) const;
    Q_INVOKABLE void download(const QString &shareUrl, const QString &name,
                              const QString &poster);
    Q_INVOKABLE double durationFor(const QString &shareUrl) const;
    // Drop a job that has not been handed over yet. Once it has, the row is Downloads' and
    // its own stop is what stops it.
    Q_INVOKABLE void cancel(const QString &key);

    // The copy in his drive is there only to make the url readable: take it back as soon as
    // the bytes are on our disk. Keyed by the handed-over url, because that is what the row
    // knows.
    Q_INVOKABLE void dropCopy(const QString &url);

    // ---- the dead-share ledger ----
    // A share the drive says does not exist is not coming back, and finding that out by
    // clicking costs a whole ladder. Every verdict - ours or the ladder's - is recorded, and
    // the wall stops listing the row. Keys are the site's own row id (`pan_id_772674`) and
    // the drive's share id, so a verdict learned by either path hides the row the other finds.
    static QString shareKeyOf(const QString &url);
    static bool isDead(const QString &url);
    // Not static: every drive's "gone" answer lands here, and the board is told about it on
    // the way out. The wall's filter (isDead) stays static because it runs while the page is
    // being parsed, before anything has an instance to ask.
    void noteDead(const QString &url, int code);
    // For whoever holds the row's hop rather than the share id: the wall filters on the
    // hop, so a verdict learned from a click has to be written under that key too.
    Q_INVOKABLE void markDead(const QString &url, int code);
    // The other verdict. A row that answered reachable is written down so the same row is
    // never asked twice: without this, opening a 331-row tab means 331 hops to the site every
    // single time, and the site's pacing is what a background check is not supposed to spend.
    Q_INVOKABLE void markAlive(const QString &hopUrl, const QString &shareUrl);
    Q_INVOKABLE bool knownAlive(const QString &url);
    // One call per drive that asks "does this share still exist" without downloading a byte.
    Q_INVOKABLE void check(const QString &shareUrl);
    // Copies left behind by a job that died with the app. Anything whose url is not in
    // `liveUrls` is stale and goes.
    Q_INVOKABLE void reconcile(const QStringList &liveUrls);

signals:
    void busyChanged();
    // A ladder has begun. `key` is the share url, stable for the whole job, so the board can
    // mark the row he clicked while there is nothing yet to mark in the download list.
    void queued(const QString &key, const QString &name, const QString &poster);
    // The url is readable and its headers are attached: the shell turns this into an ordinary
    // http job. `name` is what the poster calls the film - the drive's own name for it is
    // usually a quality label like `4k.mp4`, which is not a name a row should wear.
    //
    // 百度 answers with this too, but its `url` is a local `.m3u8`: `/share/streaming` gives
    // #EXTM3U of raw MPEG-TS segments that answer 200 with NO cookie attached, so the engine
    // reads the table off disk and stitches the segments into one `.ts`, while the row's play
    // mark opens the same table straight in mpv and never waits on the pull.
    void ready(const QString &key, const QString &url, const QString &headers,
               const QString &name, const QString &poster);
    void failed(const QString &key, const QString &note);
    // The verdict of check(). `dead` is only ever true on the drive's own words; -1 means
    // this drive has no measured answer and the row must stay where it is.
    void checked(const QString &shareUrl, bool dead, int code);
    // A share has just been written into the dead ledger, from any door: the checker's or a
    // ladder he started with a click. The board maps the share back to its row and drops it.
    void dead(const QString &shareUrl, int code);

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
        // 百度 needs none of the above: its chain is five plain https calls. Kept in the same
        // Job because the queue and `busyKey` are shared with the 夸克 ladder.
        QString provider;   // quark | baidu | xunlei | uc, so a timeout can name the right one
        QString surl;       // the /s/<id> part, leading `1` included
        QString pwd;
        QString shareid;
        QString uk;         // the sharer's uk, from the page's own locals
        QString token32;    // the bdstoken in the html - the 24-char csrfToken cookie is refused
        QString jsToken;
        QString sign;
        QString timestamp;
        QString fname;
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
    // The 夸克 ladder runs inside a tab of the browser we own (its own port, its own
    // profile), which is what took the 夸克 client out of this chain. `launch` tells a job he
    // clicked apart from a favour: handing a copy back must not start a process.
    void drivePage(const QString &domain, const QString &startUrl, const QString &label,
                   std::function<void(bool, const QString &)> done, bool launch = true,
                   int tries = 0);
    // The driven browser's own UA, so the cdn sees the UA of the tab that holds the jar.
    void fetchUa();
    void attach(const QJsonObject &target);
    void send(const QString &method, const QJsonObject &params,
              const std::function<void(const QJsonObject &)> &then);
    void eval(const QString &expression, const std::function<void(bool, const QJsonValue &)> &then);
    // An in-page call to one of the drive's own json endpoints, with the session attached.
    void api(const QString &method, const QString &url, const QJsonValue &body,
             const std::function<void(bool, const QJsonObject &)> &then);
    // The account's cookies for the host serving the file, as "Name: value" lines. Read out of
    // the cookie jar and not off whichever request the network domain reported first: a miss
    // there yields an empty Cookie, and every range then 412s in a way that looks like the cdn
    // refusing us.
    void grabHeaders(const QString &url, std::function<void(const QString &)> done);

    // ---- the ladder ----
    static QJsonObject xlOrQuarkBody(bool uc, const QString &id, const QString &shareUrl);
    void ucLadder();
    void ucWalk();
    void ucHandOff(const QString &url);
    void stepToken();
    void stepDetail();
    void stepSave();
    void stepTask(int round);
    void stepDownload(int round);
    void stepHandoff(const QString &url, const QString &serverName);
    void deleteCopy(const QString &fid, qint64 size, std::function<void()> after = {});

    // ---- 百度's ladder: five plain calls, no browser ----
    // html (jsToken + locals.mset) -> risklabel -> share/list -> share/tplconfig (sign,
    // timestamp) -> share/streaming. The spellings are the page's own; the two that cost me a
    // round each: `dir=/sharelink<uk>-<parent_fs_id>/<name>`, and `fields=sign,timestamp` as a
    // comma list - the JSON-array spelling answers errno=0 with neither key.
    void bdNext(std::function<void()> go);
    // Our own browser's baidu tab answers the calls that get errno=-9 from Qt's socket.
    void bdPage(std::function<void(bool, const QString &)> done, int tries = 0);
    void bdPageJson(const QString &tag, const QString &url,
                    std::function<void(const QJsonObject &)> then);
    void bdPageStream(const QString &url, std::function<void(bool, int, double)> then);
    void bdPageText(const QString &url,
                    std::function<void(bool, const QString &, int, double)> then);
    QString bdPlaylistPath() const;
    void bdTry(const QString &tag, const QString &url,
               std::function<void(const QJsonObject &)> then);
    void bdJar();
    void bdEnter(std::function<void()> go);
    void bdTabJar();
    void bdHtml();
    bool bdReadJsToken(const QString &html);
    void bdRisk();
    void bdList(const QString &dir, const QString &parentFid);
    void bdSign();
    void bdStream();
    // `post` is an already-encoded body, empty for the GETs this chain is made of.
    void bdForm(const QString &tag, const QString &url, const QString &post,
                std::function<void(const QJsonObject &, const QByteArray &)> then);
    QString bdCookie() const;
    // The live jar, not the file: BDCLND - the pass-code ticket that is also `sekey` - only
    // arrives on a Set-Cookie from risklabel, and rereading the file for every call would
    // make share/list answer errno=-9 forever.
    QHash<QString, QString> m_bdJar;

    // ---- 迅雷's ladder: the tree is ours, the address is the page's ----
    // Measured 2026-11-03 on one encrypted share: a single copied `x-captcha-token` answers 200
    // twice running on `/drive/v1/share` AND on `/drive/v1/share/detail?parent_id=`, so the
    // whole tree is walkable from here. Two calls are a different captcha scenario and answer
    // 400 `captcha_invalid` no matter what we send: submitting the 提取码, and
    // `/drive/v1/share/file_info`, the one that hands out the address. Those go through the tab
    // and come back as its own response body. The address itself then needs no header at all -
    // a request with nothing but `Range: bytes=0-31` answers 206 with a correct Content-Range -
    // which is what lets this end in the same `ready` handoff 百度 uses.
    void xlJar();
    // The recorded signature, without the job's failure path: a check needs the same headers
    // and must not end a download when they are missing.
    void xlLoadHeaders();
    void xlShare();
    // The share's root listing, however it was unlocked - our own call or the page's answer.
    void xlGotRoot(const QJsonObject &o);
    // Network events, which `attach` used to drop on the floor: 迅雷's address only exists
    // inside a response the page itself got, and that needs the request ids to remember.
    void onEvent(const QJsonObject &o);
    // One directory of the share. `depth` caps the walk: a share is a film, not a filesystem.
    void xlList(const QString &parent, int depth);
    void xlStep();
    void xlPick();
    // The chain of folder names down to the file the walk chose, sharer's folder first.
    QStringList xlChain(const QJsonObject &file) const;
    // The tab on the share page: attach, and watch its traffic so a response body can be read
    // back. Navigation is a separate step, because every phase of this ladder starts over at
    // the share root.
    void xlPage(std::function<void(bool, const QString &)> done, int tries = 0);
    void xlNav(const QString &url, std::function<void()> done);
    // Waits for the page's own load event and calls done once, with a cap. The ladder used to
    // sleep a flat 9 s after every navigation; measured against his profile, the event arrives
    // 0.4 s after Page.navigate, so eight and a half seconds of every hop were pure waiting.
    void xlWaitLoad(std::function<void()> done, int capMs);
    void xlFireLoad() { if (m_xlLoadDone) { const auto f = std::move(m_xlLoadDone); m_xlLoadDone = nullptr; f(); } }
    void xlPress(int x, int y);
    void xlListen();
    // Present a 提取码 the way he would: ride it in the url first (the page submits what it is
    // given), and if the list still refuses, focus the box with a real click, insert the text
    // through the browser, press 确定.
    void xlTypeCode(const QString &code, std::function<void(bool, const QJsonObject &)> then,
                    bool typedOnce = false);
    // Double-click one row's name in the file list - a single click only selects, and the
    // route's own `?path=` is client state that a cold navigate drops.
    void xlOpen(const QString &name, std::function<void(bool)> done);
    // Asks the document, every 250 ms up to a cap, whether a row with this name is there yet.
    // A folder's listing is rendered client-side, so there is no load event to wait on - but
    // the row is in the document as soon as it is, and the flat 6 s sleep was paying for the
    // slowest folder measured instead of the one in front of us.
    void xlWaitRow(const QString &name, int capMs, std::function<void()> then, int waited = 0);
    // The last 200 answer the page itself got for `pathPart` of its api, read off the wire.
    void xlBody(const QString &pathPart, std::function<void(const QJsonObject &)> then,
                int polls = 0);
    void xlFile(int i, const QStringList &chain);
    void xlHandOff(const QJsonObject &info);
    // One https call with the recorded signed headers. `tag` is for the log only.
    void xlForm(const QString &tag, const QString &url,
                std::function<void(const QJsonObject &)> then);
    QJsonObject m_xlHdr;      // authorization, x-captcha-token, x-client-id, x-device-id, UA…
    QJsonArray m_xlTree;      // every entry the walk read, files and folders together
    QStringList m_xlParents;  // "id@depth" still to list
    QSet<QString> m_xlSeenParent;
    QJsonObject m_xlBest;
    QString m_xlToken;        // the pass_code_token, once the share is unlocked
    QString m_xlEntered;      // the share url the tab is already sitting on
    std::function<void()> m_xlLoadDone;   // the one hop currently waiting on loadEventFired
    QString m_xlTabId;        // the DevTools tab the socket is on
    bool m_xlPageReady = false;
    bool m_xlListening = false;   // Network.enable has been sent on this socket
    QHash<QString, QString> m_xlUrlByRid;
    QList<QPair<QString, QString>> m_xlDone;   // rid -> url, for calls that answered
    QSet<QString> m_xlRead;                    // rids whose body we already took
    // Wall time of the ladder in flight. The 5 s page visits are the whole cost of this chain,
    // so "it works" is not enough - the number has to be on screen to be argued about.
    QElapsedTimer m_bdClock;
    QString m_bdEntered;   // the share url the tab is already sitting on
    QString m_bdCsrf;   // the csrfToken cookie: what /share/* accepts as bdstoken. The 32-char
                        // bdstoken in the html is what /api/filemanager wants, and it is refused
                        // by the share family (errno=-9 there, errno=-6 the other way round).
    int m_bdHops = 0;
    int m_bdTries = 0;
    bool m_bdPageReady = false;
    QString m_bdTabId;   // the DevTools tab the socket is on, so a closed tab is noticed
    bool m_bdPageFirst = false;
    int m_bdDelay = 150;   // ms between the share calls; 0 paced the -9 away (see bdNext)

    void fail(const QString &note);
    // Quit the browser we started once the address is handed over and nothing is queued:
    // a browser that stays up is the browser the next launch opens its tab into, and that
    // is the window he sees. Only the hand-off calls this - a failed job may still owe a
    // 转存 copy back, and that is a page call.
    void closeBrowser(const QString &why);
    // Which step a timeout tripped on: "夸克没有回应" alone cannot tell a dead tab from
    // a call the page is sitting on.
    QString stageText() const;

    QString copiesPath() const;
    void loadCopies();
    void saveCopies() const;

    QNetworkAccessManager *m_nam = nullptr;
    // What the drive quoted for the length of the file behind a share, in seconds, kept
    // against the share url so the caller of ready() can ask for the one it just got.
    QHash<QString, double> m_durations;
    QWebSocket *m_ws = nullptr;
    QTimer *m_deadline = nullptr;
    int m_rpcId = 0;
    QHash<int, std::function<void(const QJsonObject &)>> m_rpc;
    Stage m_stage = Idle;
    QList<Job> m_queue;
    Job m_job;
    QHash<QString, Copy> m_copies;   // handed-over url -> the copy that made it readable
    QString m_ua;        // the UA of the browser we drive, read off /json/version
    QString m_pageTabId;   // the DevTools tab our socket is on
    QString m_pageDomain;  // which drive that tab belongs to
    bool m_pageReady = false;
    QString m_lastAnswer;   // the drive's own answer to the last call that refused us
};
