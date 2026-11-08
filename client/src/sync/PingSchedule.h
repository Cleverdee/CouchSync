// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ClockSync.h"

#include <array>
#include <cstddef>
#include <optional>

namespace couchsync {

// Adaptive clock-sync ping schedule (docs/PROTOCOL.md, "Clock sync"). Pure: the owner
// sends the pings and runs the timer, this only says how long to wait.
//
//   * after welcome: kBurst pings kBurstMs apart;
//   * then, while the clock isn't isReady(): one ping every kCatchUpMs, at most kMaxCatchUp
//     of them (so the worst case stays far below the server's 20 msgs/s limit);
//   * then one ping every kSteadyMs;
//   * a pong whose RTT spiked (> kSpikeFactor * best + kSpikeSlackMs) asks for one extra ping
//     kFollowUpMs later, at most one per kSteadyMs.
class PingSchedule {
public:
    static constexpr int kBurst = 8;
    static constexpr int kBurstMs = 50;
    static constexpr int kCatchUpMs = 100;
    static constexpr int kMaxCatchUp = 30;
    static constexpr int kSteadyMs = 5000;
    static constexpr int kFollowUpMs = 500;
    static constexpr int kMaxOutstanding = 3;
    static constexpr double kSpikeFactor = 3.0;
    static constexpr double kSpikeSlackMs = 50;

    void reset() { *this = PingSchedule{}; }

    // Call after each ping was sent. Returns the delay (ms) until the next one.
    int onPingSent(const ClockSync &clock)
    {
        latch(clock);
        ++m_sent;
        if (m_sent < kBurst)
            return kBurstMs;
        if (!m_ready) {
            if (m_catchUp < kMaxCatchUp) {
                ++m_catchUp;
                return kCatchUpMs;
            }
            m_exhausted = true;
        }
        return kSteadyMs;
    }

    // Call for every pong received (even a rejected one).
    void pongReceived()
    {
        if (m_pongs < m_sent)
            ++m_pongs;
    }
    // After the burst, hold further pings while several are still unanswered: during a TCP stall
    // they'd pile up and arrive at the server in one clump (rate_limited), and add nothing.
    bool mayPing() const { return m_sent < kBurst || m_sent - m_pongs <= kMaxOutstanding; }

    // Call after a pong was added to the clock (`rtt` = that pong's RTT). Returns a delay (ms)
    // after which an extra ping should go out, if the pong was a spike; the owner applies it
    // only if the next ping is further away.
    std::optional<int> onPong(const ClockSync &clock, double rtt, double nowMs)
    {
        latch(clock);
        if (m_sent < kBurst || rtt <= kSpikeFactor * clock.rtt() + kSpikeSlackMs)
            return std::nullopt;
        if (m_haveFollowUp && nowMs - m_lastFollowUpMs < kSteadyMs)
            return std::nullopt;
        m_haveFollowUp = true;
        m_lastFollowUpMs = nowMs;
        return kFollowUpMs;
    }

    // The clock may be used for playback decisions: it was ready (latched, so the window's
    // best RTT moving around can't make a working client flap back to `waiting`), or, once
    // the catch-up pings ran out, any estimate (a terrible network must not block forever).
    bool usable(const ClockSync &clock) const { return m_ready || (m_exhausted && clock.hasEstimate()); }

    // True once, the first time the catch-up pings ran out without a ready clock.
    bool takeExhaustedWarning()
    {
        const bool w = m_exhausted && !m_warned;
        m_warned = m_warned || m_exhausted;
        return w;
    }

private:
    void latch(const ClockSync &clock) { m_ready = m_ready || clock.isReady(); }

    bool m_ready = false;
    int m_sent = 0;
    int m_pongs = 0;
    int m_catchUp = 0;
    bool m_exhausted = false;
    bool m_warned = false;
    bool m_haveFollowUp = false;
    double m_lastFollowUpMs = 0;
};

// Counts the messages a client sent in the last second (all types), so extra pings can yield to
// status/intents and the connection stays under the server's 20 msgs/s limit.
class SendRate {
public:
    static constexpr double kWindowMs = 1000;
    static constexpr std::size_t kPingBudget = 15;  // pings only go out while fewer were sent in the window

    void note(double nowMs)
    {
        m_stamps[m_next] = nowMs;
        m_next = (m_next + 1) % m_stamps.size();
        if (m_n < m_stamps.size())
            ++m_n;
    }
    std::size_t inWindow(double nowMs) const
    {
        std::size_t c = 0;
        for (std::size_t i = 0; i < m_n; ++i)
            if (nowMs - m_stamps[i] < kWindowMs)
                ++c;
        return c;
    }
    bool pingAllowed(double nowMs) const { return inWindow(nowMs) < kPingBudget; }
    void reset() { m_n = m_next = 0; }

private:
    std::array<double, 32> m_stamps{};
    std::size_t m_n = 0, m_next = 0;
};

}  // namespace couchsync
