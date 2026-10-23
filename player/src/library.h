#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

struct MediaRow {
    qint64 id = 0;
    QString kind;      // "video" | "audio"
    QString title;
    int year = 0;
    QString path;
    qint64 size = 0;
    double duration = 0.0;
    bool parsed = false;
    QString poster;
    // Where the user stopped this file, from the resume table. 0 when never played.
    double pos = 0.0;
};

// Owns the SQLite catalogue and the directory walk. Deliberately offline: the
// metadata resolver is a separate step so scan accuracy can be measured without
// a network or an API key.
class Library final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int mediaCount READ mediaCount NOTIFY scanned)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY scanned)
    Q_PROPERTY(int skippedCount READ skippedCount NOTIFY scanned)
    Q_PROPERTY(int unparsedCount READ unparsedCount NOTIFY scanned)
    Q_PROPERTY(int durationCount READ durationCount NOTIFY statsChanged)
    Q_PROPERTY(qint64 lastScanMs READ lastScanMs NOTIFY scanned)

public:
    explicit Library(QObject *parent = nullptr);

    int mediaCount() const { return m_mediaCount; }
    int fileCount() const { return m_fileCount; }
    int skippedCount() const { return m_skipped; }
    int unparsedCount() const { return m_unparsed; }
    int durationCount() const { return m_durationCount; }
    qint64 lastScanMs() const { return m_scanMs; }

    Q_INVOKABLE bool open(const QString &dbPath);
    // The folder picker answers with a url and Qt 6.11 has no toLocalFile() on that
    // type in QML, so a url is the one accepted shape here.
    Q_INVOKABLE void addRoot(const QUrl &folder);
    Q_INVOKABLE QStringList roots() const;
    Q_INVOKABLE void scan();
    Q_INVOKABLE QList<MediaRow> rows(int limit = 500) const;

    // Rows left over from an older catalogue, whose duration was never read.
    // Returns how many were filled; 0 seconds means "asked, and the container had
    // no answer", and the row is marked so the next run does not retry it.
    Q_INVOKABLE int fillMissingDurations(int limit = 2000);

    // A disc folder has no cheap header duration, so the catalogue learns it the
    // first time the disc is actually opened.
    Q_INVOKABLE void noteDuration(const QString &path, double seconds);

    // Where the user stopped a file, and the still of the frame they saw there.
    // Keyed by path rather than by the media row, so a film opened straight from
    // disk - never scanned in - still comes back to its minute.
    Q_INVOKABLE void saveProgress(const QString &path, double pos, double dur,
                                  const QString &shot = QString());
    // The most recently stopped file as {path, pos, dur, shot}, or empty when
    // nothing has ever been played.
    Q_INVOKABLE QVariantMap lastPlayed() const;
    // Where the resume cover for this file lives: beside the catalogue, named after
    // the path the same way the cover stills are.
    Q_INVOKABLE QString resumeShotPath(const QString &path) const;
    // Resolve what the `shot` column holds into an existing file, or an empty string.
    QString resumeShotRead(const QString &stored) const;

    // Video rows whose card still has no picture yet. Audio is left out: a track's
    // cover is its album art, which belongs to the music library.
    Q_INVOKABLE QStringList pathsNeedingCover(int limit = 20);
    Q_INVOKABLE void setPoster(const QString &path, const QString &file);

    // The card's menu. `hideMedia` clears the display only: the file is not touched and
    // stays exactly where it is, and scanning its folder again brings the card back.
    Q_INVOKABLE void hideMedia(qint64 id);
    // Renames the library row, not the file on disk. The `custom` mark is what keeps the
    // next scan from deriving the title from the file name again.
    Q_INVOKABLE void setTitle(qint64 id, const QString &title);
    // Asks the shell to open the file's folder with that file selected.
    Q_INVOKABLE bool revealInFolder(const QString &path);
    // The name the library shows for this file, but only where the user typed one:
    // otherwise mpv's own title (the file name) is just as right, and cheaper. The
    // player page reads this so a renamed card is called the same thing in both places.
    Q_INVOKABLE QString titleForPath(const QString &path) const;
    // Every playable file sitting in the same folder as the one that is playing, in the
    // order the shell lists them, each with what the catalogue knows about it: the
    // duration, the cover and - only where the user typed one - the name. The right-hand
    // playlist reads this, so a file that is also in the library is called and drawn the
    // same way in both places, and one that never got scanned still shows up with
    // nothing but its file name. Takes PlayerCore's own url, because getting from that
    // back to the file system needs QUrl, which QML has no equivalent of.
    Q_INVOKABLE QVariantList itemsBeside(const QString &mediaUrl) const;

    // ---- the user's own playlists ------------------------------------------
    // A list of paths and an order. Nothing here copies a file or adds it to the
    // catalogue: the dock plays the path, and the catalogue is only ever consulted to
    // show a duration and a cover it happens to know.
    Q_INVOKABLE QVariantList playlists() const;
    Q_INVOKABLE QVariantList playlistItems(qint64 playlistId) const;
    // Returns the new row's id, or -1. An empty name keeps the default one.
    Q_INVOKABLE qint64 createPlaylist(const QString &name = QString());
    Q_INVOKABLE void renamePlaylist(qint64 id, const QString &name);
    Q_INVOKABLE void deletePlaylist(qint64 id);
    // Appends, in the order given, skipping what is already in there.
    Q_INVOKABLE void addToPlaylist(qint64 id, const QStringList &paths);
    Q_INVOKABLE void removeFromPlaylist(qint64 id, const QString &path);
    // Trades a row's place with its neighbour: -1 up, +1 down, and a no-op at either end.
    Q_INVOKABLE void movePlaylistItem(qint64 id, const QString &path, int dir);

    // Baseline filename estimator: release-style names, "Title (2008)" folders,
    // dot/underscore separators. Returns false when nothing credible was found.
    static bool parseName(const QString &fileName, const QString &parentDir,
                          QString *title, int *year);

