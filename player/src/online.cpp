#include "online.h"
#include "netdisk.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QDesktopServices>

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

// Release names carry apostrophes as &#39; (La bataille de Gaule: L&#39;âge de fer), which
// showed on screen as the entity itself. Numeric forms are resolved first so that an
// escaped "&amp;#39;" still comes out as the six characters the page meant.
QString decodeEntities(const QString &s)
{
    static const QRegularExpression num(QStringLiteral("&#(x?)([0-9A-Fa-f]{1,6});"));
    QString o = s;
    o.replace(QStringLiteral("&nbsp;"), u" "_qs);
    o.replace(QStringLiteral("&quot;"), u"\""_qs);
    o.replace(QStringLiteral("&lt;"), u"<"_qs);
    o.replace(QStringLiteral("&gt;"), u">"_qs);
    QString out;
    int last = 0;
    auto it = num.globalMatch(o);
    while (it.hasNext()) {
        const auto m = it.next();
        bool ok = false;
        const uint code = m.captured(2).toUInt(&ok, m.captured(1).isEmpty() ? 10 : 16);
        if (!ok || code == 0 || code > 0x10FFFF)
            continue;
        out += o.mid(last, m.capturedStart() - last);
        if (code <= 0xFFFF) {
            out += QChar(static_cast<ushort>(code));
        } else {
            const char32_t cp = code;
            out += QString::fromUcs4(&cp, 1);
        }
        last = m.capturedEnd();
    }
    o = last ? out + o.mid(last) : o;
    o.replace(QStringLiteral("&amp;"), u"&"_qs);
    return o;
}

// The rows in the film's info list are one label and one run of links; reading them as
// text and splitting at the first colon keeps "导演: A / B" together whatever markup the
// links carry.
QString plainText(const QString &s)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    QString o = s;
    o.remove(tags);
    return oneLine(decodeEntities(o));
}

// Which of the site's tabs a netdisk row belongs to. The site keys its own tab row off the
// same domain, so the count a tab prints and the list behind it can never disagree.
const QHash<QString, QStringList> &panDomains()
{
    static const QHash<QString, QStringList> d{
        { QStringLiteral("baidu"),  { QStringLiteral("pan.baidu.com") } },
        { QStringLiteral("quark"),  { QStringLiteral("pan.quark.cn") } },
        { QStringLiteral("xunlei"), { QStringLiteral("pan.xunlei.com") } },
        { QStringLiteral("uc"),     { QStringLiteral("drive.uc.cn") } },
        { QStringLiteral("ali"),    { QStringLiteral("aliyundrive.com"),
                                      QStringLiteral("alipan.com") } },
    };
    return d;
}

