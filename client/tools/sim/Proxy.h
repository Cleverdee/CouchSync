// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Common.h"

#include <QObject>
#include <QString>
#include <QTcpServer>

namespace sim {

// Byte-level delay proxy between one fake client and the real server.
//
// Every chunk read from either side is held until now + latency + jitter (+ a spike with probability
// lossP) and then written to the other side. Release times are monotonic per direction, so bytes
// stay in order like TCP: a delay spike on one chunk (a modelled retransmit) holds back everything
// behind it (head-of-line blocking).
class DelayProxy : public QObject
{
public:
    struct Params {
        double upMs = 0;    // client -> server one-way latency
        double downMs = 0;  // server -> client one-way latency
        double jitterMs = 0;
        double lossP = 0;
        double spikeMinMs = 200;
        double spikeMaxMs = 1000;
    };

    DelayProxy(const QString &upstreamHost, quint16 upstreamPort, const Params &p, std::uint64_t seed,
               QObject *parent = nullptr);

    bool listen();  // 127.0.0.1, ephemeral port
    quint16 port() const { return m_server.serverPort(); }
    int spikes() const { return m_spikes; }
    int chunks() const { return m_chunks; }

    // Used by the connection pipes.
    double nextDelayMs(bool up);

private:
    class Pipe;
    QTcpServer m_server;
    QString m_host;
    quint16 m_port;
    Params m_p;
    Rng m_rng;
    int m_spikes = 0;
    int m_chunks = 0;
};

}  // namespace sim
