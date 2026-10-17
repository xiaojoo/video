#include "duration.h"
#include "library.h"
#include "playercore.h"
#include "singleinstance.h"
#include "thumbstrip.h"
#include "videohost.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QStandardPaths>
#include <QFile>
#include <QTextStream>
#include <QUrl>
#include <QWindow>
#include <QSurfaceFormat>

#include <clocale>

static QRect clientRect(HWND h)
{
    RECT c{};
    GetClientRect(h, &c);
    POINT tl{ c.left, c.top }, br{ c.right, c.bottom };
    ClientToScreen(h, &tl);
    ClientToScreen(h, &br);
    return { int(tl.x), int(tl.y), int(br.x - tl.x), int(br.y - tl.y) };
}

// Qt::Tool windows are excluded from quitOnLastWindowClosed, so the chrome
// closing must quit the app explicitly.
class CloseQuitter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *ev) override
    {
        if (ev->type() == QEvent::Close) {
            QMetaObject::invokeMethod(qApp, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
        }
        return QObject::eventFilter(watched, ev);
    }
};

int main(int argc, char *argv[])
{
    // libmpv requires LC_NUMERIC="C"; set it now so the shell does not have to
    // remember once the core is wired in.
    std::setlocale(LC_NUMERIC, "C");

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(u"Playerui"_qs);
    QGuiApplication::setOrganizationName(u"local"_qs);

    // Without an 8-bit alpha buffer the chrome window composites as opaque black
    // and hides the mpv host underneath it.
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QCommandLineParser parser;
    parser.setApplicationDescription(u"player shell"_qs);
    parser.addHelpOption();
    QCommandLineOption shot(u"shot"_qs, u"grab the chrome to FILE then exit"_qs, u"file"_qs);
    QCommandLineOption size(u"size"_qs, u"WxH"_qs, u"wh"_qs, u"1282x723"_qs);
    QCommandLineOption at(u"at"_qs, u"screen X,Y of the video window"_qs, u"xy"_qs, u"120,60"_qs);
    QCommandLineOption bg(u"bg"_qs, u"background image path (style-only mode)"_qs, u"path"_qs);
    QCommandLineOption open(u"open"_qs, u"media file to load on start"_qs, u"path"_qs);
    QCommandLineOption diag(u"diag"_qs, u"show the mpv diagnostic line"_qs);
    QCommandLineOption start(u"start"_qs, u"start playback at N seconds"_qs, u"sec"_qs);
    QCommandLineOption full(u"fullscreen"_qs, u"start the host window in fullscreen"_qs);
    QCommandLineOption root(u"root"_qs, u"media root to catalogue (repeatable)"_qs, u"path"_qs);
    QCommandLineOption db(u"db"_qs, u"catalogue database path"_qs, u"path"_qs);
    QCommandLineOption scanOnly(u"scan-only"_qs, u"scan the roots, print stats and exit"_qs);
    QCommandLineOption mode(u"mode"_qs, u"player | library"_qs, u"m"_qs, u"player"_qs);
    QCommandLineOption rootsFile(u"roots-file"_qs, u"UTF-8 file, one media root per line"_qs, u"path"_qs);
    QCommandLineOption durOf(u"duration-of"_qs, u"print the container duration of FILE and exit"_qs,
                             u"path"_qs);
    parser.addOption(shot);
    parser.addOption(size);
    parser.addOption(at);
    parser.addOption(bg);
    parser.addOption(open);
    parser.addOption(diag);
    parser.addOption(start);
    parser.addOption(full);
    parser.addOption(root);
    parser.addOption(db);
    parser.addOption(scanOnly);
    parser.addOption(mode);
    parser.addOption(rootsFile);
    parser.addOption(durOf);
    parser.process(app);

    // Standalone so the reader can be cross-checked against ffprobe per file.
    if (parser.isSet(durOf)) {
        qInfo("DURFILE %.6f", readDuration(QFileInfo(parser.value(durOf)).absoluteFilePath()));
        return 0;
    }

    // One window per user: a second launch passes its file to the one already up
    // and quits. Dev paths opt out - a probe or a --scan-only run is meant to be
    // its own process, and --db means "this is a different catalogue on purpose".
    const QString openArg = parser.isSet(open)
        ? QFileInfo(parser.value(open)).absoluteFilePath() : QString();
    const bool allowMulti = qEnvironmentVariableIsSet("PLAYERUI_MULTI")
            || parser.isSet(scanOnly) || parser.isSet(db);
    SingleInstance single;
    if (!single.start(allowMulti, openArg, parser.value(mode))) {
        qInfo("SINGLE second launch handed %s to the running window and exits",
              qPrintable(openArg));
        return 0;
    }

    const QStringList wh = parser.value(size).split(u'x');
    const int w = wh.size() == 2 ? wh.at(0).toInt() : 1282;
    const int h = wh.size() == 2 ? wh.at(1).toInt() : 723;
    const QStringList xy = parser.value(at).split(u',');
    const int atX = xy.size() == 2 ? xy.at(0).toInt() : 120;
    const int atY = xy.size() == 2 ? xy.at(1).toInt() : 60;

    Library library;
    LibraryModel libraryModel(&library);
    // The catalogue belongs to the user, not to the build tree: next to the exe
    // it disappears on a rebuild or a moved folder, which reads as "the library
    // forgot everything".
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dbPath = parser.isSet(db) ? parser.value(db)
        : dataDir + u"/library.db"_qs;
    if (!parser.isSet(db)) {
        QDir().mkpath(dataDir);
        const QString legacy = QCoreApplication::applicationDirPath() + u"/library.db"_qs;
        if (!QFileInfo::exists(dbPath) && QFileInfo::exists(legacy)) {
            if (QFile::copy(legacy, dbPath))
                qInfo("LIBRARY migrated %s -> %s", qPrintable(legacy), qPrintable(dbPath));
            else
                qWarning("LIBRARY migration failed: %s", qPrintable(legacy));
        }
    }
    library.open(dbPath);
    qInfo("LIBRARY db=%s roots=%d", qPrintable(dbPath), library.roots().size());
    QStringList rootList = parser.values(root);
    // Non-ASCII roots cannot survive .bat/.cmd codepage round-trips, so read
    // them from a UTF-8 list instead.
    if (parser.isSet(rootsFile)) {
        QFile rf(parser.value(rootsFile));
        if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&rf);
            ts.setEncoding(QStringConverter::Utf8);
            while (!ts.atEnd()) {
                const QString line = ts.readLine().trimmed();
                if (!line.isEmpty() && !line.startsWith(u'#'))
                    rootList << (QFileInfo::exists(line) ? QFileInfo(line).absoluteFilePath() : line);
            }
        } else {
            qWarning("cannot read roots file %s", qPrintable(parser.value(rootsFile)));
        }
    }
    for (const QString &r : rootList)
        library.addRoot(QUrl::fromLocalFile(QFileInfo(r).absoluteFilePath()));

    // Any library-mode start walks the stored roots. Gating this on a
    // command-line root left the page empty for everyone whose roots came from
    // the folder picker instead.
    if (parser.value(mode) == u"library"_qs)
        library.scan();

    if (parser.isSet(scanOnly)) {
        library.scan();
        const QList<MediaRow> rows = library.rows(1000);
        int shown = 0;
        for (const MediaRow &r : rows) {
            if (r.parsed)
                ++shown;
        }
        qInfo("PARSED %d/%d media rows carry a resolved title+year", shown, rows.size());
        return 0;
    }

    // Durations come from the container headers, read on the GUI thread: a few
    // kilobytes per file, no decoder and no second media core involved, so the
    // walk reports how many it could read and the library shows them as they land.
    auto pumpDurations = [&library] { library.fillMissingDurations(2000); };
    QObject::connect(&library, &Library::scanned, &library, pumpDurations);
    // A --mode library startup has already scanned by this point, so that first
    // scan's signal went out before the connection above existed. Queued, so the
    // window is on screen before the catalogue is walked again.
    QMetaObject::invokeMethod(&app, pumpDurations, Qt::QueuedConnection);

    VideoHost host;
    if (!host.create({ atX, atY, w, h }))
        return 2;

    PlayerCore player(reinterpret_cast<qintptr>(host.hwnd()), parser.isSet(full));
    ThumbStrip thumbs;
    // Cover stills share the hidden core with the scrub strip, one job at a time,
    // and are capped per scan so the strip always gets the core back first.
    auto pumpCovers = [&library, &thumbs] { thumbs.covers(library.pathsNeedingCover(20)); };
    QObject::connect(&library, &Library::scanned, &library, pumpCovers);
    QObject::connect(&thumbs, &ThumbStrip::coverReady, &library,
                     [&library](const QString &path, const QString &file) {
                         library.setPoster(path, file);
                     });
    QMetaObject::invokeMethod(&app, pumpCovers, Qt::QueuedConnection);
    QObject::connect(&host, &VideoHost::closeRequested, &app, &QCoreApplication::quit);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(u"playerCore"_qs, &player);
    engine.rootContext()->setContextProperty(u"videoHost"_qs, &host);
    engine.rootContext()->setContextProperty(u"thumbs"_qs, &thumbs);
    engine.rootContext()->setContextProperty(u"library"_qs, &library);
    engine.rootContext()->setContextProperty(u"libraryModel"_qs, &libraryModel);
    engine.rootContext()->setContextProperty(u"single"_qs, &single);
    engine.rootContext()->setContextProperty(u"shellMode"_qs, parser.value(mode));
    engine.rootContext()->setContextProperty(u"shellAutoPick"_qs,
        qEnvironmentVariableIsSet("PLAYERUI_AUTOPICK"));
    engine.rootContext()->setContextProperty(u"shellKeepUi"_qs,
        qEnvironmentVariableIsSet("PLAYERUI_KEEPUI"));
    engine.rootContext()->setContextProperty(u"shellDiag"_qs, parser.isSet(diag));
    engine.rootContext()->setContextProperty(u"shellWidth"_qs, w);
    engine.rootContext()->setContextProperty(u"shellHeight"_qs, h);
    const QString bgPath = parser.value(bg);
    engine.rootContext()->setContextProperty(u"shellBg"_qs,
        bgPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(QFileInfo(bgPath).absoluteFilePath()));

    engine.loadFromModule(u"Playerui"_qs, u"Main"_qs);
    if (engine.rootObjects().isEmpty())
        return -1;

    // The chrome is a translucent tool window pinned above the mpv host window.
    auto *chrome = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QWindow *hostSurface = QWindow::fromWinId(reinterpret_cast<WId>(host.hwnd()));
    chrome->setFlags(Qt::FramelessWindowHint | Qt::Tool);
    // The clear colour is set from Main.qml; doing it here too would silently
    // override whatever the QML side asks for.
    chrome->setTransientParent(hostSurface);
    chrome->resize(w, h);
    chrome->setGeometry(clientRect(host.hwnd()));
    chrome->show();

    // Qt will not owner-rank a window it did not create, and mpv raises its own
    // host when the VO initialises, which undoes a one-shot z-order pin. Make the
    // chrome an *owned* window of the host so Windows keeps it above.
    auto pinAboveHost = [&host, chrome] {
        HWND ch = reinterpret_cast<HWND>(chrome->winId());
        SetWindowLongPtrW(ch, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(host.hwnd()));
        // Inserting "after the host" would put the chrome *below* it; HWND_TOP
        // plus the owner link is what keeps the chrome above its owner.
        SetWindowPos(ch, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (qEnvironmentVariableIsSet("PLAYERUI_TOPMOST"))
            SetWindowPos(ch, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    pinAboveHost();

    auto *quitter = new CloseQuitter(&app);
    chrome->installEventFilter(quitter);

    // mpv replaces the host's window procedure, so neither our WM_SIZE nor our
    // WM_CLOSE handler ever runs. Poll instead: it also catches mpv resizing the
    // host on its own.
    QTimer watchdog;
    watchdog.setInterval(60);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&host, chrome, pinAboveHost] {
        if (!IsWindow(host.hwnd())) {
            QCoreApplication::quit();
            return;
        }
        // Windows leaves Qt::Tool windows out of the owner's minimisation
        // cascade, so the chrome has to follow the host's iconic state by hand.
        // QWindow::isVisible() is not usable as the check here - Qt had already
        // cached the chrome as hidden while its HWND was still on screen - so
        // both sides are read from Win32.
        HWND ch = reinterpret_cast<HWND>(chrome->winId());
        const bool iconic = IsIconic(host.hwnd()) != 0;
        if ((IsIconic(ch) != 0) != iconic)
            ShowWindow(ch, iconic ? SW_MINIMIZE : SW_RESTORE);
        if (iconic)
            return;
        // mpv resizes the host itself (fullscreen, its own WM_SIZE), so the
        // chrome has to follow the host's client rect rather than the CLI size.
        const QRect c = clientRect(host.hwnd());
        if (chrome->geometry() != c) {
            chrome->setGeometry(c);
            pinAboveHost();
            if (GetForegroundWindow() == host.hwnd())
                chrome->requestActivate();
        }
    });
    watchdog.start();

    QObject::connect(&host, &VideoHost::resized, chrome, [&host, chrome, pinAboveHost] {
        if (!IsIconic(host.hwnd())) {
            chrome->setGeometry(clientRect(host.hwnd()));
            pinAboveHost();
        }
    });
    QObject::connect(&host, &VideoHost::raised, chrome, [&host, chrome, pinAboveHost] {
        pinAboveHost();
        // mpv brings its host to the foreground on its own (fullscreen does it),
        // which takes the keyboard away from the window that owns the shortcuts.
        if (GetForegroundWindow() == host.hwnd())
            chrome->requestActivate();
    });

    // Must come after the sync connections, or the chrome never hears about it.
    if (parser.isSet(full))
        host.toggleFullscreen();

    const QRect r = clientRect(host.hwnd());
    qInfo("HOSTRECT %d %d %d %d", r.x(), r.y(), r.width(), r.height());
    qInfo("HOSTHWND %lld CHROMEHWND %lld", (qint64)(intptr_t)host.hwnd(),
          (qint64)(intptr_t)chrome->winId());
    host.activate();
    pinAboveHost();
    // The chrome, not the host, owns the keyboard: every Shortcut below lives in
    // the QML window, and mpv's own input is switched off.
    chrome->requestActivate();
    // mpv raises its host while bringing the VO up, which can outrank the chrome.
    QTimer::singleShot(1500, chrome, pinAboveHost);

    if (!parser.value(open).isEmpty()) {
        if (parser.isSet(start))
            player.setStartAt(parser.value(start).toDouble());
        player.load(QUrl::fromLocalFile(QFileInfo(parser.value(open)).absoluteFilePath()).toString());
    }

    if (parser.isSet(shot)) {
        const int hold = parser.isSet(open) ? 4000 : 1200;
        QTimer::singleShot(hold, chrome, [chrome, out = parser.value(shot)] {
            const QImage img = chrome->grabWindow();
            QDir().mkpath(QFileInfo(out).absolutePath());
            if (!img.save(out))
                qWarning("shot save failed: %s", qPrintable(out));
            else
                qInfo("shot %dx%d -> %s", img.width(), img.height(), qPrintable(out));
            QCoreApplication::quit();
        });
    }

    const int rc = app.exec();
    hostSurface->destroy();
    delete hostSurface;
    return rc;
}
