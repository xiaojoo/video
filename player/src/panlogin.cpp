#include "panlogin.h"

#include <QCoreApplication>
#include <QDateTime>
#include <initializer_list>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace {

// Our own port. 9333/9222 belong to 夸克, which starts a debug port by itself; this one is
// ours to open, and keeping it apart means the two never fight over a profile.
const int kPort = 9455;

struct Provider
{
    const char *name;
    const char *loginUrl;
    const char *domain;
    // The cookies that mean "signed in", measured rather than guessed: after he logged in
    // the domain carried XLA_CI, deviceid, xl_fp_rt (anonymous) plus sessionid, userid and
    // usernewno. A name merely looking token-ish is not enough - the first pass here
    // accepted a localStorage key called `oauth_sign_in_...` and wrote a jar with no
    // session in it, overwriting the good record taken a minute earlier.
    // The host whose requests carry the signature we have to copy.
    const char *apiHost;
    std::initializer_list<const char *> need;
    // Some drives have no cookie I can name yet, because the one that marks a session is not
    // distinguishable from the anonymous ones until he has actually signed in. For those the
    // marker is an answer: this url, asked from inside the page, that says `code=0` only when
    // the session is there. 夸克 measured tonight: anonymous it answers
    // `401 {"code":31001,"message":"require login [guest]"}` - its own words, from its own api.
    const char *probeUrl;
};

// Three rows, because those are the three whose login marker is measured. UC stays off this
// list until its own marker is known: a row in the settings card whose `need` name is a guess
// spins until timeout, which is the dead control he has told me not to ship.
const Provider kProviders[] = {
    { "xunlei", "https://pan.xunlei.com/", "xunlei.com", "api-pan.xunlei.com",
      { "sessionid", "userid" }, nullptr },
    // 百度's web api lives on the same host as its pages, and its login cookie is BDUSS -
    // one value, and without it the share endpoints refuse to hand a dlink at all.
    { "baidu", "https://pan.baidu.com/", "baidu.com", "pan.baidu.com", { "BDUSS" }, nullptr },
    // 夸克 moved onto the web this night: the ladder used to live in his client's own
    // DevTools port, which meant no client = no 夸克. Asked from a plain Chrome tab,
    // drive.quark.cn answers the member call with the drive's own json, so the client is no
    // longer needed anywhere. Its session cookie family (`_UP_*`) is indistinguishable from
    // the anonymous ones until he signs in, so this one uses the answer, not a name.
    { "quark", "https://pan.quark.cn/", "quark.cn", "drive.quark.cn", {},
      "https://drive.quark.cn/1/clouddrive/member?pr=ucpro&fr=pc" },
};

// What he calls the drive, in the line that says it did not answer. Naming 迅雷 in a 夸克
// timeout is how a wrong provider reads like a wrong machine.
QString driveName(const QString &name)
{
    if (name == QLatin1String("quark"))
        return QStringLiteral("夸克");
    if (name == QLatin1String("baidu"))
        return QStringLiteral("百度");
    return QStringLiteral("迅雷");
}

const Provider *providerFor(const QString &name)
{
    for (const Provider &p : kProviders)
        if (name == QString::fromLatin1(p.name))
            return &p;
    return nullptr;
}

QString unquote(const QString &path)
{
    static const QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
    const auto m = quoted.match(path);
    return m.hasMatch() ? m.captured(1) : path;
}

}

PanLogin::PanLogin(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    // Everything here is a loopback call to the browser on this machine; a corporate or
    // system proxy would turn that into a failure that looks like the browser being down.
    m_nam->setProxy(QNetworkProxy::NoProxy);
}

