// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncEngine.h"

#include <algorithm>
#include <cmath>

namespace couchsync {

SyncEngine::SyncEngine(SyncConfig config)
    : m_cfg(config), m_predictedSeekMs(std::clamp(config.initialSeekMs, 0.0, config.maxSeekMs))
{
    m_window.reserve(static_cast<std::size_t>(std::max(1, m_cfg.medianWindow)));
}

void SyncEngine::reset()
{
    *this = SyncEngine(m_cfg);
}

bool SyncEngine::applyState(const RoomState &state)
{
    if (m_state && state.seq <= m_state->seq)
        return false;
    m_state = state;
    resetWindow();
    m_jumpPending = true;
    m_localIntentPending = false;
    return true;
}

void SyncEngine::noteLocalIntent(double serverNowMs)
{
    m_localIntentPending = true;
    m_localIntentAt = serverNowMs;
    resetWindow();
}

void SyncEngine::cancelLocalIntent()
{
    m_localIntentPending = false;
    m_jumpPending = true;  // snap back promptly if the rejected intent moved us far
}

std::optional<double> SyncEngine::msUntilScheduledStart(double serverNowMs) const
{
    if (m_state && m_state->playing && m_state->serverTime > serverNowMs)
        return m_state->serverTime - serverNowMs;
    return std::nullopt;
}

void SyncEngine::resetWindow()
{
    m_window.clear();
}

double SyncEngine::median() const
{
    std::vector<double> v = m_window;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void SyncEngine::issueSeek(SyncActions &a, double target, double nowMs)
{
    a.seekTo = std::max(0.0, target);
    m_seekInFlight = true;
    m_seekSeen = false;
    m_seekIssuedAt = nowMs;
    m_correcting = false;
    resetWindow();
}

void SyncEngine::emitSpeed(SyncActions &a, const PlayerSample &s, double target)
{
    const bool neutral = target == m_state->rate;
    if (std::abs(target - s.currentSpeed) > m_cfg.speedEpsilon || (neutral && s.currentSpeed != target))
        a.setSpeed = target;
}

void SyncEngine::neutralSpeed(SyncActions &a, const PlayerSample &s)
{
    m_correcting = false;
    emitSpeed(a, s, m_state->rate);
}

SyncActions SyncEngine::tick(const PlayerSample &s)
{
    const double now = s.serverNowMs;
    SyncActions a;

    // --- seek tracking -------------------------------------------------------
    if (m_seekInFlight) {
        const double elapsed = now - m_seekIssuedAt;
        if (s.seeking) {
            m_seekSeen = true;
        } else if (m_seekSeen || elapsed >= m_cfg.seekStartGraceMs) {
            if (m_seekSeen) {
                const double a_ = m_cfg.seekEmaAlpha;
                m_predictedSeekMs = std::clamp(a_ * elapsed + (1 - a_) * m_predictedSeekMs, 0.0, m_cfg.maxSeekMs);
            }
            m_seekInFlight = false;
            m_lastSeekDoneAt = now;
            resetWindow();
        }
    } else if (m_prevSeeking && !s.seeking) {
        // A seek we didn't issue (user / local intent) just finished.
        m_lastSeekDoneAt = now;
        resetWindow();
    }
    m_prevSeeking = s.seeking;

    if (m_prevPaused && *m_prevPaused != s.paused)
        resetWindow();
    m_prevPaused = s.paused;

    const bool waiting = !s.fileLoaded || s.seeking || s.buffering || m_seekInFlight;
    a.status = waiting ? MemberStatus::Waiting : MemberStatus::Ready;

    if (!m_state || !s.fileLoaded)
        return a;
    const RoomState &st = *m_state;

    if (m_localIntentPending && now - m_localIntentAt > m_cfg.localIntentTimeoutMs) {
        m_localIntentPending = false;
        m_jumpPending = true;
    }

    const double expected = st.expectedPosition(now);
    const double raw = s.localPos - expected;
    const bool canSeek = !m_seekInFlight && !s.seeking;

    if (m_localIntentPending) {
        a.driftS = raw;
        neutralSpeed(a, s);
        return a;
    }

    // --- room paused ---------------------------------------------------------
    if (!st.playing) {
        neutralSpeed(a, s);
        if (!s.paused)
            a.setPaused = true;
        const double off = s.localPos - st.position;
        a.driftS = off;
        // One positional seek per state: if mpv can't land exactly, don't loop.
        if (canSeek && std::abs(off) > m_cfg.pausedToleranceS && m_pausedAlignSeq != st.seq) {
            issueSeek(a, st.position, now);
            m_pausedAlignSeq = st.seq;
        }
        m_jumpPending = false;
        return a;
    }

    // --- room playing, player paused: scheduled start ------------------------
    if (s.paused) {
        neutralSpeed(a, s);
        if (now < st.serverTime) {
            // Hold paused at state.position until the scheduled start.
            const double off = s.localPos - st.position;
            a.driftS = off;
            if (canSeek && std::abs(off) > m_cfg.seekThresholdS)
                issueSeek(a, st.position, now);
            m_jumpPending = false;
            return a;
        }
        a.setPaused = false;
        a.driftS = raw;
        if (canSeek && std::abs(raw) > m_cfg.seekThresholdS)
            issueSeek(a, st.expectedPosition(now + m_predictedSeekMs), now);
        m_jumpPending = false;
        return a;
    }

    // --- room playing, player playing: drift control -------------------------
    if (s.seeking || s.buffering || m_seekInFlight) {
        a.driftS = raw;
        neutralSpeed(a, s);
        return a;
    }
    if (m_lastSeekDoneAt && now - *m_lastSeekDoneAt < m_cfg.postSeekHoldMs) {
        a.driftS = raw;
        return a;  // speed is already neutral from the seek
    }

    m_window.push_back(raw);
    if (static_cast<int>(m_window.size()) > m_cfg.medianWindow)
        m_window.erase(m_window.begin());
    const double drift = median();
    a.driftS = drift;

    const bool jump = m_jumpPending && std::abs(raw) > m_cfg.jumpSeekThresholdS;
    m_jumpPending = false;
    if (jump || std::abs(drift) > m_cfg.seekThresholdS) {
        // Aim where the room will be when the seek completes.
        issueSeek(a, st.expectedPosition(now + m_predictedSeekMs), now);
        neutralSpeed(a, s);
        return a;
    }

    if (static_cast<int>(m_window.size()) < m_cfg.minSpeedSamples)
        return a;  // hold the current speed until the window has enough samples

    const double mag = std::abs(drift);
    if (!m_correcting && mag > m_cfg.speedStartS)
        m_correcting = true;
    else if (m_correcting && mag < m_cfg.speedStopS)
        m_correcting = false;

    double target = st.rate;
    if (m_correcting) {
        const double delta = std::clamp(m_cfg.speedGainPerS * drift, -m_cfg.maxSpeedDelta, m_cfg.maxSpeedDelta);
        target = st.rate * (1.0 - delta);
    }
    emitSpeed(a, s, target);
    return a;
}

}  // namespace couchsync
