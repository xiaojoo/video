#include "netdisk.h"

#include "panlogin.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace {

// The UA of the browser we drive, for a cookie read that lands before /json/version answers.
const char *kWebUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/144.0.0.0 Safari/537.36";

// The drive's own json endpoints, called from inside its tab. `withCredentials` is what
// carries the signed-in session; the site's monitoring hook rejects our own http stack on
// these hosts, so the page is the only caller that is answered.
const char *kXhr =
    "(m,u,b)=>new Promise(res=>{const x=new XMLHttpRequest();x.open(m,u,true);x.withCredentials=true;"
    "x.setRequestHeader('Accept','application/json');if(b)x.setRequestHeader('Content-Type','application/json');"
    "x.onload=()=>{try{res(JSON.parse(x.responseText))}catch{res({__st:x.status,__body:(x.responseText||'').slice(0,60)})}};"
    "x.onerror=()=>res({__xerr:1,__st:x.status});x.timeout=30000;x.ontimeout=()=>res({__xto:1});"
    "x.send(b?JSON.stringify(b):null);})";

// A value as a javascript literal, so a title with a quote cannot break the script.
QString jsVal(const QJsonValue &v)
{
    QJsonArray a;
    a.append(v);
    const QString s = QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    return s.mid(1, s.size() - 2);
}

QString inPage(const QString &body)
{
    return QStringLiteral("(async()=>{const j=%1;%2})()").arg(QString::fromLatin1(kXhr), body);
}

// What the drive api answers when the tab carries no session. Everything else is a real
// answer to a real request; this one means "he has not signed in to this drive in the browser
// we drive", so the line names the settings row that can fix it.
bool sessionMissing(const QJsonObject &r)
{
    if (r.value(QStringLiteral("code")).toInt() == 31001)
        return true;
    return r.value(QStringLiteral("message")).toString().contains(QStringLiteral("require login"));
}

bool looksLikeVideo(const QJsonObject &f)
{
    if (f.value(QStringLiteral("cat")).toString() == QLatin1String("video"))
        return true;
    static const QSet<QString> exts{ QStringLiteral("MP4"), QStringLiteral("MKV"),
                                      QStringLiteral("MOV"), QStringLiteral("AVI"),
                                      QStringLiteral("TS"), QStringLiteral("M2TS"),
                                      QStringLiteral("WMV"), QStringLiteral("FLV"),
                                      QStringLiteral("RMVB"), QStringLiteral("ISO") };
    return exts.contains(f.value(QStringLiteral("ext")).toString());
}

// A share usually holds the same film in several encodes. The one to take by default is the
// best that is still sane to pull: 1080p films sit at 2-8 GB, a 4K remux at 15-45 GB, and
// clicking a row on the board means "watch this", not "give me the archive". If every
// version is over the line, the smallest video is the one that will actually arrive.
constexpr qint64 kPreferMaxBytes = 8LL * 1000 * 1000 * 1000;

QJsonObject pickFrom(const QJsonArray &files)
{
    QList<QJsonObject> vids;
    for (const QJsonValue &f : files) {
        const QJsonObject fo = f.toObject();
        if (looksLikeVideo(fo))
            vids.append(fo);
    }
    if (vids.isEmpty())
        return files.isEmpty() ? QJsonObject{} : files.first().toObject();
    std::sort(vids.begin(), vids.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("size")).toDouble()
             > b.value(QStringLiteral("size")).toDouble();
    });
    for (const QJsonObject &v : vids)
        if (qint64(v.value(QStringLiteral("size")).toDouble()) <= kPreferMaxBytes)
            return v;
    return vids.last();
}

} // namespace

Netdisk::Netdisk(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    // Every request here goes to this machine's loopback. The system proxy is a tunnel on
    // another loopback port and is asked about whether the bypass list mentions it or not,
    // so the answer to a DevTools call can depend on what else is running.
    m_nam->setProxy(QNetworkProxy::NoProxy);
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("%1没有回应，卡在%2").arg(m_job.provider.isEmpty()
            ? QStringLiteral("网盘") : (m_job.provider == QLatin1String("baidu")
                                       ? QStringLiteral("百度")
                                       : (m_job.provider == QLatin1String("xunlei")
                                          ? QStringLiteral("迅雷") : QStringLiteral("夸克"))),
            stageText()));
    });
    loadCopies();
}


void Netdisk::setStage(Stage s)
{
    if (m_stage == s)
        return;
    m_stage = s;
    emit busyChanged();
    if (m_stage == Idle)
        m_deadline->stop();
}

QString Netdisk::stageText() const
{
    const bool bd = m_job.provider == QLatin1String("baidu");
    const bool xl = m_job.provider == QLatin1String("xunlei");
    switch (m_stage) {
    case Connecting: return bd ? QStringLiteral("连百度")
                               : (xl ? QStringLiteral("连迅雷") : QStringLiteral("连夸克"));
    case Laddering:  return bd ? QStringLiteral("问百度")
                               : (xl ? QStringLiteral("问迅雷") : QStringLiteral("问网盘"));
    default:         return QStringLiteral("空闲");
    }
}

void Netdisk::download(const QString &shareUrl, const QString &name, const QString &poster)
{
    static const QRegularExpression pwd(QStringLiteral("/s/([A-Za-z0-9]+)"));
    // 迅雷's own ids carry `_` and `-` (measured: `VP584lDeaKM15jVK7CG_Nv9NA1`), so the pattern
    // above would stop at the underscore and the ladder would ask for a share that never was.
    static const QRegularExpression xlId(QStringLiteral("/s/([A-Za-z0-9_-]+)"));
    const bool isXl = shareUrl.contains(QStringLiteral("pan.xunlei.com"));
    const QRegularExpression &idRe = isXl ? xlId : pwd;
    const auto m = idRe.match(shareUrl);
    if (!m.hasMatch()) {
        emit failed(shareUrl, QStringLiteral("这不是网盘的分享地址"));
        return;
    }
    Job j;
    j.share = shareUrl;
    j.name = name;
    j.poster = poster;
    j.pwdId = m.captured(1);
    j.surl = m.captured(1);
    static const QRegularExpression code(QStringLiteral("[?&](?:pwd|pass_code)=([A-Za-z0-9]{4,8})"));
    const auto c = code.match(shareUrl);
    j.pwd = c.hasMatch() ? c.captured(1) : QString();
    j.provider = shareUrl.contains(QStringLiteral("pan.baidu.com")) ? QStringLiteral("baidu")
             : isXl ? QStringLiteral("xunlei")
                    : QStringLiteral("quark");
    // Which row was pressed is the first thing a wall of 65 shares asks, and the answer is the
    // surl - the pass code itself stays out of the log.
    qInfo("nd in %s surl=%s pwd=%d q=%d", qPrintable(j.provider), qPrintable(j.surl),
          int(j.pwd.size()), int(m_queue.size()));
    m_queue.append(j);
    emit queued(shareUrl, name, poster);
    runNext();
}

void Netdisk::runNext()
{
    if (m_stage != Idle || m_queue.isEmpty())
        return;
    m_job = m_queue.takeFirst();
    setStage(Connecting);
    m_deadline->start(90000);
    if (m_job.provider == QLatin1String("baidu")) {
        // No DevTools anywhere in this chain: measured, every step answers with only the
        // recorded cookie jar attached, and the playlist it produces needs no cookie at all.
        setStage(Laddering);
        m_deadline->start(60000);
        m_bdClock.start();
        m_bdEntered.clear();
        bdJar();
        return;
    }
    if (m_job.provider == QLatin1String("xunlei")) {
        setStage(Laddering);
        m_deadline->start(90000);
        m_bdClock.start();
        m_xlEntered.clear();
        m_xlToken.clear();
        m_xlTree = QJsonArray();
        m_xlParents.clear();
        m_xlSeenParent.clear();
        m_xlBest = QJsonObject();
        xlJar();
        return;
    }
    qkPage([this](bool ok, const QString &note) {
        if (!ok) {
            const QString key = m_job.share;
            setStage(Idle);
            emit failed(key, note);
            runNext();
            return;
        }
        setStage(Laddering);
        m_deadline->start(60000);
        stepToken();
    });
}

// ------------------------------------------------------------------ DevTools

void Netdisk::attach(const QJsonObject &target)
{
    // "the tab is already sitting on this share" is a fact about the tab, not about the job:
    // a tab that was just created or re-picked is on about:blank or the drive home, and
    // believing the old answer there is how the run got 「百度这页没给播放令牌」 after a closed tab.
    m_bdEntered.clear();
    // A fresh socket sees no events from before it opened, so the traffic 迅雷's address lives
    // in has to be re-subscribed to after every attach.
    m_xlListening = false;
    m_xlUrlByRid.clear();
    m_xlDone.clear();
    m_xlRead.clear();
    if (m_ws) {
        m_ws->deleteLater();
        m_ws = nullptr;
    }
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) {
        const QJsonObject o = QJsonDocument::fromJson(msg.toUtf8()).object();
        if (!o.contains(QStringLiteral("id"))) {
            // Events used to be dropped here - "nothing subscribes to any" was true until 迅雷,
            // whose playable address exists only inside a response the page itself received.
            if (m_xlListening)
                onEvent(o);
            return;
        }
        const int id = o.value(QStringLiteral("id")).toInt();
        const auto it = m_rpc.find(id);
        if (it == m_rpc.end())
            return;
        const std::function<void(const QJsonObject &)> fn = it.value();
        m_rpc.erase(it);
        // Any answer at all is the tab being alive, so the watchdog is about silence
        // rather than about how long the whole ladder is allowed to take. Without this the
        // 60 s spent waiting for a big copy to materialise is read as a dead page:
        // measured, a job died on exactly that line while 网盘肯给下载地址 was still retrying.
        if (m_stage != Idle)
            m_deadline->start(60000);
        fn(o);
    });
    // QWebSocket takes the application proxy unless told otherwise, and this socket is a
    // loopback connection to the browser on this machine.
    m_ws->setProxy(QNetworkProxy::NoProxy);
    connect(m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
        qInfo("nd ws error=%d %s", int(e), qPrintable(m_ws ? m_ws->errorString() : QString()));
    });
    connect(m_ws, &QWebSocket::connected, this, [this] { qInfo("nd ws connected"); });
    m_ws->open(QUrl(target.value(QStringLiteral("webSocketDebuggerUrl")).toString()));
}






void Netdisk::send(const QString &method, const QJsonObject &params,
                   const std::function<void(const QJsonObject &)> &then)
{
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        qInfo("nd send %s with no socket (state=%d)", qPrintable(method),
              m_ws ? int(m_ws->state()) : -1);
        then(QJsonObject{});
        return;
    }
    const int id = ++m_rpcId;
    m_rpc.insert(id, then);
    QJsonObject req{ { QStringLiteral("id"), id }, { QStringLiteral("method"), method } };
    if (!params.isEmpty())
        req.insert(QStringLiteral("params"), params);
    m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)));
}

void Netdisk::eval(const QString &expression,
                   const std::function<void(bool, const QJsonValue &)> &then)
{
    send(QStringLiteral("Runtime.evaluate"),
         { { QStringLiteral("expression"), expression },
           { QStringLiteral("awaitPromise"), true },
           { QStringLiteral("returnByValue"), true } },
         [then](const QJsonObject &o) {
             if (!o.contains(QStringLiteral("result"))) {
                 // A CDP answer to Runtime.evaluate always carries `result`. Its absence means
                 // the command never left the process: send() answers an empty object when the
                 // socket is gone. That read as success here, and a closed 百度 tab came out on
                 // screen as 「百度回了 errno=-1」 - a number baidu never sent.
                 then(false, QJsonValue(QStringLiteral("没有可用的标签页")));
                 return;
             }
             const QJsonObject r = o.value(QStringLiteral("result")).toObject();
             const QJsonObject ex = r.value(QStringLiteral("exceptionDetails")).toObject();
             if (!ex.isEmpty()) {
                 // `text` on its own is the word "Uncaught" for anything the page threw;
                 // the sentence that says what broke is on exception.description.
                 QString d = ex.value(QStringLiteral("exception")).toObject()
                               .value(QStringLiteral("description")).toString();
                 if (d.isEmpty())
                     d = ex.value(QStringLiteral("text")).toString();
                 then(false, QJsonValue(d.isEmpty() ? QStringLiteral("EXCEPTION") : d));
                 return;
             }
             then(true, r.value(QStringLiteral("result")).toObject()
                            .value(QStringLiteral("value")));
         });
}

