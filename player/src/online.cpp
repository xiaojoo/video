#include "online.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <functional>
#include <memory>

namespace {
// The site answers 403 to anything that does not look like a browser - measured: the
// same URL is 403 with curl's default UA and 200 with this one.
const char *kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";

QString joinUrl(const QString &base, const QString &href)
{
    return QUrl(base).resolved(QUrl(href)).toString(QUrl::FullyEncoded);
}

// Scheme + host + port: the part of an address that says which site it is, so a source
// switch can be told from a category page on the same one.
QString originOf(const QString &url)
{
    const QUrl u(url);
    const QString port = u.port() > 0 ? u":"_qs + QString::number(u.port()) : QString();
    return u.scheme() + u"://"_qs + u.host() + port;
}

QString oneLine(const QString &s)
{
    QString o = s;
    o.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return o.trimmed();
}
}

Online::Online(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

QString Online::settingsPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("online.json"));
}

namespace {
bool urlTaken(const QJsonArray &list, const QString &u, int except)
{
    for (int k = 0; k < list.size(); ++k)
        if (k != except
            && list.at(k).toObject().value(QStringLiteral("url")).toString() == u)
            return true;
    return false;
}
}

void Online::loadSources()
{
    m_sources = QJsonArray();
    QFile f(settingsPath());
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
            QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("url")).toString().isEmpty())
                continue;
            // A file written before the switch existed carries no flag, and every source
            // someone had typed in was in use.
            if (!o.contains(QStringLiteral("enabled")))
                o.insert(QStringLiteral("enabled"), true);
            m_sources.append(o);
        }
    }
    emit sourcesChanged();
}

QJsonArray Online::sources() const
{
    return m_sources;
}

void Online::writeSources()
{
    QDir().mkpath(QFileInfo(settingsPath()).absolutePath());
    QFile f(settingsPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("online: cannot write %s", qPrintable(settingsPath()));
        return;
    }
    f.write(QJsonDocument(m_sources).toJson(QJsonDocument::Indented));
    emit sourcesChanged();
}

void Online::addSource(const QString &name, const QString &url)
{
    const QString u = url.trimmed();
    if (u.isEmpty() || urlTaken(m_sources, u, -1))
        return;
    const QString n = name.trimmed().isEmpty() ? QUrl(u).host() : name.trimmed();
    QJsonObject o;
    o.insert(QStringLiteral("name"), n);
    o.insert(QStringLiteral("url"), u);
    o.insert(QStringLiteral("enabled"), true);
    m_sources.append(o);
    writeSources();
}

void Online::removeSource(int index)
{
    if (index < 0 || index >= m_sources.size())
        return;
    m_sources.removeAt(index);
    writeSources();
}

void Online::updateSource(int index, const QString &name, const QString &url)
{
    if (index < 0 || index >= m_sources.size())
        return;
    const QString u = url.trimmed();
    if (u.isEmpty() || urlTaken(m_sources, u, index))
        return;
    QJsonObject o = m_sources.at(index).toObject();
    const QString was = o.value(QStringLiteral("url")).toString();
    o.insert(QStringLiteral("url"), u);
    o.insert(QStringLiteral("name"),
             name.trimmed().isEmpty() ? QUrl(u).host() : name.trimmed());
    m_sources[index] = o;
    // What is on screen came from the old address; leaving it would show one source's
    // posters under another source's pill.
    if (m_requested == was)
        open(u);
    writeSources();
}

void Online::setSourceEnabled(int index, bool on)
{
    if (index < 0 || index >= m_sources.size())
        return;
    QJsonObject o = m_sources.at(index).toObject();
    if (o.value(QStringLiteral("enabled")).toBool() == on)
        return;
    o.insert(QStringLiteral("enabled"), on);
    m_sources[index] = o;
    writeSources();
}

void Online::setBusy(bool b)
{
    // Both callers move m_pending first and then ask for this, so comparing b against
    // m_pending > 0 finds them equal every time and the signal never leaves the object -
    // which left "正在读取在线源…" on screen after the wall had already arrived.
    if (b == m_busyShown)
        return;
    m_busyShown = b;
    emit busyChanged();
}

void Online::get(const QString &url,
                 const std::function<void(const QString &, const QByteArray &, const QString &)> &then,
                 bool pageLevel)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader,
                  QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.6");
    if (pageLevel) {
        ++m_pending;
        setBusy(true);
    }
    QNetworkReply *rep = m_nam->get(req);
    // The connection is scoped to this object, so the lambda cannot outlive it.
    connect(rep, &QNetworkReply::finished, this, [this, rep, then, pageLevel]() {
        QString err;
        if (rep->error() != QNetworkReply::NoError) {
            err = QStringLiteral("%1 (%2)").arg(rep->errorString())
                      .arg(rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
            // Only the wall's own requests may say anything on the page. A magnet hop
            // that comes back 429 belongs to the dialog that asked for it; writing it
            // here replaced the posters' error line with a transfer message while the
            // dialog was showing a perfectly good list.
            if (pageLevel)
                m_error = err;
        }
        then(rep->url().toString(), rep->readAll(), err);
        if (!pageLevel) {
            rep->deleteLater();
            return;
        }
        --m_pending;
        setBusy(m_pending > 0);
        if (!m_error.isEmpty())
            emit itemsChanged();
        rep->deleteLater();
    });
}

