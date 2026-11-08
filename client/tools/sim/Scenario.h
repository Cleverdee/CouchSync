// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

namespace sim {

struct ClientSpec {
    QString name;
    double latencyMs = 50;       // one-way, each direction
    double jitterMs = 0;         // +- uniform, per TCP chunk
    double lossP = 0;            // probability per chunk of a retransmit-style delay spike
    double clockSkewPpm = 0;     // local steady clock vs real time
    double clockOffsetMs = 0;    // local steady clock epoch
    double mediaSkewPpm = 0;     // player media clock vs real time
    double seekLatencyMs = 250;  // how long a seek takes
    double posJitterMs = 10;     // +- noise on reported localPos
};

// action: "seek" (value = delta s), "pause", "play", "buffer" (value = duration ms).
// Times are seconds after the host's initial play.
struct Event {
    double atS = 0;
    QString action;
    int client = 0;
    double value = 0;
};

struct Thresholds {
    double steadyP95Ms = 30;
    double steadyMaxMs = 80;
    double convergeMs = 50;       // |true drift| must be below this...
    double convergeWindowS = 10;  // ...within this long after a disruption
    double propagationBaseMs = 300;
    double groupGapS = 2.0;       // anchors closer than this belong to one disruption
};

struct Scenario {
    QString name = QStringLiteral("default");
    std::uint64_t seed = 1;
    double spikeMinMs = 200, spikeMaxMs = 1000;
    double endAtS = 76;
    QVector<ClientSpec> clients;
    QVector<Event> events;
    Thresholds th;
};

Scenario defaultScenario();
// Returns false and sets err on failure.
bool loadScenario(const QString &path, Scenario &out, QString &err);

}  // namespace sim
