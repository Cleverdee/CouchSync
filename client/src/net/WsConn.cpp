// SPDX-License-Identifier: GPL-3.0-or-later
#include "WsConn.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTimer>

#include <utility>

namespace {

constexpr qint64 kMaxPayload = 1 << 20;

QByteArray randomBytes(int n)
{
    QByteArray b(n, 0);
    for (int i = 0; i < n; ++i)
        b[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return b;
}

}  // namespace

WsConn::WsConn(QObject *parent) : QObject(parent)
{
    connect(&m_sock, &QAbstractSocket::connected, this, &WsConn::onConnected);
    connect(&m_sock, &QSslSocket::encrypted, this, &WsConn::onConnected);
    connect(&m_sock, &QIODevice::readyRead, this, &WsConn::onReadyRead);
    connect(&m_sock, &QAbstractSocket::errorOccurred, this, &WsConn::onSocketError);
    connect(&m_sock, &QAbstractSocket::disconnected, this, &WsConn::onDisconnected);
}

void WsConn::open(const QUrl &url)
{
    m_url = url;
    m_state = State::Connecting;
    m_buf.clear();
    m_fragment.clear();
    const bool tls = url.scheme() == QLatin1String("wss");
    if (!url.isValid() || url.host().isEmpty() || (!tls && url.scheme() != QLatin1String("ws"))) {
        QTimer::singleShot(0, this, [this] { finish(QStringLiteral("Invalid server URL (expected ws://host:port/ws)")); });
        return;
    }
    const quint16 port = static_cast<quint16>(url.port(tls ? 443 : 80));
    if (tls)
        m_sock.connectToHostEncrypted(url.host(), port);
    else
        m_sock.connectToHost(url.host(), port);
}

void WsConn::onConnected()
{
    // For wss both connected() and encrypted() fire; only act after the TLS handshake.
    if (m_state != State::Connecting)
        return;
    const bool tls = m_url.scheme() == QLatin1String("wss");
    if (tls && !m_sock.isEncrypted())
        return;
    m_state = State::Handshake;
    m_key = randomBytes(16).toBase64();
    const int defPort = tls ? 443 : 80;
    QString host = m_url.host();
    if (m_url.port(defPort) != defPort)
        host += QStringLiteral(":%1").arg(m_url.port(defPort));
    QString path = m_url.path(QUrl::FullyEncoded);
    if (path.isEmpty())
        path = QStringLiteral("/");
    if (m_url.hasQuery())
        path += QLatin1Char('?') + m_url.query(QUrl::FullyEncoded);
    const QByteArray req = QStringLiteral("GET %1 HTTP/1.1\r\nHost: %2\r\nUpgrade: websocket\r\n"
                                          "Connection: Upgrade\r\nSec-WebSocket-Key: %3\r\n"
                                          "Sec-WebSocket-Version: 13\r\n\r\n")
                               .arg(path, host, QString::fromLatin1(m_key))
                               .toUtf8();
    m_sock.write(req);
}

void WsConn::onReadyRead()
{
    if (m_state == State::Idle || m_state == State::Connecting)
        return;
    m_buf += m_sock.readAll();
    if (m_state == State::Handshake && !parseHandshake())
        return;
    if (m_state == State::Open)
        parseFrames();
}

bool WsConn::parseHandshake()
{
    const int end = m_buf.indexOf("\r\n\r\n");
    if (end < 0)
        return false;
    const QByteArray head = m_buf.left(end);
    m_buf.remove(0, end + 4);
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty() || !lines.first().contains(" 101")) {
        finish(QStringLiteral("Server refused the WebSocket upgrade: %1")
                   .arg(QString::fromLatin1(lines.value(0)).trimmed()));
        return false;
    }
    const QByteArray expected =
        QCryptographicHash::hash(m_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
    bool ok = false;
    for (const QByteArray &l : lines) {
        if (l.toLower().startsWith("sec-websocket-accept:") && l.mid(l.indexOf(':') + 1).trimmed() == expected)
            ok = true;
    }
    if (!ok) {
        finish(QStringLiteral("Bad Sec-WebSocket-Accept from server"));
        return false;
    }
    m_state = State::Open;
    emit opened();
    return m_state == State::Open;
}

void WsConn::parseFrames()
{
    while (m_state == State::Open) {
        if (m_buf.size() < 2)
            return;
        const auto *p = reinterpret_cast<const uchar *>(m_buf.constData());
        const bool fin = p[0] & 0x80;
        const quint8 opcode = p[0] & 0x0f;
        const bool masked = p[1] & 0x80;
        quint64 len = p[1] & 0x7f;
        int pos = 2;
        if (len == 126) {
            if (m_buf.size() < 4)
                return;
            len = (quint64(p[2]) << 8) | p[3];
            pos = 4;
        } else if (len == 127) {
            if (m_buf.size() < 10)
                return;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | p[2 + i];
            pos = 10;
        }
        if (len > quint64(kMaxPayload)) {
            finish(QStringLiteral("Frame too large"));
            return;
        }
        const int maskLen = masked ? 4 : 0;
        if (quint64(m_buf.size()) < quint64(pos + maskLen) + len)
            return;
        QByteArray payload = m_buf.mid(pos + maskLen, static_cast<int>(len));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ p[pos + (i & 3)]);
        }
        m_buf.remove(0, pos + maskLen + static_cast<int>(len));