QJsonArray Online::parseList(const QString &html, const QString &base)
{
    static const QRegularExpression card(
        QStringLiteral("class=\"image\"[^>]*href=\"([^\"]+)\"[^>]*>\\s*<img[^>]*"
                       "src=\"([^\"]+)\"[^>]*?alt=\"([^\"]*)\""),
        QRegularExpression::DotMatchesEverythingOption);
    QJsonArray out;
    QSet<QString> seen;
    auto it = card.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString href = m.captured(1);
        if (seen.contains(href))
            continue;
        seen.insert(href);
        QJsonObject o;
        o.insert(QStringLiteral("title"), oneLine(m.captured(3)));
        o.insert(QStringLiteral("poster"), joinUrl(base, m.captured(2)));
        o.insert(QStringLiteral("href"), joinUrl(base, href));
        out.append(o);
    }
    return out;
}

QJsonArray Online::parseNav(const QString &html, const QString &base)
{
    static const QRegularExpression navRe(
        QStringLiteral("<nav[^>]*>(.*?)</nav>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression a(
        QStringLiteral("<a[^>]{0,160}href=\"([^\"]+)\"[^>]{0,80}>([^<]{1,24})</a>"));

    QString scope = html;
    const auto nm = navRe.match(html);
    if (nm.hasMatch())
        scope = nm.captured(1);

    QJsonArray out;
    QSet<QString> seen;
    auto it = a.globalMatch(scope);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString href = m.captured(1);
        const QString text = m.captured(2);
        // Same-site paths with a short one-line label. The rest of what these sites
        // put in the nav is a contact address and a "new domain" notice, and neither
        // is a category.
        if (!href.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char('\n')))
            continue;
        const QString label = oneLine(text);
        if (label.isEmpty() || label.size() > 8 || seen.contains(href))
            continue;
        seen.insert(href);
        QJsonObject o;
        o.insert(QStringLiteral("name"), label);
        o.insert(QStringLiteral("url"), joinUrl(base, href));
        out.append(o);
    }
    return out;
}

