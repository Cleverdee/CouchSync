// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>

namespace couchsync {

// Milliseconds from std::chrono::steady_clock (arbitrary epoch). This is the only
// local clock the protocol allows (docs/PROTOCOL.md, "Clock sync").
double steadyNowMs();

// NTP-style clock offset estimator. Pure: callers feed timestamps, nothing here
// reads a clock.
//
//   t0 = client send (local steady ms)   t1 = server receive (server ms)
//   t2 = server send (server ms)         t3 = client receive (local steady ms)
//   offset = ((t1 - t0) + (t2 - t3)) / 2     (server - local)
//   rtt    = (t3 - t0) - (t2 - t1)
//
// Keeps the last kWindow samples; the estimate is the offset of the sample with
// the lowest RTT (least queueing, so least asymmetry error).
class ClockSync {
public:
    static constexpr std::size_t kWindow = 16;

    // Returns false (and ignores the sample) if the values are non-finite or the
    // RTT is negative.
    bool addSample(double t0, double t1, double t2, double t3);

    bool hasEstimate() const { return m_count > 0; }
    double offset() const { return m_offset; }  // ms, server - local; 0 until hasEstimate()
    double rtt() const { return m_rtt; }        // ms, RTT of the selected sample
    double serverNow(double localSteadyMs) const { return localSteadyMs + m_offset; }
    std::size_t sampleCount() const { return m_count; }
    void reset();

private:
    struct Sample {
        double offset = 0;
        double rtt = 0;
    };
    void recompute();

    std::array<Sample, kWindow> m_samples{};
    std::size_t m_count = 0;
    std::size_t m_next = 0;  // ring buffer write index
    double m_offset = 0;
    double m_rtt = 0;
};

}  // namespace couchsync