QString panKindOf(const QString &link)
{
    for (auto d = panDomains().cbegin(); d != panDomains().cend(); ++d)
        for (const QString &domain : d.value())
            if (link.contains(domain))
                return d.key();
    return QString();
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

namespace {
// Two sites, two page shapes, and the host is the tell. sidhub is a VuePress-derived index
// whose cards are `<a class="image">` and whose seeds hop through /link_start/; xbgame is a
// WordPress site on the B2 theme whose cards are `<a class="thumb-link"><picture>` and whose
// detail page prints the magnet: urls itself - no hop, no base64.
enum class Site { Sidhub, Xbgame };

Site siteOf(const QString &url)
{
    return QUrl(url).host().endsWith(QStringLiteral("xbgame.net"), Qt::CaseInsensitive)
               ? Site::Xbgame : Site::Sidhub;
}

// ---- xbgame.net ----
// A card is the thumbnail anchor around a <picture>; the img alt carries the release name
// the h2 repeats, and both the <source> and the <img> point at the same webp thumb.
QJsonArray xbList(const QString &html, const QString &base)
{
    static const QRegularExpression card(
        QStringLiteral("<a[^>]{0,200}href=\"([^\"]+)\"[^>]{0,120}class=\"thumb-link\"[^>]*>"
                       "\\s*<picture[^>]*>(.*?)</picture>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression alt(QStringLiteral("<img[^>]*?alt=\"([^\"]*)\""));
    static const QRegularExpression src(QStringLiteral("<img[^>]*?src=\"([^\"]*)\""));
    QJsonArray out;
    QSet<QString> seen;
    auto it = card.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString href = m.captured(1);
        const auto sm = src.match(m.captured(2));
        if (seen.contains(href) || !sm.hasMatch())
            continue;
        seen.insert(href);
        const auto am = alt.match(m.captured(2));
        QJsonObject o;
        o.insert(QStringLiteral("title"),
                 oneLine(decodeEntities(am.hasMatch() ? am.captured(1) : QString())));
        o.insert(QStringLiteral("poster"), joinUrl(base, decodeEntities(sm.captured(1))));
        o.insert(QStringLiteral("href"), joinUrl(base, href));
        out.append(o);
    }
    return out;
}

// The category's own filter rail - 全部/蓝光电影/4K画质/… - one url each. These anchors are
// absolute where the other site prints paths, so both are resolved and only the same host
// is kept (the rail also links its feed and its tags).
QJsonArray xbNav(const QString &html, const QString &base)
{
    static const QRegularExpression block(
        QStringLiteral("<div class=\"filter-items\">(.*?)</div>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression a(
        QStringLiteral("<a[^>]{0,200}href=\"([^\"]+)\"[^>]{0,120}>([^<]{1,12})</a>"));
    const QString host = QUrl(base).host();
    const auto bm = block.match(html);
    if (!bm.hasMatch())
        return {};
    QJsonArray out;
    QSet<QString> seen;
    auto it = a.globalMatch(bm.captured(1));
    while (it.hasNext()) {
        const auto m = it.next();
        const QString label = oneLine(decodeEntities(m.captured(2)));
        const QString url = joinUrl(base, decodeEntities(m.captured(1)));
        if (label.isEmpty() || label.size() > 8 || seen.contains(url))
            continue;
        if (QUrl(url).host() != host)
            continue;
        seen.insert(url);
        QJsonObject o;
        o.insert(QStringLiteral("name"), label);
        o.insert(QStringLiteral("url"), url);
        out.append(o);
    }
    return out;
}

// The B2 paginator: one anchor per printed number (the current page is printed `selected`
// and still carries its href), then a pair of arrows that are not pages. The wall only
// needs the numbers - it derives the url template from any two of them - and page 1 has no
// "/page/1", which is why the template has to come from 2 and 3.
QJsonArray xbPager(const QString &html, const QString &base)
{
    const int at = html.indexOf(QStringLiteral("class=\"b2-pagenav"));
    if (at < 0)
        return {};
    const QString scope = html.mid(at, 2000);
    static const QRegularExpression a(
        QStringLiteral("<a([^>]{0,300})href=\"([^\"]+)\"([^>]{0,120})>([^<]{1,6})</a>"));
    static const QRegularExpression num(QStringLiteral("^[0-9]{1,4}$"));
    QJsonArray out;
    auto it = a.globalMatch(scope);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString label = oneLine(m.captured(4));
        if (!num.match(label).hasMatch())
            continue;
        const QString attrs = m.captured(1) + m.captured(3);
        QJsonObject o;
        o.insert(QStringLiteral("kind"), QStringLiteral("page"));
        o.insert(QStringLiteral("label"), label);
        o.insert(QStringLiteral("url"), joinUrl(base, decodeEntities(m.captured(2))));
        o.insert(QStringLiteral("active"), attrs.contains(QStringLiteral("selected")));
        out.append(o);
    }
    return out;
}

// The magnet table. Each row is one anchor around `<span class="label label-danger">4K
// </span> <span class="label label-warning">13.2GB</span> <b>RELEASE.NAME</b>`, so here the
// magnet, its size and its badges all arrive together and there is no hop to follow.
QJsonArray xbSeeds(const QString &html, const QString &base)
{
    static const QRegularExpression row(
        QStringLiteral("<a([^>]{0,300})href=\"(magnet:\\?[^\"]+)\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression b(QStringLiteral("<b>([^<]{1,400})</b>"));
    static const QRegularExpression sizeRe(
        QStringLiteral("label-warning\">([^<]{1,20})<"));
    static const QRegularExpression featureRe(
        QStringLiteral("label-danger\">([^<]{1,20})<"));
    QJsonArray out;
    QSet<QString> seen;
    auto it = row.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString magnet = decodeEntities(m.captured(2));
        if (seen.contains(magnet))
            continue;
        const QString body = m.captured(3);
        const auto tm = b.match(body);
        const QString title = oneLine(decodeEntities(tm.hasMatch() ? tm.captured(1)
                                                                    : QString()));
        if (title.isEmpty())
            continue;
        seen.insert(magnet);
        QJsonObject o;
        o.insert(QStringLiteral("label"), title);
        o.insert(QStringLiteral("title"), title);
        // The hop is the magnet itself: nothing sits behind it to fetch.
        o.insert(QStringLiteral("hop"), magnet);
        o.insert(QStringLiteral("magnet"), magnet);
        const auto sm = sizeRe.match(body);
        o.insert(QStringLiteral("size"), sm.hasMatch() ? oneLine(sm.captured(1)) : QString());
        QJsonArray feats;
        auto fi = featureRe.globalMatch(body);
        while (fi.hasNext())
            feats.append(oneLine(fi.next().captured(1)));
        o.insert(QStringLiteral("features"), feats);
        o.insert(QStringLiteral("time"), QString());
        out.append(o);
    }
    return out;
}

// The film's own block: the post h1, the first picture in the body (the cover), and the
// "◎" rows the page prints as label/value pairs in four character cells
// (◎年　　代　2026). 简介 has nothing behind it on its own line - its text is the
// paragraph that follows.
QJsonObject xbDetail(const QString &html, const QString &base)
{
    QJsonObject out;
    static const QRegularExpression h1(
        QStringLiteral("<h1[^>]*>(.*?)</h1>"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto hm = h1.match(html);
    if (hm.hasMatch())
        out.insert(QStringLiteral("title"), oneLine(plainText(hm.captured(1))));

    const int start = html.indexOf(QStringLiteral("class=\"entry-content\""));
    const int stop = start >= 0 ? html.indexOf(QStringLiteral("content-footer"), start) : -1;
    const QString body = start >= 0
            ? html.mid(start, stop > start ? stop - start : html.size() - start)
            : html;

    static const QRegularExpression img(QStringLiteral("<img[^>]*?src=\"([^\"]+)\""));
    const auto im = img.match(body);
    // The body's first picture is wrapped in a 搜狗 CDN thumbnail link. That proxy answers
    // 400 without the site's referer and 403 with it, and the origin it wraps no longer
    // resolves, so the url is not a cover this app can draw - the wall's art stands in.
    if (im.hasMatch() && !im.captured(1).contains(QStringLiteral("sogoucdn.com")))
        out.insert(QStringLiteral("poster"), joinUrl(base, decodeEntities(im.captured(1))));

    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    static const QRegularExpression br(QStringLiteral("<br\\s*/?>"));
    static const QRegularExpression para(QStringLiteral("</p>"));
    static const QRegularExpression spaces(QStringLiteral("[\\s\\x{3000}]+"));
    QString text = body;
    text.replace(br, QStringLiteral("\n"));
    text.replace(para, QStringLiteral("\n"));
    text.remove(tags);

    // The padding is what separates the label from its value here - not a colon: the
    // label is always four cells wide ("译　　名", "年　　代", "上映日期").
    static const QSet<QString> kept{ QStringLiteral("年代"), QStringLiteral("产地"),
                                     QStringLiteral("类别"), QStringLiteral("语言"),
                                     QStringLiteral("上映日期"), QStringLiteral("片长"),
                                     QStringLiteral("导演"), QStringLiteral("编剧"),
                                     QStringLiteral("主演"), QStringLiteral("简介") };
    QJsonArray meta;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (!line.startsWith(QStringLiteral("◎")))
            continue;
        const QString rest = line.mid(1);
        QString label = rest.left(4);
        QString val = rest.mid(4).trimmed();
        label.remove(spaces);
        // 简介 is printed as its own paragraph with the text in the next one, and the blank
        // line between the two </p>s would otherwise be read as "this row has no value".
        if (val.isEmpty()) {
            for (int j = i + 1; j < lines.size() && j <= i + 3; ++j) {
                const QString next = lines.at(j).trimmed();
                if (next.isEmpty())
                    continue;
                if (!next.startsWith(QStringLiteral("◎")))
                    val = next;
                break;
            }
        }
        if (!kept.contains(label) || val.isEmpty())
            continue;
        QJsonObject row;
        row.insert(QStringLiteral("k"), label);
        row.insert(QStringLiteral("v"), val);
        meta.append(row);
    }
    out.insert(QStringLiteral("meta"), meta);

    // No netdisk rows on this theme's film pages; the one tab is the magnet table, and its
    // count is the rows the dialog will actually list.
    QJsonArray tabs;
    QJsonObject seedTab;
    seedTab.insert(QStringLiteral("key"), QStringLiteral("seed"));
    seedTab.insert(QStringLiteral("name"), QStringLiteral("磁力"));
    seedTab.insert(QStringLiteral("count"), xbSeeds(html, base).size());
    tabs.append(seedTab);
    out.insert(QStringLiteral("tabs"), tabs);
    return out;
}
}

QJsonArray Online::parseList(const QString &html, const QString &base)
{
    if (siteOf(base) == Site::Xbgame)
        return xbList(html, base);
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
    if (siteOf(base) == Site::Xbgame)
        return xbNav(html, base);
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
    if (siteOf(base) == Site::Xbgame)
        return xbPager(html, base);
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
    if (siteOf(base) == Site::Xbgame)
        return xbSeeds(html, base);
    // Both kinds of hop live under /link_start/, so the query key is what tells a
    // magnet apart from a netdisk share - and only the magnet one can be downloaded.
    // The link's own attributes carry the release name whole, and the rest of the row
    // carries the badges and the age the site prints beside it.
    static const QRegularExpression seed(
        QStringLiteral("<a([^>]{0,400})href=\"([^\"]*link_start[^\"]*seed_id=[^\"]*)\""
                       "[^>]*>([^<]{1,300})</a>"));
    static const QRegularExpression sizeRe(
        QStringLiteral("class=\"size\">([^<]{1,20})<"));
    static const QRegularExpression titleRe(QStringLiteral("title=\"([^\"]*)\""));
    static const QRegularExpression featureRe(
        QStringLiteral("<code class=\"seed-feature\">([^<]{1,12})</code>"));
    static const QRegularExpression ageRe(
        QStringLiteral("<span style=\"float:right[^\"]*\"[^>]*>([^<]{1,20})</span>"));
    QJsonArray out;
    auto it = seed.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString label = oneLine(decodeEntities(m.captured(3)));
        QJsonObject o;
        o.insert(QStringLiteral("label"), label);
        // The anchor's text is cut at thirty-odd characters; its title holds the release
        // name whole, which is what a row that may be 900 px wide should show.
        const auto tm = titleRe.match(m.captured(1));
        o.insert(QStringLiteral("title"),
                 tm.hasMatch() ? oneLine(decodeEntities(tm.captured(1))) : label);
        o.insert(QStringLiteral("hop"), joinUrl(base, m.captured(2)));
        // The size, the badges and the age all sit between the link and the row's end;
        // reading only inside that span keeps one row's facts off another row.
        const int end = html.indexOf(QStringLiteral("</li>"), m.capturedEnd());
        const QString tail = html.mid(m.capturedEnd(),
                                      end > m.capturedEnd() ? end - m.capturedEnd() : 400);
        const auto s = sizeRe.match(tail);
        o.insert(QStringLiteral("size"), s.hasMatch() ? oneLine(s.captured(1)) : QString());
        QJsonArray feats;
        auto fi = featureRe.globalMatch(tail);
        while (fi.hasNext())
            feats.append(oneLine(fi.next().captured(1)));
        o.insert(QStringLiteral("features"), feats);
        const auto am = ageRe.match(tail);
        o.insert(QStringLiteral("time"), am.hasMatch() ? oneLine(am.captured(1)) : QString());
        out.append(o);
    }
    return out;
}

// The rows behind the site's other tabs. They sit in a second list on the same page, each
// marked with the netdisk it points at, and the site hides it until the tab is clicked.
// Nothing else is on the row - no size, no badges - so the title the site printed is all a
// row has to say.
QJsonArray Online::parsePans(const QString &html, const QString &base)
{
    // The film pages of the B2 theme carry no netdisk rows; their downloads are magnets.
    if (siteOf(base) == Site::Xbgame)
        return {};
    static const QRegularExpression pan(
        QStringLiteral("<a([^>]{0,600})data-link=\"([^\"]{1,40})\"([^>]{0,600})>"
                       "([^<]{1,300})</a>"));
    static const QRegularExpression hrefRe(QStringLiteral("href=\"([^\"]{1,600})\""));
    static const QRegularExpression titleRe(QStringLiteral("title=\"([^\"]*)\""));
    QJsonArray out;
    auto it = pan.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString kind = panKindOf(m.captured(2));
        const QString attrs = m.captured(1) + m.captured(3);
        const auto hm = hrefRe.match(attrs);
        if (kind.isEmpty() || !hm.hasMatch())
            continue;
        const QString label = oneLine(decodeEntities(m.captured(4)));
        const auto tm = titleRe.match(attrs);
        QJsonObject o;
        o.insert(QStringLiteral("kind"), kind);
        o.insert(QStringLiteral("label"), label);
        // The anchor text is cut with an ellipsis on the long ones; the title holds it whole.
        o.insert(QStringLiteral("title"),
                 tm.hasMatch() ? oneLine(decodeEntities(tm.captured(1))) : label);
        o.insert(QStringLiteral("hop"), joinUrl(base, hm.captured(1)));
        out.append(o);
    }
    return out;
}