        switch (opcode) {
        case 0x0: // continuation
        case 0x1: // text
        case 0x2: { // binary (ignored by the protocol, but must be reassembled)
            if (opcode != 0x0) {
                m_fragment.clear();
                m_fragmentIsText = (opcode == 0x1);
            }
            m_fragment += payload;
            if (fin) {
                const QByteArray msg = std::exchange(m_fragment, QByteArray());
                if (m_fragmentIsText)
                    emit textReceived(QString::fromUtf8(msg));
            }
            break;
        }
        case 0x8: // close
            sendFrame(0x8, payload.left(2));
            finish(QStringLiteral("Connection closed by server"));
            return;
        case 0x9: // ping
            sendFrame(0xA, payload);
            break;
        default: // pong / unknown
            break;
        }
    }
}

void WsConn::sendText(const QString &text)
{
    if (m_state == State::Open)
        sendFrame(0x1, text.toUtf8());
}

void WsConn::sendFrame(quint8 opcode, const QByteArray &payload)
{
    QByteArray f;
    f.append(static_cast<char>(0x80 | opcode));
    const int n = payload.size();
    if (n < 126) {
        f.append(static_cast<char>(0x80 | n));
    } else if (n < 65536) {
        f.append(static_cast<char>(0x80 | 126));
        f.append(static_cast<char>(n >> 8));
        f.append(static_cast<char>(n & 0xff));
    } else {
        f.append(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            f.append(static_cast<char>((quint64(n) >> (8 * i)) & 0xff));
    }
    const QByteArray mask = randomBytes(4);
    f += mask;
    for (int i = 0; i < n; ++i)
        f.append(static_cast<char>(payload[i] ^ mask[i & 3]));
    m_sock.write(f);
}

void WsConn::onSocketError()
{
    if (m_state == State::Idle)
        return;
    finish(m_sock.errorString());
}

void WsConn::onDisconnected()
{
    if (m_state == State::Idle)
        return;
    finish(QStringLiteral("Connection lost"));
}

void WsConn::finish(const QString &reason)
{
    if (m_state == State::Idle)
        return;
    m_state = State::Idle;
    m_sock.abort();
    emit closed(reason);
}

void WsConn::shutdown()
{
    disconnect(this, nullptr, nullptr, nullptr);
    if (m_state == State::Open) {
        QByteArray code;
        code.append(static_cast<char>(1000 >> 8));
        code.append(static_cast<char>(1000 & 0xff));
        sendFrame(0x8, code);
        m_state = State::Idle;
        connect(&m_sock, &QAbstractSocket::disconnected, this, &QObject::deleteLater);
        QTimer::singleShot(1000, this, &QObject::deleteLater);
        m_sock.disconnectFromHost();
    } else {
        m_state = State::Idle;
        m_sock.abort();
        deleteLater();
    }
}
