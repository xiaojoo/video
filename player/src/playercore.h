#pragma once

#include <QObject>
#include <QRect>
#include <QString>
#include <QVariantList>
#include <QtGlobal>

struct mpv_handle;

// Owns one libmpv core. Every mpv call happens on the GUI thread: the core is
// serialised behind one internal lock anyway, so a second thread buys nothing
// and would only risk two threads inside mpv_wait_event().
class PlayerCore final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY hasMediaChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    // The speaker glyph has to say "muted", so the shell needs the state and not just
    // the command that flips it.
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY chapterChanged)
    Q_PROPERTY(QString chapterTitle READ chapterTitle NOTIFY chapterChanged)
    Q_PROPERTY(QVariantList chapterStarts READ chapterStarts NOTIFY chapterChanged)
    Q_PROPERTY(QStringList chapterTitles READ chapterTitlesList NOTIFY chapterChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList subTracks READ subTracks NOTIFY tracksChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)
    Q_PROPERTY(QString mediaUrl READ mediaUrl NOTIFY mediaUrlChanged)
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    Q_PROPERTY(QString hwdecCurrent READ hwdecCurrent NOTIFY hwdecCurrentChanged)
    Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY videoCodecChanged)
    Q_PROPERTY(QString voDriven READ voDriven NOTIFY voDrivenChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(bool fullscreenActive READ fullscreenActive NOTIFY fullscreenActiveChanged)
    // The display aspect mpv settled on (after SAR and any override). Verified on
    // this build: video-params/dar does not exist, video-params/aspect does.
    Q_PROPERTY(double videoAspect READ videoAspect NOTIFY videoAspectChanged)

public:
    explicit PlayerCore(QObject *parent = nullptr);
    ~PlayerCore() override;

    // For VideoItem, which owns the render context built on this core.
    struct mpv_handle *handle() const { return m_mpv; }
    // Nothing may be loaded before the render context exists: the libmpv VO
    // answers "No render context set" and the window stays black. Until the item
    // says it is ready, load() remembers the request and replays it.
    Q_INVOKABLE void markRenderReady();

    bool hasMedia() const { return m_hasMedia; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool paused() const { return m_paused; }
    bool muted() const { return m_muted; }
    int chapter() const { return m_chapter; }
    QString chapterTitle() const { return m_chapterTitle; }
    QVariantList chapterStarts() const { return m_chapterStarts; }
    QStringList chapterTitlesList() const { return m_chapterTitles; }    QString mediaTitle() const { return m_discTitle.isEmpty() ? m_mediaTitle : m_discTitle; }
    QString mediaUrl() const { return m_mediaUrl; }
    double volume() const { return m_volume; }
    double speed() const { return m_speed; }
    QVariantList audioTracks() const { return m_audioTracks; }
    QVariantList subTracks() const { return m_subTracks; }
    QString hwdecCurrent() const { return m_hwdecCurrent; }
    QString videoCodec() const { return m_videoCodec; }
    QString voDriven() const { return m_voDriven; }

    Q_INVOKABLE void load(const QString &url);
    Q_INVOKABLE void setStartAt(double seconds);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void seekTo(double seconds);
    Q_INVOKABLE void setVolume(double percent);
    Q_INVOKABLE void setSpeed(double rate);
    // kind is "audio" or "sub"; id 0 on a subtitle means "turn them off".
    Q_INVOKABLE void selectTrack(const QString &kind, int id);
    Q_INVOKABLE void setFullscreen(bool on);
    Q_INVOKABLE void cycleProperty(const QString &name);
    Q_INVOKABLE void setPropertyString(const QString &name, const QString &value);
    Q_INVOKABLE void setPropertyInt(const QString &name, qint64 value);
    Q_INVOKABLE bool fullscreen() const { return m_fullscreen; }
    double videoAspect() const { return m_videoAspect; }
    bool fullscreenActive() const { return m_fsActive; }

signals:
    void hasMediaChanged();
    // The film reached its last frame on its own. Distinct from the media going away,
    // because `loadfile ... replace` (what the playlist queue uses) also ends the current
    // file - and that must not advance the queue a second time.
    void endOfFile();
    void positionChanged();
    void durationChanged();
    void pausedChanged();
    void mutedChanged();
    void chapterChanged();
    void mediaTitleChanged();
    void mediaUrlChanged();
    void volumeChanged();
    void speedChanged();
    void tracksChanged();
    void hwdecCurrentChanged();
    void videoCodecChanged();
    void voDrivenChanged();
    void fullscreenChanged();
    void fullscreenActiveChanged();
    void videoAspectChanged();
    void loadFailed(const QString &why);

private:
    static void onWakeup(void *ctx);
    void drainEvents();
    void loadBluray(const QString &discDir);
    void onPropertyChange(void *propPtr);
    void refreshChapters();
    void refreshTracks();

    struct mpv_handle *m_mpv = nullptr;
    bool m_hasMedia = false;
    bool m_eof = false;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_paused = false;
    bool m_muted = false;
    double m_videoAspect = 0.0;
    int m_chapter = -1;
    QStringList m_chapterTitles;
    QString m_chapterTitle;
    QVariantList m_chapterStarts;
    QString m_mediaTitle;
    QString m_mediaUrl;
    // A disc folder is titled by its own name, not by whichever clip mpv ended
    // up opening.
    QString m_discTitle;
    // A disc folder is opened as bd:// first; the biggest clip is only used when
    // libbluray refuses the layout, so the fallback has to survive the async
    // END_FILE that tells us the disc route failed.
    QString m_blurayFallback;
    bool m_blurayPending = false;
    double m_volume = 100.0;
    double m_speed = 1.0;
    // Each entry is { id, label, selected }, already narrowed to one kind.
    QVariantList m_audioTracks;
    QVariantList m_subTracks;
    QString m_hwdecCurrent, m_videoCodec, m_voDriven;
    bool m_fullscreen = false;
    bool m_fsActive = false;
    bool m_renderReady = false;
    QString m_pendingUrl;
    void *m_guard = nullptr;
};
