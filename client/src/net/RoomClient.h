// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ClockSync.h"
#include "RoomState.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>

class WsConn;

// WebSocket client for docs/PROTOCOL.md (v1): connect, hello/welcome, clock sync pings,
// intents, status, chat, host settings. Exposes a QML-friendly API; SyncController reads
// the Qt-free RoomState via currentState() / the stateChanged() signal.
class RoomClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged) // in a room (welcome received)
    Q_PROPERTY(QString roomCode READ roomCode NOTIFY roomChanged)
    Q_PROPERTY(QString memberId READ memberId NOTIFY roomChanged)
    Q_PROPERTY(bool isHost READ isHost NOTIFY membersChanged)
    Q_PROPERTY(QVariantList members READ members NOTIFY membersChanged)
    Q_PROPERTY(QVariantMap policy READ policy NOTIFY policyChanged)
    Q_PROPERTY(bool hostOnly READ hostOnly NOTIFY policyChanged)
    Q_PROPERTY(bool waitForAll READ waitForAll NOTIFY policyChanged)
    Q_PROPERTY(QVariantMap media READ media NOTIFY mediaChanged) // {title, duration} or empty
    Q_PROPERTY(QStringList waitingNames READ waitingNames NOTIFY waitingChanged)
    Q_PROPERTY(bool autoPaused READ autoPaused NOTIFY waitingChanged)
    Q_PROPERTY(QString inviteLink READ inviteLink NOTIFY roomChanged)
    Q_PROPERTY(double rttMs READ rttMs NOTIFY clockChanged)
    Q_PROPERTY(double offsetMs READ offsetMs NOTIFY clockChanged)
    // Remembered in QSettings (join dialog defaults).
    Q_PROPERTY(QString savedName READ savedName WRITE setSavedName NOTIFY savedChanged)
    Q_PROPERTY(QString savedServer READ savedServer WRITE setSavedServer NOTIFY savedChanged)

public:
    explicit RoomClient(QObject *parent = nullptr);
    ~RoomClient() override;

    bool connecting() const { return m_connecting; }
    bool connected() const { return m_inRoom; }
    QString roomCode() const { return m_roomCode; }
    QString memberId() const { return m_memberId; }
    bool isHost() const { return m_isHost; }
    QVariantList members() const { return m_members; }
    QVariantMap policy() const;
    bool hostOnly() const { return m_state && m_state->hostOnlyControls; }
    bool waitForAll() const { return !m_state || m_state->waitMode == couchsync::WaitMode::Wait; }
    QVariantMap media() const { return m_media; }
    QStringList waitingNames() const { return m_waitingNames; }
    bool autoPaused() const { return m_state && m_state->autoPaused; }
    QString inviteLink() const;
    double rttMs() const { return m_clock.rtt(); }
    double offsetMs() const { return m_clock.offset(); }
    QString savedName() const;
    QString savedServer() const;
    void setSavedName(const QString &name);
    void setSavedServer(const QString &server);

    // C++ side (SyncController).
    bool hasClock() const { return m_clock.hasEstimate(); }
    double serverNowMs() const { return m_clock.serverNow(couchsync::steadyNowMs()); }
    const std::optional<couchsync::RoomState> &currentState() const { return m_state; }
    double mediaDuration() const { return m_media.value(QStringLiteral("duration")).toDouble(); }

    Q_INVOKABLE void createRoom(const QString &serverUrl, const QString &name, const QString &password);
    Q_INVOKABLE void joinRoom(const QString &serverUrl, const QString &name, const QString &code,
                              const QString &password);
    Q_INVOKABLE void leave();
    Q_INVOKABLE void sendChat(const QString &text);
    Q_INVOKABLE void setHostOnly(bool hostOnly);
    Q_INVOKABLE void setWaitForAll(bool wait);
    Q_INVOKABLE void copyText(const QString &text);
    // Accepts a pasted couchsync://join/<host[:port]>/<code> link. Returns {server, code}, or {} if not a link.
    Q_INVOKABLE QVariantMap parseInvite(const QString &text) const;

    void sendPlay();
    void sendPause(double position);
    void sendSeek(double position);
    void sendMediaSet(const QString &title, double duration);
    void sendStatus(bool ready, double localPos, double driftS);

signals:
    void connectingChanged();
    void connectedChanged();
    void roomChanged();
    void membersChanged();
    void policyChanged();
    void mediaChanged();
    void waitingChanged();
    void clockChanged();
    void savedChanged();
    void stateChanged();   // a new state was accepted (seq increased); read currentState()
    void joined();         // welcome received
    void left();           // left or lost the room (after cleanup)
    void notAllowed();     // an intent was rejected (error not_allowed)
    void errorOccurred(const QString &code, const QString &message);
    void toast(const QString &text);
    void chatReceived(const QString &name, const QString &text, bool mine, double serverTime);

private:
    void start(const QString &serverUrl, const QString &name, const QString &code, const QString &password);
    void onOpened();
    void onClosed(const QString &reason);
    void onText(const QString &text);
    void send(const QJsonObject &obj);
    void handleWelcome(const QJsonObject &o);
    void handleState(const QJsonObject &o, bool fromWelcome);
    void handleMembers(const QJsonArray &arr);
    void handleError(const QJsonObject &o);
    void handlePong(const QJsonObject &o);
    void sendPing();
    void schedulePing();
    void cleanup(bool emitLeft);
    QString nameOf(const QString &id) const;
    void updateWaitingNames();

    QPointer<WsConn> m_ws;
    QString m_serverUrl;
    QString m_helloName;
    QString m_helloRoom; // empty = create
    QString m_helloPassword;
    bool m_connecting = false;
    bool m_inRoom = false;
    bool m_fatalHandled = false;
    QString m_roomCode;
    QString m_memberId;
    bool m_isHost = false;
    QVariantList m_members;
    QVariantMap m_media;
    QStringList m_waitingIds;
    QStringList m_waitingNames;
    std::optional<couchsync::RoomState> m_state;
    couchsync::ClockSync m_clock;
    QTimer m_pingTimer;
    int m_burstLeft = 0;
    int m_pingCount = 0;
};