QString PanLogin::browserExecutable()
{
    static QString found;
    static bool looked = false;
    if (looked)
        return found;
    looked = true;
    // The registry first. Both browsers install per machine or per user depending on who
    // ran the installer, so the two standard paths are a guess and the key is not.
    static const QStringList keys{
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion"
                       "\\App Paths\\chrome.exe"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion"
                       "\\App Paths\\chrome.exe"),
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion"
                       "\\App Paths\\msedge.exe"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion"
                       "\\App Paths\\msedge.exe")
    };
    for (const QString &k : keys) {
        // QSettings calls the default value "."; reading QString() comes back empty.
        const QString exe = unquote(QSettings(k, QSettings::NativeFormat)
                                        .value(QStringLiteral(".")).toString());
        if (!exe.isEmpty() && QFileInfo::exists(exe)) {
            found = exe;
            return found;
        }
    }
    static const QStringList fallback{
        u"C:/Program Files/Google/Chrome/Application/chrome.exe"_qs,
        u"C:/Program Files (x86)/Google/Chrome/Application/chrome.exe"_qs,
        u"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"_qs,
        u"C:/Program Files/Microsoft/Edge/Application/msedge.exe"_qs
    };
    for (const QString &f : fallback)
        if (QFileInfo::exists(f)) {
            found = f;
            return found;
        }
    return found;
}

int PanLogin::debugPort()
{
    return kPort;
}

QString PanLogin::profileRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("panlogin"));
}

QStringList PanLogin::browserArgs(const QString &startUrl, bool visible)
{
    QStringList a { QStringLiteral("--remote-debugging-port=%1").arg(kPort),
                    QStringLiteral("--user-data-dir=%1").arg(profileRoot()),
                    QStringLiteral("--no-first-run"),
                    QStringLiteral("--no-default-browser-check"),
                    QStringLiteral("--disable-sync") };
    // Off the desktop rather than headless: `--headless=new` is a different client and the
    // whole reason this tab exists is that the edge answers a browser and not a script.
    // Measured with the real profile, an offscreen window still gets 74 cookies, errno=0 on
    // share/list and 962 segments on /share/streaming.
    if (!visible)
        a << QStringLiteral("--window-position=-32000,-32000")
          << QStringLiteral("--window-size=1000,700");
    // A normal tab, not `--app`: app mode hides the address bar, and a window that
    // asks for a password without showing whose page it is, is not one he can verify.
    a << startUrl;
    return a;
}

QString PanLogin::profileDir() const
{
    return profileRoot();
}

QString PanLogin::sessionPath(const QString &provider) const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("pan-session-%1.json").arg(provider));
}

void PanLogin::setBusy(bool on)
{
    // The flag is assigned here and nowhere else. Comparing `busy()` was a self-comparison:
    // startPoll had already set m_waiting, so the guard always matched and busyChanged was
    // never emitted - the card's line moved to 正在等你在浏览器里登录 while the button next to
    // it stayed on 去登录 for the whole ten minutes.
    if (m_waiting == on)
        return;
    m_waiting = on;
    emit busyChanged();
}

void PanLogin::login(const QString &provider)
{
    const Provider *p = providerFor(provider);
    if (!p)
        return;
    const QString exe = browserExecutable();
    m_provider = provider;
    if (exe.isEmpty()) {
        m_note = tr("这台机器上没有 Chrome 或 Edge，打不开登录页");
        emit stateChanged();
        return;
    }
    QDir().mkpath(profileDir());
    // A separate --user-data-dir is what makes this possible at all: a current Chrome
    // refuses --remote-debugging-port on the profile it normally browses with.
    //
    // What a click has to do when the browser is already up was measured tonight, and the
    // old answer was wrong twice over. Launching Chrome again with a url adds ANOTHER tab -
    // the clicks here left five identical pan.quark.cn tabs on the screen - and launching it
    // with no url still opens a blank one (measured: pages 1 -> 2, quark tabs unchanged).
    // Worse, `QProcess::start` on the instance this same app already started is refused, so
    // a second drive's 去登录 did nothing at all: no tab, no window, no page for the
    // poll to attach to. That is the 「点击没有效果」 it was reported as.
    // So: ask the port first, and wake Chrome only when this drive has no page to show him.
    QNetworkReply *rep = m_nam->get(QNetworkRequest(QUrl(
        QStringLiteral("http://127.0.0.1:%1/json/list").arg(kPort))));
    connect(rep, &QNetworkReply::finished, this, [this, rep, exe, p, provider] {
        const QByteArray body = rep->readAll();
        const bool portUp = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        const QJsonArray list = QJsonDocument::fromJson(body).array();
        bool hasTab = false;
        for (const QJsonValue &v : list) {
            const QJsonObject t = v.toObject();
            if (t.value(QStringLiteral("type")).toString() == QLatin1String("page")
                && t.value(QStringLiteral("url")).toString()
                       .contains(QString::fromLatin1(p->domain))) {
                hasTab = true;
                break;
            }
        }
        if (hasTab) {
            // No process at all. The first attach raises that tab's window; a click that
            // made a fifth tab was never the wake-up call it looked like it should be.
            qInfo("pl wake provider=%s reusedTab=1 targets=%lld", qPrintable(provider),
                  qint64(list.size()));
            return;
        }
        // startDetached, not start: a detached Chrome hands the url to whatever instance
        // already runs on this profile and exits, so the second drive of one session gets
        // its tab exactly like the first one did.
        QProcess::startDetached(exe, browserArgs(QString::fromLatin1(p->loginUrl)));
        qInfo("pl launch provider=%s exe=%s portUp=%d", qPrintable(provider),
              qPrintable(QFileInfo(exe).fileName()), int(portUp));
    });

    m_before.clear();
    m_baselined = false;
    m_done = false;
    m_apiHeaders = QJsonObject();
    m_savedFromTraffic = false;
    m_reloaded = false;
    m_raised = false;
    m_jarNames.clear();
    m_reqUrl.clear();
    m_reqHdr.clear();
    m_ticks = 0;
    m_note.clear();
    // After the wait is actually running: the card's line reads `busy`, and emitting first
    // would have it redraw as "未登录" for the two seconds before the poll starts.
    startPoll();
    emit stateChanged();
}

