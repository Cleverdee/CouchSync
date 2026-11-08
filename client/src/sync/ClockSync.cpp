// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClockSync.h"

#include <algorithm>
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
    bool queued = false;
    if (m_havePrev) {
        const double gap = t0 - m_prevT0;
        // Released right behind the previous one (TCP delivers in order, so a
        // negative difference is reordering, not queueing).
        auto squeezed = [gap](double d) { return d >= 0 && d < kQueuedGapFraction * gap; };
        queued = gap > 0 && (squeezed(t1 - m_prevT1) || squeezed(t3 - m_prevT3));
    }
    m_havePrev = true;
    m_prevT0 = t0;
    m_prevT1 = t1;
    m_prevT3 = t3;
    m_samples[m_next] = Sample{(t0 + t3) / 2.0, ((t1 - t0) + (t2 - t3)) / 2.0, rtt, queued};
    m_next = (m_next + 1) % kWindow;
    if (m_count < kWindow)
        ++m_count;
    recompute();
    return true;
}

void ClockSync::recompute()
{
    const std::size_t start = (m_next + kWindow - m_count) % kWindow;
    const Sample &newest = m_samples[(m_next + kWindow - 1) % kWindow];
    m_newestMid = newest.mid;

    // Drop samples past kMaxAgeMs (only possible after a long gap in pings).
    std::size_t first = 0;
    while (first + 1 < m_count && newest.mid - m_samples[(start + first) % kWindow].mid > kMaxAgeMs)
        ++first;

    double rttMin = newest.rtt;
    for (std::size_t i = first; i < m_count; ++i)
        rttMin = std::min(rttMin, m_samples[(start + i) % kWindow].rtt);
    m_rtt = rttMin;

    // Usable samples, times relative to the newest midpoint for numerical stability.
    struct U {
        double t, y, excess, rtt, fade;  // fade: eviction taper * queued factor
        bool queued;
    };
    std::array<U, kWindow> us{};
    std::size_t n = 0;
    for (std::size_t i = first; i < m_count; ++i) {
        const Sample &s = m_samples[(start + i) % kWindow];
        const double excess = s.rtt - rttMin;
        if (excess > kMaxExcessMs)
            continue;
        // Rank in a full window: 0 = next to be evicted. Quadratic, so even a
        // dominant sample has ~no weight left by the time it is dropped.
        const std::size_t rank = i + (kWindow - m_count);
        const double taper = std::min(1.0, static_cast<double>(rank + 1) / static_cast<double>(kTaperSamples + 1));
        double fade = taper * taper * std::clamp((kMaxAgeMs - (newest.mid - s.mid)) / kTaperAgeMs, 0.0, 1.0);
        if (s.queued)
            fade *= kQueuedWeight;
        if (fade <= 0)
            continue;
        us[n++] = {s.mid - newest.mid, s.offset, excess, s.rtt, fade, s.queued};
    }
    if (n == 0) {  // can't happen (the best-RTT sample is always usable), but stay defined
        m_tm = newest.mid;
        m_ym = newest.offset;
        m_b = 0;
        return;
    }

    // The better half (by RTT) of the non-queued samples: used for the noise floor.
    std::array<std::size_t, kWindow> good{};
    std::size_t ng = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (!us[i].queued)
            good[ng++] = i;
    std::sort(good.begin(), good.begin() + ng, [&](std::size_t a, std::size_t b) { return us[a].rtt < us[b].rtt; });

    // Noise floor, pass 1: from the RTT spread of the best quartile. The single
    // best RTT is an extreme value, so trusting it alone (and re-weighting
    // everything when it ages out) would make the estimate jumpy.
    double floorMs = kNoiseFloorMs;
    if (ng > 0)
        floorMs = std::max(floorMs, kSpreadFactor * (us[good[ng / 4]].rtt - us[good[0]].rtt));

    struct Fit {
        double tm, ym, b;
    };
    std::array<double, kWindow> w{};
    auto fit = [&](double floorMs_) {
        const double floor2 = floorMs_ * floorMs_;
        double sw = 0, st = 0, sy = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const U &u = us[i];
            // Given the excess e, the asymmetry error lies within +-e/2, and with
            // one-sided queueing (the usual case, and TCP head-of-line blocking)
            // it is close to that bound: variance ~(e/2)^2. Independently, it can
            // never exceed +-rtt/2, which keeps a sample with a tiny RTT precise
            // even when the floor is uncertain.
            const double var = std::min(floor2 + u.excess * u.excess / 4.0, std::max(u.rtt * u.rtt / 4.0, 0.01));
            w[i] = u.fade / var;
            sw += w[i];
            st += w[i] * u.t;
            sy += w[i] * u.y;
        }
        const double tm = st / sw;
        const double ym = sy / sw;
        // Slope: weighted LS direction, shrunk towards 0 by a Gaussian prior
        // b ~ N(0, kSlopePriorPpm). The shrink factor uses the precisions capped
        // at 1/kSlopeMinErrMs^2: the RTT-based weights can be overconfident, and
        // two lucky samples a few seconds apart must not produce a wild skew. A
        // short time span (the initial burst) keeps b ~0 until the data show it.
        const double pCap = 1.0 / (kSlopeMinErrMs * kSlopeMinErrMs);
        double sp = 0, spt = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double p = std::min(w[i], pCap);
            sp += p;
            spt += p * us[i].t;
        }
        const double pm = spt / sp;
        double sxx = 0, sxy = 0, sxxCapped = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double dt = us[i].t - tm;
            sxx += w[i] * dt * dt;
            sxy += w[i] * dt * (us[i].y - ym);
            const double dp = us[i].t - pm;
            sxxCapped += std::min(w[i], pCap) * dp * dp;
        }
        const double prior = kSlopePriorPpm * 1e-6;
        const double shrink = sxxCapped / (sxxCapped + 1.0 / (prior * prior));
        const double maxB = kMaxSlopePpm * 1e-6;
        return Fit{tm, ym, sxx > 0 ? std::clamp(sxy / sxx * shrink, -maxB, maxB) : 0.0};
    };

    Fit f = fit(floorMs);
    // Pass 2: a low RTT doesn't guarantee a symmetric path (e.g. symmetric jitter
    // with no samples at the true floor). The residual scatter of the better half
    // of the samples shows how good the good samples really are; if that is worse
    // than the assumed floor, refit with it.
    const std::size_t k = std::min(ng, std::max<std::size_t>(kMinScatterSamples, (ng + 1) / 2));
    if (k >= kMinScatterSamples) {
        double ss = 0;
        for (std::size_t j = 0; j < k; ++j) {
            const U &u = us[good[j]];
            const double r = u.y - (f.ym + f.b * (u.t - f.tm));
            ss += r * r;
        }
        const double scatter = std::sqrt(ss / static_cast<double>(k - 1));
        if (scatter > floorMs)
            f = fit(scatter);
    }
    m_tm = newest.mid + f.tm;
    m_ym = f.ym;
    m_b = f.b;
}

void ClockSync::reset()
{
    m_count = 0;
    m_next = 0;
    m_tm = 0;
    m_ym = 0;
    m_b = 0;
    m_rtt = 0;
    m_newestMid = 0;
    m_havePrev = false;
}

}  // namespace couchsync
