// SPDX-License-Identifier: GPL-3.0-or-later
#include "RoomClient.h"

#include "SyncLog.h"
#include "WsConn.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QUrl>

#include <algorithm>
#include <cmath>

using couchsync::syncLog;

namespace {

constexpr int kProtocol = 1;
constexpr int kPingBurst = 8;
constexpr int kPingBurstIntervalMs = 50;
constexpr int kPingIntervalMs = 5000;

QString fmtTime(double sec)
{
    const int s = static_cast<int>(std::max(0.0, std::floor(sec + 0.01)));
    const int h = s / 3600;
    const int m = (s % 3600) / 60;
    const int ss = s % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(ss, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(ss, 2, 10, QLatin1Char('0'));
}

// "host[:port]" from a ws:// URL string, for invite links.
QString hostPortOf(const QString &serverUrl)
{
    const QUrl u(serverUrl);
    if (u.host().isEmpty())
        return {};
    QString hp = u.host();
    if (u.port() > 0)
        hp += QStringLiteral(":%1").arg(u.port());
    return hp;
}

}  // namespace

RoomClient::RoomClient(QObject *parent) : QObject(parent)
{
    m_pingTimer.setSingleShot(true);
    connect(&m_pingTimer, &QTimer::timeout, this, &RoomClient::sendPing);
}

RoomClient::~RoomClient()
{
    if (m_ws)
        m_ws->shutdown();
}

QString RoomClient::savedName() const
{
    return QSettings().value(QStringLiteral("room/name")).toString();
}

QString RoomClient::savedServer() const
{
    return QSettings().value(QStringLiteral("room/server"), QStringLiteral("ws://localhost:8787/ws")).toString();
}

void RoomClient::setSavedName(const QString &name)
{
    QSettings().setValue(QStringLiteral("room/name"), name);
    emit savedChanged();
}

void RoomClient::setSavedServer(const QString &server)
{
    QSettings().setValue(QStringLiteral("room/server"), server);
    emit savedChanged();
}

QVariantMap RoomClient::policy() const
{
    QVariantMap p;
    p[QStringLiteral("controls")] = hostOnly() ? QStringLiteral("host") : QStringLiteral("all");
    p[QStringLiteral("waitMode")] = waitForAll() ? QStringLiteral("wait") : QStringLiteral("nowait");
    return p;
}

QString RoomClient::inviteLink() const
{
    if (m_roomCode.isEmpty())
        return {};
    return QStringLiteral("couchsync://join/%1/%2").arg(hostPortOf(m_serverUrl), m_roomCode);
}

void RoomClient::copyText(const QString &text)
{
    if (QClipboard *cb = QGuiApplication::clipboard())
        cb->setText(text);
}

QVariantMap RoomClient::parseInvite(const QString &text) const
{
    const QString t = text.trimmed();
    static const QString prefix = QStringLiteral("couchsync://join/");
    if (!t.startsWith(prefix, Qt::CaseInsensitive))
        return {};
    const QStringList parts = t.mid(prefix.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return {};
    const QString hostPort = parts.at(0);
    const bool secure = hostPort.endsWith(QLatin1String(":443"));
    QVariantMap m;
    m[QStringLiteral("server")] = QStringLiteral("%1://%2/ws").arg(secure ? QStringLiteral("wss") : QStringLiteral("ws"), hostPort);
    m[QStringLiteral("code")] = parts.at(1).toUpper();
    return m;
}

// ---- connection lifecycle -------------------------------------------------------

void RoomClient::createRoom(const QString &serverUrl, const QString &name, const QString &password)
{
    start(serverUrl, name, QString(), password);
}

void RoomClient::joinRoom(const QString &serverUrl, const QString &name, const QString &code,
                          const QString &password)
{
    start(serverUrl, name, code.trimmed().toUpper(), password);
}

void RoomClient::start(const QString &serverUrl, const QString &name, const QString &code, const QString &password)
{
    if (m_connecting || m_inRoom)
        return;
    m_serverUrl = serverUrl.trimmed();
    m_helloName = name.trimmed();
    m_helloRoom = code;
    m_helloPassword = password;
    m_fatalHandled = false;
    m_connecting = true;
    emit connectingChanged();

    syncLog(QStringLiteral("connecting to %1 as '%2' (%3)")
                .arg(m_serverUrl, m_helloName, code.isEmpty() ? QStringLiteral("create") : QStringLiteral("join ") + code));
    m_ws = new WsConn(this);
    connect(m_ws, &WsConn::opened, this, &RoomClient::onOpened);
    connect(m_ws, &WsConn::closed, this, &RoomClient::onClosed);
    connect(m_ws, &WsConn::textReceived, this, &RoomClient::onText);
    m_ws->open(QUrl(m_serverUrl));
}

void RoomClient::leave()
{
    if (!m_connecting && !m_inRoom)
        return;
    syncLog(QStringLiteral("leaving room %1").arg(m_roomCode));
    cleanup(true);
}

void RoomClient::cleanup(bool emitLeft)
{
    if (m_ws) {
        m_ws->shutdown(); // disconnects its signals; deletes itself
        m_ws = nullptr;
    }
    m_pingTimer.stop();
    const bool wasInRoom = m_inRoom;
    const bool wasConnecting = m_connecting;
    m_inRoom = false;
    m_connecting = false;
    m_roomCode.clear();
    m_memberId.clear();
    m_isHost = false;
    m_members.clear();
    m_media.clear();
    m_waitingIds.clear();
    m_waitingNames.clear();
    m_state.reset();
    m_clock.reset();
    m_burstLeft = 0;
    m_pingCount = 0;
    if (wasConnecting)
        emit connectingChanged();
    if (wasInRoom)
        emit connectedChanged();
    emit roomChanged();
    emit membersChanged();
    emit policyChanged();
    emit mediaChanged();
    emit waitingChanged();
    emit clockChanged();
    if (emitLeft && (wasInRoom || wasConnecting))
        emit left();
}

void RoomClient::onOpened()
{
    QJsonObject hello{
        {QStringLiteral("type"), QStringLiteral("hello")},
        {QStringLiteral("protocol"), kProtocol},
        {QStringLiteral("client"), QStringLiteral("couchsync/0.1.0")},
        {QStringLiteral("name"), m_helloName},
        {QStringLiteral("room"), m_helloRoom.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m_helloRoom)},
    };
    if (!m_helloPassword.isEmpty())
        hello.insert(QStringLiteral("password"), m_helloPassword);
    send(hello);
}

void RoomClient::onClosed(const QString &reason)
{
    const bool relevant = m_connecting || m_inRoom;
    const bool wasInRoom = m_inRoom;
    const bool already = m_fatalHandled;
    syncLog(QStringLiteral("socket closed: %1").arg(reason));
    if (relevant && !already) {
        emit errorOccurred(wasInRoom ? QStringLiteral("disconnected") : QStringLiteral("connect_failed"),
                           wasInRoom ? QStringLiteral("Disconnected from the server: %1").arg(reason)
                                     : QStringLiteral("Could not connect: %1").arg(reason));
    }
    cleanup(true);
}

void RoomClient::send(const QJsonObject &obj)
{
    if (m_ws && m_ws->isOpen())
        m_ws->sendText(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

// ---- incoming ---------------------------------------------------------------------

void RoomClient::onText(const QString &text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject())
        return;
    const QJsonObject o = doc.object();
    const QString type = o.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("welcome")) {
        handleWelcome(o);
    } else if (type == QLatin1String("state")) {
        handleState(o, false);
    } else if (type == QLatin1String("members")) {
        handleMembers(o.value(QStringLiteral("members")).toArray());
    } else if (type == QLatin1String("pong")) {
        handlePong(o);
    } else if (type == QLatin1String("error")) {
        handleError(o);
    } else if (type == QLatin1String("chat")) {
        const QString from = o.value(QStringLiteral("from")).toString();
        syncLog(QStringLiteral("chat from %1: %2")
                    .arg(o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("text")).toString()));
        emit chatReceived(o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("text")).toString(),
                          from == m_memberId, o.value(QStringLiteral("serverTime")).toDouble());
    }
}