// ------------------------------------------------------------------ our browser
//
// 夸克's ladder used to borrow his client: only its drive window made `file/download` hand
// over a readable url for a saved copy (measured 2026-11-01), so 夸克 was a feature that
// needed a second program running. Tonight it runs inside a tab of the browser we already
// start for 迅雷 and 百度, which takes the client out of this chain - and his own browser
// windows, downloads and quota accounting stay out of this app's way.
//
// `launch` is the difference between a job and a favour: a download he clicked may bring the
// browser up, but returning a copy he is owed after the fact may not start a process.
void Netdisk::qkPage(std::function<void(bool, const QString &)> done, bool launch, int tries)
{
    fetchUa();
    const int port = PanLogin::debugPort();
    QNetworkReply *rep = m_nam->get(QNetworkRequest(QUrl(
        QStringLiteral("http://127.0.0.1:%1/json/list").arg(port))));
    connect(rep, &QNetworkReply::finished, this, [this, rep, port, done, launch, tries] {
        const QByteArray body = rep->readAll();
        const bool up = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        if (!up) {
            if (!launch) {
                done(false, QStringLiteral("我们的浏览器没开着，夸克那一页起不来"));
                return;
            }
            if (tries == 0) {
                const QString exe = PanLogin::browserExecutable();
                if (exe.isEmpty()) {
                    done(false, QStringLiteral("这台机器上没有 Chrome 或 Edge，夸克那一页起不来"));
                    return;
                }
                // Offscreen: the ladder needs a page that behaves like a browser, not a
                // window he is asked to watch. 设置-网盘登录 is the one place it opens
                // visibly, and that is PanLogin::login's own call.
                QProcess::startDetached(exe, PanLogin::browserArgs(
                    QStringLiteral("https://pan.quark.cn/"), false));
                qInfo("nd qk browser launched");
            }
            if (tries > 9) {
                done(false, QStringLiteral("等不到夸克的标签页起来"));
                return;
            }
            QTimer::singleShot(1500, this, [this, done, launch, tries] {
                qkPage(done, launch, tries + 1);
            });
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(body).array();
        QJsonObject pick;
        for (const QJsonValue &v : arr) {
            const QJsonObject t = v.toObject();
            // `quark.cn` rather than `pan.quark.cn`: the drive redirects its own page to
            // /list#/list/all, and a tab left sitting on the sign-in card still carries the
            // session cookies the ladder needs.
            if (t.value(QStringLiteral("type")).toString() == QLatin1String("page")
                && t.value(QStringLiteral("url")).toString().contains(QStringLiteral("quark.cn"))) {
                pick = t;
                break;
            }
        }
        if (pick.isEmpty()) {
            m_qkReady = false;
            // /json/new ignores its url argument on this Chrome, so the tab comes up blank and
            // gets navigated here. A blank tab is also how a fake "not signed in" is made:
            // from a null origin the SameSite=Lax session cookie is never sent.
            QNetworkReply *n = m_nam->put(
                QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/new").arg(port))),
                QByteArray());
            connect(n, &QNetworkReply::finished, this, [this, n, done] {
                const QJsonObject t = QJsonDocument::fromJson(n->readAll()).object();
                const QString ws = t.value(QStringLiteral("webSocketDebuggerUrl")).toString();
                m_qkTabId = t.value(QStringLiteral("id")).toString();
                n->deleteLater();
                if (ws.isEmpty()) {
                    done(false, QStringLiteral("开不出夸克的标签页"));
                    return;
                }
                attach({ { QStringLiteral("webSocketDebuggerUrl"), ws } });
                // attach() only opens the socket; a command sent before it is up is dropped.
                QTimer::singleShot(700, this, [this, done] {
                    send(QStringLiteral("Page.enable"), QJsonObject{},
                         [this, done](const QJsonObject &) {
                        send(QStringLiteral("Page.navigate"),
                             { { QStringLiteral("url"),
                                 QStringLiteral("https://pan.quark.cn/clouddrive/list") } },
                             [this, done](const QJsonObject &) {
                                QTimer::singleShot(6000, this, [this, done] {
                                    // Which page the ladder is about to run inside: a
                                    // navigation that silently bounced to a sign-in card
                                    // reads from here as the drive refusing the share.
                                    eval(QStringLiteral("location.href"),
                                         [this, done](bool, const QJsonValue &v) {
                                        qInfo("nd qk page=%s", qPrintable(v.toString()
                                            .toUtf8().toPercentEncoding().left(90).constData()));
                                        m_qkReady = true;
                                        done(true, QString());
                                    });
                                });
                             });
                        });
                });
            });
            return;
        }
        const QString id = pick.value(QStringLiteral("id")).toString();
        if (m_qkReady && id == m_qkTabId && m_ws
            && m_ws->state() == QAbstractSocket::ConnectedState) {
            done(true, QString());
            return;
        }
        // A flag that claims "we hold a tab" without re-checking that the tab is still in the
        // browser's list and the socket still connected sits until the deadline and blames the
        // drive for a silence the browser never caused.
        m_qkReady = false;
        m_qkTabId = id;
        attach(pick);
        QTimer::singleShot(700, this, [this, done] {
            send(QStringLiteral("Page.enable"), QJsonObject{},
                 [this, done](const QJsonObject &) {
                    m_qkReady = true;
                    done(true, QString());
                 });
        });
    });
}

void Netdisk::fetchUa()
{
    // The UA of the browser whose cookies are about to be handed to the cdn, so the read
    // carries the UA of the tab that collected the jar.
    if (!m_ua.isEmpty())
        return;
    QNetworkReply *rep = m_nam->get(QNetworkRequest(QUrl(QStringLiteral(
        "http://127.0.0.1:%1/json/version").arg(PanLogin::debugPort()))));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        rep->deleteLater();
        m_ua = o.value(QStringLiteral("User-Agent")).toString();
        qInfo("nd qk ua=%d chars", m_ua.size());
    });
}

void Netdisk::api(const QString &method, const QString &url, const QJsonValue &body,
                  const std::function<void(bool, const QJsonObject &)> &then)
{
    const QString expr = inPage(QStringLiteral("return await j(%1,%2%3);")
                                    .arg(jsVal(QJsonValue(method)), jsVal(QJsonValue(url)),
                                         body.isUndefined() ? QString()
                                                            : QStringLiteral(",") + jsVal(body)));
    eval(expr, [then](bool ok, const QJsonValue &v) {
        then(ok, ok ? v.toObject()
                    : QJsonObject{ { QStringLiteral("message"), v.toString() } });
    });
}

// ------------------------------------------------------------------ the ladder

void Netdisk::stepToken()
{
    api(QStringLiteral("POST"),
        QStringLiteral("https://pan.quark.cn/1/clouddrive/share/sharepage/token?pr=ucpro&fr=pc"),
        QJsonValue{ QJsonObject{ { QStringLiteral("pwd_id"), m_job.pwdId },
                                 { QStringLiteral("passcode"), QString() } } },
        [this](bool ok, const QJsonObject &r) {
            if (!ok) {
                fail(QStringLiteral("问不到这个分享（%1）")
                         .arg(r.value(QStringLiteral("message")).toString().left(60)));
                return;
            }
            qInfo("nd token code=%d", r.value(QStringLiteral("code")).toInt());
            if (sessionMissing(r)) {
                fail(QStringLiteral("夸克还没登录：设置-网盘登录里点「去登录」"));
                return;
            }
            m_job.stoken = r.value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("stoken")).toString();
            if (m_job.stoken.isEmpty()) {
                const int code = r.value(QStringLiteral("code")).toInt();
                fail(code == 41011 || code == 41012 ? QStringLiteral("这个分享已经失效了")
                                                    : QStringLiteral("分享打不开（%1）").arg(code));
                return;
            }
            stepDetail();
        });
}

void Netdisk::stepDetail()
{
    // The folder walk happens in one page call: a film's share is usually one or two levels
    // deep and a round trip per directory would show up as a stall on the board.
    const QString expr = inPage(QStringLiteral(
        "const P=%1,S=%2;"
        "const files=[];let dirs=['0'];"
        "for(let d=0;d<5&&dirs.length;d++){const nx=[];"
        "for(const dir of dirs.splice(0,dirs.length)){"
        "const r=await j('GET','https://pan.quark.cn/1/clouddrive/share/sharepage/detail"
        "?pr=ucpro&fr=pc&pwd_id='+P+'&stoken='+encodeURIComponent(S)+'&pdir_fid='+dir"
        "+'&force=0&_page=1&_size=50');"
        "for(const it of ((r.data&&r.data.list)||[])){"
        "if(it.dir)nx.push(it.fid);else files.push({fid:it.fid,tk:it.share_fid_token||'',"
        "size:it.size||0,cat:it.obj_category||'',ext:(it.file_ext||'').toUpperCase(),"
        "name:it.file_name||''});}}"
        "dirs=nx;}"
        "files.sort((a,b)=>b.size-a.size);"
        "return {n:files.length, files:files.slice(0,60)};")
                                    .arg(jsVal(QJsonValue(m_job.pwdId)),
                                         jsVal(QJsonValue(m_job.stoken))));
    eval(expr, [this](bool ok, const QJsonValue &v) {
        const QJsonArray files = ok ? v.toObject().value(QStringLiteral("files")).toArray()
                                    : QJsonArray{};
        if (files.isEmpty()) {
            // Two different things look like this from here: an empty folder, and a page
            // call that never answered. Only one of them is the site's fault.
            fail(ok ? QStringLiteral("这个分享里没有文件")
                    : QStringLiteral("读分享目录没成功：%1").arg(v.toString().left(80)));
            return;
        }
        const QJsonObject pick = pickFrom(files);
        if (pick.isEmpty()) {
            fail(QStringLiteral("这个分享里没有可用的文件"));
            return;
        }
        m_job.fid = pick.value(QStringLiteral("fid")).toString();
        m_job.fidToken = pick.value(QStringLiteral("tk")).toString();
        m_job.size = qint64(pick.value(QStringLiteral("size")).toDouble());
        qInfo("nd detail n=%d pick=%lld bytes", files.size(), m_job.size);
        if (m_job.name.isEmpty())
            m_job.name = pick.value(QStringLiteral("name")).toString();
        stepSave();
    });
}

void Netdisk::stepSave()
{
    api(QStringLiteral("POST"),
        QStringLiteral("https://pan.quark.cn/1/clouddrive/share/sharepage/save?pr=ucpro&fr=pc"),
        QJsonValue{ QJsonObject{ { QStringLiteral("pwd_id"), m_job.pwdId },
                                 { QStringLiteral("stoken"), m_job.stoken },
                                 { QStringLiteral("fid_list"), QJsonArray{ m_job.fid } },
                                 { QStringLiteral("fid_token_list"),
                                   QJsonArray{ m_job.fidToken } },
                                 { QStringLiteral("to_pdir_fid"), QStringLiteral("0") } } },
        [this](bool ok, const QJsonObject &r) {
            if (!ok || sessionMissing(r)) {
                fail(sessionMissing(r) ? QStringLiteral("夸克还没登录：设置-网盘登录里点「去登录」")
                                       : QStringLiteral("转存没做成"));
                return;
            }
            const QJsonObject d = r.value(QStringLiteral("data")).toObject();
            m_job.copyFid = d.value(QStringLiteral("first_fid")).toString();
            if (m_job.copyFid.isEmpty()) {
                const QJsonArray tops = d.value(QStringLiteral("save_as")).toObject()
                                          .value(QStringLiteral("save_as_top_fids")).toArray();
                if (!tops.isEmpty())
                    m_job.copyFid = tops.first().toString();
            }
            m_job.taskId = d.value(QStringLiteral("task_id")).toString();
            qInfo("nd save code=%d copy=%d task=%d", r.value(QStringLiteral("code")).toInt(),
                  int(!m_job.copyFid.isEmpty()), int(!m_job.taskId.isEmpty()));
            if (!m_job.copyFid.isEmpty())
                stepDownload(0);
            else if (!m_job.taskId.isEmpty())
                stepTask(0);
            else
                fail(QStringLiteral("转存没有返回副本（网盘可能满了，code=%1）")
                         .arg(r.value(QStringLiteral("code")).toInt()));
        });
}

void Netdisk::stepTask(int round)
{
    if (round > 30) {
        fail(QStringLiteral("转存没完成（网盘可能满了）"));
        return;
    }
    api(QStringLiteral("GET"),
        QStringLiteral("https://pan.quark.cn/1/clouddrive/task?pr=ucpro&fr=pc&task_id=%1"
                       "&retry_index=%2").arg(m_job.taskId, QString::number(round)),
        QJsonValue{},
        [this, round](bool, const QJsonObject &r) {
            const QJsonObject d = r.value(QStringLiteral("data")).toObject();
            QString fid = d.value(QStringLiteral("first_fid")).toString();
            if (fid.isEmpty()) {
                const QJsonArray tops = d.value(QStringLiteral("save_as")).toObject()
                                          .value(QStringLiteral("save_as_top_fids")).toArray();
                if (!tops.isEmpty())
                    fid = tops.first().toString();
            }
            qInfo("nd task round=%d hasFid=%d", round, int(!fid.isEmpty()));
            if (!fid.isEmpty()) {
                m_job.copyFid = fid;
                stepDownload(0);
                return;
            }
            QTimer::singleShot(1500, this, [this, round] { stepTask(round + 1); });
        });
}

