// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RoomState.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace couchsync {

// All thresholds of the drift controller. Defaults are the values from the
// docs/DESIGN.md Decisions log (2026-11-08, "Drift correction has a buffer").
struct SyncConfig {
    double speedStartS = 0.040;       // start speed correction when |drift| > this
    double speedStopS = 0.015;        // stop (speed = rate exactly) when |drift| < this
    double speedGainPerS = 0.5;       // k in speed = rate * (1 - clamp(k * drift, +-maxSpeedDelta))
    double maxSpeedDelta = 0.05;      // +-5 %
    double seekThresholdS = 2.0;      // seek when |smoothed drift| > this
    double jumpSeekThresholdS = 0.25;  // seek on the first tick after a state change when |raw drift| > this (0.25-0.5 s by speed took 5-10 s)
    double initialSeekMs = 300;       // initial predicted seek duration
    double seekEmaAlpha = 0.3;        // EMA weight of a new seek-duration measurement
    double maxSeekMs = 2000;          // predicted seek duration is clamped to [0, maxSeekMs]
    double postSeekHoldMs = 500;      // no drift evaluation for this long after a seek completes
    int medianWindow = 5;             // drift smoothing: median of the last N samples
    int minSpeedSamples = 3;          // samples needed before speed decisions (seek decisions need 1)
    double pausedToleranceS = 0.040;  // paused room: seek to state.position if further off than this
    double speedEpsilon = 0.001;      // only emit setSpeed when it changes by more than this
    double localIntentTimeoutMs = 1000;  // see SyncEngine::noteLocalIntent
    double seekStartGraceMs = 1000;   // see "Integration contract" below
};

// One observation of the local player. Every input carries its own time; the
// engine never reads a clock.
struct PlayerSample {
    double serverNowMs = 0;  // ClockSync::serverNow(steadyNowMs()) at sampling time
    double localPos = 0;     // s, room timeline (mpv time-pos for now)
    bool paused = false;     // mpv `pause` property (user pause), NOT core-idle
    bool seeking = false;    // a seek is in progress (see contract below)
    bool buffering = false;  // mpv paused-for-cache
    double currentSpeed = 1.0;  // mpv `speed`
    bool fileLoaded = false;
};

enum class MemberStatus { Ready, Waiting };

struct SyncActions {
    std::optional<bool> setPaused;
    std::optional<double> seekTo;    // s, absolute; use an exact seek
    std::optional<double> setSpeed;
    MemberStatus status = MemberStatus::Waiting;
    std::optional<double> driftS;    // local - expected (smoothed when available); for `status`
    bool hasCommands() const { return setPaused || seekTo || setSpeed; }
};

// Pure playback-sync controller (no Qt, no clocks, no I/O).
//
// Integration contract
// --------------------
// * Call applyState() for every `state` message, tick() on a timer (~100 ms) and
//   apply the returned commands to mpv in this order: setPaused, seekTo, setSpeed.
//   Send `status` from SyncActions::status / driftS (on change and every 1 s).
// * Set PlayerSample::seeking = true from the moment you issue a seek command
//   until mpv reports MPV_EVENT_PLAYBACK_RESTART (mpv's `seeking` property alone
//   can lag the command). Seek durations are measured from the tick that issued
//   seekTo until the first tick that sees seeking == false, so ticking right
//   after PLAYBACK_RESTART makes the measurement more precise. If the engine
//   never sees seeking == true within seekStartGraceMs it assumes the seek
//   finished and does not use it for the seek-time estimate.
// * Commands are idempotent; re-emitting the same setPaused because the mpv
//   property hasn't updated yet is harmless.
// * msUntilScheduledStart() lets you arm a single-shot timer so the unpause at a
//   scheduled start isn't late by up to one tick period.
// * Call reset() when leaving/joining a room (seq numbering restarts).
class SyncEngine {
public:
    explicit SyncEngine(SyncConfig config = {});

    // Applies a room state. Ignored (returns false) if seq <= the last applied seq.
    // An applied state resets the drift window, arms the jump check (seek if the
    // raw drift exceeds jumpSeekThresholdS on the next tick) and ends a pending
    // local intent.
    bool applyState(const RoomState &state);

    SyncActions tick(const PlayerSample &sample);

    // Optimistic local intents (docs/PROTOCOL.md: clients apply their own intent
    // right away, then reconcile with the resulting state).
    //
    // Call this right after the user plays/pauses/seeks locally and the intent
    // has been sent. Until the next applied state (any seq above the current one)
    // or localIntentTimeoutMs elapses, tick() issues no play/pause, seek or speed
    // correction commands (speed is put back to state.rate), so the engine
    // doesn't drag the player back to the pre-intent room state while the server
    // round trip is in flight. Status and drift are still reported.
    //
    // Reconciliation is then ordinary control against the new state; e.g. the
    // server schedules a seek-while-playing 150 ms ahead, which shows up as a
    // small positive drift that speed correction removes.
    // If the intent is rejected (error not_allowed), call cancelLocalIntent(); the
    // next tick then pulls the player back to the room state. If you can't, the
    // timeout does the same. Caveat: if another member's state is applied before
    // ours, the engine follows it until ours arrives (a brief tug, by design:
    // last intent processed by the server wins).
    void noteLocalIntent(double serverNowMs);
    void cancelLocalIntent();
    bool localIntentPending() const { return m_localIntentPending; }

    void reset();

    const std::optional<RoomState> &state() const { return m_state; }
    const SyncConfig &config() const { return m_cfg; }
    double predictedSeekMs() const { return m_predictedSeekMs; }
    bool seekInFlight() const { return m_seekInFlight; }
    bool correcting() const { return m_correcting; }
    // Time until state.serverTime if the room is playing with a future start; else nullopt.
    std::optional<double> msUntilScheduledStart(double serverNowMs) const;

private:
    void resetWindow();
    void issueSeek(SyncActions &a, double target, double nowMs);
    void neutralSpeed(SyncActions &a, const PlayerSample &s);
    void emitSpeed(SyncActions &a, const PlayerSample &s, double target);
    double median() const;

    SyncConfig m_cfg;
    std::optional<RoomState> m_state;

    std::vector<double> m_window;  // raw drift samples, oldest first
    bool m_correcting = false;
    bool m_jumpPending = false;

    bool m_seekInFlight = false;
    bool m_seekSeen = false;
    double m_seekIssuedAt = 0;
    std::optional<double> m_lastSeekDoneAt;
    double m_predictedSeekMs;

    bool m_prevSeeking = false;
    std::optional<bool> m_prevPaused;
    std::optional<std::int64_t> m_pausedAlignSeq;  // seq for which a paused-position seek was issued

    bool m_localIntentPending = false;
    double m_localIntentAt = 0;
};

}  // namespace couchsync