void PanLogin::cancel()
{
    stopPoll();
    m_note.clear();
    emit stateChanged();
}

bool PanLogin::hasSession(const QString &provider) const
{
    return QFileInfo::exists(sessionPath(provider));
}

void PanLogin::logout(const QString &provider)
{
    // Both files hold his session: the jar with the signed headers, and 百度's bdstoken pair.
    // Counts only in the log - a name is safe, a value never is.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    int gone = 0;
    for (const QString &n : { QStringLiteral("pan-session-%1.json").arg(provider),
                              QStringLiteral("pan-token-%1.json").arg(provider) }) {
        const QString p = QDir(dir).filePath(n);
        if (QFile::exists(p) && QFile::remove(p))
            ++gone;
    }
    qInfo("pl logout provider=%s files removed=%d", qPrintable(provider), gone);
    emit stateChanged();
}

QString PanLogin::stateText(const QString &provider) const
{
    if (m_provider == provider && busy())
        return m_note.isEmpty() ? tr("正在等你在浏览器里登录…") : m_note;
    QFile f(sessionPath(provider));
    if (!f.open(QIODevice::ReadOnly))
        return tr("未登录");
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const int n = o.value(QStringLiteral("cookies")).toArray().size();
    return tr("已登录 · 记录于 %1 · %2 条")
        .arg(QDateTime::fromSecsSinceEpoch(
                 qint64(o.value(QStringLiteral("saved")).toDouble()))
                 .toString(QStringLiteral("MM-dd hh:mm")))
        .arg(n);
}

