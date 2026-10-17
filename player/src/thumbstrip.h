#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QTimer>

struct mpv_handle;

// Frame previews for the scrub bar: a strip of stills spread over the movie's
// duration, written once into a cache directory and reused on every later watch.
// The same core also cuts the library's cover stills (one frame at t=0 each).
//
// It runs a second, hidden libmpv core on an off-screen window. That core is
// created lazily and the queue keeps it to one job at a time - the measured dose
// for a whole movie is 10 loads, against the 500 that made the process deadlock
// in an injected IME at exit.
class ThumbStrip final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList frames READ frames NOTIFY framesChanged)
    Q_PROPERTY(bool building READ building NOTIFY buildingChanged)

public:
    explicit ThumbStrip(QObject *parent = nullptr);
    ~ThumbStrip() override;

    QStringList frames() const { return m_urls; }
    bool building() const { return m_stripRunning; }

    Q_INVOKABLE void build(const QString &url, double duration, int count = 10);
    // One cover still per library path, queued behind whatever the scrub bar needs.
    // A path that already has a file in the cache never costs a load.
    Q_INVOKABLE void covers(const QStringList &paths);
    Q_INVOKABLE void clear();

signals:
    void framesChanged();
    void buildingChanged();
    void coverReady(QString mediaPath, QString file);

private:
    enum Stage { Idle, WaitingLoad, WaitingPresent, WaitingFile };

    struct Job {
        QString key;          // the library path this still belongs to (empty for the strip)
        QString source;       // local file handed to loadfile
        QList<double> times;  // one entry per frame to grab
        QString dir;          // where the jpgs land
        bool cover = false;
    };

    bool ensureCore();
    QString target() const;
    void startStrip(const Job &job);
    void abortActive();
    void startNext();
    void startFrame();
    void takeShot();
    void pollTick();
    void finish();
    void drain();

    mpv_handle *m_mpv = nullptr;
    QTimer m_poll;
    QElapsedTimer m_clock;
    QStringList m_urls;
    QQueue<Job> m_queue;
    Job m_active;
    QSet<QString> m_coverKeys;   // asked for this session, so a dead file is not retried
    QString m_stripUrl;          // file:// of the movie the strip belongs to
    double m_stripDuration = 0.0;
    int m_index = 0;
    int m_polls = 0;
    Stage m_stage = Idle;
    bool m_activeLive = false;
    bool m_stripRunning = false;
    qint64 m_wid = 0;       // the off-screen host window
    void *m_guard = nullptr;  // QPointer<ThumbStrip> handed to mpv's wakeup callback
};