void Netdisk::stepDownload(int round)
{
    // The copy answers 23018 until the drive has materialised it, which for a big
    // file is tens of seconds; three minutes of asking is the window in which the
    // same call has been seen to go from refused to a live url.
    if (round > 90) {
        fail(QStringLiteral("网盘不肯给下载地址（%1）").arg(m_lastAnswer));
        return;
    }
    // `sys=win32&ve=<client version>` went with the client: a web tab is not asked to name a
    // program it does not run. What this shape answers is being measured, so no claim about
    // whether the two parameters mattered is made here.
    api(QStringLiteral("POST"),
        QStringLiteral("https://drive-pc.quark.cn/1/clouddrive/file/download?pr=ucpro&fr=pc"),
        QJsonValue{ QJsonObject{ { QStringLiteral("fids"), QJsonArray{ m_job.copyFid } } } },
        [this, round](bool ok, const QJsonObject &r) {
            // Kept for the failure line: the code is the only thing that says whether the
            // copy has not materialised yet, the account cannot download it, or the call
            // itself was refused.
            m_lastAnswer = ok ? QStringLiteral("code=%1 %2")
                                 .arg(r.value(QStringLiteral("code")).toInt())
                                 .arg(r.value(QStringLiteral("message")).toString().left(40))
                              : QStringLiteral("页面没回答");
            qInfo("nd dl round=%d %s", round, qPrintable(ok
                ? QStringLiteral("code=%1").arg(r.value(QStringLiteral("code")).toInt())
                : QStringLiteral("no answer")));
            const QJsonArray data = r.value(QStringLiteral("data")).toArray();
            const QJsonObject o = data.isEmpty() ? QJsonObject{} : data.first().toObject();
            const QString url = o.value(QStringLiteral("download_url")).toString();
            if (!url.isEmpty()) {
                stepHandoff(url, o.value(QStringLiteral("file_name")).toString());
                return;
            }
            // 23018 "download file size limit" is what a copy that has not materialised
            // yet answers, not what an oversized file answers: the same call seconds later
            // hands over a 16 GB url (measured). So retry before reporting a limit.
            QTimer::singleShot(2000, this, [this, round] { stepDownload(round + 1); });
        });
}

void Netdisk::stepHandoff(const QString &url, const QString &serverName)
{
    // The url is not readable on its own: measured, the cdn answers 412 to every request that
    // does not carry the account's cookies and 206 as soon as they are. So the handoff is the
    // url *plus* the headers, and the pull itself is our engine's - which is what makes the
    // 并发数 in the settings card mean something here (measured on 5.6 GB: the client holds
    // ~100 KB/s per task, the engine with the cookie does 2.78 MB/s at four connections).
    grabHeaders(url, [this, url, serverName](const QString &headers) {
        if (headers.isEmpty()) {
            fail(QStringLiteral("拿不到夸克的登录凭据"));
            return;
        }
        // The copy is owed back once the bytes land, and it is owed back even if this app
        // dies first, so it is recorded before the handoff and persisted.
        Copy c;
        c.fid = m_job.copyFid;
        c.size = m_job.size;
        m_copies.insert(url, c);
        saveCopies();
        // The poster's title is the name; the drive's own (`4k.mp4`) is only the fallback,
        // and it is what a headless run has to go on because nobody passed a title.
        const QString shown = m_job.name.isEmpty() ? serverName : m_job.name;
        qInfo("nd ready bytes=%lld headers=%d name_chars=%d", m_job.size, headers.size(),
              shown.size());
        setStage(Idle);
        emit ready(m_job.share, url, headers, shown, m_job.poster);
        runNext();
    });
}

void Netdisk::grabHeaders(const QString &url, std::function<void(const QString &)> done)
{
    static const QRegularExpression hostRe(QStringLiteral("^([a-z0-9.-]+)//"));
    const QString host = QUrl(url).host();
    if (host.isEmpty()) {
        done(QString());
        return;
    }
    // The tab's own jar, not a captured request: whichever request the network domain reports
    // first yields an empty Cookie about as often as a real one, and an empty Cookie 412s in
    // a way that looks exactly like the cdn refusing us. Measured on this account: 346
    // cookies, 50 of them for a quark.cn host, and the read answers 206.
    send(QStringLiteral("Network.getAllCookies"), QJsonObject{}, [this, done, host](const QJsonObject &o) {
        const QJsonArray jar = o.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("cookies")).toArray();
        QStringList pairs;
        for (const QJsonValue &v : jar) {
            const QJsonObject c = v.toObject();
            QString domain = c.value(QStringLiteral("domain")).toString();
            if (domain.startsWith(QLatin1Char('.')))
                domain.remove(0, 1);
            const bool fits = host == domain || host.endsWith(QLatin1Char('.') + domain);
            if (!fits)
                continue;
            const QString name = c.value(QStringLiteral("name")).toString();
            const QString value = c.value(QStringLiteral("value")).toString();
            if (name.isEmpty() || value.isEmpty())
                continue;
            pairs << (name + QLatin1Char('=') + value);
        }
        if (pairs.isEmpty()) {
            qInfo("nd cookies host=%s jar=%d none fit", qPrintable(host), jar.size());
            done(QString());
            return;
        }
        // The UA of the browser that collected the jar goes along with it. Measured
        // 2026-11-01 that the cookie alone opens the 412 and the client's own `QuarkPC/<ve>`
        // changes no rate, so this is fidelity rather than a requirement.
        const QString headers = QStringLiteral("Cookie: %1\r\nUser-Agent: %2")
                                    .arg(pairs.join(QStringLiteral("; ")),
                                         m_ua.isEmpty() ? QString::fromLatin1(kWebUa) : m_ua);
        qInfo("nd cookies host=%s jar=%d sent=%d header_len=%d", qPrintable(host), jar.size(),
              pairs.size(), headers.size());
        done(headers);
    });
}

void Netdisk::deleteCopy(const QString &fid, qint64 size, std::function<void()> after)
{
    if (fid.isEmpty()) {
        if (after)
            after();
        return;
    }
    // The page is attached only while a ladder runs, and a copy is usually owed back long
    // after: ask for the tab again, but do not start a browser for it - if everything of
    // ours is closed, the copy waits for the next time one of them is up.
    qkPage([this, fid, size, after](bool ok, const QString &) {
        if (!ok) {
            qInfo("nd copy waits: no quark page");
            if (after)
                after();
            return;
        }
        // Byte-checked first: the save api can hand back the id of an earlier copy that has
        // since been deleted, and a delete by a stale id must never reach one of his own files.
        const QString expr = inPage(QStringLiteral(
            "const F=%1,S=%2;"
            "const i=await j('GET','https://drive.quark.cn/1/clouddrive/file/info?pr=ucpro&fr=pc&fid='+F);"
            "if(String((i.data&&i.data.size))!==String(S)) return {skipped:1,code:i.code};"
            "const d=await j('POST','https://drive.quark.cn/1/clouddrive/file/delete?pr=ucpro&fr=pc',"
            "{action_type:2,filelist:[F],exclude_fids:[]});"
            "return {code:d.code};")
                                        .arg(jsVal(QJsonValue(fid)),
                                             jsVal(QJsonValue(QString::number(size)))));
        eval(expr, [after, fid](bool ok, const QJsonValue &v) {
            // One line either way: a copy that quietly stayed is a quota leak with no trace,
            // and tonight's was only found because the drive was listed by hand.
            qInfo("nd copy returned=%d fid=%s %s", int(ok), qPrintable(fid.left(8)),
                  qPrintable(ok ? QString() : v.toString().left(60)));
            if (after)
                after();
        });
    }, false);
}

QString Netdisk::copiesPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("netdisk-copies.json"));
}

void Netdisk::loadCopies()
{
    QFile f(copiesPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    for (const QString &url : o.keys()) {
        const QJsonObject c = o.value(url).toObject();
        Copy cp;
        cp.fid = c.value(QStringLiteral("fid")).toString();
        cp.size = qint64(c.value(QStringLiteral("size")).toDouble());
        if (!cp.fid.isEmpty())
            m_copies.insert(url, cp);
    }
    if (!m_copies.isEmpty())
        qInfo("nd copies owed=%d", m_copies.size());
}

void Netdisk::saveCopies() const
{
    QJsonObject o;
    for (auto it = m_copies.constBegin(); it != m_copies.constEnd(); ++it)
        o.insert(it.key(), QJsonObject{ { QStringLiteral("fid"), it->fid },
                                        { QStringLiteral("size"), double(it->size) } });
    QDir().mkpath(QFileInfo(copiesPath()).path());
    QFile f(copiesPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("nd copies save failed: %s", qPrintable(f.errorString()));
        return;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void Netdisk::dropCopy(const QString &url)
{
    const auto it = m_copies.constFind(url);
    if (it == m_copies.constEnd())
        return;
    const Copy cp = it.value();
    m_copies.remove(url);
    saveCopies();
    qInfo("nd drop copy bytes=%lld", cp.size);
    deleteCopy(cp.fid, cp.size);
}

void Netdisk::reconcile(const QStringList &liveUrls)
{
    if (m_copies.isEmpty())
        return;
    QStringList stale;
    for (const QString &url : m_copies.keys())
        if (!liveUrls.contains(url))
            stale << url;
    if (stale.isEmpty())
        return;
    // A copy whose row is gone - finished and removed, cancelled, or left by a run that died
    // - is quota we spent and no longer need. Measured, a 5.6 GB copy is one row in `转存内容`
    // until it is deleted, so this is the difference between a tidy drive and a leaking one.
    qInfo("nd stale copies=%d", stale.size());
    for (const QString &url : stale)
        dropCopy(url);
}

void Netdisk::fail(const QString &note)
{
    const QString key = m_job.share;
    const QString fid = m_job.copyFid;
    const qint64 size = m_job.size;
    setStage(Idle);
    emit failed(key, note);
    if (!fid.isEmpty()) {
        // The copy is ours and it is the thing that costs him quota: take it back even when
        // the job died before any byte was pulled.
        //
        // Record the debt before paying it, because paying it is a page call and a page call
        // does not happen if this process ends first. Measured tonight: a 23018 failure left a
        // 5.6 GB copy in his drive because the harness quit the moment the row was told it
        // failed, and the ledger only ever learned about a copy at the hand-off.
        Copy c;
        c.fid = fid;
        c.size = size;
        m_copies.insert(key, c);
        saveCopies();
        qInfo("nd fail with copy: %s", qPrintable(note));
        deleteCopy(fid, size);
    }
    runNext();
}

void Netdisk::cancel(const QString &key)
{
    // Once a job has been handed over its bytes belong to Downloads, and the row's own stop is
    // what stops them; the copy then goes with the row, not with this call.
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).share == key) {
            m_queue.removeAt(i);
            emit failed(key, QStringLiteral("已取消"));
            return;
        }
    }
    if (m_stage != Idle && m_job.share == key)
        fail(QStringLiteral("已取消"));
}



// ------------------------------------------------------------------ 百度
//
// Five plain https calls and no browser anywhere, measured end to end on his own non-VIP
// session: the share html (jsToken + the locals blob) -> risklabel -> share/list ->
// share/tplconfig for sign+timestamp -> share/streaming. The last one answers #EXTM3U of raw
// MPEG-TS segments, and both the playlist and its segments answer 200 with NO cookie attached,
// so the hand-off is a url for mpv rather than a url for the range engine. Nothing is saved to
// his drive on the way, so there is no copy to owe back.

namespace {

const char *kBdUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

// Escape exactly what urllib.parse.quote escapes - everything except [A-Za-z0-9-._~] and `/`.
// Two spellings were measured against the same folder listing, seconds apart, same cookies:
// `dir=/sharelink.../K %20...` answers errno=0 and `dir=%2Fsharelink...` answers errno=-9, so
// the slash must stay literal. `+` must NOT survive either: baidu form-decodes it to a space
// and that silently corrupts the bdstoken.
QString bdPct(const QString &s)
{
    const QByteArray raw = s.toUtf8();
    QString out;
    out.reserve(raw.size() * 2);
    for (int i = 0; i < raw.size(); ++i) {
        const quint8 c = quint8(raw.at(i));
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += QChar(c);
        } else {
            out += QLatin1Char('%');
            out += QString::number(int(c), 16).rightJustified(2, QLatin1Char('0')).toUpper();
        }
    }
    return out;
}

QString bdQuery(const QList<QPair<QString, QString>> &items)
{
    QStringList parts;
    for (const QPair<QString, QString> &p : items)
        parts << QStringLiteral("%1=%2").arg(p.first, bdPct(p.second));
    return parts.join(QLatin1Char('&'));
}

