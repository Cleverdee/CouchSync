// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncController.h"

#include "ClockSync.h"
#include "MpvItem.h"
#include "RoomClient.h"
#include "SyncLog.h"

#include <algorithm>
#include <cmath>

using couchsync::MemberStatus;
using couchsync::PlayerSample;
using couchsync::steadyNowMs;
using couchsync::syncLog;

namespace {
constexpr double kExpectPauseMs = 2000;
constexpr double kOwnSeekMs = 3000;
constexpr double kSeekStuckMs = 3000;
constexpr double kOwnSeekLandToleranceS = 1.5;
constexpr double kStatusEveryMs = 1000;
constexpr int kSeekIntentCoalesceMs = 80;
constexpr double kDurationMismatchS = 2.0;
}  // namespace

SyncController::SyncController(QObject *parent) : QObject(parent)
{
    m_tick.setInterval(100);
    connect(&m_tick, &QTimer::timeout, this, &SyncController::onTick);

    m_startTimer.setSingleShot(true);
    m_startTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_startTimer, &QTimer::timeout, this, &SyncController::onTick);

    m_seekIntentTimer.setSingleShot(true);
    connect(&m_seekIntentTimer, &QTimer::timeout, this, &SyncController::flushSeekIntent);
}

bool SyncController::logSync() const
{
    return couchsync::syncLogEnabled();
}

void SyncController::setLogSync(bool on)
{
    if (couchsync::syncLogEnabled() == on)
        return;
    couchsync::syncLogEnabled() = on;
    emit logSyncChanged();
}

bool SyncController::inRoom() const
{
    return m_room && m_room->connected();
}

void SyncController::setPlayer(MpvItem *player)
{
    if (m_player == player)
        return;
    if (m_player)
        m_player->disconnect(this);
    m_player = player;
    if (m_player) {
        m_lastPaused = m_player->paused();
        connect(m_player, &MpvItem::pausedChanged, this, &SyncController::onPlayerPaused);
        connect(m_player, &MpvItem::speedChanged, this, &SyncController::onPlayerSpeed);
        connect(m_player, &MpvItem::fileStarted, this, &SyncController::onFileStarted);
        connect(m_player, &MpvItem::fileLoadedChanged, this, &SyncController::onFileLoadedChanged);
        connect(m_player, &MpvItem::durationChanged, this, [this] {
            checkMedia();
            maybeSendMedia();
        });
        connect(m_player, &MpvItem::seekStarted, this, &SyncController::onSeekStarted);
        connect(m_player, &MpvItem::playbackRestart, this, &SyncController::onPlaybackRestart);
    }
    emit playerChanged();
}

void SyncController::setRoom(RoomClient *room)
{
    if (m_room == room)
        return;
    if (m_room)
        m_room->disconnect(this);
    m_room = room;
    if (m_room) {
        connect(m_room, &RoomClient::joined, this, &SyncController::onJoined);
        connect(m_room, &RoomClient::left, this, &SyncController::onLeft);
        connect(m_room, &RoomClient::stateChanged, this, &SyncController::onRoomState);
        connect(m_room, &RoomClient::notAllowed, this, &SyncController::onNotAllowed);
        connect(m_room, &RoomClient::toast, this, [this](const QString &text) {
            syncLog(QStringLiteral("toast: %1").arg(text));
            emit toast(text);
        });
        connect(m_room, &RoomClient::mediaChanged, this, &SyncController::checkMedia);
        connect(m_room, &RoomClient::membersChanged, this, [this] {
            checkMedia();
            maybeSendMedia();
        });
    }
    emit roomChanged();
}

// ---- room lifecycle ------------------------------------------------------------------

void SyncController::onJoined()
{
    m_engine.reset();
    m_expectPause.reset();
    m_ownSeekPending = false;
    m_seekKind = SeekKind::None;
    m_seeking = false;
    m_expectedSpeed = 1.0;
    m_lastStatusReady.reset();
    m_mediaSentFor = -1;
    m_loadSerialAtJoin = m_loadSerial;
    if (m_player)
        m_lastPaused = m_player->paused();
    onRoomState(); // the welcome state
    m_tick.start();
    checkMedia();
    maybeSendMedia();
}

void SyncController::onLeft()
{
    m_tick.stop();
    m_startTimer.stop();
    m_seekIntentTimer.stop();
    m_engine.reset();
    m_expectPause.reset();
    m_ownSeekPending = false;
    m_seeking = false;
    m_seekKind = SeekKind::None;
    m_expectedSpeed = 1.0;
    if (m_player && std::abs(m_player->speed() - 1.0) > 1e-6)
        m_player->setSpeed(1.0);
    setCountdown(0);
    m_countdownArmed = false;
    m_driftMs = 0;
    m_ready = false;
    emit infoChanged();
    if (!m_mediaWarning.isEmpty()) {
        m_mediaWarning.clear();
        emit mediaWarningChanged();
    }
}