QJsonArray Online::parsePager(const QString &html, const QString &base)
{
    // The paginator these VuePress-derived sites emit is a run of spans: the current page
    // is a bare <span class="page active">, every other one wraps an <a>, and the two
    // ends carry class="prev" / class="next". On the first page the "prev" anchor is still
    // printed with a `disabled` attribute instead of an href, so it has to be read and
    // greyed rather than assumed absent - which is also why an empty url means "no hop".
    static const QRegularExpression block(
        QStringLiteral("class=\"page-nav\"(.{0,4000})"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression span(
        QStringLiteral("<span class=\"(page|prev|next)( active)?\">(.{0,140}?)</span>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression hrefRe(QStringLiteral("href=\"([^\"]{1,140})\""));
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    static const QRegularExpression nbsp(QStringLiteral("\xc2\xa0"));

    const auto bm = block.match(html);
    if (!bm.hasMatch())
        return {};

    QJsonArray out;
    auto it = span.globalMatch(bm.captured(1));
    while (it.hasNext()) {
        const auto m = it.next();
        const QString frag = m.captured(3);
        const auto hm = hrefRe.match(frag);
        QJsonObject o;
        o.insert(QStringLiteral("kind"), m.captured(1));
        o.insert(QStringLiteral("label"), oneLine(QString(frag).remove(tags).replace(nbsp,
                                                                                     u" "_qs)));
        o.insert(QStringLiteral("url"), hm.hasMatch() ? joinUrl(base, hm.captured(1))
                                                      : QString());
        o.insert(QStringLiteral("active"), !m.captured(2).isEmpty());
        out.append(o);
    }
    return out;
}

QJsonArray Online::parseSeeds(const QString &html, const QString &base)
{
    // Both kinds of hop live under /link_start/, so the query key is what tells a
    // magnet apart from a netdisk share - and only the magnet one can be downloaded.
    static const QRegularExpression seed(
        QStringLiteral("<a[^>]{0,200}href=\"([^\"]*link_start[^\"]*seed_id=[^\"]*)\""
                       "[^>]*>([^<]{1,300})</a>"));
    static const QRegularExpression sizeRe(
        QStringLiteral("class=\"size\">([^<]{1,20})<"));
    QJsonArray out;
    auto it = seed.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        QJsonObject o;
        o.insert(QStringLiteral("label"), oneLine(m.captured(2)));
        o.insert(QStringLiteral("hop"), joinUrl(base, m.captured(1)));
        // The size is the element right after the link; looking only forward keeps a
        // row's size from being read off a different row.
        const QString tail = html.mid(m.capturedEnd(), 400);
        const auto s = sizeRe.match(tail);
        o.insert(QStringLiteral("size"), s.hasMatch() ? oneLine(s.captured(1)) : QString());
        out.append(o);
    }
    return out;
}

QString Online::decodeMagnet(const QString &html)
{
    // These pages keep the magnet base64-encoded and hand it to window.atob in an
    // inline script, so no magnet: URL ever appears in the markup.
    static const QRegularExpression b64(QStringLiteral("[\"']([A-Za-z0-9+/=]{40,})[\"']"));
    auto it = b64.globalMatch(html);
    while (it.hasNext()) {
        const QString s = QString::fromUtf8(
            QByteArray::fromBase64(it.next().captured(1).toLatin1()));
        if (s.startsWith(QStringLiteral("magnet:")))
            return s;
    }
    static const QRegularExpression plain(
        QStringLiteral("(magnet:\\?[^\"'<>\\s]{20,})"));
    const auto m = plain.match(html);
    return m.hasMatch() ? m.captured(1) : QString();
}

void Online::open(const QString &url)
{
    // Another source: the categories in hand were published by the site being left, and
    // one of them clicked while this fetch runs would take the wall over there. The row
    // goes empty until this fetch brings the new source's own - a category page on the
    // same site keeps them, which is why this compares origins rather than urls.
    if (!m_nav.isEmpty() && originOf(url) != m_navSite) {
        m_nav = QJsonArray();
        emit itemsChanged();
    }
    m_requested = url;
    get(url, [this](const QString &finalUrl, const QByteArray &bytes, const QString &err) {
        if (!err.isEmpty())
            return;   // get() already put it in m_error
        const QString html = QString::fromUtf8(bytes);
        m_error.clear();
        m_final = finalUrl;
        m_items = parseList(html, finalUrl);
        m_nav = parseNav(html, finalUrl);
        m_navSite = originOf(finalUrl);
        m_pager = parsePager(html, finalUrl);
        if (m_items.isEmpty())
            m_error = tr("这一页上没有解析到海报（站点可能换了结构）");
        emit itemsChanged();
    });
}

// What a failed transfer says in the magnet dialog. The raw QNetworkReply text names the
// whole url, which is 90 characters of noise inside a 620 px box; the status code is the
// only part worth reading, and 429 ("you asked too often") is the one that actually
// happens on these sites.
static QString magnetFailure(const QString &err)
{
    const int code = err.section(QLatin1Char('('), -1).remove(QLatin1Char(')')).toInt();
    if (code >= 400)
        return QStringLiteral("站点回了 %1，稍等几秒再点一次").arg(code);
    if (code > 0)
        return QStringLiteral("连不上站点（%1）").arg(code);
    return QStringLiteral("连不上站点");
}

void Online::magnets(const QString &url)
{
    // Nothing here may touch the wall's busy flag or its error line: this request is
    // answered inside the dialog that asked for it.
    get(url, [this, url](const QString &finalUrl, const QByteArray &bytes, const QString &err) {
        if (!err.isEmpty()) {
            emit magnetsReady(url, QJsonArray{}, magnetFailure(err));
            return;
        }
        const QJsonArray seeds = parseSeeds(QString::fromUtf8(bytes), finalUrl);
        if (seeds.isEmpty()) {
            emit magnetsReady(url, QJsonArray{}, tr("这一页上没有磁力链接"));
            return;
        }
        // One request per seed's hop; the answer is emitted when the last one lands.
        auto left = std::make_shared<int>(seeds.size());
        auto got = std::make_shared<QJsonArray>();
        for (int i = 0; i < seeds.size(); ++i) {
            const QJsonObject s = seeds.at(i).toObject();
            get(s.value(QStringLiteral("hop")).toString(),
                [this, url, s, left, got](const QString &, const QByteArray &hopBytes,
                                          const QString &) {
                    QJsonObject o = s;
                    o.insert(QStringLiteral("magnet"),
                             decodeMagnet(QString::fromUtf8(hopBytes)));
                    got->append(o);
                    if (--(*left) == 0) {
                        QJsonArray out;
                        for (const QJsonValue &v : *got)
                            if (!v.toObject().value(QStringLiteral("magnet")).toString().isEmpty())
                                out.append(v);
                        emit magnetsReady(url, out, out.isEmpty()
                                ? tr("跳页里解不出磁力（编码方式可能改了）") : QString());
                    }
                },
                false);
        }
    }, false);
}