void RoomClient::handleWelcome(const QJsonObject &o)
{
    m_memberId = o.value(QStringLiteral("memberId")).toString();
    m_roomCode = o.value(QStringLiteral("room")).toString();
    m_connecting = false;
    m_inRoom = true;
    m_clock.reset();
    syncLog(QStringLiteral("welcome: member=%1 room=%2 serverTime=%3")
                .arg(m_memberId, m_roomCode)
                .arg(o.value(QStringLiteral("serverTime")).toDouble(), 0, 'f', 1));
    emit connectingChanged();
    emit connectedChanged();
    emit roomChanged();
    handleMembers(o.value(QStringLiteral("members")).toArray());
    handleState(o.value(QStringLiteral("state")).toObject(), true);
    emit joined();

    // Ping burst: 8 pings ~50 ms apart, then one every 5 s.
    m_burstLeft = kPingBurst;
    m_pingCount = 0;
    m_pingTimer.start(kPingBurstIntervalMs);
}

void RoomClient::handleMembers(const QJsonArray &arr)
{
    QVariantList list;
    bool host = false;
    for (const QJsonValue &v : arr) {
        const QJsonObject m = v.toObject();
        QVariantMap e;
        const QString id = m.value(QStringLiteral("id")).toString();
        e[QStringLiteral("id")] = id;
        e[QStringLiteral("name")] = m.value(QStringLiteral("name")).toString();
        e[QStringLiteral("host")] = m.value(QStringLiteral("host")).toBool();
        e[QStringLiteral("state")] = m.value(QStringLiteral("state")).toString();
        e[QStringLiteral("driftMs")] = m.value(QStringLiteral("drift")).toDouble() * 1000.0;
        e[QStringLiteral("isMe")] = (id == m_memberId);
        if (id == m_memberId)
            host = m.value(QStringLiteral("host")).toBool();
        list.append(e);
    }
    m_members = list;
    const bool hostChanged = (host != m_isHost);
    m_isHost = host;
    updateWaitingNames();
    emit membersChanged();
    if (hostChanged)
        syncLog(QStringLiteral("host status: %1").arg(host ? QStringLiteral("host") : QStringLiteral("member")));
    syncLog(QStringLiteral("members: %1").arg(
        [&] {
            QStringList names;
            for (const QVariant &v : list) {
                const QVariantMap m = v.toMap();
                names << QStringLiteral("%1%2(%3)").arg(m.value("name").toString(), m.value("host").toBool() ? QStringLiteral("*") : QString(),
                                                        m.value("state").toString());
            }
            return names.join(QLatin1String(", "));
        }()));
}