QByteArray PanLogin::session(const QString &provider) const
{
    QFile f(sessionPath(provider));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void PanLogin::startPoll()
{
    if (!m_poll) {
        m_poll = new QTimer(this);
        m_poll->setInterval(2000);
        connect(m_poll, &QTimer::timeout, this, &PanLogin::stepList);
    }
    setBusy(true);
    m_poll->start();
    // The first read is the baseline, and it cannot happen before the browser is up and
    // the page has loaded, so kick it once now rather than waiting out the interval.
    QTimer::singleShot(1500, this, &PanLogin::stepList);
}

void PanLogin::stopPoll()
{
    if (m_poll)
        m_poll->stop();
    setBusy(false);
    if (m_ws) {
        m_ws->close();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
}

void PanLogin::finish(const QString &note)
{
    stopPoll();
    m_note = note;
    emit stateChanged();
}

void PanLogin::stepList()
{
    if (++m_ticks > 300) {   // ten minutes of waiting is generous for a scan-code login
        finish(tr("等不到登录，%1那边没有动静").arg(driveName(m_provider)));
        return;
    }
    if (!m_provider.isEmpty() && !providerFor(m_provider))
        return;
    QNetworkReply *rep = m_nam->get(
        QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/list").arg(kPort))));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QByteArray body = rep->readAll();
        const bool up = rep->error() == QNetworkReply::NoError;
        rep->deleteLater();
        if (!up) {
            // The port answering and then stopping is the browser being closed - by him, or
            // by Chrome refusing a second instance on that profile. Saying nothing is what
            // made the first run look like a hang: the card stayed on 正在等你登录 forever.
            if (m_baselined)
                finish(tr("浏览器被关掉了，没等到登录"));
            else if (m_ticks > 40)
                finish(tr("端口 %1 一直没回应，Chrome 可能没起来").arg(kPort));
            return;
        }
        const QJsonArray list = QJsonDocument::fromJson(body).array();
        const Provider *p = providerFor(m_provider);
        QString wsUrl;
        QString pageUrl;
        for (const QJsonValue &v : list) {
            const QJsonObject t = v.toObject();
            if (t.value(QStringLiteral("type")).toString() != QStringLiteral("page"))
                continue;
            const QString url = t.value(QStringLiteral("url")).toString();
            if (p && url.contains(QString::fromLatin1(p->domain))) {
                wsUrl = t.value(QStringLiteral("webSocketDebuggerUrl")).toString();
                pageUrl = url;
                break;
            }
            if (wsUrl.isEmpty()) {
                wsUrl = t.value(QStringLiteral("webSocketDebuggerUrl")).toString();
                pageUrl = url;
            }
        }
        if (wsUrl.isEmpty())
            return;   // not up yet; the next tick asks again
        if (!m_baselined)
            // Which tab the jar is being read from, because reading the first page target
            // found is how the first run ended up on a blank tab with nothing in its jar.
            qInfo("pl list n=%d page=%s", list.size(),
                  qPrintable(pageUrl.left(60).toUtf8().toPercentEncoding()));
        m_wsUrl = wsUrl;
        readCookies();
    });
}

void PanLogin::readCookies()
{
    // The same tab as the last poll: ask the live socket instead of rebuilding it. The
    // signed-call check is made of network events, and a socket thrown away and reopened every
    // two seconds misses the whole burst a reload produces inside one gap.
    if (m_ws && m_attached == m_wsUrl
        && m_ws->state() == QAbstractSocket::ConnectedState) {
        m_cookieId = send2(QStringLiteral("Network.getAllCookies"));
        return;
    }
    if (m_ws) {
        m_ws->close();
        m_ws->deleteLater();
    }
    m_attached = m_wsUrl;
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_ws->setProxy(QNetworkProxy::NoProxy);
    m_haveStore = false;
    connect(m_ws, &QWebSocket::connected, this, [this] {
        send2(QStringLiteral("Network.enable"));
        m_cookieId = send2(QStringLiteral("Network.getAllCookies"));
        if (!m_raised) {
            // The window has to come to him. A 去登录 that records a session in a window
            // behind everything else is exactly the "clicking does nothing" he reported.
            m_raised = true;
            send2(QStringLiteral("Page.enable"));
            send2(QStringLiteral("Page.bringToFront"));
        }
    });
    connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) {
        const QJsonObject o = QJsonDocument::fromJson(msg.toUtf8()).object();
        // Watched from the first attach, not only after the login is saved: an already
        // signed-in tab is exactly the case the cookie marker cannot see, and its traffic is
        // the proof.
        watchForApiHeaders(o);
        const int id = o.value(QStringLiteral("id")).toInt();
        if (id == m_cookieId) {
            m_jar = o.value(QStringLiteral("result")).toObject()
                        .value(QStringLiteral("cookies")).toArray();
            askStore();
        } else if (id == m_evalId) {
            m_store = o.value(QStringLiteral("result")).toObject()
                          .value(QStringLiteral("result")).toObject()
                          .value(QStringLiteral("value")).toObject();
            m_haveStore = true;
            const Provider *pr = providerFor(m_provider);
            if (pr && pr->probeUrl) {
                // No cookie name can tell this drive's session from its anonymous jar yet,
                // so ask the drive instead of reading the jar.
                askProbe();
                return;
            }
            decide();
        } else if (id == m_probeId) {
            const QJsonValue v = o.value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("value"));
            const QJsonObject j = QJsonDocument::fromJson(v.toString().toUtf8()).object();
            m_probeIn = j.value(QStringLiteral("code")).toInt(-1) == 0;
            m_probeDone = true;
            // The code and its length, never the message: it carries his account's own words.
            qInfo("pl probe provider=%s code=%d signed=%d", qPrintable(m_provider),
                  j.value(QStringLiteral("code")).toInt(-1), int(m_probeIn));
            decide();
        }
    });
    connect(m_ws, &QWebSocket::errorOccurred, this, [this] {
        // Percent-encoded: the localised Windows error text came back as ten question marks
        // through the console code page, which told me nothing about why the socket died.
        qInfo("pl ws error %s",
              qPrintable(m_ws ? m_ws->errorString().toUtf8().toPercentEncoding() : QByteArray()));
    });
    m_ws->open(QUrl(m_wsUrl));
}

