// SPDX-License-Identifier: GPL-3.0-or-later
#include "Proxy.h"

#include <QHostAddress>
#include <QQueue>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace sim {

namespace {
struct Chunk {
    double releaseMs;
    QByteArray data;
};
}  // namespace

class DelayProxy::Pipe : public QObject
{
public:
    Pipe(DelayProxy &proxy, QTcpSocket *client, const QString &host, quint16 port) : QObject(&proxy), m_proxy(proxy)
    {
        m_up.src = client;
        m_down.dst = client;
        m_down.src = m_up.dst = new QTcpSocket(this);
        client->setParent(this);
        for (QTcpSocket *s : {client, m_up.dst})
            s->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        for (Dir *d : {&m_up, &m_down}) {
            d->timer.setSingleShot(true);
            d->timer.setTimerType(Qt::PreciseTimer);
            connect(&d->timer, &QTimer::timeout, this, [this, d] { flush(*d); });
            connect(d->src, &QTcpSocket::readyRead, this, [this, d] { onData(*d); });
            connect(d->src, &QTcpSocket::disconnected, this, [this, d] {
                d->srcClosed = true;
                flush(*d);
                maybeDone();
            });
        }
        m_up.isUp = true;
        connect(m_up.dst, &QTcpSocket::errorOccurred, this, [this] {
            if (m_up.dst->state() != QAbstractSocket::ConnectedState)
                m_down.dst->disconnectFromHost();
        });
        m_up.dst->connectToHost(host, port);
    }

private:
    struct Dir {
        QTcpSocket *src = nullptr;
        QTcpSocket *dst = nullptr;
        QQueue<Chunk> q;
        QTimer timer;
        double last = 0;
        bool srcClosed = false;
        bool isUp = false;
    };

    void onData(Dir &d)
    {
        const QByteArray data = d.src->readAll();
        if (data.isEmpty())
            return;
        const double now = realNowMs();
        const double release = std::max(now + m_proxy.nextDelayMs(d.isUp), d.last);
        d.last = release;
        d.q.enqueue({release, data});
        if (!d.timer.isActive())
            arm(d);
    }

    void arm(Dir &d)
    {
        if (d.q.isEmpty())
            return;
        const double wait = d.q.head().releaseMs - realNowMs();
        d.timer.start(wait <= 0 ? 0 : static_cast<int>(std::ceil(wait)));
    }

    void flush(Dir &d)
    {
        const double now = realNowMs();
        while (!d.q.isEmpty() && d.q.head().releaseMs <= now + 0.5) {
            if (d.dst->state() != QAbstractSocket::ConnectedState) {
                if (d.dst->state() == QAbstractSocket::UnconnectedState && d.dst->error() != QAbstractSocket::UnknownSocketError)
                    return;  // dead
                d.timer.start(1);  // upstream still connecting
                return;
            }
            d.dst->write(d.q.dequeue().data);
        }
        if (!d.q.isEmpty()) {
            arm(d);
            return;
        }
        if (d.srcClosed && d.dst->state() == QAbstractSocket::ConnectedState)
            d.dst->disconnectFromHost();
    }

    void maybeDone()
    {
        if (m_up.srcClosed && m_down.srcClosed)
            deleteLater();
    }

    DelayProxy &m_proxy;
    Dir m_up, m_down;
};

DelayProxy::DelayProxy(const QString &upstreamHost, quint16 upstreamPort, const Params &p, std::uint64_t seed,
                       QObject *parent)
    : QObject(parent), m_host(upstreamHost), m_port(upstreamPort), m_p(p), m_rng(seed)
{
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *c = m_server.nextPendingConnection())
            new Pipe(*this, c, m_host, m_port);
    });
}

bool DelayProxy::listen()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

double DelayProxy::nextDelayMs(bool up)
{
    ++m_chunks;
    double d = (up ? m_p.upMs : m_p.downMs) + m_rng.sym(m_p.jitterMs);
    if (m_p.lossP > 0 && m_rng.uni() < m_p.lossP) {
        d += m_p.spikeMinMs + m_rng.uni() * (m_p.spikeMaxMs - m_p.spikeMinMs);
        ++m_spikes;
    }
    return std::max(0.0, d);
}

}  // namespace sim