QJsonObject Online::parseDetail(const QString &html, const QString &base)
{
    if (siteOf(base) == Site::Xbgame)
        return xbDetail(html, base);
    QJsonObject out;
    static const QRegularExpression h1(
        QStringLiteral("<h1 id=\"cover\"[^>]*>(.*?)</h1>"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto hm = h1.match(html);
    if (hm.hasMatch()) {
        // The heading carries a "#" anchor inside it, which reads as part of the title.
        QString title = plainText(hm.captured(1));
        while (title.startsWith(QLatin1Char('#')))
            title = title.mid(1).trimmed();
        out.insert(QStringLiteral("title"), title);
    }

    static const QRegularExpression cover(
        QStringLiteral("<div class=\"cover-container\">\\s*<img src=\"([^\"]+)\""));
    const auto cm = cover.match(html);
    if (cm.hasMatch())
        out.insert(QStringLiteral("poster"), joinUrl(base, cm.captured(1)));

    // The info block is the list right under the cover. Every row is "label: value", and
    // the site prints rows it has nothing for (an empty 单集片长:, an empty 又名:) - a row
    // with no value is dropped rather than drawn as a blank line.
    static const QRegularExpression block(
        QStringLiteral("<div class=\"cover-container\">(.*?)</div>"),
        QRegularExpression::DotMatchesEverythingOption);
    QJsonArray meta;
    // 首播/豆瓣评分/常用标签 are the rows he crossed out - the first showing, the site's
    // own score and the tags repeat what the download rows already say. 集数 he asked to
    // keep. 又名 is the site's alias list, and it is the longest label on the page for the
    // least information.
    static const QSet<QString> dropped{ QStringLiteral("首播"),
                                        QStringLiteral("豆瓣评分"),
                                        QStringLiteral("常用标签"),
                                        QStringLiteral("又名") };
    // Three labels are shortened rather than drawn as the site's form-field wording: the
    // column's label cell is 74 px, and 制片国家/地区 ran into its own value.
    static const QMap<QString, QString> renamed{
        { QStringLiteral("制片国家/地区"), QStringLiteral("国家") },
        { QStringLiteral("资源更新于"), QStringLiteral("更新") },
        { QStringLiteral("单集片长"), QStringLiteral("片长") },
    };
    const auto bm = block.match(html);
    if (bm.hasMatch()) {
        static const QRegularExpression li(
            QStringLiteral("<li>(.*?)</li>"),
            QRegularExpression::DotMatchesEverythingOption);
        auto it = li.globalMatch(bm.captured(1));
        while (it.hasNext()) {
            const QString line = plainText(it.next().captured(1));
            const int at = line.indexOf(QLatin1Char(':'));
            if (at <= 0)
                continue;
            const QString key = renamed.value(line.left(at).trimmed(), line.left(at).trimmed());
            QString val = line.mid(at + 1).trimmed();
            // 主演 ends with the rest of the cast printed as "......".
            while (val.endsWith(QStringLiteral("…")) || val.endsWith(QLatin1Char('.')))
                val.chop(1);
            val = val.trimmed();
            if (val.endsWith(QLatin1Char('/')))
                val.chop(1);
            val = val.trimmed();
            if (key.isEmpty() || val.isEmpty() || dropped.contains(key))
                continue;
            QJsonObject row;
            row.insert(QStringLiteral("k"), key);
            row.insert(QStringLiteral("v"), val);
            meta.append(row);
        }
    }
    out.insert(QStringLiteral("meta"), meta);

    // 磁力(33) 百度(76) … - the nav labels are served with 0 on every pan tab and the
    // site's own script rewrites them from the pan-link list once the page runs
    // (switchTab's $(document).ready block counts `.pan-links a` by data-link). Reading
    // the same list gives the numbers his screenshot shows: 百度 76, 夸克 133, 迅雷 79,
    // UC 3, 阿里 0 on 坠落2 - the markup itself only says 0. Counting the rows the dialog
    // will actually list is also what keeps a tab from advertising links it cannot show.
    QHash<QString, int> panCount;
    for (const QJsonValue &v : parsePans(html, base))
        ++panCount[v.toObject().value(QStringLiteral("kind")).toString()];

    static const QRegularExpression tab(
        QStringLiteral("<a class=\"nav-link ([a-z]+)-tab[^\"]*\"[^>]*>([^<(]{1,8})\\((\\d+)\\)"
                       "</a>"));
    QJsonArray tabs;
    auto ti = tab.globalMatch(html);
    while (ti.hasNext()) {
        const auto m = ti.next();
        const QString key = m.captured(1);
        QJsonObject t;
        t.insert(QStringLiteral("key"), key);
        t.insert(QStringLiteral("name"), oneLine(m.captured(2)));
        t.insert(QStringLiteral("count"),
                 panCount.contains(key) ? panCount.value(key) : m.captured(3).toInt());
        tabs.append(t);
    }
    out.insert(QStringLiteral("tabs"), tabs);
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

QString Online::decodeShare(const QString &html)
{
    // A pan hop does not redirect: it prints an interstitial that carries the address
    // twice - in the script that draws the 扫码转存 QR, and in the 前往网盘转存 anchor.
    // The script's variable is the same shape for every netdisk, so it goes first.
    static const QRegularExpression js(QStringLiteral("var panLink = \"([^\"]{1,300})\""));
    const auto jm = js.match(html);
    if (jm.hasMatch())
        return decodeEntities(jm.captured(1));
    static const QRegularExpression direct(
        QStringLiteral("class=\"direct-pan\"[^>]*href=\"([^\"]{1,300})\""));
    const auto dm = direct.match(html);
    return dm.hasMatch() ? decodeEntities(dm.captured(1)) : QString();
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
    const QString asked = url;
    get(url, [this, asked](const QString &finalUrl, const QByteArray &bytes, const QString &err) {
        if (!err.isEmpty()) {
            emit listReady(asked, false);   // whoever is waiting on this url may stop
            return;   // get() already put it in m_error
        }
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
        emit listReady(asked, true);
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
            emit magnetsReady(url, QJsonArray{}, magnetFailure(err), QJsonObject{});
            return;
        }
        const QString html = QString::fromUtf8(bytes);
        // The film's own facts come from the same answer as the seeds, so the dialog can
        // fill its top half without a second request.
        const QJsonObject detail = parseDetail(html, finalUrl);
        const QJsonArray seeds = parseSeeds(html, finalUrl);
        // The other tabs' rows are already on this page - the site only hides them until a
        // tab is clicked - so they come along keyed by tab and a click has nothing to fetch.
        auto lists = std::make_shared<QJsonObject>();
        for (const QJsonValue &v : parsePans(html, finalUrl)) {
            const QString k = v.toObject().value(QStringLiteral("kind")).toString();
            QJsonArray group = lists->value(k).toArray();
            group.append(v);
            lists->insert(k, group);
        }
        if (seeds.isEmpty()) {
            QJsonObject d = detail;
            d.insert(QStringLiteral("lists"), *lists);
            emit magnetsReady(url, QJsonArray{},
                              lists->isEmpty() ? tr("这一页上没有链接") : QString(), d);
            return;
        }
        // Some sites print the magnet: url on the page itself. There is nothing behind it
        // to hop to, and asking the site once per row would be one request per magnet for
        // an answer already in hand.
        bool inlineMagnets = true;
        for (const QJsonValue &v : seeds)
            if (v.toObject().value(QStringLiteral("magnet")).toString().isEmpty())
                inlineMagnets = false;
        if (inlineMagnets) {
            QJsonObject d = detail;
            lists->insert(QStringLiteral("seed"), seeds);
            d.insert(QStringLiteral("lists"), *lists);
            emit magnetsReady(url, seeds, QString(), d);
            return;
        }
        // One request per seed's hop; the answer is emitted when the last one lands.
        auto left = std::make_shared<int>(seeds.size());
        auto got = std::make_shared<QJsonArray>();
        for (int i = 0; i < seeds.size(); ++i) {
            const QJsonObject s = seeds.at(i).toObject();
            get(s.value(QStringLiteral("hop")).toString(),
                [this, url, s, left, got, detail, lists](const QString &,
                                                         const QByteArray &hopBytes,
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
                        QJsonObject d = detail;
                        lists->insert(QStringLiteral("seed"), out);
                        d.insert(QStringLiteral("lists"), *lists);
                        emit magnetsReady(url, out, out.isEmpty()
                                ? tr("跳页里解不出磁力（编码方式可能改了）") : QString(), d);
                    }
                },
                false);
        }
    }, false);
}

namespace {
// The 夸克 client - and it is the client that holds the signed-in netdisk session, since
// the system default browser on this box is not signed in to 夸克. The lookup itself lives
// with the client driver, which needs the same path to bring the client up with a debug
// port. It reads the scheme handler rather than StartMenuInternet because on this machine
// that second entry carries no command line at all: the old walk came back empty and a
// 夸克 row opened the default browser instead, silently.
QString quarkExe() { return Netdisk::clientExecutable(); }
}

void Online::shareUrl(const QString &hop)
{
    // The hop is fetched rather than opened: it answers with an interstitial that carries
    // the address, and it is that address the netdisk driver needs.
    get(hop, [this, hop](const QString &, const QByteArray &bytes, const QString &err) {
        if (!err.isEmpty()) {
            emit shareFailed(magnetFailure(err));
            return;
        }
        const QString url = decodeShare(QString::fromUtf8(bytes));
        if (url.isEmpty()) {
            emit shareFailed(tr("跳页里没有网盘地址（站点的跳页可能改了）"));
            return;
        }
        emit shareUrlReady(hop, url);
    }, false);
}

void Online::openShare(const QString &hop)
{
    // The hop is fetched rather than opened: it answers with an interstitial, and the
    // address behind it is what a row should hand to a browser. Like the magnet hop, this
    // one belongs to the dialog that asked and says nothing to the wall.
    get(hop, [this, hop](const QString &, const QByteArray &bytes, const QString &err) {
        if (!err.isEmpty()) {
            emit shareFailed(magnetFailure(err));
            return;
        }
        const QString url = decodeShare(QString::fromUtf8(bytes));
        if (url.isEmpty()) {
            emit shareFailed(tr("跳页里没有网盘地址（站点的跳页可能改了）"));
            return;
        }
        const QString quark = url.contains(QStringLiteral("pan.quark.cn"))
                                  ? quarkExe() : QString();
        if (!quark.isEmpty() && QProcess::startDetached(quark, { url }))
            return;
        if (!QDesktopServices::openUrl(QUrl(url)))
            emit shareFailed(tr("打不开这个网盘地址"));
    }, false);
}
