#include "netdisk.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
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

// The ports worth asking for a DevTools endpoint. 9333 is the one the client listens on by
// itself - measured on a 夸克 started normally, with no debugging flag at all - and 9222 is
// the one we ask for when we have to start the client ourselves. Which one answers is not
// knowable from here, so both are asked.
const QList<int> kPorts{ 9333, 9222 };

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
// answer to a real request; this one means "ask him to sign in", so the row brings
// the login page up rather than reporting a failure.
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
        fail(QStringLiteral("夸克没有回应，卡在%1").arg(stageText()));
    });
    loadCopies();
}

QString Netdisk::clientExecutable()
{
    static QString found;
    static bool looked = false;
    if (looked)
        return found;
    looked = true;
    static const QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
    // The scheme handler is the entry that actually carries a command line here: the
    // StartMenuInternet registration exists but its command is empty, which is why the
    // lookup in online.cpp silently fell back to the default browser.
    static const QStringList keys{
        QStringLiteral("HKCU\\Software\\Classes\\qkclouddrive\\shell\\open\\command"),
        QStringLiteral("HKCU\\Software\\Classes\\qklink\\shell\\open\\command"),
    };
    for (const QString &key : keys) {
        const QString cmd = QSettings(key, QSettings::NativeFormat).value(QString()).toString();
        const auto m = quoted.match(cmd);
        const QString exe = m.hasMatch() ? m.captured(1) : cmd.section(QLatin1Char(' '), 0, 0);
        if (!exe.isEmpty() && QFileInfo::exists(exe)) {
            found = exe;
            return found;
        }
    }
    static const QStringList roots{
        QStringLiteral("HKCU\\Software\\Clients\\StartMenuInternet"),
        QStringLiteral("HKLM\\Software\\Clients\\StartMenuInternet"),
    };
    for (const QString &root : roots) {
        const QSettings reg(root, QSettings::NativeFormat);
        QStringList names = reg.childGroups();
        names += reg.childKeys();
        for (const QString &name : names) {
            if (!name.contains(QStringLiteral("quark"), Qt::CaseInsensitive))
                continue;
            const QString cmd = QSettings(root + QLatin1Char('/') + name
                                          + QStringLiteral("/shell/open/command"),
                                          QSettings::NativeFormat)
                                    .value(QStringLiteral(".")).toString();
            const auto m = quoted.match(cmd);
            const QString exe = m.hasMatch() ? m.captured(1) : cmd.section(QLatin1Char(' '), 0, 0);
            if (!exe.isEmpty() && QFileInfo::exists(exe)) {
                found = exe;
                return found;
            }
        }
    }
    return found;
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
    switch (m_stage) {
    case Connecting: return QStringLiteral("连夸克");
    case Laddering:  return QStringLiteral("问网盘");
    default:         return QStringLiteral("空闲");
    }
}

void Netdisk::download(const QString &shareUrl, const QString &name, const QString &poster)
{
    static const QRegularExpression pwd(QStringLiteral("/s/([A-Za-z0-9]+)"));
    const auto m = pwd.match(shareUrl);
    if (!m.hasMatch()) {
        emit failed(shareUrl, QStringLiteral("这不是夸克的分享地址"));
        return;
    }
    Job j;
    j.share = shareUrl;
    j.name = name;
    j.poster = poster;
    j.pwdId = m.captured(1);
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
    ensurePage([this](bool ok, const QString &note) {
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
    if (m_ws) {
        m_ws->deleteLater();
        m_ws = nullptr;
    }
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) {
        const QJsonObject o = QJsonDocument::fromJson(msg.toUtf8()).object();
        if (!o.contains(QStringLiteral("id")))
            return;   // an event; nothing here subscribes to any
        const int id = o.value(QStringLiteral("id")).toInt();
        const auto it = m_rpc.find(id);
        if (it == m_rpc.end())
            return;
        const std::function<void(const QJsonObject &)> fn = it.value();
        m_rpc.erase(it);
        // Any answer at all is the client being alive, so the watchdog is about silence
        // rather than about how long the whole ladder is allowed to take. Without this the
        // 60 s spent waiting for a big copy to materialise is read as a dead client:
        // measured, a job died on exactly that line while 网盘肯给下载地址 was still retrying.
        if (m_stage != Idle)
            m_deadline->start(60000);
        fn(o);
    });
    // QWebSocket takes the application proxy unless told otherwise, and this socket is a
    // loopback connection to the client on this machine.
    m_ws->setProxy(QNetworkProxy::NoProxy);
    connect(m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
        qInfo("nd ws error=%d %s", int(e), qPrintable(m_ws ? m_ws->errorString() : QString()));
    });
    connect(m_ws, &QWebSocket::connected, this, [this] { qInfo("nd ws connected"); });
    m_ws->open(QUrl(target.value(QStringLiteral("webSocketDebuggerUrl")).toString()));
}