void SyncController::onRoomState()
{
    if (!m_room || !m_room->currentState())
        return;
    const couchsync::RoomState &st = *m_room->currentState();
    if (!m_engine.applyState(st))
        return;
    if (m_room->hasClock()) {
        const auto ms = m_engine.msUntilScheduledStart(m_room->serverNowMs());
        m_countdownArmed = ms && *ms > 1000;
    } else {
        m_countdownArmed = false;
    }
    onTick(); // react right away (jump seek, pause/unpause)
}

void SyncController::onNotAllowed()
{
    m_engine.cancelLocalIntent();
    onTick();
}

// ---- tick ----------------------------------------------------------------------------

void SyncController::onTick()
{
    if (!m_player || !inRoom() || !m_room->hasClock())
        return;
    const double nowLocal = steadyNowMs();
    const double serverNow = m_room->serverNowMs();

    if (m_seeking && nowLocal - m_seekingSince > kSeekStuckMs) {
        m_seeking = false; // PLAYBACK_RESTART never came (e.g. seek on a failed load)
        m_ownSeekPending = false;
    }
    if (m_loading && !m_player->hasMedia())
        m_loading = false;

    PlayerSample s;
    s.serverNowMs = serverNow;
    s.localPos = m_player->currentPosition();
    s.paused = m_player->paused();
    s.seeking = m_seeking;
    if (m_seeking && m_engine.seekInFlight())
        m_seekObserved = true;
    s.buffering = m_player->buffering();
    s.currentSpeed = m_player->speed();
    s.fileLoaded = m_player->fileLoaded() && !m_loading;

    const couchsync::SyncActions a = m_engine.tick(s);

    // Order matters: setPaused, seekTo, setSpeed.
    if (a.setPaused && *a.setPaused != m_player->paused()) {
        m_expectPause = *a.setPaused;
        m_expectPauseUntil = nowLocal + kExpectPauseMs;
        m_player->setPaused(*a.setPaused);
        syncLog(QStringLiteral("action: %1").arg(*a.setPaused ? QStringLiteral("pause") : QStringLiteral("play")));
    }
    if (a.seekTo) {
        m_ownSeekPending = true;
        m_seekObserved = false;
        m_ownSeekTarget = *a.seekTo;
        m_ownSeekUntil = nowLocal + kOwnSeekMs;
        m_seeking = true;
        m_seekingSince = nowLocal;
        m_player->seekExact(*a.seekTo);
        syncLog(QStringLiteral("action: seek %1 (local %2, predicted seek %3 ms)")
                    .arg(*a.seekTo, 0, 'f', 3)
                    .arg(s.localPos, 0, 'f', 3)
                    .arg(m_engine.predictedSeekMs(), 0, 'f', 0));
    }
    if (a.setSpeed) {
        m_expectedSpeed = *a.setSpeed;
        m_player->setSpeed(*a.setSpeed);
        syncLog(QStringLiteral("action: speed %1").arg(*a.setSpeed, 0, 'f', 3));
    }

    // Status: on change and every second.
    const bool ready = (a.status == MemberStatus::Ready);
    const double drift = a.driftS.value_or(0.0);
    if (!m_lastStatusReady || *m_lastStatusReady != ready || nowLocal - m_lastStatusAt >= kStatusEveryMs) {
        m_lastStatusReady = ready;
        m_lastStatusAt = nowLocal;
        m_room->sendStatus(ready, s.localPos, drift);
    }

    // Readout.
    const double driftMs = drift * 1000.0;
    if (std::abs(driftMs - m_driftMs) >= 0.5 || ready != m_ready) {
        m_driftMs = driftMs;
        m_ready = ready;
        emit infoChanged();
    }

    if (couchsync::syncLogEnabled() && nowLocal - m_lastLogAt >= 1000) {
        m_lastLogAt = nowLocal;
        syncLog(QStringLiteral("tick: drift=%1 ms ready=%2 pos=%3 paused=%4 speed=%5 rtt=%6 ms offset=%7 ms")
                    .arg(driftMs, 0, 'f', 1)
                    .arg(ready)
                    .arg(s.localPos, 0, 'f', 3)
                    .arg(s.paused)
                    .arg(s.currentSpeed, 0, 'f', 3)
                    .arg(m_room->rttMs(), 0, 'f', 1)
                    .arg(m_room->offsetMs(), 0, 'f', 1));
    }

    armStartTimer(serverNow);
}

void SyncController::armStartTimer(double serverNow)
{
    const auto ms = m_engine.msUntilScheduledStart(serverNow);
    if (ms && *ms > 0) {
        // Fire just after the scheduled start so the engine sees serverNow > serverTime.
        m_startTimer.start(static_cast<int>(std::ceil(*ms)) + 1);
        if (m_countdownArmed)
            setCountdown(std::clamp(static_cast<int>(std::ceil(*ms / 1000.0)), 1, 9));
        else
            setCountdown(0);
    } else {
        m_startTimer.stop();
        m_countdownArmed = false;
        setCountdown(0);
    }
}

void SyncController::setCountdown(int c)
{
    if (m_countdown == c)
        return;
    m_countdown = c;
    emit countdownChanged();
}

