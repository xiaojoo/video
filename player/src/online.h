#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

// A poster on a source's list page, and the magnets found behind it.
//
// The shape is deliberately small. Measured on the site the user pointed at: a card
// is `<a class="image" href="/movies/ID/"><img src alt>`; the detail page lists seeds
// as `<a href="/link_start/?seed_id=…">label</a> <code class="size">1.35G</code>`; and
// the hop carries the magnet as base64 in an inline script handed to window.atob.
// Two facts decide the implementation: the site answers 403 to a non-browser
// User-Agent, and the magnet is not in the HTML - so both are handled here.
class Online final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QJsonArray items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QJsonArray nav READ nav NOTIFY itemsChanged)
    Q_PROPERTY(QJsonArray pager READ pager NOTIFY itemsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY itemsChanged)
    // Which page the menu row should mark as current.
    Q_PROPERTY(QString currentUrl READ currentUrl NOTIFY itemsChanged)
    Q_PROPERTY(QJsonArray sources READ sources NOTIFY sourcesChanged)

public:
    explicit Online(QObject *parent = nullptr);

    // Sources are name + start url + whether the row is switched on, kept in a small file
    // beside the catalogue rather than in its tables: they are settings, not media, and
    // must survive a rescan.
    Q_INVOKABLE void loadSources();
    Q_INVOKABLE QJsonArray sources() const;
    Q_INVOKABLE void addSource(const QString &name, const QString &url);
    Q_INVOKABLE void removeSource(int index);
    Q_INVOKABLE void updateSource(int index, const QString &name, const QString &url);
    Q_INVOKABLE void setSourceEnabled(int index, bool on);

    // A list page: posters, titles, and the source's own navigation for the rail.
    Q_INVOKABLE void open(const QString &url);
    // The links behind one poster: the detail page, then one hop per seed.
    Q_INVOKABLE void magnets(const QString &url);
    // One row of the netdisk tabs. The page keeps the address behind its own hop, so this
    // reads the hop and hands the address to a browser - 夸克's own when it is installed,
    // because that is where the signed-in session is.
    Q_INVOKABLE void openShare(const QString &hop);

    // The base64 payload these pages hand to window.atob. Exposed for the probe.
    static QString decodeMagnet(const QString &html);
    // The netdisk address a pan hop prints in its script and in its 前往网盘转存 anchor.
    static QString decodeShare(const QString &html);
    // What the detail page says about the film itself: the h1, the cover, the list of
    // label/value rows under it, and the download tabs with the counts the site printed.
    static QJsonObject parseDetail(const QString &html, const QString &base);

    QJsonArray items() const { return m_items; }
    QJsonArray nav() const { return m_nav; }
    QJsonArray pager() const { return m_pager; }
    // What the last setBusy said, not m_pending > 0: the callers move m_pending before
    // they ask for the repaint, so reading it here would report a state nobody was told
    // about.
    bool busy() const { return m_busyShown; }
    QString error() const { return m_error; }
    // Where the last list page actually landed: these sites bounce off a
    // redirector domain, and the UI should say which one it is talking to.
    QString finalUrl() const { return m_final; }
    QString currentUrl() const { return m_requested; }

    // Parsing is split out so --online-probe can run the same code the UI runs.
    static QJsonArray parseList(const QString &html, const QString &base);
    static QJsonArray parseNav(const QString &html, const QString &base);
    static QJsonArray parsePager(const QString &html, const QString &base);
    static QJsonArray parseSeeds(const QString &html, const QString &base);
    // The rows behind the site's other tabs: one per netdisk link on the page, each keyed
    // by the tab it belongs to.
    static QJsonArray parsePans(const QString &html, const QString &base);

signals:
    void itemsChanged();
    void busyChanged();
    void sourcesChanged();
    void magnetsReady(const QString &url, const QJsonArray &links, const QString &note,
                      const QJsonObject &detail);
    // A netdisk row was clicked and nothing could be opened. The dialog owns this line, not
    // the wall's error property: the posters behind are fine.
    void shareFailed(const QString &note);
    // A list page has been fetched and parsed, and `url` names which one. `ok` is false
    // when the transfer failed, so whoever asked can stop waiting for it.
    // `itemsChanged` cannot be used for this: open() emits it before the request even goes
    // out, to clear the category row of the site being left.
    void listReady(const QString &url, bool ok);

private:
    // `then` runs for every reply, successful or not: a hop that comes back 429 has
    // to be counted, or a caller waiting on N of them never answers at all.
    // `pageLevel` is the wall's own request: it is the one that drives the skeleton
    // (busy) and the page's error line. A magnet fetch is not - see magnets().
    void get(const QString &url,
             const std::function<void(const QString &, const QByteArray &, const QString &)> &then,
             bool pageLevel = true);
    void setBusy(bool b);
    QString settingsPath() const;
    // One place writes the file: a row's enabled flag has to survive every other edit, and
    // rewriting the list through addSource could not carry it.
    void writeSources();

    QNetworkAccessManager *m_nam = nullptr;
    QJsonArray m_items;
    QJsonArray m_nav;
    // The origin m_nav was parsed off, so open() can tell a source switch from one more
    // category page of the site already shown.
    QString m_navSite;
    QJsonArray m_pager;
    QString m_error;
    QString m_final;
    QString m_requested;
    QJsonArray m_sources;
    int m_pending = 0;
    bool m_busyShown = false;
};
