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
#include <QIcon>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>

#include <QTimer>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QFile>
#include <QTextStream>
#include <QUrl>

#include <QWindow>
#include <QSurfaceFormat>

#include <clocale>
#include <cmath>

// Above this, a mismatch between the window and the film is a real letterbox and
// must stay; at or below it, it is a rounding hairline we can snap away.
constexpr int kHairlineMaxPx = 2;

int main(int argc, char *argv[])
{
    // libmpv requires LC_NUMERIC="C"; set it now so the shell does not have to
    // remember once the core is wired in.
    std::setlocale(LC_NUMERIC, "C");

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(u"Playerui"_qs);
    QGuiApplication::setOrganizationName(u"local"_qs);
    // The taskbar and the alt-tab strip take this one, and it is the same .ico that
    // appPlayerui.rc puts into the exe, so the two surfaces cannot disagree: every size is
    // a frame of its own, 16 and 24 from the simplified figure (four 13-unit holes land on
    // 1.6 px each and stop reading as film). The SVG is deliberately not registered - a
    // scalable entry counts as a match for every size in Qt's lookup, so it would win the
    // 16 px request and hand back the four-hole cut.
    app.setWindowIcon(QIcon(u":/app/app.ico"_qs));

    QCommandLineParser parser;
    parser.setApplicationDescription(u"player shell"_qs);
    parser.addHelpOption();
    QCommandLineOption shot(u"shot"_qs, u"grab the chrome to FILE then exit"_qs, u"file"_qs);
    QCommandLineOption shotAt(u"shot-at"_qs, u"grab N milliseconds after the window is up "
                                          u"(for the startup frames: who is painted first)"_qs,
                              u"ms"_qs);
    QCommandLineOption size(u"size"_qs, u"WxH"_qs, u"wh"_qs, u"1282x723"_qs);
    QCommandLineOption at(u"at"_qs, u"screen X,Y of the video window (default: centred on "
                                  u"the work area)"_qs, u"xy"_qs, u""_qs);
    QCommandLineOption bg(u"bg"_qs, u"background image path (style-only mode)"_qs, u"path"_qs);
    QCommandLineOption open(u"open"_qs, u"media file to load on start"_qs, u"path"_qs);
    QCommandLineOption diag(u"diag"_qs, u"show the mpv diagnostic line"_qs);
    QCommandLineOption start(u"start"_qs, u"start playback at N seconds"_qs, u"sec"_qs);
    QCommandLineOption paused(u"paused"_qs, u"hold the first frame: for measuring the chrome and the transitions"_qs);
    QCommandLineOption full(u"fullscreen"_qs, u"start the host window in fullscreen"_qs);
    QCommandLineOption root(u"root"_qs, u"media root to catalogue (repeatable)"_qs, u"path"_qs);
    QCommandLineOption db(u"db"_qs, u"catalogue database path"_qs, u"path"_qs);
    QCommandLineOption scanOnly(u"scan-only"_qs, u"scan the roots, print stats and exit"_qs);
    QCommandLineOption mode(u"mode"_qs, u"player | library"_qs, u"m"_qs, u"player"_qs);
    QCommandLineOption rootsFile(u"roots-file"_qs, u"UTF-8 file, one media root per line"_qs, u"path"_qs);
    QCommandLineOption durOf(u"duration-of"_qs, u"print the container duration of FILE and exit"_qs,
                             u"path"_qs);
    parser.addOption(shot);
    parser.addOption(shotAt);
    parser.addOption(size);
    parser.addOption(at);
    parser.addOption(bg);
    parser.addOption(open);
    parser.addOption(diag);
    parser.addOption(start);
    parser.addOption(paused);
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
    // No `--at` means: open in the middle of the screen's work area. The fixed 120,60 it
    // used to fall back to put the window in a corner on every panel, and on a 4K one
    // that corner is nowhere near where the eye starts. The work area rather than the
    // full geometry, so the task bar counts as space the window cannot sit in; and
    // clamped, because a window bigger than the screen would centre into negative
    // coordinates and lose its top-left corner off the panel.
    int atX = 120;
    int atY = 60;
    if (xy.size() == 2) {
        atX = xy.at(0).toInt();
        atY = xy.at(1).toInt();
    } else if (QScreen *scr = QGuiApplication::primaryScreen()) {
        const QRect avail = scr->availableGeometry();
        atX = qMax(avail.x(), avail.x() + (avail.width() - w) / 2);
        atY = qMax(avail.y(), avail.y() + (avail.height() - h) / 2);
    }
    qInfo("PLACE %s at %d,%d size %dx%d", xy.size() == 2 ? "--at" : "centred", atX, atY, w, h);

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

    // One window for everything: the QML window carries the caption, the picture
    // (VideoItem) and the chrome. VideoHost only adopts its HWND afterwards.
    VideoHost host;
    PlayerCore player;
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

    // The film name goes into the system caption through Window.title in Main.qml -
    // there is only one window now, so Qt is the only writer.

    // What to open on this run: the command line first, then wherever the user
    // stopped last time. Resolved before the engine so QML can be told the number
    // and draw the "continue" affordance from it.
    QString startPath = parser.value(open);
    double resumeAt = -1.0;
    QString resumeShot;
    if (startPath.isEmpty() && parser.value(mode) == u"player"_qs) {
        const QVariantMap last = library.lastPlayed();
        const QString lp = last.value(u"path"_qs).toString();
        if (!lp.isEmpty() && QFileInfo::exists(lp)) {
            startPath = lp;
            resumeAt = last.value(u"pos"_qs).toDouble();
            resumeShot = last.value(u"shot"_qs).toString();
        }
    }

    // Writing the record back is QML's job, not this one's: the frame that has to
    // become the cover lives in VideoItem's texture, and the write only belongs once
    // that copy is on disk. See the timer and the snapshot handler in Main.qml.

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
    // >= 0 means: this run came back to a film rather than being asked for one, and
    // it is sitting at that second waiting for the user to say go.
    engine.rootContext()->setContextProperty(u"shellResumeAt"_qs, resumeAt);
    // The still of the frame they left on, if one was saved. Painted over the picture
    // until the film's own first frame reaches the screen: measured, the window is up
    // for 320 ms before anything is drawn there.
    engine.rootContext()->setContextProperty(u"shellResumeShot"_qs,
        (resumeAt > 0.0 && QFileInfo::exists(resumeShot))
            ? QUrl::fromLocalFile(QFileInfo(resumeShot).absoluteFilePath()) : QUrl());
    engine.rootContext()->setContextProperty(u"shellWidth"_qs, w);
    engine.rootContext()->setContextProperty(u"shellHeight"_qs, h);
    engine.rootContext()->setContextProperty(u"shellX"_qs, atX);
    engine.rootContext()->setContextProperty(u"shellY"_qs, atY);
    const QString bgPath = parser.value(bg);
    engine.rootContext()->setContextProperty(u"shellBg"_qs,
        bgPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(QFileInfo(bgPath).absoluteFilePath()));

    engine.loadFromModule(u"Playerui"_qs, u"Main"_qs);
    if (engine.rootObjects().isEmpty())
        return -1;

    // The QML window is the player: caption, picture and chrome in one surface.
    // Its size and position come from shellWidth/shellHeight/shellX/shellY so it is
    // created at the right rect rather than resized onto the screen.
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!win)
        return -1;
    host.adopt(win);

    // A window whose height is not an exact multiple of the film's aspect leaves a
    // 1 px hairline bar at the top and bottom (1282x723 is not 16:9: 1282*9/16 =
    // 721.125, so only 721 rows of picture exist and one row of black is left at
    // each edge). Snap the client height to the exact value - but only when the fix
    // is a couple of pixels, so a real letterbox (2.40 in 16:9, 281 px of it) stays.
    auto snapHairline = [&player, &host] {
        const double a = player.videoAspect();
        if (a <= 0.0 || host.fullscreen() || host.mini())
            return;
        const QRect c = host.clientGeometry();
        const int want = qRound(c.width() / a);
        if (want == c.height() || qAbs(want - c.height()) > kHairlineMaxPx)
            return;
        qInfo("HAIRLINE %dx%d -> %dx%d  aspect=%.6f  delta=%d",
              c.width(), c.height(), c.width(), want, a, c.height() - want);
        host.setClientSize(c.width(), want);
    };
    QObject::connect(&player, &PlayerCore::videoAspectChanged, &host, snapHairline);
    // Leaving fullscreen and leaving mini restore the old rect, and the aspect has
    // not changed by then, so nothing else would ever fire again. Both signals go out
    // after the rect has been committed.
    QObject::connect(&host, &VideoHost::fullscreenChanged, &host, snapHairline);
    QObject::connect(&host, &VideoHost::miniChanged, &host, [&host, snapHairline] {
        if (!host.mini())
            snapHairline();
    });

    if (parser.isSet(full))
        host.toggleFullscreen();

    const QRect c = host.clientGeometry();
    qInfo("CLIENTRECT %d %d %d %d", c.x(), c.y(), c.width(), c.height());
    qInfo("HWND %lld", (qint64)(intptr_t)win->winId());
    win->requestActivate();

    if (!startPath.isEmpty()) {
        if (resumeAt > 0.0)
            player.setStartAt(resumeAt);
        else if (parser.isSet(start))
            player.setStartAt(parser.value(start).toDouble());
        // Queued inside PlayerCore until VideoItem reports its render context.
        player.load(QUrl::fromLocalFile(QFileInfo(startPath).absoluteFilePath()).toString());
        // Set after the request: the core honours it as the file comes up, so the
        // frame under the chrome is the same one every time it is measured. A resumed
        // film waits on its breakpoint for the same reason the measuring path waits
        // on its first frame.
        if (resumeAt > 0.0 || parser.isSet(paused))
            player.setPropertyString("pause", "yes");
    }

    if (parser.isSet(shot)) {
        const int hold = parser.isSet(shotAt) ? parser.value(shotAt).toInt()
                             : (parser.isSet(open) ? 4000 : 1200);
        QTimer::singleShot(hold, win, [win, out = parser.value(shot)] {
            // One window, so this now captures the picture *and* the chrome - the
            // old grab could only see the chrome floating over mpv's surface.
            const QImage img = win->grabWindow();
            QDir().mkpath(QFileInfo(out).absolutePath());
            if (!img.save(out))
                qWarning("shot save failed: %s", qPrintable(out));
            else
                qInfo("shot %dx%d -> %s", img.width(), img.height(), qPrintable(out));
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