void Netdisk::findPort(std::function<void(int)> done, int index)
{
    if (index >= kPorts.size()) {
        done(0);
        return;
    }
    const int port = kPorts.at(index);
    QNetworkReply *rep = m_nam->get(
        QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/version").arg(port))));
    connect(rep, &QNetworkReply::finished, this, [this, rep, port, done, index] {
        const QString body = QString::fromUtf8(rep->readAll());
        const int err = int(rep->error());
        rep->deleteLater();
        qInfo("nd port %d err=%d bytes=%lld", port, err, qint64(body.size()));
        // The browser string is what says this is 夸克's own endpoint and not another
        // Chromium on the machine answering on the same port.
        if (err == int(QNetworkReply::NoError) && body.contains(QStringLiteral("QuarkPC"))) {
            m_port = port;
            done(port);
            return;
        }
        findPort(done, index + 1);
    });
}

void Netdisk::ensurePage(std::function<void(bool, const QString &)> done, bool launch)
{
    if (m_port == 0) {
        findPort([this, done, launch](int port) {
            if (port == 0) {
                if (!launch) {
                    done(false, QStringLiteral("夸克没有开着调试端口"));
                    return;
                }
                launchClient(done);
                return;
            }
            pageOnPort(port, done, launch);
        });
        return;
    }
    pageOnPort(m_port, done, launch);
}

void Netdisk::pageOnPort(int port, std::function<void(bool, const QString &)> done, bool launch)
{
    QNetworkReply *rep = m_nam->get(
        QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/list").arg(port))));
    connect(rep, &QNetworkReply::finished, this, [this, rep, port, done, launch] {
        const QString body = QString::fromUtf8(rep->readAll());
        const bool httpOk = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        const QJsonArray list = QJsonDocument::fromJson(body.toUtf8()).array();
        qInfo("nd list port=%d ok=%d n=%lld", port, int(httpOk), qint64(list.size()));

        // The drive window inside the client, not a web tab on pan.quark.cn: only the window
        // carries chrome.quarkDownloadsPrivate and chrome.downloads, which is where the
        // handoff and the progress poll are read from. Measured on a running client, that
        // window is uccd://cloud.quark/clouddrive/renderer/index.html. A pan.quark.cn tab is
        // taken only when the window is not open, because the ladder itself does run there.
        QJsonObject best;
        for (const QJsonValue &v : list) {
            const QJsonObject t = v.toObject();
            if (t.value(QStringLiteral("type")).toString() != QLatin1String("page"))
                continue;
            const QString url = t.value(QStringLiteral("url")).toString();
            if (url.contains(QStringLiteral("clouddrive/renderer/index.html"))) {
                best = t;
                break;
            }
            if (url.contains(QStringLiteral("pan.quark.cn")) && best.isEmpty())
                best = t;
        }
        if (!best.isEmpty()) {
            qInfo("nd attach %s", qPrintable(best.value(QStringLiteral("url")).toString().left(64)));
            attach(best);
            clientVersion();
            // The socket handshake is asynchronous; the first call is made after it is
            // up rather than threading a callback through QWebSocket::connected, which
            // would have to be re-armed on every reconnect.
            QTimer::singleShot(500, this, [done] { done(true, QString()); });
            return;
        }

        if (!httpOk || list.isEmpty()) {
            m_port = 0;   // it answered a moment ago and not now: look again next time
            if (!launch) {
                done(false, QStringLiteral("夸克的调试端口关了"));
                return;
            }
            launchClient(done);
            return;
        }

        if (!launch) {
            done(false, QStringLiteral("夸克开着，但没有网盘页面的标签"));
            return;
        }
        // Ask the running client for a tab and navigate it. /json/new's own url argument is
        // ignored by this build - measured, it opens about:blank - so the navigation has to
        // be a second call over that tab's socket.
        QNetworkReply *n = m_nam->put(
            QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/new").arg(port))),
            QByteArray());
        connect(n, &QNetworkReply::finished, this, [this, n, done] {
            const QJsonObject t = QJsonDocument::fromJson(n->readAll()).object();
            n->deleteLater();
            if (t.value(QStringLiteral("webSocketDebuggerUrl")).toString().isEmpty()) {
                done(false, QStringLiteral("开不出夸克的标签页"));
                return;
            }
            attach(t);
            send(QStringLiteral("Page.enable"), QJsonObject{}, [this, done](const QJsonObject &) {
                send(QStringLiteral("Page.navigate"),
                     { { QStringLiteral("url"),
                         QStringLiteral("https://pan.quark.cn/clouddrive/list") } },
                     [this, done](const QJsonObject &) {
                         QTimer::singleShot(9000, this, [this, done] {
                             ensurePage([done](bool ok, const QString &note) { done(ok, note); },
                                        false);
                         });
                     });
            });
        });
    });
}

void Netdisk::launchClient(std::function<void(bool, const QString &)> done)
{
    const QString exe = clientExecutable();
    if (exe.isEmpty()) {
        done(false, QStringLiteral("这台机器上没有装夸克客户端"));
        return;
    }
    qInfo("nd launch %s", qPrintable(exe));
    // His own profile, so a session he signed in once is reused instead of asked
    // for again. The drive page is its first tab.
    QProcess::startDetached(exe, { QStringLiteral("--remote-debugging-port=%1").arg(kPorts.last()),
                                   QStringLiteral("https://pan.quark.cn/clouddrive/list") });
    m_port = 0;
    QTimer::singleShot(7000, this, [this, done] { ensurePage(done, false); });
}

void Netdisk::clientVersion()
{
    QNetworkReply *rep = m_nam->get(
        QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/version").arg(m_port))));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString ua = QJsonDocument::fromJson(rep->readAll()).object()
                              .value(QStringLiteral("User-Agent")).toString();
        rep->deleteLater();
        static const QRegularExpression re(QStringLiteral("QuarkPC/([0-9.]+)"));
        const auto m = re.match(ua);
        if (m.hasMatch())
            m_ve = m.captured(1);
    });
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
                m_askLogin = true;
                fail(QStringLiteral("夸克里还没登录"));
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
                if (sessionMissing(r))
                    m_askLogin = true;
                fail(sessionMissing(r) ? QStringLiteral("夸克里还没登录")
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
    api(QStringLiteral("POST"),
        QStringLiteral("https://drive-pc.quark.cn/1/clouddrive/file/download"
                       "?pr=ucpro&fr=pc&sys=win32&ve=%1").arg(m_ve),
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
    // The jar, not a captured request: taking whichever request the network domain reports
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
        // The 夸克 UA goes along with it: LinkSwift sets one for its 增强下载 too, and it is
        // what the drive window sends. Measured it changes no rate, but it costs nothing and
        // it keeps the request looking like the one we just proved works.
        const QString headers = QStringLiteral("Cookie: %1\r\nUser-Agent: %2")
                                    .arg(pairs.join(QStringLiteral("; ")),
                                         QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                                                        "Chrome/144.0.0.0 Safari/537.36 QuarkPC/%1")
                                             .arg(m_ve));
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
    // after: ask for the drive window again, but do not start the client for it - if he has
    // closed 夸克, the copy waits for the next time it is open.
    ensurePage([this, fid, size, after](bool ok, const QString &) {
        if (!ok) {
            qInfo("nd copy waits: no client");
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
        eval(expr, [after](bool, const QJsonValue &) {
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
    if (m_askLogin) {
        m_askLogin = false;
        openLogin();
    }
    if (!fid.isEmpty()) {
        // The copy is ours and it is the thing that costs him quota: take it back even when
        // the job died before any byte was pulled.
        qInfo("nd fail with copy: %s", qPrintable(note));
        deleteCopy(fid, size);
    }
    runNext();
}

// ------------------------------------------------------------------ session

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

void Netdisk::checkSession()
{
    if (m_stage != Idle)
        return;
    setStage(Connecting);
    m_deadline->start(90000);
    ensurePage([this](bool ok, const QString &note) {
        if (!ok) {
            setStage(Idle);
            emit sessionReady(false, false, note);
            return;
        }
        api(QStringLiteral("GET"),
            QStringLiteral("https://pan.quark.cn/1/clouddrive/member?pr=ucpro&fr=pc"),
            QJsonValue{},
            [this](bool, const QJsonObject &r) {
                const QString type = r.value(QStringLiteral("data")).toObject()
                                        .value(QStringLiteral("member_type")).toString();
                const bool in = r.value(QStringLiteral("code")).toInt() == 0;
                setStage(Idle);
                emit sessionReady(in, type.contains(QStringLiteral("VIP")),
                                  in ? type : QStringLiteral("还没登录"));
            });
    });
}

void Netdisk::openLogin()
{
    if (m_stage != Idle)
        return;
    setStage(Connecting);
    m_deadline->start(90000);
    ensurePage([this](bool ok, const QString &note) {
        if (!ok) {
            setStage(Idle);
            emit sessionReady(false, false, note);
            return;
        }
        send(QStringLiteral("Page.navigate"),
             { { QStringLiteral("url"), QStringLiteral("https://pan.quark.cn/") } },
             [this](const QJsonObject &) {
                 setStage(Idle);
                 emit sessionReady(false, false, QStringLiteral("已经在夸克里打开登录页，登进去再点一次"));
             });
    });
}
