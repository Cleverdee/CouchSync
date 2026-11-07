// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QSslSocket>
#include <QString>
#include <QUrl>

// Minimal RFC 6455 WebSocket client (text frames only) on top of QSslSocket.
//
// Why not QWebSocket: the Qt WebSockets module is not part of the local Qt install
// (aqt installed only the base modules). The interface mirrors QWebSocket's so it can
// be swapped for it later: open(), sendText(), shutdown(), opened/textReceived/closed.
//
// Lifetime: call shutdown() instead of deleting; the object flushes a close frame and
// deletes itself.
class WsConn : public QObject
{
    Q_OBJECT
public:
    explicit WsConn(QObject *parent = nullptr);

    void open(const QUrl &url); // ws:// or wss://
    void sendText(const QString &text);
    bool isOpen() const { return m_state == State::Open; }
    // Sends a close frame, disconnects all signals and deletes itself shortly after.
    void shutdown();

signals:
    void opened();
    void textReceived(const QString &text);
    // Emitted once when the connection fails or ends (not after shutdown()).
    void closed(const QString &reason);

private:
    enum class State { Idle, Connecting, Handshake, Open };

    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onDisconnected();
    bool parseHandshake();
    void parseFrames();
    void sendFrame(quint8 opcode, const QByteArray &payload);
    void finish(const QString &reason);

    QSslSocket m_sock;
    State m_state = State::Idle;
    QUrl m_url;
    QByteArray m_key;
    QByteArray m_buf;
    QByteArray m_fragment;
    bool m_fragmentIsText = false;
};