QString RoomClient::nameOf(const QString &id) const
{
    for (const QVariant &v : m_members) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() == id)
            return m.value(QStringLiteral("name")).toString();
    }
    return QStringLiteral("Someone");
}

void RoomClient::updateWaitingNames()
{
    QStringList names;
    for (const QString &id : std::as_const(m_waitingIds))
        names << (id == m_memberId ? QStringLiteral("you") : nameOf(id));
    if (names != m_waitingNames) {
        m_waitingNames = names;
        emit waitingChanged();
    }
}

void RoomClient::handleState(const QJsonObject &o, bool fromWelcome)
{
    couchsync::RoomState st;
    st.seq = static_cast<std::int64_t>(o.value(QStringLiteral("seq")).toDouble());
    if (m_state && st.seq <= m_state->seq)
        return;
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

    const std::optional<couchsync::RoomState> prev = m_state;
    m_state = st;

    // Media.
    QVariantMap media;
    const QJsonValue mv = o.value(QStringLiteral("media"));
    if (mv.isObject()) {
        media[QStringLiteral("title")] = mv.toObject().value(QStringLiteral("title")).toString();
        media[QStringLiteral("duration")] = mv.toObject().value(QStringLiteral("duration")).toDouble();
    }
    const bool mediaDiff = (media != m_media);
    m_media = media;

    // Waiting list.
    QStringList waiting;
    for (const QJsonValue &v : o.value(QStringLiteral("waitingFor")).toArray())
        waiting << v.toString();
    const bool waitingDiff = (waiting != m_waitingIds);
    m_waitingIds = waiting;
    updateWaitingNames();

    syncLog(QStringLiteral("state seq=%1 playing=%2 pos=%3 serverTime=%4 autoPaused=%5 waiting=%6")
                .arg(st.seq)
                .arg(st.playing)
                .arg(st.position, 0, 'f', 3)
                .arg(st.serverTime, 0, 'f', 1)
                .arg(st.autoPaused)
                .arg(m_waitingNames.join(QLatin1Char(','))));

    if (!prev || prev->hostOnlyControls != st.hostOnlyControls || prev->waitMode != st.waitMode)
        emit policyChanged();
    if (mediaDiff)
        emit mediaChanged();
    if (waitingDiff || !prev || prev->autoPaused != st.autoPaused)
        emit waitingChanged();

    // Toasts for the cause (other people's actions only).
    if (!fromWelcome) {
        const QJsonObject cause = o.value(QStringLiteral("cause")).toObject();
        const QString ctype = cause.value(QStringLiteral("type")).toString();
        const QString by = cause.value(QStringLiteral("by")).toString();
        const bool mine = (by == m_memberId);
        const QString who = nameOf(by);
        if (!mine && !by.isEmpty()) {
            if (ctype == QLatin1String("play"))
                emit toast(QStringLiteral("%1 resumed").arg(who));
            else if (ctype == QLatin1String("pause"))
                emit toast(QStringLiteral("%1 paused").arg(who));
            else if (ctype == QLatin1String("seek"))
                emit toast(QStringLiteral("%1 seeked to %2").arg(who, fmtTime(st.position)));
            else if (ctype == QLatin1String("policy"))
                emit toast(QStringLiteral("%1 changed room settings").arg(who));
            else if (ctype == QLatin1String("media"))
                emit toast(QStringLiteral("%1 loaded %2").arg(who, media.value(QStringLiteral("title")).toString()));
        }
        if (st.autoPaused && !m_waitingNames.isEmpty() && waitingDiff)
            emit toast(QStringLiteral("Waiting for %1…").arg(m_waitingNames.join(QLatin1String(", "))));
    }

    emit stateChanged();
}