// ---- local action detection ---------------------------------------------------------------

void SyncController::localIntentNow()
{
    if (m_room && m_room->hasClock())
        m_engine.noteLocalIntent(m_room->serverNowMs());
}

void SyncController::onPlayerPaused()
{
    if (!m_player)
        return;
    const bool paused = m_player->paused();
    if (paused == m_lastPaused)
        return;
    m_lastPaused = paused;
    if (!inRoom())
        return;

    const double now = steadyNowMs();
    if (m_expectPause && *m_expectPause == paused && now < m_expectPauseUntil) {
        m_expectPause.reset(); // engine-issued
        return;
    }
    if (!m_player->fileLoaded() || m_loading)
        return;

    // Local user action.
    if (paused) {
        const double pos = m_player->position();
        syncLog(QStringLiteral("local: pause at %1").arg(pos, 0, 'f', 3));
        m_room->sendPause(pos);
    } else {
        syncLog(QStringLiteral("local: play"));
        m_room->sendPlay();
    }
    localIntentNow();
}

void SyncController::onPlayerSpeed()
{
    if (!m_player || !inRoom())
        return;
    if (std::abs(m_player->speed() - m_expectedSpeed) > 0.0005) {
        syncLog(QStringLiteral("speed changed to %1 (expected %2): reverting")
                    .arg(m_player->speed(), 0, 'f', 3)
                    .arg(m_expectedSpeed, 0, 'f', 3));
        m_player->setSpeed(m_expectedSpeed);
    }
}

void SyncController::onFileStarted()
{
    m_loading = true;
}

void SyncController::onFileLoadedChanged()
{
    if (m_player && m_player->fileLoaded())
        ++m_loadSerial;
    checkMedia();
    maybeSendMedia();
}

void SyncController::onSeekStarted()
{
    if (!inRoom())
        return;
    const double now = steadyNowMs();
    m_seeking = true;
    m_seekingSince = now;
    if (m_loading) {
        m_seekKind = SeekKind::Ignore;
        return;
    }
    if (m_ownSeekPending && now < m_ownSeekUntil) {
        m_seekKind = SeekKind::Own;
    } else {
        m_seekKind = SeekKind::Local;
        localIntentNow(); // keep the engine quiet while the user scrubs
    }
}

void SyncController::onPlaybackRestart(double position)
{
    m_loading = false;
    if (!inRoom()) {
        m_seeking = false;
        m_ownSeekPending = false;
        m_seekKind = SeekKind::None;
        return;
    }
    // A short seek can finish before any 100 ms tick saw seeking == true; the engine would then
    // wait out its whole grace period as "waiting" (and the server would autopause the room).
    // So show it one tick with seeking == true, immediately followed by the real one below.
    if (m_seeking && m_engine.seekInFlight() && !m_seekObserved)
        onTick();
    m_seeking = false;
    SeekKind kind = m_seekKind;
    m_seekKind = SeekKind::None;
    if (m_ownSeekPending) {
        // The user may have seeked on top of the engine's seek: then we did not land on the target.
        if (kind == SeekKind::Own && std::abs(position - m_ownSeekTarget) > kOwnSeekLandToleranceS)
            kind = SeekKind::Local;
        m_ownSeekPending = false;
    }
    if (kind == SeekKind::Local) {
        syncLog(QStringLiteral("local: seek to %1").arg(position, 0, 'f', 3));
        m_seekIntentPos = position;
        if (!m_seekIntentTimer.isActive())
            m_seekIntentTimer.start(kSeekIntentCoalesceMs);
    }
    onTick(); // measure the seek duration as precisely as possible
}

void SyncController::flushSeekIntent()
{
    if (!inRoom())
        return;
    m_room->sendSeek(m_seekIntentPos);
    localIntentNow();
}

// ---- media ----------------------------------------------------------------------------------

void SyncController::checkMedia()
{
    QString warning;
    if (inRoom() && m_player && !m_room->isHost()) {
        const double md = m_room->mediaDuration();
        const double ld = m_player->duration();
        if (md > 0 && ld > 0 && std::abs(md - ld) > kDurationMismatchS)
            warning = QStringLiteral("Different file? Durations differ");
    }
    if (warning != m_mediaWarning) {
        m_mediaWarning = warning;
        emit mediaWarningChanged();
        if (!warning.isEmpty())
            emit toast(warning);
    }
}

void SyncController::maybeSendMedia()
{
    if (!inRoom() || !m_player || !m_room->isHost() || !m_player->fileLoaded())
        return;
    const double dur = m_player->duration();
    if (dur <= 0 || m_mediaSentFor == m_loadSerial)
        return;
    // media.set resets the room to position 0 / paused: only send it for a file loaded after
    // joining, or when the room has no media yet (never for a host handover with the same file).
    if (!m_room->media().isEmpty() && m_loadSerial == m_loadSerialAtJoin)
        return;
    m_mediaSentFor = m_loadSerial;
    m_room->sendMediaSet(m_player->fileName(), dur);
}
