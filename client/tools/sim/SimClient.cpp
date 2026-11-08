// SPDX-License-Identifier: GPL-3.0-or-later
#include "SimClient.h"

#include "WsConn.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include <algorithm>
#include <cmath>

using couchsync::MemberStatus;
using couchsync::PlayerSample;

namespace sim {

namespace {
const QString kType = QStringLiteral("type");
}

bool parseStateRecord(const QJsonObject &o, StateRecord &out)
{
    couchsync::RoomState st;
    st.seq = static_cast<std::int64_t>(o.value(QStringLiteral("seq")).toDouble());
    st.playing = o.value(QStringLiteral("playing")).toBool();
    st.position = o.value(QStringLiteral("position")).toDouble();
    st.serverTime = o.value(QStringLiteral("serverTime")).toDouble();
    st.rate = o.value(QStringLiteral("rate")).toDouble(1.0);
    st.autoPaused = o.value(QStringLiteral("autoPaused")).toBool();
    const QJsonObject pol = o.value(QStringLiteral("policy")).toObject();
    st.hostOnlyControls = pol.value(QStringLiteral("controls")).toString() == QLatin1String("host");
    st.waitMode = pol.value(QStringLiteral("waitMode")).toString() == QLatin1String("nowait")
                      ? couchsync::WaitMode::NoWait
                      : couchsync::WaitMode::Wait;
    out.st = st;
    const QJsonObject cause = o.value(QStringLiteral("cause")).toObject();
    out.cause = cause.value(kType).toString();
    out.by = cause.value(QStringLiteral("by")).toString();
    out.hasMedia = o.value(QStringLiteral("media")).isObject();
    out.waitingFor.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("waitingFor")).toArray())
        out.waitingFor << v.toString();
    return true;
}

// ---- SimConn ----------------------------------------------------------------------

SimConn::SimConn(const QString &name, const QString &host, quint16 port, QObject *parent)
    : QObject(parent), m_name(name), m_host(host), m_port(port)
{
    m_pingTimer.setSingleShot(true);
    m_pingTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pingTimer, &QTimer::timeout, this, &SimConn::sendPing);
}

SimConn::~SimConn()
{
    if (m_ws)
        m_ws->shutdown();
}

double SimConn::pingIntervalMs(int pingsSent) const
{
    return pingsSent < 8 ? 50 : 5000;
}

void SimConn::connectToRoom(const QString &roomCode)
{
    m_ws = new WsConn(this);
    connect(m_ws, &WsConn::opened, this, [this, roomCode] {
        QJsonObject hello{
            {kType, QStringLiteral("hello")},
            {QStringLiteral("protocol"), 1},
            {QStringLiteral("client"), QStringLiteral("couchsync-sim/0.1.0")},
            {QStringLiteral("name"), m_name},
            {QStringLiteral("room"), roomCode.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(roomCode)},
        };
        send(hello);
    });
    connect(m_ws, &WsConn::closed, this, [this](const QString &why) { emit failed(QStringLiteral("%1: connection closed: %2").arg(m_name, why)); });
    connect(m_ws, &WsConn::textReceived, this, &SimConn::onText);
    m_ws->open(QUrl(QStringLiteral("ws://%1:%2/ws").arg(m_host).arg(m_port)));
}