void RoomClient::handleError(const QJsonObject &o)
{
    const QString code = o.value(QStringLiteral("code")).toString();
    const QString message = o.value(QStringLiteral("message")).toString();
    syncLog(QStringLiteral("error from server: %1 (%2)").arg(code, message));
    if (code == QLatin1String("not_allowed")) {
        emit notAllowed();
        emit toast(message.isEmpty() ? QStringLiteral("Only the host can do that") : message);
        return;
    }
    // Everything else is fatal and followed by a close from the server.
    m_fatalHandled = true;
    emit errorOccurred(code, message.isEmpty() ? code : message);
    cleanup(true);
}

// ---- clock sync -------------------------------------------------------------------

void RoomClient::sendPing()
{
    if (!m_inRoom)
        return;
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("t0"), couchsync::steadyNowMs()}});
    if (m_burstLeft > 0)
        --m_burstLeft;
    m_pingTimer.start(m_burstLeft > 0 ? kPingBurstIntervalMs : kPingIntervalMs);
}

void RoomClient::handlePong(const QJsonObject &o)
{
    const double t3 = couchsync::steadyNowMs();
    const double t0 = o.value(QStringLiteral("t0")).toDouble();
    const double t1 = o.value(QStringLiteral("t1")).toDouble();
    const double t2 = o.value(QStringLiteral("t2")).toDouble();
    if (!m_clock.addSample(t0, t1, t2, t3))
        return;
    ++m_pingCount;
    syncLog(QStringLiteral("clock: offset=%1 ms rtt=%2 ms samples=%3 (n=%4)")
                .arg(m_clock.offset(), 0, 'f', 1)
                .arg(m_clock.rtt(), 0, 'f', 1)
                .arg(m_clock.sampleCount())
                .arg(m_pingCount));
    emit clockChanged();
}

// ---- outgoing ---------------------------------------------------------------------

void RoomClient::sendChat(const QString &text)
{
    if (!m_inRoom || text.isEmpty())
        return;
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")}, {QStringLiteral("text"), text.left(500)}});
}

void RoomClient::sendPlay()
{
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("intent.play")}});
}

void RoomClient::sendPause(double position)
{
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("intent.pause")}, {QStringLiteral("position"), position}});
}

void RoomClient::sendSeek(double position)
{
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("intent.seek")}, {QStringLiteral("position"), position}});
}

void RoomClient::sendMediaSet(const QString &title, double duration)
{
    syncLog(QStringLiteral("media.set '%1' duration=%2").arg(title).arg(duration, 0, 'f', 1));
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("media.set")},
                     {QStringLiteral("title"), title.left(512)},
                     {QStringLiteral("duration"), duration}});
}

void RoomClient::setHostOnly(bool hostOnly)
{
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("intent.setPolicy")},
                     {QStringLiteral("controls"), hostOnly ? QStringLiteral("host") : QStringLiteral("all")}});
}

void RoomClient::setWaitForAll(bool wait)
{
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("intent.setPolicy")},
                     {QStringLiteral("waitMode"), wait ? QStringLiteral("wait") : QStringLiteral("nowait")}});
}

void RoomClient::sendStatus(bool ready, double localPos, double driftS)
{
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("status")},
                     {QStringLiteral("state"), ready ? QStringLiteral("ready") : QStringLiteral("waiting")},
                     {QStringLiteral("localPos"), localPos},
                     {QStringLiteral("drift"), driftS}});
}