signals:
    void scanned();
    void rowChanged();
    // A card left or entered the grid without a scan (hidden, renamed).
    void rowsChanged();
    // Anything about the user's lists changed: a list created, named, deleted, or an
    // entry added, removed, reordered. One signal for both the list of playlists and the
    // entries of the open one, because the dock reads them together.
    void playlistsChanged();
    // The header's 时长 N/M is filled by the backfill as well as by a scan, so it
    // needs a signal of its own; re-emitting scanned() would re-enter the pump.
    void statsChanged();

private:
    void ensureSchema();
    void walk(const QString &dirPath, int depth);
    void record(const QString &absPath, const QString &kind, qint64 size, qint64 mtime);
    // The media row owning this path when the user named it, and clearing the display
    // mark when a scan walks over the file again.
    qint64 customMediaIdOfPath(const QString &absPath) const;
    void unhidePath(const QString &absPath);
    QStringList pathsWithoutDuration(int limit);
    void countDurations();
    void setDuration(const QString &path, double seconds);

    QSqlDatabase m_db;
    QString m_dir;      // where the catalogue file lives: the resume covers go beside it
    QStringList m_roots;
    QSet<QString> m_videoExt;
    QSet<QString> m_audioExt;
    QSet<QString> m_skipDirs;
    int m_mediaCount = 0;
    int m_fileCount = 0;
    int m_skipped = 0;
    int m_unparsed = 0;
    int m_durationCount = 0;
    int m_withDuration = 0;
    double m_durationMs = 0.0;
    qint64 m_scanMs = 0;
    qint64 m_seenFiles = 0;
};

class LibraryModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        YearRole,
        KindRole,
        PathRole,
        DurationRole,
        ParsedRole,
        PosterRole,
        PosRole,
        // The card menu acts on a row by its media id, not by its position: a
        // hidden card changes the numbering under the grid.
        IdRole,
    };

    explicit LibraryModel(Library *lib, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void reload();

private:
    Library *m_lib = nullptr;
    QList<MediaRow> m_rows;
    // Durations land one file at a time; a reset per file would repaint the whole
    // grid hundreds of times over a scan, so they are coalesced.
    QTimer m_coalesce;
};
