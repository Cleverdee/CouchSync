#pragma once

#include <QQuickFramebufferObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

struct MpvCore;

// Qt Quick item that plays video through libmpv's OpenGL render API.
class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY hasMediaChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    Q_PROPERTY(double subDelay READ subDelay NOTIFY subDelayChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)

public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    Renderer *createRenderer() const override;

    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool paused() const { return m_paused; }
    bool hasMedia() const { return m_hasMedia; }
    bool playing() const { return m_hasMedia && !m_paused; }
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    QVariantList tracks() const { return m_tracks; }
    double subDelay() const { return m_subDelay; }
    QString mediaTitle() const { return m_mediaTitle; }

    // `path` may be a local path, a file:// URL or another URL.
    Q_INVOKABLE void loadFile(const QString &path);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void seek(double seconds); // absolute
    Q_INVOKABLE void setVolume(double volume);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void setAudioTrack(int id);
    Q_INVOKABLE void setSubTrack(int id); // 0 = off
    Q_INVOKABLE void addSubtitle(const QString &path);
    Q_INVOKABLE void adjustSubDelay(double delta);
    Q_INVOKABLE void resetSubDelay();
    // Forwards a Qt key event to mpv's input system. Returns true if it was sent.
    Q_INVOKABLE bool sendKey(int qtKey, int modifiers, const QString &text);
    Q_INVOKABLE void sendKeyName(const QString &mpvKey);
    // Persists the playback position of the current file (watch-later entry).
    Q_INVOKABLE void saveWatchLater();

    // Drains the mpv event queue; runs on the Qt thread (queued from mpv's wakeup callback).
    void processEvents();
    // Called on the Qt thread once the renderer has created mpv's render context.
    void onRenderContextReady();

signals:
    void positionChanged();
    void durationChanged();
    void pausedChanged();
    void hasMediaChanged();
    void playingChanged();
    void volumeChanged();
    void mutedChanged();
    void tracksChanged();
    void subDelayChanged();
    void mediaTitleChanged();
    void shutdown(); // mpv quit (e.g. the "q" key); the UI should close

private:
    void command(const QStringList &args);
    void setPropertyString(const char *name, const QString &value);
    void setPropertyDouble(const char *name, double value);

    std::shared_ptr<MpvCore> m_core;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_paused = false;
    bool m_hasMedia = false;
    double m_volume = 100.0;
    bool m_muted = false;
    QVariantList m_tracks;
    double m_subDelay = 0.0;
    QString m_mediaTitle;
    bool m_shutdown = false;
    bool m_renderReady = false;
    QString m_pendingFile; // loadfile before the render context exists leaves mpv with no video output
};