void PanLogin::askStore()
{
    // A single-page drive keeps its bearer token in localStorage, not in a cookie, so the
    // jar alone cannot tell me whether he is signed in. Keys and values come back together;
    // only the key names are ever logged.
    const QJsonObject params{
        { QStringLiteral("expression"), QStringLiteral(
            "({keys: Object.keys(localStorage), store: Object.fromEntries("
            "Object.entries(localStorage))})") },
        { QStringLiteral("returnByValue"), true }
    };
    const int id = ++m_rpcId;
    m_evalId = id;
    const QJsonObject req{ { QStringLiteral("id"), id },
                           { QStringLiteral("method"), QStringLiteral("Runtime.evaluate") },
                           { QStringLiteral("params"), params } };
    m_ws->sendTextMessage(QString::fromUtf8(
        QJsonDocument(req).toJson(QJsonDocument::Compact)));
}

void PanLogin::askProbe()
{
    const Provider *p = providerFor(m_provider);
    if (!p || !p->probeUrl) {
        decide();
        return;
    }
    // Asked from inside the page, with the page's own credentials. Measured on 夸克 at
    // 02:30 while nobody was signed in: `401 {"code":31001,"message":"require login [guest]"}`
    // - the drive's own words for "anonymous", and the same call answers code=0 once he has
    // scanned. That is a marker the drive maintains, not one I picked off a jar.
    const QJsonObject params{
        { QStringLiteral("expression"),
          QStringLiteral("(async()=>{const q=new XMLHttpRequest();q.open('GET','")
              + QString::fromLatin1(p->probeUrl) + QStringLiteral("',true);"
                "q.withCredentials=true;q.timeout=12000;"
                "const t=await new Promise(res=>{q.onload=()=>res(q.responseText||'');"
                "q.onerror=()=>res('');q.ontimeout=()=>res('');q.send();});"
                "let j=null;try{j=JSON.parse(t)}catch(e){}"
                "return JSON.stringify({code:j&&j.code!==undefined?j.code:-1});})()") },
        { QStringLiteral("awaitPromise"), true },
        { QStringLiteral("returnByValue"), true }
    };
    m_probeDone = false;
    m_probeId = send2(QStringLiteral("Runtime.evaluate"), params);
}

// `.xunlei.com` and `xunlei.com` are the same site. The first pass here stripped every dot
// from the cookie's domain and compared it against a pattern that still had one, so nothing
// ever matched and the wait read an empty jar forever - it logged `cookies=0` while the
// browser plainly had three.
namespace {
bool onDomain(const QString &host, const QString &domain)
{
    const QString h = host.startsWith(QLatin1Char('.')) ? host.mid(1) : host;
    return h == domain || h.endsWith(QLatin1Char('.') + domain);
}
}