QString bdNote(int e)
{
    switch (e) {
    case 2:     return QStringLiteral("百度的参数不对（errno=2）");
    case 13:    return QStringLiteral("提取码不对");
    case -9:    return QStringLiteral("百度不认这个会话，去设置-网盘登录重新登一次");
    case 116:   return QStringLiteral("该分享不存在了");
    case 140:   return QStringLiteral("百度说链接出错了（errno=140）");
    case -12:   return QStringLiteral("百度的会话掉了，去设置-网盘登录重新登一次");
    case 31023: return QStringLiteral("这个清晰度要超级会员");
    case 31339: return QStringLiteral("百度的签名对不上，再点一次");
    case -999:  return QStringLiteral("百度回的不是 json");
    default:    return QStringLiteral("百度回了 errno=%1").arg(e);
    }
}

// What an answer should be said as. An errno of -1 is what the callers fall back to when the
// object has no errno in it at all - which is exactly the shape of "the tab did not answer" -
// so the page's own words win when they are there instead of inventing a baidu number.
QString bdNote(const QJsonObject &o)
{
    const QString m = o.value(QStringLiteral("message")).toString();
    if (!m.isEmpty())
        return m;
    return bdNote(o.value(QStringLiteral("errno")).toInt(-1));
}

void bdAddApp(QList<QPair<QString, QString>> &q, const QString &web)
{
    q << qMakePair(QStringLiteral("channel"), QStringLiteral("chunlei"));
    q << qMakePair(QStringLiteral("web"), web);
    q << qMakePair(QStringLiteral("app_id"), QStringLiteral("250528"));
    q << qMakePair(QStringLiteral("clienttype"), QStringLiteral("0"));
}

} // namespace

QString Netdisk::bdCookie() const
{
    QStringList pairs;
    for (auto it = m_bdJar.constBegin(); it != m_bdJar.constEnd(); ++it) {
        if (it.key().isEmpty() || it.value().isEmpty())
            continue;
        pairs << QStringLiteral("%1=%2").arg(it.key(), it.value());
    }
    return pairs.join(QStringLiteral("; "));
}

void Netdisk::bdForm(const QString &tag, const QString &url, const QString &post,
                     std::function<void(const QJsonObject &, const QByteArray &)> then)
{
    QNetworkRequest req{ QUrl(url) };
    // Follow hops by hand. The ticket BDCLND rides on the 302 out of /s/<id>?pwd=, and a reply
    // that has already been redirected hands back only the FINAL response's headers, so the
    // automatic policy loses it and every later share call answers errno=-9.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("User-Agent", QByteArray(kBdUa));
    // Not optional: without an `Accept` line the share page answers a 17 KB verification
    // instead of the 75 KB real page (measured both ways from python too), and every share
    // call after that answers errno=-9. Qt sends no Accept at all unless it is set here.
    req.setRawHeader("Accept", QByteArray("*/*"));
    // Last, the way a browser sends it: Cookie ahead of User-Agent is a signature no
    // real client produces, and the folder listing was the only call that noticed.
    req.setRawHeader("Cookie", bdCookie().toUtf8());
    req.setRawHeader("Referer", QStringLiteral("https://pan.baidu.com/s/%1?pwd=%2")
                                   .arg(m_job.surl, m_job.pwd).toUtf8());
    if (!post.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded;charset=UTF-8"));
    {
        // The outgoing url with the three secret values masked: the fastest way to compare
        // against a replay from a different client.
        QString shown;
        const QStringList parts = url.split(QLatin1Char('&'));
        for (const QString &part : parts) {
            const QString k = part.section(QLatin1Char('='), 0, 0);
            shown += (k == QLatin1String("bdstoken") || k == QLatin1String("sign")
                      || k == QLatin1String("jsToken"))
                     ? k + QStringLiteral("=<set>") : part;
            shown += QLatin1Char('&');
        }
        qInfo("nd bd send %s url=%s", qPrintable(tag), qPrintable(shown.left(400)));
    }
    QNetworkReply *rep = post.isEmpty() ? m_nam->get(req) : m_nam->post(req, post.toUtf8());
    connect(rep, &QNetworkReply::finished, this, [this, rep, tag, url, then] {
        const QByteArray raw = rep->readAll();
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QList<QNetworkReply::RawHeaderPair> pairs = rep->rawHeaderPairs();
        rep->deleteLater();
        // Names and values go into the live jar; nothing about them is logged, because a
        // ticket value is a credential. Two Set-Cookie lines arrive as two pairs, which is
        // why this walks the pairs instead of asking for one header.
        for (const QNetworkReply::RawHeaderPair &h : pairs) {
            if (h.first.toLower() != QByteArray("set-cookie"))
                continue;
            const QByteArray &sc = h.second;
            const int eq = sc.indexOf('=');
            const int semi = sc.indexOf(';', eq);
            if (eq > 0) {
                const QString name = QString::fromUtf8(sc.left(eq)).trimmed();
                const QString val = QString::fromUtf8(
                    sc.mid(eq + 1, semi < 0 ? -1 : semi - eq - 1)).trimmed();
                if (!name.isEmpty() && !val.isEmpty())
                    m_bdJar.insert(name, val);
            }
        }
        if (http >= 300 && http < 400 && m_bdHops < 12) {
            QString loc;
            for (const QNetworkReply::RawHeaderPair &h : pairs)
                if (h.first.toLower() == QByteArray("location"))
                    loc = QString::fromUtf8(h.second);
            if (!loc.isEmpty()) {
                ++m_bdHops;
                bdForm(tag, QUrl(url).resolved(loc).toString(), QString(), then);
                return;
            }
        }
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        // baidu's own words tell a ticket problem from a spelling one far faster than an errno
        // does. Named local first: qPrintable() of a temporary inside qInfo() is already dead
        // by the time the printf runs and comes back as garbage (measured twice).
        QString note;
        if (o.value(QStringLiteral("errno")).toInt(0) != 0) {
            QString body = QString::fromUtf8(raw.left(220));
            body.remove(QRegularExpression(QStringLiteral("[A-Za-z0-9+/]{12,}=*")));
            body.remove(QLatin1Char('\n'));
            note = QStringLiteral(" msg=") + body.left(120);
        }
        qInfo("nd bd %s http=%d errno=%d bytes=%d%s", qPrintable(tag), http,
              o.value(QStringLiteral("errno")).toInt(-999), int(raw.size()), qPrintable(note));
        then(o, raw);
    });
}

void Netdisk::bdNext(std::function<void()> go)
{
    // baidu's edge answers the first share call from a non-browser client and errno=-9 from the
    // second one back to back: the app's own folder url, replayed by the probe seconds later
    // with the same cookies, returns errno=0 and three entries. So the ladder breathes between
    // calls instead of firing them as fast as the socket allows.
    QTimer::singleShot(m_bdDelay, this, std::move(go));
}

void Netdisk::bdJar()
{
    m_bdJar.clear();
    m_bdHops = 0;
    m_bdTries = 0;
    QFile f(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("pan-session-baidu.json")));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object()
                                   .value(QStringLiteral("cookies")).toArray();
        f.close();
        for (const QJsonValue &v : arr) {
            const QJsonObject c = v.toObject();
            QString dom = c.value(QStringLiteral("domain")).toString();
            while (dom.startsWith(QLatin1Char('.')))
                dom.remove(0, 1);
            if (dom != QLatin1String("baidu.com") && !dom.endsWith(QStringLiteral(".baidu.com")))
                continue;   // BDCLND is scoped to pan.baidu.com; dropping the subdomain-scoped
                            // cookies is what makes every later share call answer errno=-9.
            const QString name = c.value(QStringLiteral("name")).toString();
            const QString val = c.value(QStringLiteral("value")).toString();
            if (!name.isEmpty() && !val.isEmpty())
                m_bdJar.insert(name, val);
        }
    }
    m_bdCsrf = m_bdJar.value(QStringLiteral("csrfToken"));
    qInfo("nd bd jar fields=%d csrf=%d", int(m_bdJar.size()), int(m_bdCsrf.size()));
    if (m_bdJar.isEmpty()) {
        fail(QStringLiteral("还没有百度的登录记录，去设置-网盘登录里点「去登录」"));
        return;
    }
    // Start at the list, not at the page: the list answers us directly, hands back
    // share_id/uk itself, and the page is only needed at the end for jsToken. A share with a
    // pass code is the exception - without the visit nothing has presented it, and every call
    // answers errno=-9 until it has.
    if (!m_job.pwd.isEmpty()) {
        bdEnter([this] { bdNext([this] { bdList(QString(), QString()); }); });
        return;
    }
    bdNext([this] { bdList(QString(), QString()); });
}

