// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>

namespace couchsync {

// Milliseconds from std::chrono::steady_clock (arbitrary epoch). This is the only
// local clock the protocol allows (docs/PROTOCOL.md, "Clock sync").
double steadyNowMs();

// NTP-style clock offset estimator with skew (frequency) tracking. Pure: callers
// feed timestamps, nothing here reads a clock.
//
//   t0 = client send (local steady ms)   t1 = server receive (server ms)
//   t2 = server send (server ms)         t3 = client receive (local steady ms)
//   offset = ((t1 - t0) + (t2 - t3)) / 2     (server - local)
//   rtt    = (t3 - t0) - (t2 - t1)
//
// The two clocks run at slightly different rates (tens to hundreds of ppm), so the
// offset is a line, not a constant: offset(t) = a + b * (t - tm), t = local time.
// Each sample is placed at its local midpoint (t0 + t3) / 2 and the line is fitted
// by weighted least squares over the recent samples:
//
// * Weight by queueing: the offset error of a sample lies within +-excess/2, where
//   excess = its RTT minus the best RTT in the window, and with one-sided queueing
//   it is near that bound, so w = 1 / (floor^2 + excess^2 / 4). The floor is at
//   least kNoiseFloorMs and grows with the spread of the best quartile of RTTs
//   (the best RTT is itself an extreme value, so it must not be trusted alone),
//   and, in a second pass, with the residual scatter of the better half of the
//   samples (a low RTT doesn't guarantee a symmetric path).
//   The variance is also capped at rtt^2 / 4, since the error can never exceed
//   +-rtt/2. Low-RTT samples dominate; delay spikes (hundreds of ms, typically
//   one-sided) get a weight near zero, and anything over kMaxExcessMs is ignored.
// * Prior on the slope (b ~ N(0, kSlopePriorPpm)): the LS slope is shrunk by
//   S / (S + 1/prior^2), S = sum of p * (t - tp)^2 with the precisions p capped at
//   1/kSlopeMinErrMs^2 (RTT-based weights can be overconfident). With samples
//   spanning only a short time (the initial ping burst) b stays ~0 and the fit is
//   a weighted mean (like the old min-RTT pick, but averaging); as the time span
//   grows the data take over smoothly, with no mode switch. |b| <= kMaxSlopePpm.
// * Head-of-line blocking: TCP holds everything behind a delayed segment, so after
//   a stall several pongs (e.g. the initial burst, sent 50 ms apart) arrive
//   back-to-back, all skewed the same way. A sample whose gap to the previous one
//   got compressed to under kQueuedGapFraction of the send gap on the way up (t1)
//   or down (t3) is marked as queued: it gets kQueuedWeight and is left out of the
//   floor's spread.
// * No steps on eviction: weights fade out over the oldest kTaperSamples ranks of
//   the window and over the last kTaperAgeMs before kMaxAgeMs, so a sample has
//   ~no influence by the time it is dropped.
class ClockSync {
public:
    static constexpr std::size_t kWindow = 32;       // samples kept (~160 s at one ping per 5 s)
    static constexpr double kMaxAgeMs = 180000;      // ...and at most this old (vs the newest sample)
    static constexpr double kTaperAgeMs = 40000;     // weight ramps down over the last 40 s before kMaxAgeMs
    static constexpr std::size_t kTaperSamples = 10; // ...and (quadratically) over the 10 oldest ranks of a full window
    static constexpr double kNoiseFloorMs = 1.0;     // minimum assumed error of a best-RTT sample
    static constexpr double kSpreadFactor = 0.29;    // ...and at least this * (q25 RTT - best RTT)
    static constexpr std::size_t kMinScatterSamples = 4; // samples needed for the residual-scatter floor
    static constexpr double kQueuedGapFraction = 0.1; // see "Head-of-line blocking"
    static constexpr double kQueuedWeight = 0.01;    // weight factor for a head-of-line-blocked sample
    static constexpr double kMaxExcessMs = 250;      // samples with rtt > best + this are ignored
    static constexpr double kSlopePriorPpm = 300;    // prior on the skew (1 sigma)
    static constexpr double kSlopeMinErrMs = 5;      // no sample counts as better than this for the slope's shrinkage
    static constexpr double kMaxSlopePpm = 1000;     // |skew| clamp

    // Returns false (and ignores the sample) if the values are non-finite or the
    // RTT is negative.
    bool addSample(double t0, double t1, double t2, double t3);

    bool hasEstimate() const { return m_count > 0; }
    // ms, server - local, at local steady time localMs. 0 until hasEstimate().
    double offsetAt(double localMs) const { return m_ym + m_b * (localMs - m_tm); }
    // Offset at the newest sample's midpoint (for display/logging).
    double offset() const { return offsetAt(m_newestMid); }
    double skewPpm() const { return m_b * 1e6; }  // estimated server-vs-local rate difference
    double rtt() const { return m_rtt; }          // ms, lowest RTT in the window
    double serverNow(double localSteadyMs) const { return localSteadyMs + offsetAt(localSteadyMs); }
    std::size_t sampleCount() const { return m_count; }
    void reset();

private:
    struct Sample {
        double mid = 0;  // local ms, (t0 + t3) / 2
        double offset = 0;
        double rtt = 0;
        bool queued = false;  // arrived in a head-of-line-blocked train (see addSample)
    };
    void recompute();

    std::array<Sample, kWindow> m_samples{};
    std::size_t m_count = 0;
    std::size_t m_next = 0;  // ring buffer write index
    double m_tm = 0;         // fit: offset(t) = m_ym + m_b * (t - m_tm)
    double m_ym = 0;
    double m_b = 0;
    double m_rtt = 0;
    double m_newestMid = 0;
    bool m_havePrev = false;  // previous accepted sample's timestamps, for queueing detection
    double m_prevT0 = 0, m_prevT1 = 0, m_prevT3 = 0;
};

}  // namespace couchsync