void PanLogin::decide()
{
    const Provider *p = providerFor(m_provider);
    if (!p || !m_haveStore || m_done)
        return;
    const QString domain = QString::fromLatin1(p->domain);
    // Named apart from the onDomain() helper below: a local with the helper's name shadows
    // it, and the call then reads as a subscript on a QStringList.
    QStringList jarNames;
    for (const QJsonValue &v : m_jar) {
        const QJsonObject cookie = v.toObject();
        if (onDomain(cookie.value(QStringLiteral("domain")).toString(), domain))
            jarNames << cookie.value(QStringLiteral("name")).toString();
    }
    m_jarNames = jarNames;
    QStringList marks = jarNames;
    const QJsonArray keys = m_store.value(QStringLiteral("keys")).toArray();
    for (int i = 0; i < keys.size(); ++i)
        marks << keys.at(i).toString();

    QStringList missing;
    for (const char *n : p->need)
        if (!jarNames.contains(QString::fromLatin1(n)))
            missing << QString::fromLatin1(n);
    // Either a cookie pair I can name (迅雷, 百度) or the drive's own answer (夸克). The probe
    // has no opinion before it has answered, so the first polls keep waiting rather than
    // saving an anonymous jar over a good one.
    const bool signedIn = p->probeUrl ? (m_probeDone && m_probeIn) : missing.isEmpty();

    if (!m_baselined) {
        if (marks.isEmpty())
            return;   // the page has not put anything up yet; ask again next tick
        m_before = QSet<QString>(marks.cbegin(), marks.cend());
        m_baselined = true;
        qInfo("pl baseline provider=%s marks=%d names=%s", qPrintable(m_provider),
              marks.size(),
              marks.join(QLatin1Char(',')).left(300).toUtf8().toPercentEncoding().constData());
        if (signedIn) {
            qInfo("pl already signed in provider=%s", qPrintable(m_provider));
            save(jarNames);
            return;
        }
        // The jar alone cannot see a login the page keeps in memory, so give it one chance to
        // prove itself by calling its own api - which is also the only way the signed headers
        // exist before he has scanned anything.
        requestReload();
        return;
    }
    QStringList fresh;
    const QSet<QString> seen = m_before;
    for (const QString &m : marks)
        if (!seen.contains(m))
            fresh << m;
    if (!signedIn) {
        // Say what is missing rather than saving something that cannot download anything.
        if (!fresh.isEmpty())
            qInfo("pl poll provider=%s fresh=%s still missing=%s", qPrintable(m_provider),
                  fresh.join(QLatin1Char(',')).toUtf8().toPercentEncoding().constData(),
                  missing.join(QLatin1Char(',')).toUtf8().toPercentEncoding().constData());
        return;
    }
    save(jarNames);
}

void PanLogin::save(const QStringList &marks)
{
    m_done = true;
    QJsonObject out;
    out.insert(QStringLiteral("provider"), m_provider);
    out.insert(QStringLiteral("saved"), double(QDateTime::currentSecsSinceEpoch()));
    out.insert(QStringLiteral("cookies"), m_jar);
    out.insert(QStringLiteral("local"), m_store.value(QStringLiteral("store")).toObject());
    QFile f(sessionPath(m_provider));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(tr("登录成功了，但记录写不下去：%1").arg(f.errorString()));
        return;
    }
    f.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
    f.close();
    // Names only. The values are his session.
    qInfo("pl saved provider=%s cookies=%d fresh=%s", qPrintable(m_provider), m_jar.size(),
          marks.join(QLatin1Char(',')).left(200).toUtf8().toPercentEncoding().constData());
    const Provider *pr = providerFor(m_provider);
    if (pr && pr->probeUrl) {
        // 夸克's ladder sends the jar and nothing else - measured, its api answers a plain
        // withCredentials XHR from a normal tab. Reloading to copy four headers would find
        // none and would then report "没抓到接口的签名头" as if something had gone wrong.
        finish(QString());
        return;
    }
    captureHeaders();
}

int PanLogin::send2(const QString &method, const QJsonObject &params)
{
    if (!m_ws)
        return 0;
    const int id = ++m_rpcId;
    QJsonObject req{ { QStringLiteral("id"), id }, { QStringLiteral("method"), method } };
    if (!params.isEmpty())
        req.insert(QStringLiteral("params"), params);
    m_ws->sendTextMessage(QString::fromUtf8(
        QJsonDocument(req).toJson(QJsonDocument::Compact)));
    return id;
}

void PanLogin::requestReload()
{
    if (m_reloaded || m_done)
        return;
    m_reloaded = true;
    qInfo("pl reload provider=%s to make the page call its own api", qPrintable(m_provider));
    send2(QStringLiteral("Page.enable"));
    send2(QStringLiteral("Page.reload"));
}