bool Netdisk::bdReadJsToken(const QString &html)
{
    // jsToken is not an attribute: the page hands it to itself through
    // eval(decodeURIComponent('function fn(a){window.jsToken=a};fn("<hex>")')).
    static const QRegularExpression tok(
        QStringLiteral("decodeURIComponent\\('([^']*jsToken[^']*)'\\)"));
    static const QRegularExpression hex(QStringLiteral("fn\\(\"([0-9a-fA-F]+)\"\\)"));
    m_job.jsToken.clear();
    const auto t = tok.match(html);
    if (t.hasMatch()) {
        const QString inner = QUrl::fromPercentEncoding(t.captured(1).toUtf8());
        const auto h = hex.match(inner);
        if (h.hasMatch())
            m_job.jsToken = h.captured(1);
    }
    static const QRegularExpression ls(
        QStringLiteral("locals\\.mset\\((\\{.*?\\})\\);"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto l = ls.match(html);
    const QJsonObject o = l.hasMatch()
        ? QJsonDocument::fromJson(l.captured(1).toUtf8()).object() : QJsonObject();
    const qint64 sid = o.value(QStringLiteral("shareid")).toVariant().toLongLong();
    if (sid)
        m_job.shareid = QString::number(sid);
    if (m_job.uk.isEmpty())
        m_job.uk = o.value(QStringLiteral("share_uk")).toString();
    m_job.token32 = o.value(QStringLiteral("bdstoken")).toString();
    qInfo("nd bd html read jsToken=%d bytes=%d", int(m_job.jsToken.size()), int(html.size()));
    return !m_job.jsToken.isEmpty();
}

void Netdisk::bdEnter(std::function<void()> go)
{
    const QString u = QStringLiteral("https://pan.baidu.com/s/%1").arg(m_job.surl)
                      + (m_job.pwd.isEmpty() ? QString()
                                             : QStringLiteral("?pwd=%1").arg(m_job.pwd));
    bdPage([this, u, go](bool ok, const QString &note) {
        if (!ok) {
            fail(note.isEmpty() ? QStringLiteral("百度的标签页起不来") : note);
            return;
        }
        if (m_bdEntered == u) {
            // The tab is already sitting on this share - bdHtml used to navigate the very same
            // url a second time and pay another 5 s for a page that was already there.
            qInfo("nd bd enter reused");
            go();
            return;
        }
        m_bdEntered = u;
        send(QStringLiteral("Page.navigate"), { { QStringLiteral("url"), u } },
             [this, go](const QJsonObject &) {
                 QTimer::singleShot(5000, this, [this, go] {
                     // The visit is what presents the pass code, and BDCLND lands in the tab's
                     // jar - not in the recorded file we send on our own socket. Measured on a
                     // code we had never presented: the list call answered errno=-9 to Qt and
                     // errno=-9 to the tab as well, until this navigate happens.
                     bdTabJar();
                     go();
                 });
             });
    });
}

void Netdisk::bdTabJar()
{
    send(QStringLiteral("Network.getAllCookies"), QJsonObject{}, [this](const QJsonObject &o) {
        const QJsonArray jar = o.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("cookies")).toArray();
        int moved = 0;
        for (const QJsonValue &v : jar) {
            const QJsonObject c = v.toObject();
            const QString name = c.value(QStringLiteral("name")).toString();
            const QString value = c.value(QStringLiteral("value")).toString();
            const QString domain = c.value(QStringLiteral("domain")).toString();
            if (name.isEmpty() || value.isEmpty()
                || !domain.contains(QStringLiteral("baidu.com")))
                continue;
            if (m_bdJar.value(name) != value)
                ++moved;
            m_bdJar.insert(name, value);
        }
        qInfo("nd bd tab jar read=%d moved=%d now=%d", int(jar.size()), moved,
              int(m_bdJar.size()));
    });
}

void Netdisk::bdHtml()
{
    // Let the tab actually visit the share: a browser navigation gets the page he would see,
    // and that page's own script sets window.jsToken. Asking for the same url over Qt's socket
    // answers a 15 KB verification page with no token in it (measured twice) - which is what
    // the edge serves to a client it does not recognise.
    const QString u = QStringLiteral("https://pan.baidu.com/s/%1").arg(m_job.surl)
                      + (m_job.pwd.isEmpty() ? QString()
                                             : QStringLiteral("?pwd=%1").arg(m_job.pwd));
    bdEnter([this, u] {
        eval(QStringLiteral("JSON.stringify({t:window.jsToken||'',h:location.href})"),
             [this, u](bool eok, const QJsonValue &v) {
                const QJsonObject o = eok
                    ? QJsonDocument::fromJson(v.toString().toUtf8()).object()
                    : QJsonObject();
                m_job.jsToken = o.value(QStringLiteral("t")).toString();
                qInfo("nd bd tab token=%d", int(m_job.jsToken.size()));
                if (!m_job.jsToken.isEmpty()) {
                    bdNext([this] { bdStream(); });
                    return;
                }
                bdForm(QStringLiteral("html"), u, QString(),
                       [this](const QJsonObject &, const QByteArray &raw) {
                    if (!bdReadJsToken(QString::fromUtf8(raw))) {
                        fail(QStringLiteral("百度这页没给播放令牌"));
                        return;
                    }
                    bdNext([this] { bdStream(); });
                });
             });
    });
}

void Netdisk::bdRisk()
{
    if (m_job.pwd.isEmpty()) {
        bdList(QString(), QString());   // 公开分享不需要验码
        return;
    }
    QList<QPair<QString, QString>> q;
    bdAddApp(q, QStringLiteral("1"));
    q << qMakePair(QStringLiteral("uk"), m_job.uk);
    q << qMakePair(QStringLiteral("share_id"), m_job.shareid);
    q << qMakePair(QStringLiteral("pwd"), m_job.pwd);
    q << qMakePair(QStringLiteral("bdstoken"), m_bdCsrf);
    bdForm(QStringLiteral("risklabel"),
           QStringLiteral("https://pan.baidu.com/share/risklabel?") + bdQuery(q), QString(),
           [this](const QJsonObject &o, const QByteArray &) {
        const int e = o.value(QStringLiteral("errno")).toInt(-1);
        if (e != 0) {
            fail(e == 13 ? QStringLiteral("百度的提取码不对") : bdNote(o));
            return;
        }
        bdNext([this] { bdList(QString(), QString()); });
    });
}

void Netdisk::bdList(const QString &dir, const QString &parentFid)
{
    const bool sub = !dir.isEmpty();
    QList<QPair<QString, QString>> q;
    // The share family wants app_id=250528 with web=5 on the root call and web=1 inside a
    // folder; mixing the two spellings is exactly the errno=140 "啊哦，链接出错了" answer.
    bdAddApp(q, QStringLiteral("5"));   // both list calls; web=1 is the page's own
                                              // spelling and it needs sekey alongside
    // `shorturl` rides on BOTH calls: the folder listing the page itself sends carries it
    // alongside shareid/uk, and dropping it is what makes the sub-call answer errno=-9.
    QString shorturl = m_job.surl;
    if (shorturl.startsWith(QLatin1Char('1')))
        shorturl.remove(0, 1);
    q << qMakePair(QStringLiteral("shorturl"), shorturl);
    if (sub) {
        q << qMakePair(QStringLiteral("shareid"), m_job.shareid);
        q << qMakePair(QStringLiteral("uk"), m_job.uk);
        q << qMakePair(QStringLiteral("root"), QStringLiteral("0"));
        q << qMakePair(QStringLiteral("dir"), dir);
    } else {
        q << qMakePair(QStringLiteral("root"), QStringLiteral("1"));
    }
    q << qMakePair(QStringLiteral("num"), QStringLiteral("100"));
    q << qMakePair(QStringLiteral("page"), QStringLiteral("1"));
    q << qMakePair(QStringLiteral("order"), QStringLiteral("name"));
    q << qMakePair(QStringLiteral("desc"), QStringLiteral("0"));
    q << qMakePair(QStringLiteral("showempty"), QStringLiteral("0"));
    q << qMakePair(QStringLiteral("view_mode"), QStringLiteral("1"));
    q << qMakePair(QStringLiteral("bdstoken"), m_bdCsrf);
    bdTry(sub ? QStringLiteral("list dir") : QStringLiteral("list root"),
          QStringLiteral("https://pan.baidu.com/share/list?") + bdQuery(q),
          [this, parentFid](const QJsonObject &o) {
        const int e = o.value(QStringLiteral("errno")).toInt(-1);
        if (e != 0) {
            // The re-present is only worth making once the list has handed back the ids it
            // needs; asked with empty uk/share_id it answers errno=200025 (measured).
            if (!m_job.pwd.isEmpty() && m_bdTries < 2 && !m_job.shareid.isEmpty()
                && !m_job.uk.isEmpty()) {
                ++m_bdTries;
                bdNext([this] { bdRisk(); });   // re-present the pass code, then list again
                return;
            }
            fail(bdNote(o));
            return;
        }
        if (m_job.shareid == QLatin1String("0") || m_job.shareid.isEmpty())
            m_job.shareid = QString::number(
                o.value(QStringLiteral("share_id")).toVariant().toLongLong());
        if (m_job.uk.isEmpty())
            m_job.uk = o.value(QStringLiteral("uk")).toVariant().toString();
        const QJsonArray arr = o.value(QStringLiteral("list")).toArray();
        if (arr.isEmpty()) {
            fail(QStringLiteral("百度这个分享里没有文件"));
            return;
        }
        // The flag is `isdir`. Reading `is_dir` instead finds nothing, and the first pass then
        // went on to treat the size-0 directory itself as the file and got errno=2 back.
        QJsonObject file, folder;
        for (const QJsonValue &v : arr) {
            const QJsonObject f = v.toObject();
            // toVariant first: this endpoint answers `isdir` as a string, and QJsonValue::toInt()
            // on a string is 0 - which silently made every folder a size-0 file.
            if (f.value(QStringLiteral("isdir")).toVariant().toInt() == 1) {
                if (folder.isEmpty())
                    folder = f;
                continue;
            }
            const qint64 sz = f.value(QStringLiteral("size")).toVariant().toLongLong();
            if (file.isEmpty() || sz > file.value(QStringLiteral("size")).toVariant().toLongLong())
                file = f;
        }
        if (file.isEmpty() && !folder.isEmpty() && parentFid.isEmpty()) {
            const QString name = folder.value(QStringLiteral("server_filename")).toString();
            const QString pfid = QString::number(
                qint64(folder.value(QStringLiteral("fs_id")).toVariant().toLongLong()));
            const QString d = QStringLiteral("/sharelink%1-%2/%3").arg(m_job.uk, pfid, name);
            bdNext([this, d, pfid] { bdList(d, pfid); });
            return;
        }
        if (file.isEmpty()) {
            fail(QStringLiteral("分享里只有目录，目录里没有文件"));
            return;
        }
        m_job.fid = QString::number(qint64(file.value(QStringLiteral("fs_id")).toVariant().toLongLong()));
        m_job.fname = file.value(QStringLiteral("server_filename")).toString();
        m_job.size = qint64(file.value(QStringLiteral("size")).toVariant().toLongLong());
        qInfo("nd bd picked fid=%s orig=%lld name=%d", qPrintable(m_job.fid), m_job.size,
              int(m_job.fname.size()));
        // Rows on these walls point at whatever the poster uploaded, and some of them hold
        // pictures. Asking /share/streaming for one answers a one-line error that reads exactly
        // like a broken ladder, so the suffix is checked here instead.
        static const QStringList videos { QStringLiteral("mp4"), QStringLiteral("mkv"),
            QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("ts"),
            QStringLiteral("m4v"), QStringLiteral("flv"), QStringLiteral("wmv"),
            QStringLiteral("mpg"), QStringLiteral("webm") };
        if (!videos.contains(QFileInfo(m_job.fname).suffix().toLower())) {
            fail(QStringLiteral("这个分享里最大的不是视频文件"));
            return;
        }
        bdNext([this] { bdSign(); });
    });
}

void Netdisk::bdSign()
{
    QList<QPair<QString, QString>> q;
    q << qMakePair(QStringLiteral("surl"), m_job.surl);
    q << qMakePair(QStringLiteral("shareid"), m_job.shareid);
    q << qMakePair(QStringLiteral("uk"), m_job.uk);
    // A comma list, because that is what the page sends: its lazy loader does String(keys) on
    // an array. Asked as a JSON array this answers errno=0 with neither key present, which
    // reads exactly like a permission problem and is a spelling one.
    q << qMakePair(QStringLiteral("fields"), QStringLiteral("sign,timestamp"));
    q << qMakePair(QStringLiteral("view_mode"), QStringLiteral("1"));
    bdTry(QStringLiteral("tplconfig"),
          QStringLiteral("https://pan.baidu.com/share/tplconfig?") + bdQuery(q),
          [this](const QJsonObject &o) {
        QJsonObject r = o.value(QStringLiteral("result")).toObject();
        if (r.isEmpty())
            r = o.value(QStringLiteral("data")).toObject();   // the loader does (e.result||e.data)[key]
        m_job.sign = r.value(QStringLiteral("sign")).toString();
        m_job.timestamp = QString::number(
            qint64(r.value(QStringLiteral("timestamp")).toVariant().toLongLong()));
        if (m_job.sign.isEmpty() || m_job.timestamp == QLatin1String("0")) {
            fail(QStringLiteral("百度没给播放签名，%1").arg(bdNote(o)));
            return;
        }
        bdNext([this] { bdHtml(); });   // only for jsToken, last of the chain
    });
}

void Netdisk::bdStream()
{
    QList<QPair<QString, QString>> q;
    bdAddApp(q, QStringLiteral("1"));
    q << qMakePair(QStringLiteral("uk"), m_job.uk);
    q << qMakePair(QStringLiteral("shareid"), m_job.shareid);
    q << qMakePair(QStringLiteral("fid"), m_job.fid);
    // AUTO_1080 measured on his non-VIP account hands back 1920x804 h264+aac; it is the
    // *named* tiers M3U8_FLV_264_720/1080 that answer errno=31023.
    q << qMakePair(QStringLiteral("type"), QStringLiteral("M3U8_AUTO_1080"));
    q << qMakePair(QStringLiteral("vip"), QStringLiteral("0"));
    q << qMakePair(QStringLiteral("sign"), m_job.sign);
    q << qMakePair(QStringLiteral("timestamp"), m_job.timestamp);
    q << qMakePair(QStringLiteral("jsToken"), m_job.jsToken);
    const QString api = QStringLiteral("https://pan.baidu.com/share/streaming?") + bdQuery(q);
    // The playlist is taken from the tab, not from our own socket. Measured with the same
    // cookies and a valid token: the tab and python both get 962 segments / 9617 s, while
    // Qt's socket gets a 10 segment / 100 s preview - the anonymous answer, i.e. the edge
    // does not count our session at all for this call. The segments are absolute
    // baidupcs.com urls that need no cookie, so writing what the tab fetched to a local
    // .m3u8 gives mpv the whole film and nothing else has to be proxied.
    const auto handOff = [this](const QString &file, int segs, double dur) {
        qInfo("nd bd table segs=%d hours=%.2f original=%lld ms=%lld", segs, dur / 3600.0,
              m_job.size, m_bdClock.isValid() ? qint64(m_bdClock.elapsed()) : -1LL);
        setStage(Idle);
        // The table is the job's source, and the row plays from it directly: `ready` is the
        // same hand-off the 夸克 ladder makes, so 下载列表, 并发数, the resume and the row's
        // play mark all come for free instead of a second path that jumps to the player.
        emit ready(m_job.share, file, QString(),
                   m_job.name.isEmpty() ? m_job.fname : m_job.name, m_job.poster);
        runNext();
    };
    bdPageText(api, [this, handOff](bool okp, const QString &text, int segs, double dur) {
        if (!okp || !text.startsWith("#EXTM3U") || segs < 2) {
            fail(QStringLiteral("百度的播放表拿不到（标签页回了 %1 段）").arg(segs));
            return;
        }
        QFile f(bdPlaylistPath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail(QStringLiteral("播放表写不下去"));
            return;
        }
        f.write(text.toUtf8());
        f.close();
        qInfo("nd bd playlist %s segs=%d", qPrintable(QFileInfo(f).fileName()), segs);
        // The plain path, not a file:// url: the row's source is stat'd as a file, and the
        // engine reads the table off disk before it goes out over https for the segments.
        handOff(QFileInfo(f).absoluteFilePath(), segs, dur);
    });
}

// ---------------------------------------------------------- the page is the fallback client
//
// Measured tonight: the app's own folder-listing url, sent by python with the same cookies,
// answers errno=0 and three entries in the same minute that Qt's socket gets errno=-9 for the
// identical request. Pacing, `Connection: close`, the TLS backend, header order and both
// bdstoken spellings were each tried and moved nothing, so the difference is made by whatever
// the edge sees in a non-browser client. The site's own tab IS a browser, so when we are
// refused the tab asks - the same shape the 夸克 ladder uses for endpoints its host will not
// answer to anyone else.

void Netdisk::bdPage(std::function<void(bool, const QString &)> done, int tries)
{
    // `m_bdPageReady` used to short-circuit here for the rest of the run. Measured: close the
    // tab between two page calls of one job and the websocket is dead, the command goes
    // nowhere, no callback ever runs, and the job sits until the 60 s deadline saying
    // 「百度没有回应」. So the flag only means "we hold a tab id", and every call checks that
    // the tab is still in the browser's own list.
    const int port = PanLogin::debugPort();
    QNetworkReply *rep = m_nam->get(QNetworkRequest(QUrl(
        QStringLiteral("http://127.0.0.1:%1/json/list").arg(port))));
    connect(rep, &QNetworkReply::finished, this, [this, rep, port, done, tries] {
        const QByteArray body = rep->readAll();
        const bool up = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        if (!up) {
            if (tries == 0) {
                const QString exe = PanLogin::browserExecutable();
                if (exe.isEmpty()) {
                    done(false, QStringLiteral("这台机器上没有 Chrome 或 Edge，百度那一发问不动"));
                    return;
                }
                // Not a window he is asked to watch: the ladder needs a browser, not an
                // audience. The one place a window must appear is 设置-网盘登录, and that is
                // PanLogin::login's own call with visible left at true.
                QProcess::startDetached(exe, PanLogin::browserArgs(
                    QStringLiteral("https://pan.baidu.com/"), false));
                qInfo("nd bd browser launched");
            }
            if (tries > 9) {
                done(false, QStringLiteral("等不到百度的标签页起来"));
                return;
            }
            QTimer::singleShot(1500, this, [this, done, tries] { bdPage(done, tries + 1); });
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(body).array();
        QJsonObject pick;
        for (const QJsonValue &v : arr) {
            const QJsonObject t = v.toObject();
            if (t.value(QStringLiteral("type")).toString() == QLatin1String("page")
                && t.value(QStringLiteral("url")).toString()
                       .contains(QStringLiteral("pan.baidu.com"))) {
                pick = t;
                break;
            }
        }
        if (pick.isEmpty()) {
            m_bdPageReady = false;
            // No baidu tab yet. /json/new ignores its url argument on this Chrome, so the tab
            // comes up blank and gets navigated by us once we hold its socket.
            QNetworkReply *n = m_nam->put(
                QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/new").arg(port))),
                QByteArray());
            connect(n, &QNetworkReply::finished, this, [this, n, done] {
                const QJsonObject t = QJsonDocument::fromJson(n->readAll()).object();
                const QString ws = t.value(QStringLiteral("webSocketDebuggerUrl")).toString();
                m_bdTabId = t.value(QStringLiteral("id")).toString();
                n->deleteLater();
                if (ws.isEmpty()) {
                    done(false, QStringLiteral("开不出百度的标签页"));
                    return;
                }
                attach({ { QStringLiteral("webSocketDebuggerUrl"), ws } });
                // attach() only starts the socket; a command sent before it opens is dropped.
                QTimer::singleShot(700, this, [this, done] {
                send(QStringLiteral("Page.enable"), QJsonObject{}, [this, done](const QJsonObject &) {
                    send(QStringLiteral("Page.navigate"),
                         { { QStringLiteral("url"),
                             QStringLiteral("https://pan.baidu.com/disk/main") } },
                         [this, done](const QJsonObject &) {
                             QTimer::singleShot(3000, this, [this, done] {
                                 m_bdPageReady = true;
                                 done(true, QString());
                             });
                         });
                });
                });
            });
            return;
        }
        const QString id = pick.value(QStringLiteral("id")).toString();
        if (m_bdPageReady && id == m_bdTabId && m_ws
            && m_ws->state() == QAbstractSocket::ConnectedState) {
            done(true, QString());
            return;
        }
        m_bdPageReady = false;
        m_bdTabId = id;
        attach(pick);
        QTimer::singleShot(700, this, [this, done] {
            send(QStringLiteral("Page.enable"), QJsonObject{}, [this, done](const QJsonObject &) {
                m_bdPageReady = true;
                done(true, QString());
            });
        });
    });
}

void Netdisk::bdPageJson(const QString &tag, const QString &url,
                         std::function<void(const QJsonObject &)> then)
{
    bdPage([this, tag, url, then](bool ok, const QString &note) {
        if (!ok) {
            QJsonObject e;
            e.insert(QStringLiteral("errno"), -1);
            e.insert(QStringLiteral("message"), note);
            then(e);
            return;
        }
        api(QStringLiteral("GET"), url, QJsonValue(),
            [this, tag, then](bool ok2, const QJsonObject &r) {
                qInfo("nd bd page %s answered=%d errno=%d", qPrintable(tag), int(ok2),
                      r.value(QStringLiteral("errno")).toInt(-999));
                then(r);
            });
    });
}

void Netdisk::bdPageStream(const QString &url, std::function<void(bool, int, double)> then)
{
    bdPage([this, url, then](bool ok, const QString &) {
        if (!ok) {
            then(false, 0, 0.0);
            return;
        }
        // Counted inside the page so a megabyte of playlist never crosses the wire: mpv reads
        // the same url itself afterwards, and the segments answer 200 with no cookie at all.
        const QString read = inPage(QStringLiteral(
            "const x=await new Promise(res=>{const q=new XMLHttpRequest();"
            "q.open('GET',%1,true);q.withCredentials=true;q.timeout=30000;"
            "q.onload=()=>res(q.responseText||'');q.onerror=()=>res('');q.ontimeout=()=>res('');"
            "q.send();});"
            "const L=(x||'').split('\\n');"
            "let segs=0,dur=0;"
            "for(const l of L){if(!l)continue;"
            "if(l.startsWith('#EXTINF:')){dur+=parseFloat(l.slice(8))||0;}"
            "else if(!l.startsWith('#'))++segs;}"
            "return JSON.stringify({ok:(x||'').startsWith('#EXTM3U'),segs,dur});"))
            .arg(jsVal(QJsonValue(url)));
        eval(read, [then](bool eok, const QJsonValue &v) {
            const QJsonObject o = eok
                ? QJsonDocument::fromJson(v.toString().toUtf8()).object() : QJsonObject();
            then(o.value(QStringLiteral("ok")).toBool(), o.value(QStringLiteral("segs")).toInt(),
                 o.value(QStringLiteral("dur")).toDouble());
        });
    });
}

void Netdisk::bdTry(const QString &tag, const QString &url,
                    std::function<void(const QJsonObject &)> then)
{
    if (m_bdPageFirst) {
        bdPageJson(tag, url, then);
        return;
    }
    bdForm(tag, url, QString(), [this, tag, url, then](const QJsonObject &o, const QByteArray &) {
        if (o.value(QStringLiteral("errno")).toInt(0) != -9) {
            then(o);
            return;
        }
        qInfo("nd bd %s direct=-9, asking the page", qPrintable(tag));
        bdPageJson(tag, url, then);
    });
}

QString Netdisk::bdPlaylistPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("bd-play-%1.m3u8").arg(m_job.fid));
}