void SimConn::send(const QJsonObject &o)
{
    if (m_ws)
        m_ws->sendText(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void SimConn::onText(const QString &text)
{
    const double recvReal = realNowMs();
    const double t3 = localNow();
    const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = o.value(kType).toString();
    if (type == QLatin1String("welcome")) {
        m_roomCode = o.value(QStringLiteral("room")).toString();
        m_inRoom = true;
        StateRecord rec;
        parseStateRecord(o.value(QStringLiteral("state")).toObject(), rec);
        rec.recvRealMs = recvReal;
        m_lastSeq = rec.st.seq;
        m_states.push_back(rec);
        sendPing();
        onWelcomeExtra();
        emit joined();
        emit stateAccepted(rec);
    } else if (type == QLatin1String("state")) {
        StateRecord rec;
        parseStateRecord(o, rec);
        if (rec.st.seq <= m_lastSeq)
            return;
        rec.recvRealMs = recvReal;
        m_lastSeq = rec.st.seq;
        m_states.push_back(rec);
        emit stateAccepted(rec);
    } else if (type == QLatin1String("pong")) {
        m_clock.addSample(o.value(QStringLiteral("t0")).toDouble(), o.value(QStringLiteral("t1")).toDouble(),
                          o.value(QStringLiteral("t2")).toDouble(), t3);
    } else if (type == QLatin1String("error")) {
        emit failed(QStringLiteral("%1: server error %2: %3")
                        .arg(m_name, o.value(QStringLiteral("code")).toString(), o.value(QStringLiteral("message")).toString()));
    }
}

void SimConn::sendPing()
{
    if (!m_inRoom)
        return;
    send(QJsonObject{{kType, QStringLiteral("ping")}, {QStringLiteral("t0"), localNow()}});
    ++m_pingsSent;
    m_pingTimer.start(static_cast<int>(pingIntervalMs(m_pingsSent)));
}

// ---- SimClient --------------------------------------------------------------------

SimClient::SimClient(const ClientSpec &spec, const QString &host, quint16 port, std::uint64_t seed, QObject *parent)
    : SimConn(spec.name, host, port, parent), m_spec(spec), m_rng(seed)
{
    m_tickTimer.setInterval(100);
    m_tickTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_tickTimer, &QTimer::timeout, this, &SimClient::tick);
    for (QTimer *t : {&m_startTimer, &m_seekTimer, &m_bufferTimer}) {
        t->setSingleShot(true);
        t->setTimerType(Qt::PreciseTimer);
    }
    connect(&m_startTimer, &QTimer::timeout, this, &SimClient::tick);
    connect(&m_seekTimer, &QTimer::timeout, this, &SimClient::finishSeek);
    connect(&m_bufferTimer, &QTimer::timeout, this, [this] {
        advance(realNowMs());
        m_buffering = false;
        tick();
    });
    connect(this, &SimConn::stateAccepted, this, &SimClient::onState);
}

void SimClient::onWelcomeExtra()
{
    m_lastAdvance = realNowMs();
    m_tickTimer.start();
}

// Player model --------------------------------------------------------------------

void SimClient::advance(double nowMs)
{
    double from = m_lastAdvance;
    if (nowMs <= from)
        return;
    m_lastAdvance = nowMs;
    if (m_seekFrozen) {
        if (nowMs < m_seekDoneAt)
            return;
        from = m_seekDoneAt;
        m_seekFrozen = false;
    }
    if (!m_paused && !m_buffering)
        m_pos += (nowMs - from) / 1000.0 * m_speed * (1 + m_spec.mediaSkewPpm * 1e-6);
}

double SimClient::positionAt(double realMs)
{
    advance(realMs);
    return m_pos;
}

void SimClient::startSeek(double target, SeekKind kind)
{
    const double now = realNowMs();
    advance(now);
    m_pos = std::max(0.0, target);
    m_seeking = true;
    m_seekFrozen = true;
    m_seekKind = kind;
    m_seekDoneAt = now + m_spec.seekLatencyMs;
    m_seekTimer.start(static_cast<int>(std::ceil(m_spec.seekLatencyMs)));
}

void SimClient::finishSeek()
{
    advance(realNowMs());
    // A short seek can finish before any tick saw seeking == true: show it once (as the app does).
    if (m_seekKind == SeekKind::Engine && m_engine.seekInFlight() && !m_seekObserved)
        tick();
    m_seeking = false;
    if (m_seekKind == SeekKind::User) {
        send(QJsonObject{{kType, QStringLiteral("intent.seek")}, {QStringLiteral("position"), m_pos}});
        m_engine.noteLocalIntent(serverNowEst());
    }
    tick();
}

void SimClient::sendMediaSet(const QString &title, double duration)
{
    send(QJsonObject{{kType, QStringLiteral("media.set")}, {QStringLiteral("title"), title}, {QStringLiteral("duration"), duration}});
}

void SimClient::userPlay()
{
    advance(realNowMs());
    m_paused = false;
    send(QJsonObject{{kType, QStringLiteral("intent.play")}});
    m_engine.noteLocalIntent(serverNowEst());
}

void SimClient::userPause()
{
    advance(realNowMs());
    m_paused = true;
    send(QJsonObject{{kType, QStringLiteral("intent.pause")}, {QStringLiteral("position"), m_pos}});
    m_engine.noteLocalIntent(serverNowEst());
}

void SimClient::userSeekBy(double deltaS)
{
    advance(realNowMs());
    startSeek(m_pos + deltaS, SeekKind::User);
    m_engine.noteLocalIntent(serverNowEst());
}

void SimClient::startBuffering(double ms)
{
    advance(realNowMs());
    m_buffering = true;
    m_bufferTimer.start(static_cast<int>(ms));
}

// Controller loop -------------------------------------------------------------------

void SimClient::onState(const StateRecord &rec)
{
    m_arrivals.push_back({rec.st.seq, rec.recvRealMs});
    if (!m_engine.applyState(rec.st))
        return;
    tick();  // react right away, like SyncController::onRoomState
}

void SimClient::tick()
{
    if (!inRoom() || !clock().hasEstimate())
        return;
    const double now = realNowMs();
    advance(now);
    const double serverNow = serverNowEst();

    PlayerSample s;
    s.serverNowMs = serverNow;
    s.localPos = m_pos + m_rng.sym(m_spec.posJitterMs / 1000.0);
    s.paused = m_paused;
    s.seeking = m_seeking;
    if (m_seeking && m_engine.seekInFlight())
        m_seekObserved = true;
    s.buffering = m_buffering;
    s.currentSpeed = m_speed;
    s.fileLoaded = true;

    const couchsync::SyncActions a = m_engine.tick(s);

    if (a.driftS)
        m_engineDriftMs = *a.driftS * 1000.0;
    if (a.setPaused)
        m_paused = *a.setPaused;
    if (a.seekTo) {
        ++m_engineSeeks;
        m_seekObserved = false;
        startSeek(*a.seekTo, SeekKind::Engine);
    }
    if (a.setSpeed) {
        m_speed = *a.setSpeed;
        m_minSpeed = std::min(m_minSpeed, m_speed);
        m_maxSpeed = std::max(m_maxSpeed, m_speed);
    }

    const bool ready = (a.status == MemberStatus::Ready);
    if (!m_lastReady || *m_lastReady != ready || now - m_lastStatusAt >= 1000) {
        m_lastReady = ready;
        m_lastStatusAt = now;
        sendStatus(ready, s.localPos, a.driftS.value_or(0.0));
    }
    armStartTimer();
}

void SimClient::armStartTimer()
{
    const auto ms = m_engine.msUntilScheduledStart(serverNowEst());
    if (ms && *ms > 0)
        m_startTimer.start(static_cast<int>(std::ceil(*ms)) + 1);
    else
        m_startTimer.stop();
}

void SimClient::sendStatus(bool ready, double localPos, double drift)
{
    send(QJsonObject{{kType, QStringLiteral("status")},
                     {QStringLiteral("state"), ready ? QStringLiteral("ready") : QStringLiteral("waiting")},
                     {QStringLiteral("localPos"), localPos},
                     {QStringLiteral("drift"), drift}});
}

}  // namespace sim
