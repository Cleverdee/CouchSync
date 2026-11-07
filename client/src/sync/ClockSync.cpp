// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClockSync.h"

#include <chrono>
#include <cmath>

namespace couchsync {

double steadyNowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool ClockSync::addSample(double t0, double t1, double t2, double t3)
{
    if (!std::isfinite(t0) || !std::isfinite(t1) || !std::isfinite(t2) || !std::isfinite(t3))
        return false;
    const double rtt = (t3 - t0) - (t2 - t1);
    if (rtt < 0)
        return false;
    m_samples[m_next] = Sample{((t1 - t0) + (t2 - t3)) / 2.0, rtt};
    m_next = (m_next + 1) % kWindow;
    if (m_count < kWindow)
        ++m_count;
    recompute();
    return true;
}

void ClockSync::recompute()
{
    // Walk oldest -> newest and use <= so the newest sample wins RTT ties.
    const std::size_t start = (m_next + kWindow - m_count) % kWindow;
    const Sample *best = nullptr;
    for (std::size_t i = 0; i < m_count; ++i) {
        const Sample &s = m_samples[(start + i) % kWindow];
        if (!best || s.rtt <= best->rtt)
            best = &s;
    }
    if (best) {
        m_offset = best->offset;
        m_rtt = best->rtt;
    }
}

void ClockSync::reset()
{
    m_count = 0;
    m_next = 0;
    m_offset = 0;
    m_rtt = 0;
}

}  // namespace couchsync