void Netdisk::bdPageText(const QString &url,
                         std::function<void(bool, const QString &, int, double)> then)
{
    bdPage([this, url, then](bool ok, const QString &) {
        if (!ok) {
            then(false, QString(), 0, 0.0);
            return;
        }
        // Fetched in the page, counted in the page, and the text comes back so it can be
        // written out for mpv: a megabyte over the DevTools socket is fine, a megabyte per
        // second through it would not be.
        const QString read = QStringLiteral(
            "(async()=>{const q=new XMLHttpRequest();q.open('GET',")
            + jsVal(QJsonValue(url)) + QStringLiteral(
            ",true);q.withCredentials=true;q.timeout=30000;"
            "const t=await new Promise(res=>{q.onload=()=>res(q.responseText||'');"
            "q.onerror=()=>res('');q.ontimeout=()=>res('');q.send();});"
            "const L=t.split('\\n');let segs=0,dur=0;"
            "for(const l of L){if(!l)continue;"
            "if(l.startsWith('#EXTINF:')){dur+=parseFloat(l.slice(8))||0;}"
            "else if(!l.startsWith('#'))++segs;}"
            "return JSON.stringify({ok:t.startsWith('#EXTM3U'),segs,dur,st:q.status,"
            "head:t.slice(0,200),text:segs>1?t:''});})()");
        eval(read, [then](bool eok, const QJsonValue &v) {
            const QJsonObject o = eok
                ? QJsonDocument::fromJson(v.toString().toUtf8()).object() : QJsonObject();
            if (!o.value(QStringLiteral("ok")).toBool()
                || o.value(QStringLiteral("segs")).toInt() < 2) {
                // The tab answered *something*; without its words "1 段" cannot be told apart
                // from a refused transcode. Long tokens are stripped before it reaches the log.
                QString body = o.value(QStringLiteral("head")).toString().left(160);
                body.remove(QRegularExpression(QStringLiteral("[A-Za-z0-9+/]{12,}=*")));
                body.replace(QLatin1Char('\n'), QLatin1Char(' '));
                qInfo("nd bd page text http=%d segs=%d bytes=%d msg=%s",
                      o.value(QStringLiteral("st")).toInt(-1),
                      o.value(QStringLiteral("segs")).toInt(),
                      int(body.size()), qPrintable(body.left(120)));
            }
            then(o.value(QStringLiteral("ok")).toBool(),
                 o.value(QStringLiteral("text")).toString(),
                 o.value(QStringLiteral("segs")).toInt(),
                 o.value(QStringLiteral("dur")).toDouble());
        });
    });
}

// ------------------------------------------------------------------ 迅雷

namespace {

// The drive's own words for the two refusals this ladder can meet. `captcha_invalid` with an
// empty detail is the important one: it does NOT mean the token expired, it means the token was
// signed for a different action - measured, presenting it with a non-empty `pass_code` gives
// this same answer whether the code is right, wrong, or belongs to another share.
QString xlNote(const QJsonObject &o)
{
    const QJsonArray det = o.value(QStringLiteral("error_details")).toArray();
    const QString d = det.isEmpty()
        ? QString() : det.first().toObject().value(QStringLiteral("detail")).toString();
    const QString e = o.value(QStringLiteral("error")).toString();
    if (e == QLatin1String("captcha_invalid"))
        return QStringLiteral("迅雷这发要它自己那一页去问（验证令牌是按动作签的），"
                              "会话掉了的话去设置-网盘登录重新记一次");
    if (e == QLatin1String("invalid_argument") && d.contains(QStringLiteral("device_id")))
        return QStringLiteral("迅雷要浏览器自己那颗 device_id");
    if (!d.isEmpty())
        return QStringLiteral("迅雷回：%1").arg(d.left(80));
    return QStringLiteral("迅雷回了 %1").arg(e.isEmpty() ? QStringLiteral("不是 json") : e);
}

bool xlIsVideo(const QJsonObject &f)
{
    if (f.value(QStringLiteral("mime_type")).toString().startsWith(QStringLiteral("video/")))
        return true;
    static const QRegularExpression ext(
        QStringLiteral("\\.(mp4|mkv|mov|avi|ts|m4v|flv|wmv|mpg|webm|rmvb)$"),
        QRegularExpression::CaseInsensitiveOption);
    return ext.match(f.value(QStringLiteral("name")).toString()).hasMatch();
}

// Sizes come back as STRINGS. Measured the hard way: a folder's `"0"` is truthy, so `!size`
// called every folder a file and the walk never entered one.
qint64 xlBytes(const QJsonObject &f)
{
    return qint64(f.value(QStringLiteral("size")).toString().toDouble());
}

const char *const kXlApi = "https://api-pan.xunlei.com/drive/v1";

// The pass_code_token is base64, so it carries `+` and `/`. Sent raw, a `+` in a query means a
// space to the server and it answers 400 "illegal base64 data at input byte 19" - measured twice,
// once in python and once here, and both times the wording made it look like the drive refusing
// us rather than our own encoding. Everything else in this api's queries is unreserved.
QString xlPct(const QString &s)
{
    const QByteArray raw = s.toUtf8();
    QString out;
    out.reserve(raw.size() * 2);
    for (int i = 0; i < raw.size(); ++i) {
        const quint8 c = quint8(raw.at(i));
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += QChar(c);
        } else {
            out += QLatin1Char('%');
            out += QString::number(int(c), 16).rightJustified(2, QLatin1Char('0')).toUpper();
        }
    }
    return out;
}

} // namespace