void PanLogin::watchForApiHeaders(const QJsonObject &o)
{
    const QString method = o.value(QStringLiteral("method")).toString();
    const QJsonObject p = o.value(QStringLiteral("params")).toObject();
    const QString rid = p.value(QStringLiteral("requestId")).toString();
    if (rid.isEmpty())
        return;
    if (method == QStringLiteral("Network.requestWillBeSent")) {
        m_reqUrl.insert(rid, p.value(QStringLiteral("request")).toObject()
                              .value(QStringLiteral("url")).toString());
        return;
    }
    const Provider *pr = providerFor(m_provider);
    const QString url = m_reqUrl.value(rid);
    if (!pr || !url.contains(QLatin1String(pr->apiHost)))
        return;
    // The extra-info event is the only one that carries the headers the browser actually put
    // on the wire - the plain request event shows what the page asked for, before the
    // interceptors added the signature.
    if (method == QStringLiteral("Network.requestWillBeSentExtraInfo")) {
        const QJsonObject hdr = p.value(QStringLiteral("headers")).toObject();
        if (!hdr.contains(QLatin1String("authorization")))
            return;
        m_reqHdr.insert(rid, hdr);
        if (!m_apiHeaders.isEmpty())
            return;
        QJsonObject keep;
        for (const QString &k : { QStringLiteral("authorization"),
                                  QStringLiteral("x-captcha-token"),
                                  QStringLiteral("x-client-id"), QStringLiteral("x-device-id"),
                                  QStringLiteral("user-agent"), QStringLiteral("referer"),
                                  QStringLiteral("origin"), QStringLiteral("content-type") })
            if (hdr.contains(k))
                keep.insert(k, hdr.value(k));
        m_apiHeaders = keep;
        qInfo("pl api headers captured=%d captcha_len=%d", keep.size(),
              keep.value(QStringLiteral("x-captcha-token")).toString().size());
        return;
    }
    // A signed call that came back 200. This, and not a cookie name, is what says the tab is
    // his: measured 2026-11-02, the web login keeps its bearer in the page's memory and the
    // `sessionid` cookie that used to mark a signed-in jar leaves within hours - so a tab
    // listing 39 files and answering 200 carried a jar the old marker called anonymous, and
    // the wait ran out its ten minutes saying 等不到登录.
    if (method == QStringLiteral("Network.responseReceived")) {
        if (m_done || m_jar.isEmpty() || !m_haveStore || !m_reqHdr.contains(rid))
            return;
        if (p.value(QStringLiteral("response")).toObject()
                .value(QStringLiteral("status")).toInt() != 200)
            return;
        qInfo("pl signed call answered 200 provider=%s jar=%d; already signed in",
              qPrintable(m_provider), m_jar.size());
        m_savedFromTraffic = true;
        save(m_jarNames);
    }
}

void PanLogin::captureHeaders()
{
    if (!m_ws) {
        finish(QString());
        return;
    }
    m_capturing = true;
    // Only start over when what we hold was not just proved by a 200: headers copied off an
    // expired bearer would otherwise be thrown away for a reload window that may miss.
    if (!m_savedFromTraffic)
        m_apiHeaders = QJsonObject();
    m_reqUrl.clear();
    m_reqHdr.clear();
    send2(QStringLiteral("Network.enable"));
    send2(QStringLiteral("Page.enable"));
    // Reload, because the page signed its first requests before we subscribed to the
    // network domain and those headers are gone by the time we ask.
    send2(QStringLiteral("Page.reload"));
    QTimer::singleShot(9000, this, &PanLogin::finishCapture);
}

void PanLogin::finishCapture()
{
    m_capturing = false;
    if (!m_apiHeaders.isEmpty()) {
        QFile f(sessionPath(m_provider));
        QJsonObject out;
        if (f.open(QIODevice::ReadOnly)) {
            out = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
        }
        out.insert(QStringLiteral("api_headers"), m_apiHeaders);
        out.insert(QStringLiteral("headers_at"), double(QDateTime::currentSecsSinceEpoch()));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
            f.close();
        }
    }
    // No headers is not a failed login: the session is on disk either way, and the ladder
    // can ask for another capture run if it turns out it needs the signature.
    finish(m_apiHeaders.isEmpty() ? tr("登录记下来了，但没抓到接口的签名头") : QString());
}
