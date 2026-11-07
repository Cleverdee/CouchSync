// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SyncEngine.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <optional>

class MpvItem;
class RoomClient;

// Glue between RoomClient (network), couchsync::SyncEngine (drift controller) and MpvItem.
//
// Local vs engine-issued actions (critical, see docs/PROTOCOL.md "Intents"):
//   The user can pause/seek through the controls bar, mpv keybindings or clicks; the
//   engine does the same through MpvItem. All of them end up as mpv property changes /
//   events, so we classify each change as it comes back from mpv:
//   * pause: before the controller issues setPaused(x) it records an "expected pause"
//     (value + 2 s deadline), but only if mpv's current value differs. A `pause` property
//     change matching that record is engine-issued and swallowed; any other change is a
//     local user action -> intent.play / intent.pause{time-pos}.
//   * seek: before the controller issues a seek it records an "own seek" (target + 3 s
//     deadline). MPV_EVENT_SEEK inside that window is attributed to the engine, otherwise
//     it is a local seek. At MPV_EVENT_PLAYBACK_RESTART the final time-pos is checked
//     against the own-seek target (> 1.5 s off means the user seeked on top of ours), and
//     a local seek becomes intent.seek{time-pos} (coalesced, 80 ms trailing).
//   * speed: the controller remembers the speed it last commanded; a different value in
//     the room (user pressed [ ] etc.) is put back immediately.
//   * file loading (START_FILE .. first PLAYBACK_RESTART) is ignored: the initial/resume
//     seek is neither engine nor user.
//   Local intents also call SyncEngine::noteLocalIntent(); error not_allowed calls
//   cancelLocalIntent() so a rejected action reverts through the engine (whose own
//   commands are again swallowed by the records above, so nothing echoes).
class SyncController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(MpvItem *player READ player WRITE setPlayer NOTIFY playerChanged)
    Q_PROPERTY(RoomClient *room READ room WRITE setRoom NOTIFY roomChanged)
    Q_PROPERTY(double driftMs READ driftMs NOTIFY infoChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY infoChanged)
    Q_PROPERTY(int countdown READ countdown NOTIFY countdownChanged) // 0 = none
    Q_PROPERTY(QString mediaWarning READ mediaWarning NOTIFY mediaWarningChanged)
    Q_PROPERTY(bool logSync READ logSync WRITE setLogSync NOTIFY logSyncChanged)

public:
    explicit SyncController(QObject *parent = nullptr);

    MpvItem *player() const { return m_player; }
    RoomClient *room() const { return m_room; }
    void setPlayer(MpvItem *player);
    void setRoom(RoomClient *room);
    double driftMs() const { return m_driftMs; }
    bool ready() const { return m_ready; }
    int countdown() const { return m_countdown; }
    QString mediaWarning() const { return m_mediaWarning; }
    bool logSync() const;
    void setLogSync(bool on);

signals:
    void playerChanged();
    void roomChanged();
    void infoChanged();
    void countdownChanged();
    void mediaWarningChanged();
    void logSyncChanged();
    void toast(const QString &text);

private:
    bool inRoom() const;
    void onJoined();
    void onLeft();
    void onRoomState();
    void onNotAllowed();
    void onTick();
    void onPlayerPaused();
    void onPlayerSpeed();
    void onFileStarted();
    void onFileLoadedChanged();
    void onSeekStarted();
    void onPlaybackRestart(double position);
    void localIntentNow();
    void flushSeekIntent();
    void armStartTimer(double serverNow);
    void setCountdown(int c);
    void checkMedia();
    void maybeSendMedia();

    MpvItem *m_player = nullptr;
    RoomClient *m_room = nullptr;
    couchsync::SyncEngine m_engine;

    QTimer m_tick;
    QTimer m_startTimer;
    QTimer m_seekIntentTimer;
    double m_seekIntentPos = 0;

    // Engine-issued command records (see class comment).
    std::optional<bool> m_expectPause;
    double m_expectPauseUntil = 0;
    bool m_lastPaused = false;
    bool m_ownSeekPending = false;
    double m_ownSeekTarget = 0;
    double m_ownSeekUntil = 0;
    enum class SeekKind { None, Own, Local, Ignore };
    SeekKind m_seekKind = SeekKind::None;
    bool m_seeking = false;
    bool m_seekObserved = false; // a tick saw seeking == true for the engine's in-flight seek
    double m_seekingSince = 0;
    bool m_loading = false;
    double m_expectedSpeed = 1.0;

    // status reporting
    std::optional<bool> m_lastStatusReady;
    double m_lastStatusAt = 0;
    double m_lastLogAt = 0;

    // media.set / duration check
    int m_loadSerial = 0;
    int m_mediaSentFor = -1;
    int m_loadSerialAtJoin = 0;

    double m_driftMs = 0;
    bool m_ready = false;
    int m_countdown = 0;
    bool m_countdownArmed = false;
    QString m_mediaWarning;
};