void Netdisk::xlJar()
{
    m_xlHdr = QJsonObject();
    QFile f(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("pan-session-xunlei.json")));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        const QJsonObject h = d.value(QStringLiteral("api_headers")).toObject();
        for (const QString &k : { QStringLiteral("authorization"),
                                  QStringLiteral("x-captcha-token"),
                                  QStringLiteral("x-client-id"), QStringLiteral("x-device-id"),
                                  QStringLiteral("user-agent"), QStringLiteral("referer"),
                                  QStringLiteral("origin") })
            if (!h.value(k).toString().isEmpty())
                m_xlHdr.insert(k, h.value(k));
        // The header set is minutes-to-hours old depending on when he last ran 去登录, and the
        // token covers a growing list of actions - so its age belongs in the log next to the
        // answer it produced. Lengths and the clock only; the values are his session.
        qInfo("nd xl jar headers=%d captcha_len=%d bearer_len=%d headers_at=%lld age=%llds",
              int(m_xlHdr.size()),
              m_xlHdr.value(QStringLiteral("x-captcha-token")).toString().size(),
              m_xlHdr.value(QStringLiteral("authorization")).toString().size(),
              qint64(d.value(QStringLiteral("headers_at")).toDouble()),
              qint64(QDateTime::currentSecsSinceEpoch()
                     - d.value(QStringLiteral("headers_at")).toDouble()));
    }
    if (m_xlHdr.isEmpty()) {
        fail(QStringLiteral("还没有迅雷的登录记录，去设置-网盘登录里点「去登录」"));
        return;
    }
    xlShare();
}

void Netdisk::xlForm(const QString &tag, const QString &url,
                     std::function<void(const QJsonObject &)> then)
{
    QNetworkRequest req{ QUrl(url) };
    for (auto it = m_xlHdr.constBegin(); it != m_xlHdr.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    req.setRawHeader("Accept", QByteArray("application/json"));
    {
        // The ids are not secret and are what a comparison needs; the two tickets are.
        QString shown = url;
        shown.remove(QRegularExpression(QStringLiteral(
            "(pass_code|pass_code_token|authorization)=[^&]*")))
            .replace(QRegularExpression(QStringLiteral("&{2,}")), QStringLiteral("&"));
        qInfo("nd xl send %s url=%s", qPrintable(tag), qPrintable(shown.left(260)));
    }
    QNetworkReply *rep = m_nam->get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, tag, then] {
        const QByteArray raw = rep->readAll();
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const int err = int(rep->error());
        rep->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        if (m_stage != Idle)
            m_deadline->start(90000);
        qInfo("nd xl answer %s http=%d err=%d bytes=%lld keys=%d", qPrintable(tag), http, err,
              qint64(raw.size()), int(o.size()));
        then(o);
    });
}

void Netdisk::xlShare()
{
    const QString url = QStringLiteral("%1/share?share_id=%2&pass_code=&limit=100&keyword="
                                      "&pass_code_token=&page_token=&scene=NORMAL"
                                      "&order=DEFAULT_ORDER&thumbnail_size=SIZE_LARGE")
                            .arg(QString::fromLatin1(kXlApi), m_job.surl);
    xlForm(QStringLiteral("share"), url, [this](const QJsonObject &o) {
        if (o.contains(QStringLiteral("error"))) {
            fail(xlNote(o));
            return;
        }
        const QString st = o.value(QStringLiteral("share_status")).toString();
        const QJsonArray files = o.value(QStringLiteral("files")).toArray();
        qInfo("nd xl share %s status=%s num=%d files=%d token_len=%d", qPrintable(m_job.surl),
              qPrintable(st), o.value(QStringLiteral("file_num")).toInt(), int(files.size()),
              o.value(QStringLiteral("pass_code_token")).toString().size());
        if (st == QLatin1String("PASS_CODE_EMPTY")) {
            if (m_job.pwd.isEmpty()) {
                fail(QStringLiteral("这个迅雷分享要提取码"));
                return;
            }
            xlTypeCode(m_job.pwd, [this](bool ok, const QJsonObject &r) {
                if (!ok) {
                    fail(QStringLiteral("迅雷那一页没把提取码交进去"));
                    return;
                }
                xlGotRoot(r);
            });
            return;
        }
        if (files.isEmpty()) {
            fail(QStringLiteral("这个迅雷分享里没有文件"));
            return;
        }
        xlGotRoot(o);
    });
}

void Netdisk::xlGotRoot(const QJsonObject &o)
{
    m_xlToken = o.value(QStringLiteral("pass_code_token")).toString();
    m_xlTree = o.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &v : m_xlTree) {
        const QJsonObject f = v.toObject();
        if (f.value(QStringLiteral("kind")).toString() == QLatin1String("drive#folder"))
            m_xlParents.append(QStringLiteral("%1@1")
                                   .arg(f.value(QStringLiteral("id")).toString()));
    }
    qInfo("nd xl root entries=%d folders=%d token_len=%d", int(m_xlTree.size()),
          int(m_xlParents.size()), int(m_xlToken.size()));
    xlStep();
}

void Netdisk::xlList(const QString &parent, int depth)
{
    const QString url = QStringLiteral("%1/share/detail?share_id=%2&parent_id=%3"
                                       "&pass_code_token=%4&limit=100&keyword=&page_token="
                                       "&scene=NORMAL&order=DEFAULT_ORDER"
                                       "&thumbnail_size=SIZE_LARGE")
                            .arg(QString::fromLatin1(kXlApi), m_job.surl, parent, xlPct(m_xlToken));
    xlForm(QStringLiteral("detail"), url, [this, parent, depth](const QJsonObject &o) {
        if (o.contains(QStringLiteral("error"))) {
            // One unreadable folder must not kill a share that has three others.
            qInfo("nd xl detail skipped: %s", qPrintable(xlNote(o)));
            xlStep();
            return;
        }
        const QJsonArray files = o.value(QStringLiteral("files")).toArray();
        for (const QJsonValue &v : files) {
            const QJsonObject f = v.toObject();
            m_xlTree.append(v);
            if (f.value(QStringLiteral("kind")).toString() == QLatin1String("drive#folder")
                && depth + 1 < 4 && m_xlTree.size() < 200)
                m_xlParents.append(QStringLiteral("%1@%2")
                                       .arg(f.value(QStringLiteral("id")).toString())
                                            .arg(depth + 1));
        }
        qInfo("nd xl detail depth=%d kids=%d tree=%d", depth, int(files.size()),
              int(m_xlTree.size()));
        xlStep();
    });
}

void Netdisk::xlStep()
{
    while (!m_xlParents.isEmpty()) {
        const QString p = m_xlParents.takeFirst();
        const int at = p.lastIndexOf(QLatin1Char('@'));
        const QString id = p.left(at);
        const int depth = p.mid(at + 1).toInt();
        if (m_xlSeenParent.contains(id))
            continue;
        m_xlSeenParent.insert(id);
        xlList(id, depth);
        return;
    }
    xlPick();
}

void Netdisk::xlPick()
{
    QList<QJsonObject> vids;
    for (const QJsonValue &v : m_xlTree) {
        const QJsonObject f = v.toObject();
        if (f.value(QStringLiteral("kind")).toString() != QLatin1String("drive#file"))
            continue;
        if (xlIsVideo(f))
            vids.append(f);
    }
    if (vids.isEmpty()) {
        fail(QStringLiteral("这个迅雷分享里最大的不是视频文件"));
        return;
    }
    std::sort(vids.begin(), vids.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return xlBytes(a) > xlBytes(b);
    });
    m_xlBest = vids.last();
    for (const QJsonObject &v : vids)
        if (xlBytes(v) <= kPreferMaxBytes) {
            m_xlBest = v;
            break;
        }
    m_job.fid = m_xlBest.value(QStringLiteral("id")).toString();
    m_job.fname = m_xlBest.value(QStringLiteral("name")).toString();
    m_job.size = xlBytes(m_xlBest);
    qInfo("nd xl pick bytes=%lld videos=%d tree=%d name=%s", m_job.size, int(vids.size()),
          int(m_xlTree.size()), qPrintable(m_job.fname.left(70).toUtf8().toPercentEncoding()));
    const QStringList chain = xlChain(m_xlBest);
    xlPage([this, chain](bool ok, const QString &note) {
        if (!ok) {
            fail(note);
            return;
        }
        // Always start from the share root: the tab may still be sitting in a folder from the
        // last job, and the chain below is counted from the root.
        m_xlEntered.clear();
        xlNav(m_job.share, [this, chain] { xlFile(0, chain); });
    }, 0);
}

QStringList Netdisk::xlChain(const QJsonObject &file) const
{
    QHash<QString, QJsonObject> byId;
    for (const QJsonValue &v : m_xlTree) {
        const QJsonObject f = v.toObject();
        byId.insert(f.value(QStringLiteral("id")).toString(), f);
    }
    QStringList out;
    for (QJsonObject f = file; !f.isEmpty(); ) {
        out.prepend(f.value(QStringLiteral("name")).toString());
        const QString p = f.value(QStringLiteral("parent_id")).toString();
        f = byId.value(p);   // the share's own root is not an entry, which ends the walk
    }
    return out;
}

void Netdisk::xlFile(int i, const QStringList &chain)
{
    if (i < chain.size() - 1) {
        xlOpen(chain[i], [this, i, chain](bool opened) {
            if (!opened) {
                fail(QStringLiteral("迅雷那一页没打开「%1」").arg(chain[i].left(28)));
                return;
            }
            // Measured on his share: the folder's own listing arrives ~4 s after the double
            // click and the row under it cannot be clicked before it is in the document.
            QTimer::singleShot(6000, this, [this, i, chain] { xlFile(i + 1, chain); });
        });
        return;
    }
    // The last name is the file. Double-clicking it opens the page's own player, and the
    // player's `/share/file_info` call is the only thing in this chain that answers with an
    // address - a fetch we start from the same console gets 400 "device_id is empty", because
    // 迅雷 signs inside its own http client.
    xlOpen(chain.isEmpty() ? QString() : chain.last(), [this](bool opened) {
        if (!opened) {
            fail(QStringLiteral("迅雷那一页没把片子打开"));
            return;
        }
        xlBody(QStringLiteral("/drive/v1/share/file_info"), [this](const QJsonObject &o) {
            if (o.isEmpty()) {
                fail(QStringLiteral("迅雷的播放地址没等到（那一页没发 file_info）"));
                return;
            }
            xlHandOff(o);
        });
    });
}

void Netdisk::xlHandOff(const QJsonObject &info)
{
    if (info.contains(QStringLiteral("error"))) {
        fail(xlNote(info));
        return;
    }
    const QJsonArray medias = info.value(QStringLiteral("file_info")).toObject()
                                  .value(QStringLiteral("medias")).toArray();
    QList<QJsonObject> free;
    int paid = 0;
    for (const QJsonValue &v : medias) {
        const QJsonObject m = v.toObject();
        const QString link = m.value(QStringLiteral("link")).toObject()
                               .value(QStringLiteral("url")).toString();
        if (link.isEmpty() || link.contains(QStringLiteral("pay.xunlei.com"))) {
            ++paid;
            continue;
        }
        free.append(m);
    }
    if (free.isEmpty()) {
        fail(QStringLiteral("迅雷这一发只给了要超级会员的清晰度（%1 档）").arg(paid));
        return;
    }
    // `priority` is the drive's own order: measured 3 = 720P and 4 = the 480P it defaults to,
    // so the lowest number that is free is the best picture this account is allowed.
    std::sort(free.begin(), free.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("priority")).toInt()
             < b.value(QStringLiteral("priority")).toInt();
    });
    const QJsonObject m = free.first();
    const QString url = m.value(QStringLiteral("link")).toObject()
                            .value(QStringLiteral("url")).toString();
    if (url.isEmpty()) {
        fail(QStringLiteral("迅雷回的清晰度里没有地址"));
        return;
    }
    qInfo("nd xl handoff medias=%d free=%d paid=%d res=%s origin_bytes=%lld ms=%lld",
          int(medias.size()), int(free.size()), paid,
          qPrintable(m.value(QStringLiteral("resolution_name")).toString()), m_job.size,
          m_bdClock.isValid() ? qint64(m_bdClock.elapsed()) : -1LL);
    setStage(Idle);
    // No headers, and that is measured rather than assumed: the address answers 206 with a
    // correct Content-Range to a request that carries nothing but `Range: bytes=0-31`. So this
    // is the same hand-off 百度 and 夸克 make and the row gets 下载列表, 并发数, the resume and
    // its play mark for free.
    //
    // The extension comes from what the drive actually handed, not from a guess: a rendition
    // with no `resolution_name` is the original file (measured: the first 32 bytes are
    // `1a 45 df a3`, Matroska, and the size is the share's own 17.9 GiB), while the transcoded
    // ones begin `47 40 11 10` - MPEG-TS - whatever the share called the original. Naming a
    // stream after the mkv, or an mkv after the stream, is the same lie twice this week.
    const bool transcode = !m.value(QStringLiteral("resolution_name")).toString().isEmpty()
        || m.value(QStringLiteral("category")).toString().contains(QStringLiteral("transcode"));
    const QString suffix = transcode ? QStringLiteral("ts")
                                     : QFileInfo(m_job.fname).suffix();
    const QString film = m_job.name.isEmpty() ? QFileInfo(m_job.fname).completeBaseName()
                                              : m_job.name;
    emit ready(m_job.share, url, QString(),
               suffix.isEmpty() ? film : QStringLiteral("%1.%2").arg(film, suffix),
               m_job.poster);
    runNext();
}

