// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests + deterministic closed-loop simulation for the Qt-free sync core.
#include "ClockSync.h"
#include "RoomState.h"
#include "SyncEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

using namespace couchsync;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do {                                                                         \
        ++g_checks;                                                              \
        const double va_ = (a), vb_ = (b);                                       \
        if (!(std::abs(va_ - vb_) <= (tol))) {                                   \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s = %.6f, expected %.6f +- %g\n",        \
                        __FILE__, __LINE__, #a, va_, vb_, (double)(tol));        \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Deterministic RNG (xorshift64*), so the simulation is reproducible everywhere.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
    double uni()  // [0, 1)
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<double>((s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
    }
    double sym(double a) { return (uni() * 2 - 1) * a; }  // [-a, a)
};

// ---------------------------------------------------------------------------
// ClockSync
static void testClockSync()
{
    std::printf("ClockSync\n");
    // Server clock = local + 1000 ms.
    const double trueOffset = 1000;
    auto sample = [&](ClockSync &c, double t0, double up, double down, double proc = 1) {
        const double t1 = t0 + up + trueOffset;
        const double t2 = t1 + proc;
        const double t3 = t2 - trueOffset + down;
        c.addSample(t0, t1, t2, t3);
    };

    {
        ClockSync c;
        CHECK(!c.hasEstimate());
        sample(c, 0, 20, 20);  // symmetric -> exact
        CHECK(c.hasEstimate());
        CHECK_NEAR(c.offset(), trueOffset, 1e-9);
        CHECK_NEAR(c.rtt(), 40, 1e-9);
        CHECK_NEAR(c.serverNow(500), 1500, 1e-9);
    }
    {
        // Asymmetric jittery samples have offset error (up - down) / 2; the
        // low-RTT sample (5/7 ms) must dominate. (With min-RTT selection this
        // expected the exact offset; the weighted fit lets the high-RTT samples
        // contribute a few hundredths of a ms.)
        ClockSync c;
        sample(c, 0, 80, 10);     // err +35
        sample(c, 100, 10, 120);  // err -55
        sample(c, 200, 5, 7);     // err -1, rtt 12
        sample(c, 300, 60, 30);   // err +15
        sample(c, 400, 200, 15);  // err +92
        CHECK_NEAR(c.offset(), trueOffset - 1, 1.0);
        CHECK_NEAR(c.rtt(), 12, 1e-9);

        // A naive mean of the offsets would be badly off.
        CHECK(!c.addSample(0, 10, 5, -100));  // negative rtt rejected
        CHECK_NEAR(c.offset(), trueOffset - 1, 1.0);
    }
    {
        // Eviction is gradual: a lone good sample loses its influence while it
        // ages through the oldest ranks of the window instead of dropping out in
        // one step. (Replaces "window is 16: the best sample's eviction steps the
        // offset by 10 ms", which tested the old min-RTT selection.)
        ClockSync c;
        sample(c, 0, 1, 1);  // rtt 2, exact
        double prev = 0, maxStep = 0;
        for (int i = 1; i <= 40; ++i) {
            sample(c, i * 100.0, 30, 10);  // rtt 40, err +10
            if (i > 1)
                maxStep = std::max(maxStep, std::abs(c.offset() - prev));
            prev = c.offset();
            if (i == 20)
                CHECK_NEAR(c.offset(), trueOffset, 1.0);  // still dominated by the rtt-2 sample
        }
        CHECK(c.sampleCount() == ClockSync::kWindow);
        CHECK_NEAR(c.offset(), trueOffset + 10, 1e-6);  // gone: only the err +10 samples are left
        CHECK_NEAR(c.rtt(), 40, 1e-9);
        std::printf("  lone good sample fading out: max offset step per sample %.2f ms\n", maxStep);
        CHECK(maxStep < 2.5);  // 10 ms total change spread over the taper
    }
    {
        // Zero skew: the slope stays 0 and serverNow extrapolates exactly.
        ClockSync c;
        for (int i = 0; i < 20; ++i)
            sample(c, i * 100.0, 10, 10);
        CHECK_NEAR(c.skewPpm(), 0, 1e-6);
        CHECK_NEAR(c.serverNow(1e6), 1e6 + trueOffset, 1e-6);
        c.reset();
        CHECK(!c.hasEstimate());
        CHECK(c.sampleCount() == 0);
    }
    {
        const double a = steadyNowMs();
        const double b = steadyNowMs();
        CHECK(b >= a);
    }
}

// ---------------------------------------------------------------------------
// ClockSync with clock skew, on the app's ping schedule (8 pings 50 ms apart,
// then one every 5 s) over 10 minutes. One-way delays are base + queueing
// jitter (0..jitterMs, skewed towards 0 like real queueing; default 0..6 ms, a
// decent home connection), and a fraction of the packets gets a 200-1000 ms
// one-sided delay spike. The error scales with the jitter: the heavy case below
// and the sim's uniform +-30 ms per-direction jitter are several times worse.
// The estimate is checked against true server time every 100 ms (between
// pings, so the extrapolation is tested too), and the step in serverNow caused
// by each new sample is recorded.
struct SkewRunResult {
    double maxErrFirstMin = 0;  // ms, first 60 s
    double maxErr = 0;          // ms, after 60 s
    double meanAbsErr = 0;      // ms, after 60 s
    double maxStep = 0;         // ms, serverNow change caused by one new sample, after 60 s
    double skewPpm = 0;         // final estimate
};

static SkewRunResult runSkew(double skewPpm, std::uint64_t seed, double baseMs = 25, double jitterMs = 6,
                             double spikeP = 0.03)
{
    Rng rng(seed);
    ClockSync c;
    const double localEpoch = 5.0e6, serverEpoch = 1.0e9;
    auto localAt = [&](double rt) { return rt * (1 + skewPpm * 1e-6) + localEpoch; };
    auto serverAt = [&](double rt) { return rt + serverEpoch; };
    auto delay = [&]() {
        const double u = rng.uni();
        double d = baseMs + jitterMs * u * u;  // mostly small queueing delay, sometimes up to +jitter
        if (rng.uni() < spikeP)
            d += 200 + 800 * rng.uni();
        return d;
    };
    struct Pending {
        double t0, t1, t2, recvRt;
    };
    std::vector<Pending> inflight;
    std::vector<double> sends;
    for (int i = 0; i < 8; ++i)
        sends.push_back(i * 50.0);
    for (double t = 5000; t <= 600000; t += 5000)
        sends.push_back(t);
    for (double rt : sends) {
        const double up = delay(), down = delay();
        Pending p;
        p.t0 = localAt(rt);
        p.t1 = serverAt(rt + up);
        p.t2 = p.t1 + 0.3;
        p.recvRt = rt + up + 0.3 + down;
        inflight.push_back(p);
    }
    std::sort(inflight.begin(), inflight.end(), [](const Pending &a, const Pending &b) { return a.recvRt < b.recvRt; });

    SkewRunResult r;
    double errSum = 0;
    int errN = 0;
    std::size_t next = 0;
    for (double rt = 0; rt <= 600000; rt += 100) {
        while (next < inflight.size() && inflight[next].recvRt <= rt) {
            const Pending &p = inflight[next++];
            const double at = localAt(p.recvRt);
            const bool had = c.hasEstimate();
            const double before = had ? c.serverNow(at) : 0;
            c.addSample(p.t0, p.t1, p.t2, at);
            if (had && p.recvRt > 60000)
                r.maxStep = std::max(r.maxStep, std::abs(c.serverNow(at) - before));
        }
        if (!c.hasEstimate())
            continue;
        const double err = std::abs(c.serverNow(localAt(rt)) - serverAt(rt));
        if (rt < 60000) {
            r.maxErrFirstMin = std::max(r.maxErrFirstMin, err);
        } else {
            r.maxErr = std::max(r.maxErr, err);
            errSum += err;
            ++errN;
        }
    }
    r.meanAbsErr = errN ? errSum / errN : 0;
    r.skewPpm = c.skewPpm();
    return r;
}

static void testClockSkew()
{
    std::printf("ClockSync skew tracking (10 min, ping every 5 s, 0..6 ms queueing jitter + 3 %% delay spikes)\n");
    for (double skew : {200.0, -300.0, 0.0}) {
        double worst = 0, worstFirst = 0, worstStep = 0, worstMean = 0, worstSkewErr = 0;
        for (std::uint64_t seed = 1; seed <= 5; ++seed) {
            const SkewRunResult r = runSkew(skew, seed);
            worst = std::max(worst, r.maxErr);
            worstFirst = std::max(worstFirst, r.maxErrFirstMin);
            worstStep = std::max(worstStep, r.maxStep);
            worstMean = std::max(worstMean, r.meanAbsErr);
            // Local runs fast by s ppm -> server - local changes by -s/(1+s) per local ms.
            worstSkewErr = std::max(worstSkewErr, std::abs(r.skewPpm + skew / (1 + skew * 1e-6)));
        }
        std::printf("  skew %+4.0f ppm, 5 seeds: max err %.2f ms after 60 s (worst mean %.2f), %.2f ms in the "
                    "first 60 s; max step per sample %.2f ms; skew estimate off by <= %.1f ppm\n",
                    skew, worst, worstMean, worstFirst, worstStep, worstSkewErr);
        CHECK(worst < 2.0);
        CHECK(worstFirst < 5.0);
        CHECK(worstStep < 2.0);
        CHECK(worstSkewErr < 20.0);
    }
    {
        // Heavier network: 80 ms base, 0..20 ms queueing jitter, 5 % spikes.
        double worst = 0, worstStep = 0;
        for (std::uint64_t seed = 11; seed <= 15; ++seed) {
            const SkewRunResult r = runSkew(200, seed, 80, 20, 0.05);
            worst = std::max(worst, r.maxErr);
            worstStep = std::max(worstStep, r.maxStep);
        }
        std::printf("  heavier jitter (80 ms + 0..20 ms, 5 %% spikes), skew +200 ppm, 5 seeds: max err %.2f ms, max step %.2f ms\n",
                    worst, worstStep);
        CHECK(worst < 7.0);
        CHECK(worstStep < 4.0);
    }
}

// ---------------------------------------------------------------------------
// RoomState::expectedPosition
static void testExpectedPosition()
{
    std::printf("expectedPosition\n");
    RoomState s;
    s.playing = true;
    s.position = 100;
    s.serverTime = 10000;
    s.rate = 1.0;
    CHECK_NEAR(s.expectedPosition(12500), 102.5, 1e-9);
    s.rate = 2.0;
    CHECK_NEAR(s.expectedPosition(12500), 105.0, 1e-9);
    s.rate = 1.0;
    // Future serverTime (scheduled start): holds.
    CHECK_NEAR(s.expectedPosition(9850), 100.0, 1e-9);
    CHECK_NEAR(s.expectedPosition(10000), 100.0, 1e-9);
    // Paused: holds regardless of time.
    s.playing = false;
    CHECK_NEAR(s.expectedPosition(99999), 100.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Closed-loop simulation.
//
// Real time t (ms) drives everything. The local steady clock runs with a skew
// (ppm) against the server clock; ClockSync estimates the offset from simulated
// pings with random asymmetric delays (8 at start, then every 5 s), so the
// engine sees a slowly wandering serverNow just like in the app. The fake
// player advances at `speed` in real time, seeks take `seekLatencyMs` (frozen
// at the target while seeking, then playback resumes from the target), and the
// reported localPos has +-jitter noise.
struct Sim {
    SyncConfig cfg;
    SyncEngine eng;
    ClockSync clock;
    Rng rng;

    double t = 0;  // real ms
    double skewPpm = 50;        // local steady clock vs server clock
    double playerSkewPpm = 0;   // player media clock vs real time (e.g. audio clock drift)
    double jitterS = 0.010;
    double localEpoch = 123456.0;
    double serverEpoch = 9876543.0;
    double nextPingAt = 0;

    RoomState room;  // last state sent to the engine
    std::int64_t seq = 0;

    // fake player
    double pos = 0;
    bool paused = false;
    double speed = 1.0;
    bool seeking = false;
    double seekDoneAt = 0;
    double seekLatencyMs = 250;

    // stats
    int seeks = 0;
    double minSpeed = 1, maxSpeed = 1;
    SyncActions last;
    std::function<void(const SyncActions &, const PlayerSample &)> onTick;

    explicit Sim(std::uint64_t seed, SyncConfig c = {}) : cfg(c), eng(c), rng(seed)
    {
        // Initial clock sync: 8 pings 50 ms apart.
        t = -400;
        for (int i = 0; i < 8; ++i) {
            ping();
            t += 50;
        }
        t = 0;
        nextPingAt = 5000;
    }

    double localNow() const { return t * (1 + skewPpm * 1e-6) + localEpoch; }
    double serverTrue() const { return t + serverEpoch; }
    double localAt(double rt) const { return rt * (1 + skewPpm * 1e-6) + localEpoch; }
    double serverNowEst() const { return clock.serverNow(localNow()); }

    void ping()
    {
        const double up = 15 + 25 * rng.uni();
        const double down = 15 + 25 * rng.uni();
        const double t0 = localAt(t);
        const double t1 = t + up + serverEpoch;
        const double t2 = t1 + 0.5;
        const double t3 = localAt(t + up + 0.5 + down);
        clock.addSample(t0, t1, t2, t3);
    }

    // Engine-perceived drift without the sample noise.
    double cleanDrift() const { return pos - room.expectedPosition(serverNowEst()); }
    // Drift against true server time (includes clock-offset estimation error).
    double trueDrift() const { return pos - room.expectedPosition(serverTrue()); }

    void sendState(RoomState s)
    {
        s.seq = ++seq;
        room = s;
        eng.applyState(s);
    }
    // Room playing with `position` reached at true server time "now".
    RoomState playingNow(double position, double startDelayMs = 0) const
    {
        RoomState s;
        s.playing = true;
        s.position = position;
        s.serverTime = serverTrue() + startDelayMs;
        return s;
    }

    void advance(double dt)
    {
        const double end = t + dt;
        double from = t;
        if (seeking && seekDoneAt <= end) {
            seeking = false;
            from = seekDoneAt;
        }
        if (!seeking && !paused)
            pos += (end - from) / 1000.0 * speed * (1 + playerSkewPpm * 1e-6);
        t = end;
    }

    void step(double dt = 100)
    {
        advance(dt);
        if (t >= nextPingAt) {
            ping();
            nextPingAt += 5000;
        }
        PlayerSample s;
        s.serverNowMs = serverNowEst();
        s.localPos = pos + rng.sym(jitterS);
        s.paused = paused;
        s.seeking = seeking;
        s.buffering = false;
        s.currentSpeed = speed;
        s.fileLoaded = true;
        last = eng.tick(s);
        if (last.setPaused)
            paused = *last.setPaused;
        if (last.seekTo) {
            ++seeks;
            seeking = true;
            seekDoneAt = t + seekLatencyMs;
            pos = *last.seekTo;
        }
        if (last.setSpeed) {
            speed = *last.setSpeed;
            minSpeed = std::min(minSpeed, speed);
            maxSpeed = std::max(maxSpeed, speed);
        }
        if (onTick)
            onTick(last, s);
    }

    bool converged() const
    {
        return !seeking && !eng.seekInFlight() && speed == room.rate && last.driftS &&
               std::abs(*last.driftS) < 0.015;
    }

    // Runs until converged (returns elapsed ms) or maxMs (returns -1).
    double runUntilConverged(double maxMs)
    {
        const double start = t;
        while (t - start < maxMs) {
            step();
            if (converged())
                return t - start;
        }
        return -1;
    }
};

static void testConvergeBySpeed(double playerSkewPpm)
{
    std::printf("sim (a)+(c): 1 s behind -> speed only, then 5 min stability "
                "(clock skew 50 ppm, player media-clock skew %.0f ppm, +-10 ms jitter)\n",
                playerSkewPpm);
    // Models drift that built up gradually, not a fresh state jump (those seek above
    // jumpSeekThresholdS), so keep the jump path out of the way here.
    SyncConfig cfg;
    cfg.jumpSeekThresholdS = 2.0;
    Sim sim(42, cfg);
    sim.playerSkewPpm = playerSkewPpm;
    sim.sendState(sim.playingNow(600.0));
    sim.pos = 600.0 - 1.0;

    const double tConv = sim.runUntilConverged(30000);
    std::printf("  converged after %.1f s (seeks=%d, speed range [%.4f, %.4f], clean drift %.1f ms, true drift %.1f ms)\n",
                tConv / 1000, sim.seeks, sim.minSpeed, sim.maxSpeed, sim.cleanDrift() * 1000,
                sim.trueDrift() * 1000);
    CHECK(tConv > 0 && tConv <= 30000);
    CHECK(sim.seeks == 0);
    CHECK(sim.minSpeed >= 0.95 - 1e-12 && sim.maxSpeed <= 1.05 + 1e-12);
    CHECK(std::abs(sim.cleanDrift()) < 0.025);

    // (c) 5 minutes after convergence: speed exactly 1.0 for >= 95 % of ticks.
    int ticks = 0, atUnity = 0, episodes = 0;
    double maxAbsDrift = 0;
    bool wasCorrecting = false;
    for (double end = sim.t + 300000; sim.t < end;) {
        sim.step();
        ++ticks;
        if (sim.speed == 1.0)
            ++atUnity;
        if (sim.eng.correcting() && !wasCorrecting)
            ++episodes;
        wasCorrecting = sim.eng.correcting();
        maxAbsDrift = std::max(maxAbsDrift, std::abs(sim.cleanDrift()));
    }
    const double pct = 100.0 * atUnity / ticks;
    std::printf("  5 min run: speed == 1.0 for %.2f %% of ticks, %d correction episodes, max |clean drift| %.1f ms, seeks=%d\n",
                pct, episodes, maxAbsDrift * 1000, sim.seeks);
    CHECK(pct >= 95.0);
    CHECK(sim.seeks == 0);
    CHECK(maxAbsDrift < 0.060);
}

static void testConvergeBySeek()
{
    std::printf("sim (b): 5 s behind -> one seek, then convergence\n");
    Sim sim(7);
    sim.seekLatencyMs = 250;
    sim.sendState(sim.playingNow(600.0));
    sim.pos = 600.0 - 5.0;
    const double tConv = sim.runUntilConverged(10000);
    std::printf("  converged after %.1f s (seeks=%d, clean drift %.1f ms)\n", tConv / 1000, sim.seeks,
                sim.cleanDrift() * 1000);
    CHECK(tConv > 0 && tConv <= 10000);
    CHECK(sim.seeks == 1);
    // and it stays put
    for (int i = 0; i < 200; ++i)
        sim.step();
    CHECK(sim.seeks == 1);
    CHECK(std::abs(sim.cleanDrift()) < 0.040);
}

static void testScheduledPlay()
{
    std::printf("sim (d): scheduled play at serverTime + 150 ms\n");
    for (double phase : {0.0, 37.0, 99.0}) {
        Sim sim(11);
        RoomState paused;
        paused.playing = false;
        paused.position = 50;
        paused.serverTime = sim.serverTrue();
        sim.sendState(paused);
        sim.pos = 50;
        sim.paused = true;
        sim.advance(phase);
        for (int i = 0; i < 10; ++i)
            sim.step();
        CHECK(sim.paused);
        CHECK(sim.seeks == 0);

        RoomState play = sim.playingNow(50.0, 150 + phase);
        sim.sendState(play);
        bool unpaused = false;
        bool early = false;
        double unpauseAt = 0;
        sim.onTick = [&](const SyncActions &a, const PlayerSample &s) {
            const bool due = s.serverNowMs >= play.serverTime;
            if (a.setPaused && !*a.setPaused) {
                if (!due)
                    early = true;
                if (!unpaused)
                    unpauseAt = s.serverNowMs;
                unpaused = true;
            } else if (due && !unpaused) {
                early = true;  // first due tick must unpause; reuse flag as failure marker
            }
        };
        for (int i = 0; i < 10; ++i)
            sim.step();
        std::printf("  phase %2.0f ms: unpaused %.1f ms after scheduled start\n", phase,
                    unpauseAt - play.serverTime);
        CHECK(unpaused);
        CHECK(!early);
        CHECK(!sim.paused);
        CHECK(unpauseAt - play.serverTime < 100.0 + 1e-9);
        CHECK(sim.seeks == 0);
    }
}

static void testLatePlay()
{
    std::printf("sim (d2): a play state that arrives 1 s after its scheduled start -> one seek, not 20 s of speed-up\n");
    Sim sim(13);
    RoomState paused;
    paused.playing = false;
    paused.position = 50;
    paused.serverTime = sim.serverTrue();
    sim.sendState(paused);
    sim.pos = 50;
    sim.paused = true;
    for (int i = 0; i < 10; ++i)
        sim.step();
    CHECK(sim.paused);

    // Scheduled start was 1 s ago (delivery delayed by a loss spike).
    sim.sendState(sim.playingNow(50.0, -1000));
    const double tConv = sim.runUntilConverged(5000);
    std::printf("  converged after %.1f s (seeks=%d, clean drift %.1f ms)\n", tConv / 1000, sim.seeks, sim.cleanDrift() * 1000);
    CHECK(!sim.paused);
    CHECK(sim.seeks == 1);
    CHECK(tConv > 0 && tConv <= 5000);
}

static void testRemotePause()
{
    std::printf("sim (e): remote pause lands within 40 ms of state.position\n");
    Sim sim(5);
    sim.sendState(sim.playingNow(300.0));
    sim.pos = 300.0;
    for (int i = 0; i < 50; ++i)
        sim.step();
    for (double lag : {0.3, -0.02}) {  // the pauser was 300 ms behind us / 20 ms ahead
        RoomState p;
        p.playing = false;
        p.position = sim.room.expectedPosition(sim.serverTrue()) - lag;
        p.serverTime = sim.serverTrue();
        sim.sendState(p);
        for (int i = 0; i < 20; ++i)
            sim.step();
        std::printf("  pauser offset %+.0f ms: paused=%d, |pos - state.position| = %.1f ms\n", lag * 1000,
                    sim.paused, std::abs(sim.pos - p.position) * 1000);
        CHECK(sim.paused);
        CHECK(std::abs(sim.pos - p.position) <= 0.040);
        CHECK(sim.speed == 1.0);
        // resume for the next round
        sim.sendState(sim.playingNow(sim.pos, 150));
        for (int i = 0; i < 30; ++i)
            sim.step();
        CHECK(!sim.paused);
    }
}

static void testSeqOrdering()
{
    std::printf("(f): stale seqs are ignored\n");
    SyncEngine e;
    RoomState s;
    s.seq = 5;
    s.position = 10;
    CHECK(e.applyState(s));
    RoomState old = s;
    old.seq = 4;
    old.position = 99;
    CHECK(!e.applyState(old));
    old.seq = 5;
    CHECK(!e.applyState(old));
    CHECK(e.state()->position == 10);
    RoomState next = s;
    next.seq = 6;
    next.position = 20;
    CHECK(e.applyState(next));
    CHECK(e.state()->position == 20);
    e.reset();
    CHECK(!e.state());
    CHECK(e.applyState(old));  // seq restarts after reset (new room)
}

static void testSeekEma()
{
    std::printf("sim (g): seek-time EMA adapts to the player's latency\n");
    Sim sim(99);
    sim.seekLatencyMs = 650;
    sim.sendState(sim.playingNow(1000.0));
    sim.pos = 1000.0;
    for (int i = 0; i < 20; ++i)
        sim.step();
    std::printf("  predicted:");
    double prev = sim.eng.predictedSeekMs();
    bool monotone = true;
    for (int k = 0; k < 8; ++k) {
        const int before = sim.seeks;
        sim.sendState(sim.playingNow(sim.room.expectedPosition(sim.serverTrue()) + 30.0));  // remote seek +30 s
        for (int i = 0; i < 50; ++i)
            sim.step();
        CHECK(sim.seeks == before + 1);
        monotone = monotone && sim.eng.predictedSeekMs() > prev;
        prev = sim.eng.predictedSeekMs();
        std::printf(" %.0f", sim.eng.predictedSeekMs());
    }
    std::printf(" ms (true latency 650 ms, measured with 100 ms tick quantisation)\n");
    CHECK(monotone);
    CHECK(sim.eng.predictedSeekMs() > 600 && sim.eng.predictedSeekMs() <= 760);
    // After adapting, a remote seek lands close: the post-seek drift is small.
    sim.sendState(sim.playingNow(sim.room.expectedPosition(sim.serverTrue()) - 20.0));
    for (int i = 0; i < 9; ++i)
        sim.step();
    std::printf("  drift right after an adapted seek: %.0f ms\n", sim.cleanDrift() * 1000);
    CHECK(std::abs(sim.cleanDrift()) < 0.120);
}

static void testLocalIntent()
{
    std::printf("local intent: no fighting the user's own seek; snap back on timeout\n");
    Sim sim(3);
    sim.sendState(sim.playingNow(200.0));
    sim.pos = 200.0;
    for (int i = 0; i < 20; ++i)
        sim.step();

    // User seeks +60 s locally; the integrator performs the seek and notes the intent.
    sim.eng.noteLocalIntent(sim.serverNowEst());
    sim.seeking = true;
    sim.seekDoneAt = sim.t + 200;
    sim.pos += 60.0;
    for (int i = 0; i < 5; ++i)
        sim.step();
    CHECK(sim.seeks == 0);
    CHECK(sim.eng.localIntentPending());

    // Server confirms (position p, start in 150 ms) -> no seek needed.
    sim.sendState(sim.playingNow(sim.pos, 150));
    CHECK(!sim.eng.localIntentPending());
    for (int i = 0; i < 100; ++i)
        sim.step();
    CHECK(sim.seeks == 0);
    CHECK(std::abs(sim.cleanDrift()) < 0.040);

    // Rejected intent (no state ever comes): after the timeout we're pulled back.
    sim.eng.noteLocalIntent(sim.serverNowEst());
    sim.pos += 30.0;
    for (int i = 0; i < 8; ++i)
        sim.step();
    CHECK(sim.seeks == 0);
    for (int i = 0; i < 30; ++i)
        sim.step();
    CHECK(sim.seeks == 1);
    CHECK(std::abs(sim.cleanDrift()) < 0.5);
}

static void testStatus()
{
    std::printf("status reporting\n");
    SyncEngine e;
    PlayerSample s;
    s.fileLoaded = false;
    CHECK(e.tick(s).status == MemberStatus::Waiting);
    s.fileLoaded = true;
    CHECK(e.tick(s).status == MemberStatus::Ready);
    s.buffering = true;
    CHECK(e.tick(s).status == MemberStatus::Waiting);
    s.buffering = false;
    s.seeking = true;
    CHECK(e.tick(s).status == MemberStatus::Waiting);
    s.seeking = false;
    CHECK(e.tick(s).status == MemberStatus::Ready);
    CHECK(!e.tick(s).hasCommands());  // no state yet -> no commands
}

int main()
{
    testClockSync();
    testClockSkew();
    testExpectedPosition();
    testSeqOrdering();
    testStatus();
    testConvergeBySpeed(100);
    testConvergeBySpeed(500);  // harsh: forces repeated correction episodes
    testConvergeBySeek();
    testScheduledPlay();
    testLatePlay();
    testRemotePause();
    testSeekEma();
    testLocalIntent();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
