// SPDX-License-Identifier: GPL-3.0-or-later
// couchsync_sim: headless sync simulator. Runs N fake clients (each behind a latency/jitter/loss proxy,
// with its own skewed clock and fake player) against a real room server, in real time, and asserts
// convergence, steady-state drift and propagation against ground truth. See client/README.md.
#include "Proxy.h"
#include "Scenario.h"
#include "SimClient.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace sim;

namespace {

struct Sample {
    double t;      // real ms
    float driftMs;  // true drift: model position - server's true expected position
    bool paused;
    float clockErrMs;    // client's estimate of server time minus true server time
    float engineDriftMs; // what the engine believed its drift was
};

struct Anchor {
    double t;
    QString label;
};

struct Group {
    double first = 0, last = 0;
    QStringList labels;
};

QString rel(double tMs, double base)
{
    return QStringLiteral("%1").arg((tMs - base) / 1000.0, 6, 'f', 1);
}

void out(const QString &s)
{
    std::fputs(s.toLocal8Bit().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

class Runner : public QObject
{
public:
    Runner(const Scenario &sc, const QString &host, quint16 port, const QString &csv, bool verbose)
        : m_sc(sc), m_host(host), m_port(port), m_csv(csv), m_verbose(verbose)
    {
    }

    void start()
    {
        DelayProxy::Params base;
        base.spikeMinMs = m_sc.spikeMinMs;
        base.spikeMaxMs = m_sc.spikeMaxMs;
        for (int i = 0; i < m_sc.clients.size(); ++i) {
            const ClientSpec &cs = m_sc.clients[i];
            DelayProxy::Params p = base;
            p.upMs = p.downMs = cs.latencyMs;
            p.jitterMs = cs.jitterMs;
            p.lossP = cs.lossP;
            auto proxy = std::make_unique<DelayProxy>(m_host, m_port, p, m_sc.seed * 1000 + 17 * (i + 1), this);
            if (!proxy->listen()) {
                fail(QStringLiteral("proxy listen failed"));
                return;
            }
            auto *c = new SimClient(cs, QStringLiteral("127.0.0.1"), proxy->port(), m_sc.seed * 7919 + i, this);
            connect(c, &SimConn::failed, this, [this](const QString &why) { fail(why); });
            m_proxies.push_back(std::move(proxy));
            m_clients.push_back(c);
        }
        m_obs = new RefObserver(m_host, m_port, this);
        connect(m_obs, &SimConn::failed, this, [this](const QString &why) { fail(why); });
        connect(m_obs, &SimConn::stateAccepted, this, [this](const StateRecord &r) {
            if (m_verbose)
                out(QStringLiteral("  [%1] state seq=%2 cause=%3 by=%4 playing=%5 pos=%6 autoPaused=%7 waiting=%8")
                        .arg(rel(r.recvRealMs, m_playStart > 0 ? m_playStart : m_t0))
                        .arg(r.st.seq).arg(r.cause, r.by).arg(r.st.playing).arg(r.st.position, 0, 'f', 3)
                        .arg(r.st.autoPaused).arg(r.waitingFor.join(QLatin1Char(','))));
        });

        m_t0 = realNowMs();
        connect(m_clients[0], &SimConn::joined, this, [this] {
            const QString code = m_clients[0]->roomCode();
            for (int i = 1; i < m_clients.size(); ++i)
                m_clients[i]->connectToRoom(code);
            m_obs->connectToRoom(code);
            m_clients[0]->sendMediaSet(QStringLiteral("sim.mkv"), 7200.0);
        });
        m_clients[0]->connectToRoom(QString());

        m_poll.setInterval(100);
        connect(&m_poll, &QTimer::timeout, this, &Runner::checkReady);
        m_poll.start();
    }

private:
    void fail(const QString &why)
    {
        if (m_done)
            return;
        m_done = true;
        out(QStringLiteral("SIM ERROR: %1").arg(why));
        QCoreApplication::exit(2);
    }

    void checkReady()
    {
        if (m_playStart > 0)
            return;
        if (realNowMs() - m_t0 > 30000) {
            fail(QStringLiteral("setup timeout (clients not ready after 30 s)"));
            return;
        }
        if (!m_obs->inRoom() || m_obs->clock().sampleCount() < 8)
            return;
        for (SimClient *c : m_clients) {
            if (!c->inRoom() || c->clock().sampleCount() < 8 || c->states().isEmpty() || !c->states().last().hasMedia)
                return;
        }
        // Everyone joined. Start only once every client reports `ready` (its clock estimate is
        // ready, file loaded), then wait one more second: people join, then press play later.
        const double now = realNowMs();
        if (m_joinedAt == 0)
            m_joinedAt = now;
        bool allReady = true;
        for (SimClient *c : m_clients)
            allReady = allReady && c->reportedReady();
        if (!allReady) {
            m_allReadyAt = 0;
            if (now - m_joinedAt > 10000) {
                m_poll.stop();
                m_done = true;
                out(QStringLiteral("RESULT: FAIL (1)\n  - clients not all ready within 10 s of joining"));
                QCoreApplication::exit(1);
            }
            return;
        }
        if (m_allReadyAt == 0)
            m_allReadyAt = now;
        if (now - m_allReadyAt < 1000)
            return;
        m_poll.stop();
        begin();
    }

    void begin()
    {
        m_playStart = realNowMs();
        out(QStringLiteral("scenario '%1': %2 clients, ends %3 s after play, seed %4")
                .arg(m_sc.name).arg(m_clients.size()).arg(m_sc.endAtS).arg(m_sc.seed));
        m_samples.resize(m_clients.size());
        m_clients[0]->userPlay();
        m_anchors.push_back({m_playStart, QStringLiteral("play")});
        for (const Event &e : m_sc.events) {
            auto *t = new QTimer(this);
            t->setSingleShot(true);
            t->setTimerType(Qt::PreciseTimer);
            connect(t, &QTimer::timeout, this, [this, e] { runEvent(e); });
            t->start(static_cast<int>(e.atS * 1000));
        }
        m_sampler.setInterval(20);
        m_sampler.setTimerType(Qt::PreciseTimer);
        connect(&m_sampler, &QTimer::timeout, this, &Runner::sample);
        m_sampler.start();
        QTimer::singleShot(static_cast<int>(m_sc.endAtS * 1000), this, &Runner::finish);
    }

    void runEvent(const Event &e)
    {
        SimClient *c = m_clients[e.client];
        const double now = realNowMs();
        if (m_verbose)
            out(QStringLiteral("  [%1] script: %2 on %3").arg(rel(now, m_playStart), e.action, c->spec().name));
        if (e.action == QLatin1String("seek"))
            c->userSeekBy(e.value);
        else if (e.action == QLatin1String("pause"))
            c->userPause();
        else if (e.action == QLatin1String("play"))
            c->userPlay();
        else if (e.action == QLatin1String("buffer"))
            c->startBuffering(e.value);
        else
            out(QStringLiteral("unknown action %1").arg(e.action));
        m_anchors.push_back({now, QStringLiteral("%1 %2").arg(e.action, c->spec().name)});
        if (e.action == QLatin1String("buffer"))
            m_bufferStarts.push_back(now);
    }

    void sample()
    {
        if (!m_obs->state())
            return;
        const double now = realNowMs();
        const double expected = m_obs->expectedAt(now);
        for (int i = 0; i < m_clients.size(); ++i) {
            SimClient *c = m_clients[i];
            const double pos = c->positionAt(now);
            m_samples[i].push_back({now, static_cast<float>((pos - expected) * 1000.0), c->paused(),
                                    static_cast<float>(c->serverNowEst() - m_obs->serverAt(now)),
                                    static_cast<float>(c->engineDriftMs())});
        }
    }

    // ---- analysis ----------------------------------------------------------------------

    static bool changed(const StateRecord &a, const StateRecord &b)
    {
        return a.st.playing != b.st.playing || std::abs(a.st.position - b.st.position) > 1e-6 ||
               (b.st.playing && std::abs(a.st.serverTime - b.st.serverTime) > 1e-3);
    }

    void finish()
    {
        if (m_done)
            return;
        m_sampler.stop();
        m_endT = realNowMs();
        const int code = analyze();
        m_done = true;
        QCoreApplication::exit(code);
    }

    int analyze()
    {
        const Thresholds &th = m_sc.th;
        const int n = m_clients.size();
        const double base = m_playStart;
        int failures = 0;
        QStringList failMsgs;
        auto failIf = [&](bool bad, const QString &msg) {
            if (bad) {
                ++failures;
                failMsgs << msg;
            }
        };

        // Observer state changes (ground truth).
        QVector<StateRecord> changes;
        const QVector<StateRecord> &obs = m_obs->states();
        for (int i = 1; i < obs.size(); ++i) {
            if (obs[i].recvRealMs >= m_playStart - 500 && changed(obs[i - 1], obs[i]))
                changes.push_back(obs[i]);
        }
        QVector<Anchor> anchors = m_anchors;
        for (const StateRecord &r : changes)
            anchors.push_back({r.recvRealMs, r.cause});
        std::sort(anchors.begin(), anchors.end(), [](const Anchor &a, const Anchor &b) { return a.t < b.t; });

        QVector<Group> groups;
        for (const Anchor &a : anchors) {
            if (groups.isEmpty() || a.t - groups.last().last > th.groupGapS * 1000) {
                Group g;
                g.first = g.last = a.t;
                g.labels << a.label;
                groups.push_back(g);
            } else {
                groups.last().last = a.t;
                if (!groups.last().labels.contains(a.label))
                    groups.last().labels << a.label;
            }
        }

        // Convergence per disruption group and client.
        out(QStringLiteral("\nDisruptions: time (s after the last event) until |true drift| < %1 ms for good; window %2 s").arg(th.convergeMs).arg(th.convergeWindowS));
        QVector<double> maxConv(n, 0.0);
        QString hdr = QStringLiteral("  %1  %2").arg(QStringLiteral("t(s)"), 6).arg(QStringLiteral("event"), -30);
        for (int i = 0; i < n; ++i)
            hdr += QStringLiteral(" %1").arg(m_clients[i]->spec().name.left(8), 8);
        out(hdr);
        QVector<std::pair<double, double>> excluded;
        for (int gi = 0; gi < groups.size(); ++gi) {
            const Group &g = groups[gi];
            double wEnd = g.last + th.convergeWindowS * 1000;
            excluded.push_back({g.first, wEnd});
            if (gi + 1 < groups.size())
                wEnd = std::min(wEnd, groups[gi + 1].first);
            wEnd = std::min(wEnd, m_endT);
            const double tail = std::min(500.0, 0.25 * (wEnd - g.first));
            QString line = QStringLiteral("  %1  %2").arg(rel(g.first, base)).arg(g.labels.join(QLatin1Char('+')).left(30), -30);
            for (int i = 0; i < n; ++i) {
                double lastViol = -1;
                bool tailViol = false;
                for (const Sample &s : m_samples[i]) {
                    if (s.t < g.first || s.t > wEnd)
                        continue;
                    if (std::abs(s.driftMs) >= th.convergeMs) {
                        lastViol = s.t;
                        if (s.t >= wEnd - tail)
                            tailViol = true;
                    }
                }
                const double conv = lastViol < 0 ? 0.0 : std::max(0.0, lastViol - g.last) / 1000.0;
                maxConv[i] = std::max(maxConv[i], conv);
                if (tailViol) {
                    line += QStringLiteral(" %1").arg(QStringLiteral("NO"), 8);
                    failIf(true, QStringLiteral("%1 did not converge within %2 ms after '%3' at t=%4 s")
                                     .arg(m_clients[i]->spec().name).arg(th.convergeMs).arg(g.labels.join(QLatin1Char('+')), rel(g.first, base).trimmed()));
                } else {
                    line += QStringLiteral(" %1").arg(conv, 8, 'f', 2);
                }
            }
            out(line);
        }

        // Steady state: everything outside the disruption windows.
        out(QStringLiteral("\nSteady state (outside the %1 s settling windows), true drift in ms:").arg(th.convergeWindowS));
        out(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8").arg(QStringLiteral("client"), -9).arg(QStringLiteral("lat"), 4)
                .arg(QStringLiteral("samples"), 8).arg(QStringLiteral("p95"), 7).arg(QStringLiteral("max"), 7)
                .arg(QStringLiteral("maxConv"), 8).arg(QStringLiteral("seeks"), 6).arg(QStringLiteral("speed range  | diag: clk err mean/max, engine-view p95")));
        for (int i = 0; i < n; ++i) {
            QVector<double> v, ev;
            double ceSum = 0, ceMax = 0;
            for (const Sample &s : m_samples[i]) {
                if (s.t < m_playStart)
                    continue;
                bool ex = false;
                for (const auto &w : excluded) {
                    if (s.t >= w.first && s.t <= w.second) {
                        ex = true;
                        break;
                    }
                }
                if (!ex) {
                    v.push_back(std::abs(static_cast<double>(s.driftMs)));
                    ev.push_back(std::abs(static_cast<double>(s.engineDriftMs)));
                    ceSum += s.clockErrMs;
                    ceMax = std::max(ceMax, std::abs(static_cast<double>(s.clockErrMs)));
                }
            }
            std::sort(v.begin(), v.end());
            std::sort(ev.begin(), ev.end());
            const double evP95 = ev.isEmpty() ? 0 : ev[std::min<qsizetype>(ev.size() - 1, static_cast<qsizetype>(ev.size() * 0.95))];
            const double ceMean = v.isEmpty() ? 0 : ceSum / v.size();
            const double p95 = v.isEmpty() ? 0 : v[std::min<qsizetype>(v.size() - 1, static_cast<qsizetype>(v.size() * 0.95))];
            const double mx = v.isEmpty() ? 0 : v.last();
            SimClient *c = m_clients[i];
            out(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7  [%8, %9]  | %10 / %11, %12")
                    .arg(c->spec().name.left(9), -9).arg(static_cast<int>(c->spec().latencyMs), 4).arg(v.size(), 8)
                    .arg(p95, 7, 'f', 1).arg(mx, 7, 'f', 1).arg(maxConv[i], 8, 'f', 2).arg(c->engineSeeks(), 6)
                    .arg(c->minSpeed(), 0, 'f', 3).arg(c->maxSpeed(), 0, 'f', 3).arg(ceMean, 0, 'f', 1).arg(ceMax, 0, 'f', 1).arg(evP95, 0, 'f', 1));
            failIf(v.size() < 50, QStringLiteral("%1: too few steady-state samples (%2)").arg(c->spec().name).arg(v.size()));
            failIf(p95 >= th.steadyP95Ms, QStringLiteral("%1: steady p95 %2 ms >= %3 ms").arg(c->spec().name).arg(p95, 0, 'f', 1).arg(th.steadyP95Ms));
            failIf(mx >= th.steadyMaxMs, QStringLiteral("%1: steady max %2 ms >= %3 ms").arg(c->spec().name).arg(mx, 0, 'f', 1).arg(th.steadyMaxMs));
        }

        // Propagation of play / pause / seek.
        out(QStringLiteral("\nPropagation: ms from the server's state change until the client's model reflects it (limit %1 ms + one-way latency;")
                .arg(th.propagationBaseMs));
        out(QStringLiteral("  'S' = delivery was delayed by a loss spike, exempt from the limit; 'W' = client was waiting at the broadcast, exempt but must converge within 10 s of becoming ready; the processing time after delivery is still limited to %1 ms):")
                .arg(th.propagationBaseMs));
        QString ph = QStringLiteral("  %1  %2").arg(QStringLiteral("t(s)"), 6).arg(QStringLiteral("cause"), -10);
        for (int i = 0; i < n; ++i)
            ph += QStringLiteral(" %1").arg(m_clients[i]->spec().name.left(8), 9);
        out(ph);
        for (const StateRecord &r : changes) {
            if (r.cause != QLatin1String("play") && r.cause != QLatin1String("pause") && r.cause != QLatin1String("seek") &&
                r.cause != QLatin1String("autopause"))
                continue;
            QString line = QStringLiteral("  %1  %2").arg(rel(r.recvRealMs, base)).arg(r.cause, -10);
            for (int i = 0; i < n; ++i) {
                SimClient *c = m_clients[i];
                double tClient = -1;
                for (const SimClient::Arrival &a : c->arrivals()) {
                    if (a.seq == r.st.seq) {
                        tClient = a.realMs;
                        break;
                    }
                }
                double tReflect = -1;
                for (const Sample &s : m_samples[i]) {
                    if (s.t < r.recvRealMs - 1000)
                        continue;
                    bool ok;
                    if (r.cause == QLatin1String("seek"))
                        ok = std::abs(s.driftMs) < 1000;
                    else if (r.cause == QLatin1String("play"))
                        ok = !s.paused;
                    else
                        ok = s.paused;
                    if (ok) {
                        tReflect = s.t;
                        break;
                    }
                }
                if (tClient < 0 || tReflect < 0) {
                    line += QStringLiteral(" %1").arg(QStringLiteral("MISSING"), 9);
                    failIf(true, QStringLiteral("%1 never reflected %2 at t=%3 s").arg(c->spec().name, r.cause, rel(r.recvRealMs, base).trimmed()));
                    continue;
                }
                const double delay = std::max(0.0, tReflect - r.recvRealMs);
                const double proc = std::max(0.0, tReflect - tClient);
                const double netDelay = tClient - r.recvRealMs;
                const double lat = c->spec().latencyMs;
                const bool spike = netDelay > lat + c->spec().jitterMs + 25;
                // Only a client that was `ready` when this was broadcast is held to the propagation
                // limit; a `waiting` one must instead converge within 10 s of becoming ready.
                const bool wasReady = c->wasReadyAt(r.recvRealMs);
                if (!wasReady) {
                    const double tR = c->readyFrom(r.recvRealMs);
                    bool conv = false;
                    if (tR >= 0) {
                        const double wEnd = std::min(tR + th.convergeWindowS * 1000, m_endT);
                        for (const Sample &s : m_samples[i]) {
                            if (s.t < tR || s.t > wEnd || std::abs(s.driftMs) >= th.convergeMs)
                                continue;
                            bool stays = true;
                            for (const Sample &s2 : m_samples[i])
                                if (s2.t > s.t && s2.t <= s.t + 1000 && std::abs(s2.driftMs) >= th.convergeMs) {
                                    stays = false;
                                    break;
                                }
                            if (stays) {
                                conv = true;
                                break;
                            }
                        }
                    }
                    line += QStringLiteral(" %1W").arg(delay, 8, 'f', 0);
                    failIf(!conv, QStringLiteral("%1 was waiting at %2 (t=%3 s) and did not converge within %4 s of becoming ready")
                                      .arg(c->spec().name, r.cause, rel(r.recvRealMs, base).trimmed()).arg(th.convergeWindowS));
                    continue;
                }
                const double limit = th.propagationBaseMs + lat;
                line += QStringLiteral(" %1%2").arg(delay, 8, 'f', 0).arg(spike ? QLatin1Char('S') : QLatin1Char(' '));
                failIf(!spike && delay > limit, QStringLiteral("%1: %2 at t=%3 s reflected after %4 ms (limit %5 ms)")
                                                    .arg(c->spec().name, r.cause, rel(r.recvRealMs, base).trimmed()).arg(delay, 0, 'f', 0).arg(limit, 0, 'f', 0));
                failIf(proc > th.propagationBaseMs, QStringLiteral("%1: %2 at t=%3 s took %4 ms to apply after delivery")
                                                        .arg(c->spec().name, r.cause, rel(r.recvRealMs, base).trimmed()).arg(proc, 0, 'f', 0));
            }
            out(line);
        }

        // Autopause / autoresume.
        for (double tb : m_bufferStarts) {
            double tPause = -1, tResume = -1;
            for (const StateRecord &r : obs) {
                if (r.recvRealMs < tb)
                    continue;
                if (tPause < 0 && r.cause == QLatin1String("autopause"))
                    tPause = r.recvRealMs;
                else if (tPause >= 0 && tResume < 0 && r.cause == QLatin1String("autoresume"))
                    tResume = r.recvRealMs;
            }
            out(QStringLiteral("\nWait-for-everyone: buffering at t=%1 s -> autopause at %2 s, autoresume at %3 s")
                    .arg(rel(tb, base).trimmed(), tPause < 0 ? QStringLiteral("-") : rel(tPause, base).trimmed(),
                         tResume < 0 ? QStringLiteral("-") : rel(tResume, base).trimmed()));
            failIf(tPause < 0, QStringLiteral("no autopause after the buffering stall at t=%1 s").arg(rel(tb, base).trimmed()));
            failIf(tPause >= 0 && tResume < 0, QStringLiteral("no autoresume after the autopause at t=%1 s").arg(rel(tPause, base).trimmed()));
        }

        int spikes = 0;
        for (const auto &p : m_proxies)
            spikes += p->spikes();
        out(QStringLiteral("\nNetwork: %1 loss spikes injected over %2 chunks; %3 disruption groups, %4 server state changes")
                .arg(spikes).arg([this] { int c = 0; for (const auto &p : m_proxies) c += p->chunks(); return c; }()).arg(groups.size()).arg(changes.size()));

        if (!m_csv.isEmpty()) {
            QFile f(m_csv);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&f);
                ts << "t_s,client,drift_ms,paused,clock_err_ms,engine_drift_ms\n";
                for (int i = 0; i < n; ++i)
                    for (const Sample &s : m_samples[i])
                        ts << (s.t - base) / 1000.0 << ',' << i << ',' << s.driftMs << ',' << (s.paused ? 1 : 0) << ',' << s.clockErrMs << ',' << s.engineDriftMs << '\n';
            }
        }

        if (failures == 0) {
            out(QStringLiteral("\nRESULT: PASS"));
            return 0;
        }
        out(QStringLiteral("\nRESULT: FAIL (%1)").arg(failures));
        for (const QString &m : failMsgs)
            out(QStringLiteral("  - %1").arg(m));
        return 1;
    }

    Scenario m_sc;
    QString m_host;
    quint16 m_port;
    QString m_csv;
    bool m_verbose;
    bool m_done = false;

    std::vector<std::unique_ptr<DelayProxy>> m_proxies;
    QVector<SimClient *> m_clients;
    RefObserver *m_obs = nullptr;
    QTimer m_poll, m_sampler;
    double m_t0 = 0, m_playStart = 0, m_endT = 0, m_joinedAt = 0, m_allReadyAt = 0;
    QVector<Anchor> m_anchors;
    QVector<double> m_bufferStarts;
    QVector<QVector<Sample>> m_samples;
};

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("couchsync_sim"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("CouchSync headless sync simulator (needs a running room server)"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("server"), QStringLiteral("Room server WebSocket URL (default ws://127.0.0.1:8787/ws)"), QStringLiteral("url"),
                      QStringLiteral("ws://127.0.0.1:8787/ws")});
    parser.addOption({QStringLiteral("scenario"), QStringLiteral("Scenario JSON file (default: built-in scenario)"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("seed"), QStringLiteral("RNG seed for the impairment"), QStringLiteral("n")});
    parser.addOption({QStringLiteral("csv"), QStringLiteral("Write all drift samples to this CSV file"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("verbose"), QStringLiteral("Print every server state change and scripted action")});
    parser.process(app);

    Scenario sc = defaultScenario();
    if (parser.isSet(QStringLiteral("scenario"))) {
        QString err;
        if (!loadScenario(parser.value(QStringLiteral("scenario")), sc, err)) {
            std::fprintf(stderr, "scenario: %s\n", err.toLocal8Bit().constData());
            return 2;
        }
    }
    if (parser.isSet(QStringLiteral("seed")))
        sc.seed = parser.value(QStringLiteral("seed")).toULongLong();

    const QUrl url(parser.value(QStringLiteral("server")));
    if (!url.isValid() || url.host().isEmpty() || url.scheme() != QLatin1String("ws")) {
        std::fprintf(stderr, "--server must be a ws:// URL (the impairment proxies are plain TCP)\n");
        return 2;
    }

    Runner runner(sc, url.host(), static_cast<quint16>(url.port(80)), parser.value(QStringLiteral("csv")),
                  parser.isSet(QStringLiteral("verbose")));
    QTimer::singleShot(0, &runner, [&runner] { runner.start(); });
    // Hard stop in case something hangs (setup <= 30 s, run <= endAtS).
    QTimer::singleShot(static_cast<int>((sc.endAtS + 45) * 1000), &app, [] {
        out(QStringLiteral("SIM ERROR: global timeout"));
        QCoreApplication::exit(2);
    });
    return app.exec();
}
