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

    // Video rows whose card still has no picture yet. Audio is left out: a track's
    // cover is its album art, which belongs to the music library.
    Q_INVOKABLE QStringList pathsNeedingCover(int limit = 20);
    Q_INVOKABLE void setPoster(const QString &path, const QString &file);

    // Baseline filename estimator: release-style names, "Title (2008)" folders,
    // dot/underscore separators. Returns false when nothing credible was found.
    static bool parseName(const QString &fileName, const QString &parentDir,
                          QString *title, int *year);

signals:
    void scanned();
    void rowChanged();
    // The header's 时长 N/M is filled by the backfill as well as by a scan, so it
    // needs a signal of its own; re-emitting scanned() would re-enter the pump.
    void statsChanged();

private:
    void ensureSchema();
    void walk(const QString &dirPath, int depth);
    void record(const QString &absPath, const QString &kind, qint64 size, qint64 mtime);
    QStringList pathsWithoutDuration(int limit);
    void countDurations();
    void setDuration(const QString &path, double seconds);

    QSqlDatabase m_db;
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
