// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ClockSync.h"
#include "Common.h"
#include "PingSchedule.h"
#include "RoomState.h"
#include "Scenario.h"
#include "SyncEngine.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <optional>

class WsConn;

namespace sim {

struct StateRecord {
    couchsync::RoomState st;
    QString cause;  // cause.type
    QString by;
    QStringList waitingFor;
    bool hasMedia = false;
    double recvRealMs = 0;
};

bool parseStateRecord(const QJsonObject &o, StateRecord &out);

// Common connection logic: hello/welcome, ping-based clock sync (couchsync::PingSchedule: burst, catch-up
// until ready, steady, spike follow-up; same as the app), state tracking. `localNow()` is the client's own (possibly skewed) steady clock.
class SimConn : public QObject
{
    Q_OBJECT
public:
    SimConn(const QString &name, const QString &host, quint16 port, QObject *parent);
    ~SimConn() override;

    void connectToRoom(const QString &roomCode);  // empty = create
    bool inRoom() const { return m_inRoom; }
    QString roomCode() const { return m_roomCode; }
    const couchsync::ClockSync &clock() const { return m_clock; }
    const QVector<StateRecord> &states() const { return m_states; }
    void send(const QJsonObject &o);

    virtual double localNow() const = 0;

signals:
    void joined();
    void failed(const QString &why);
    void stateAccepted(const sim::StateRecord &rec);

protected:
    // Delay until the next ping, called after each one was sent (`pingsSent` includes it).
    virtual int nextPingDelayMs(int pingsSent);
    // The clock may be acted on (see couchsync::PingSchedule::usable).
    bool clockUsable() const { return m_sched.usable(m_clock); }
    virtual void onWelcomeExtra() {}

private:
    void onText(const QString &text);
    void sendPing();

    QString m_name, m_host;
    quint16 m_port;
    QPointer<WsConn> m_ws;
    QString m_roomCode;
    bool m_inRoom = false;
    couchsync::ClockSync m_clock;
    QTimer m_pingTimer;
    couchsync::PingSchedule m_sched;
    couchsync::SendRate m_sendRate;
    int m_pingsSent = 0;
    bool m_pingHeld = false;
    QVector<StateRecord> m_states;
    std::int64_t m_lastSeq = 0;
};

// Direct (unimpaired) connection that observes the room. It provides the ground truth: the room
// state exactly as the server holds it, and a precise server clock (pings every 500 ms over loopback).
class RefObserver : public SimConn
{
    Q_OBJECT
public:
    RefObserver(const QString &host, quint16 port, QObject *parent = nullptr)
        : SimConn(QStringLiteral("truth"), host, port, parent)
    {
    }
    double localNow() const override { return realNowMs(); }
    // True server time (ms) at real time `realMs`.
    double serverAt(double realMs) const { return clock().serverNow(realMs); }
    const couchsync::RoomState *state() const { return states().isEmpty() ? nullptr : &states().last().st; }
    double expectedAt(double realMs) const { return state() ? state()->expectedPosition(serverAt(realMs)) : 0.0; }

protected:
    int nextPingDelayMs(int n) override { return n < 8 ? 50 : 500; }
};

// One fake participant: protocol client + fake player + the controller loop (same structure as
// SyncController: 100 ms tick, apply actions in order setPaused, seekTo, setSpeed, status on change
// and every 1 s, immediate tick after a state, single-shot timer for scheduled starts).
class SimClient : public SimConn
{
    Q_OBJECT
public:
    SimClient(const ClientSpec &spec, const QString &host, quint16 port, std::uint64_t seed, QObject *parent = nullptr);

    const ClientSpec &spec() const { return m_spec; }
    double localNow() const override { return realNowMs() * (1 + m_spec.clockSkewPpm * 1e-6) + m_spec.clockOffsetMs; }
    double serverNowEst() const { return clock().serverNow(localNow()); }

    // Ground-truth model state at real time `realMs` (advances the model).
    double positionAt(double realMs);
    bool paused() const { return m_paused; }
    bool buffering() const { return m_buffering; }
    // The engine's own (smoothed) drift estimate from its last tick, ms; for diagnostics only.
    double engineDriftMs() const { return m_engineDriftMs; }

    // Scripted user actions.
    void sendMediaSet(const QString &title, double duration);
    void userPlay();
    void userPause();
    void userSeekBy(double deltaS);
    void startBuffering(double ms);

    // Stats.
    // Status the client last reported, and when it changed (real ms), for the runner's checks.
    bool reportedReady() const { return m_lastReady && *m_lastReady; }
    bool wasReadyAt(double realMs) const
    {
        bool r = false;
        for (const auto &h : m_readyHistory) {
            if (h.first > realMs)
                break;
            r = h.second;
        }
        return r;
    }
    // First time at or after realMs when the client reported ready (-1 = never).
    double readyFrom(double realMs) const
    {
        bool r = wasReadyAt(realMs);
        if (r)
            return realMs;
        for (const auto &h : m_readyHistory)
            if (h.first > realMs && h.second)
                return h.first;
        return -1;
    }
    int engineSeeks() const { return m_engineSeeks; }
    double minSpeed() const { return m_minSpeed; }
    double maxSpeed() const { return m_maxSpeed; }
    // Real-time arrival of each accepted state (by seq).
    struct Arrival {
        std::int64_t seq;
        double realMs;
    };
    const QVector<Arrival> &arrivals() const { return m_arrivals; }

protected:
    void onWelcomeExtra() override;

private:
    enum class SeekKind { Engine, User };

    void advance(double nowMs);
    void tick();
    void onState(const StateRecord &rec);
    void startSeek(double target, SeekKind kind);
    void finishSeek();
    void armStartTimer();
    void sendStatus(bool ready, double localPos, double drift);

    ClientSpec m_spec;
    Rng m_rng;
    couchsync::SyncEngine m_engine;
    QTimer m_tickTimer, m_startTimer, m_seekTimer, m_bufferTimer;

    // fake player
    double m_pos = 0;
    bool m_paused = true;
    double m_speed = 1.0;
    bool m_seeking = false;      // reported flag, true until the seek's "playback restart"
    bool m_seekFrozen = false;   // position frozen until m_seekDoneAt
    double m_seekDoneAt = 0;
    SeekKind m_seekKind = SeekKind::Engine;
    bool m_seekObserved = false;
    bool m_buffering = false;
    double m_lastAdvance = 0;

    std::optional<bool> m_lastReady;
    QVector<std::pair<double, bool>> m_readyHistory;
    double m_lastStatusAt = -1e9;
    int m_engineSeeks = 0;
    double m_engineDriftMs = 0;
    double m_minSpeed = 1, m_maxSpeed = 1;
    QVector<Arrival> m_arrivals;
};

}  // namespace sim