void Netdisk::onEvent(const QJsonObject &o)
{
    const QString method = o.value(QStringLiteral("method")).toString();
    const QJsonObject p = o.value(QStringLiteral("params")).toObject();
    const QString rid = p.value(QStringLiteral("requestId")).toString();
    if (rid.isEmpty())
        return;
    if (method == QStringLiteral("Network.requestWillBeSent")) {
        const QString u = p.value(QStringLiteral("request")).toObject()
                            .value(QStringLiteral("url")).toString();
        if (u.contains(QStringLiteral("api-pan.xunlei.com")))
            m_xlUrlByRid.insert(rid, u);
        return;
    }
    if (method == QStringLiteral("Network.responseReceived")) {
        const QString u = m_xlUrlByRid.value(rid);
        if (u.isEmpty())
            return;
        // Only 200s are worth a body read: the preflight duplicates answer 204 and Chrome has
        // nothing kept for them (`No resource with given identifier found`).
        if (p.value(QStringLiteral("response")).toObject()
                .value(QStringLiteral("status")).toInt() == 200)
            m_xlDone.append(qMakePair(rid, u));
    }
}

void Netdisk::xlBody(const QString &pathPart, std::function<void(const QJsonObject &)> then,
                     int polls)
{
    for (int i = m_xlDone.size() - 1; i >= 0; --i) {
        const QString rid = m_xlDone.at(i).first;
        const QString u = m_xlDone.at(i).second;
        if (!u.contains(pathPart) || m_xlRead.contains(rid))
            continue;
        m_xlRead.insert(rid);
        send(QStringLiteral("Network.getResponseBody"),
             { { QStringLiteral("requestId"), rid } },
             [this, pathPart, then, polls](const QJsonObject &o) {
                 const QJsonObject r = o.value(QStringLiteral("result")).toObject();
                 const QString raw = r.value(QStringLiteral("body")).toString();
                 if (raw.isEmpty()) {
                     // The body may already be gone from the tab's cache. Keep looking.
                     xlBody(pathPart, then, polls);
                     return;
                 }
                 const QByteArray bytes = r.value(QStringLiteral("base64Encoded")).toBool()
                     ? QByteArray::fromBase64(raw.toUtf8()) : raw.toUtf8();
                 then(QJsonDocument::fromJson(bytes).object());
             });
        return;
    }
    if (polls > 44) {   // 22 s of waiting, which is longer than the page's own player takes
        then(QJsonObject());
        return;
    }
    if (m_stage != Idle)
        m_deadline->start(90000);   // a poll is not a drive that stopped answering
    QTimer::singleShot(500, this, [this, pathPart, then, polls] {
        xlBody(pathPart, then, polls + 1);
    });
}

void Netdisk::xlOpen(const QString &name, std::function<void(bool)> done)
{
    if (name.isEmpty()) {
        done(false);
        return;
    }
    // The leaf text element is not the row: 迅雷's list binds its handlers on an ancestor, and
    // a synthetic click on the name alone selects without opening. Measured with both.
    const QString expr = QStringLiteral(
        "(() => { const want=%1;"
        "const els=[...document.querySelectorAll('*')].filter(e=>!e.children.length"
        "&&(e.innerText||'').trim().startsWith(want)&&e.offsetWidth>0);"
        "if(!els.length)return 'not found';"
        "let row=els[0];for(let i=0;i<6&&row.parentElement;i++){row=row.parentElement;"
        "if(/row|item|cell|file|list/i.test(String(row.className)))break;}"
        "for(const t of ['click','dblclick'])row.dispatchEvent(new MouseEvent(t,"
        "{bubbles:true,cancelable:true,view:window}));"
        "return 'sent '+row.tagName+'.'+String(row.className).slice(0,40);})()")
                             .arg(jsVal(QJsonValue(name)));
    eval(expr, [name, done](bool ok, const QJsonValue &v) {
        const QString s = ok ? v.toString() : QString();
        qInfo("nd xl open %s -> %s",
              qPrintable(name.left(24).toUtf8().toPercentEncoding()),
              qPrintable(s.left(60).toUtf8().toPercentEncoding()));
        done(ok && s.startsWith(QStringLiteral("sent")));
    });
}

void Netdisk::xlListen()
{
    m_xlListening = true;
    send(QStringLiteral("Page.enable"), QJsonObject{}, [](const QJsonObject &) {});
    send(QStringLiteral("Runtime.enable"), QJsonObject{}, [](const QJsonObject &) {});
    send(QStringLiteral("Network.enable"), QJsonObject{}, [](const QJsonObject &) {});
}
void Netdisk::xlNav(const QString &url, std::function<void()> done)
{
    // The share page is a nuxt app whose own `?path=` is client state: measured, a cold
    // navigate to `?path=/<folder>/<folder>` comes back with the query rewritten to just
    // `?pass_code=` and the list on the root. So every phase starts at the root and walks by
    // double-clicking, and m_xlEntered only saves the repeat of the very same phase.
    if (m_xlEntered == url) {
        done();
        return;
    }
    m_xlEntered = url;
    send(QStringLiteral("Page.navigate"), { { QStringLiteral("url"), url } },
         [](const QJsonObject &) {});
    QTimer::singleShot(9000, this, [this, done] {
        if (m_stage != Idle)
            m_deadline->start(90000);
        done();
    });
}

void Netdisk::xlPage(std::function<void(bool, const QString &)> done, int tries)
{
    // The same shape as bdPage, including the reason for it: a flag that claims "we hold a tab"
    // without re-checking that the tab is still in the browser's list and the socket still
    // connected sits until the deadline and blames the drive for a silence the browser never
    // caused.
    const int port = PanLogin::debugPort();
    QNetworkReply *rep = m_nam->get(QNetworkRequest(QUrl(
        QStringLiteral("http://127.0.0.1:%1/json/list").arg(port))));
    connect(rep, &QNetworkReply::finished, this, [this, rep, port, done, tries] {
        const QByteArray body = rep->readAll();
        const bool up = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        if (!up) {
            if (tries == 0) {
                const QString exe = PanLogin::browserExecutable();
                if (exe.isEmpty()) {
                    done(false, QStringLiteral("这台机器上没有 Chrome 或 Edge，迅雷那一页起不来"));
                    return;
                }
                // Offscreen: this ladder needs a page that behaves like a browser, not a
                // window he is asked to watch. 设置-网盘登录 is the one place that opens it
                // visibly, and that is PanLogin::login's own call.
                QProcess::startDetached(exe, PanLogin::browserArgs(
                    QStringLiteral("https://pan.xunlei.com/"), false));
                qInfo("nd xl browser launched");
            }
            if (tries > 9) {
                done(false, QStringLiteral("等不到迅雷的标签页起来"));
                return;
            }
            QTimer::singleShot(1500, this, [this, done, tries] { xlPage(done, tries + 1); });
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(body).array();
        QJsonObject pick;
        for (const QJsonValue &v : arr) {
            const QJsonObject t = v.toObject();
            if (t.value(QStringLiteral("type")).toString() == QLatin1String("page")
                && t.value(QStringLiteral("url")).toString()
                       .contains(QStringLiteral("pan.xunlei.com"))) {
                pick = t;
                break;
            }
        }
        if (pick.isEmpty()) {
            m_xlPageReady = false;
            // /json/new ignores its url argument on this Chrome, so the tab comes up blank and
            // gets navigated by whoever asked for it.
            QNetworkReply *n = m_nam->put(
                QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/new").arg(port))),
                QByteArray());
            connect(n, &QNetworkReply::finished, this, [this, n, done] {
                const QJsonObject t = QJsonDocument::fromJson(n->readAll()).object();
                const QString ws = t.value(QStringLiteral("webSocketDebuggerUrl")).toString();
                m_xlTabId = t.value(QStringLiteral("id")).toString();
                n->deleteLater();
                if (ws.isEmpty()) {
                    done(false, QStringLiteral("开不出迅雷的标签页"));
                    return;
                }
                attach({ { QStringLiteral("webSocketDebuggerUrl"), ws } });
                // attach() only opens the socket; a command sent before it is up is dropped.
                QTimer::singleShot(700, this, [this, done] {
                    xlListen();
                    m_xlPageReady = true;
                    done(true, QString());
                });
            });
            return;
        }
        const QString id = pick.value(QStringLiteral("id")).toString();
        if (m_xlPageReady && id == m_xlTabId && m_xlListening && m_ws
            && m_ws->state() == QAbstractSocket::ConnectedState) {
            done(true, QString());
            return;
        }
        m_xlPageReady = false;
        m_xlTabId = id;
        attach(pick);
        QTimer::singleShot(700, this, [this, done] {
            xlListen();
            m_xlPageReady = true;
            done(true, QString());
        });
    });
}

void Netdisk::xlPress(int x, int y)
{
    for (const char *t : { "mousePressed", "mouseReleased" })
        send(QStringLiteral("Input.dispatchMouseEvent"),
             { { QStringLiteral("type"), QString::fromLatin1(t) },
               { QStringLiteral("x"), x }, { QStringLiteral("y"), y },
               { QStringLiteral("button"), QStringLiteral("left") },
               { QStringLiteral("clickCount"), 1 },
               { QStringLiteral("buttons"),
                 QByteArray(t) == QByteArray("mousePressed") ? 1 : 0 } },
             [](const QJsonObject &) {});
}

void Netdisk::xlTypeCode(const QString &code,
                         std::function<void(bool, const QJsonObject &)> then, bool typedOnce)
{
    // Rebuilt from the share id rather than hung onto `m_job.share`: the hop page's url already
    // carries the code, and appending a second `pass_code=` was measured to leave the tab on
    // 「网络异常，请稍后重试」with no code box at all.
    const QString url = QStringLiteral("https://pan.xunlei.com/s/%1?pass_code=%2")
                            .arg(m_job.surl, code);
    const auto answer = [this, code, then, typedOnce](const QJsonObject &o) {
        if (o.value(QStringLiteral("share_status")).toString() == QLatin1String("OK")) {
            qInfo("nd xl code accepted by the url=%d", int(o.size()));
            then(true, o);
            return;
        }
        if (typedOnce || !m_xlPageReady) {
            then(false, o);
            return;
        }
        // The address carried the code and the page still will not list it, so type it the way
        // he would: focus the box with a real click, put the text in through the browser, and
        // press the page's own 确定 with a real click at its own rectangle. Measured on his
        // encrypted share, this is what turns PASS_CODE_EMPTY into share_status=OK with an
        // 88-character pass_code_token to walk the tree on.
        eval(QStringLiteral("(() => {const i=document.querySelector('input');"
                            "if(!i)return '';i.focus();const b=i.getBoundingClientRect();"
                            "const btn=[...document.querySelectorAll('button')].find(x=>"
                            "x.innerText.trim()==='确定');const r=btn?btn.getBoundingClientRect()"
                            ":null;return JSON.stringify([Math.round(b.left+b.width/2),"
                            "Math.round(b.top+b.height/2),r?Math.round(r.left+r.width/2):0,"
                            "Math.round(r.top+r.height/2)]);})()"),
             [this, code, then](bool ok, const QJsonValue &v) {
                 const QJsonArray xy = QJsonDocument::fromJson(v.toString().toUtf8()).array();
                 if (!ok || xy.size() < 4) {
                     qInfo("nd xl code box not found");
                     then(false, QJsonObject());
                     return;
                 }
                 const int ix = xy.at(0).toInt(), iy = xy.at(1).toInt();
                 const int bx = xy.at(2).toInt(), by = xy.at(3).toInt();
                 xlPress(ix, iy);
                 send(QStringLiteral("Input.insertText"),
                      { { QStringLiteral("text"), code } }, [](const QJsonObject &) {});
                 QTimer::singleShot(400, this, [this, bx, by, then] {
                     xlPress(bx, by);
                     QTimer::singleShot(9000, this, [this, then] {
                         xlBody(QStringLiteral("/drive/v1/share?"), [then](const QJsonObject &o) {
                             then(o.value(QStringLiteral("share_status")).toString()
                                      == QLatin1String("OK"), o);
                         });
                     });
                 });
             });
    };
    xlPage([this, url, answer](bool ok, const QString &) {
        if (!ok) {
            answer(QJsonObject());
            return;
        }
        m_xlEntered.clear();   // the code ride is a fresh visit even if this tab saw the root
        xlNav(url, [this, answer] {
            // `answer` is the whole decision: OK means the url carried it, anything else means
            // type it in and read the page's next answer.
            xlBody(QStringLiteral("/drive/v1/share?"), answer);
        });
    }, 0);
}
