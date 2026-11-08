// SPDX-License-Identifier: GPL-3.0-or-later
#include "Scenario.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace sim {

Scenario defaultScenario()
{
    Scenario s;
    struct Row {
        const char *name;
        double lat, clk, media, off;
    };
    const Row rows[] = {
        {"c0-host", 20, +200, +100, 1234.5},
        {"c1", 80, -200, -100, 987654.3},
        {"c2", 150, +120, +60, 5555.5},
        {"c3", 300, -150, -80, 42424242.0},
    };
    for (const Row &r : rows) {
        ClientSpec c;
        c.name = QString::fromLatin1(r.name);
        c.latencyMs = r.lat;
        c.jitterMs = 30;
        c.lossP = 0.02;
        c.clockSkewPpm = r.clk;
        c.mediaSkewPpm = r.media;
        c.clockOffsetMs = r.off;
        s.clients.push_back(c);
    }
    s.events = {
        {20, QStringLiteral("seek"), 1, 300},
        {40, QStringLiteral("pause"), 0, 0},
        {43, QStringLiteral("play"), 0, 0},
        {55, QStringLiteral("buffer"), 2, 2000},
    };
    s.endAtS = 76;
    return s;
}

bool loadScenario(const QString &path, Scenario &out, QString &err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        err = QStringLiteral("%1: %2").arg(path, pe.errorString());
        return false;
    }
    const QJsonObject o = doc.object();
    Scenario s;
    s.name = o.value(QStringLiteral("name")).toString(QFileInfo(path).baseName());
    s.seed = static_cast<std::uint64_t>(o.value(QStringLiteral("seed")).toDouble(1));
    s.spikeMinMs = o.value(QStringLiteral("spikeMinMs")).toDouble(s.spikeMinMs);
    s.spikeMaxMs = o.value(QStringLiteral("spikeMaxMs")).toDouble(s.spikeMaxMs);
    s.endAtS = o.value(QStringLiteral("endAtS")).toDouble(s.endAtS);
    const QJsonObject th = o.value(QStringLiteral("thresholds")).toObject();
    s.th.steadyP95Ms = th.value(QStringLiteral("steadyP95Ms")).toDouble(s.th.steadyP95Ms);
    s.th.steadyMaxMs = th.value(QStringLiteral("steadyMaxMs")).toDouble(s.th.steadyMaxMs);
    s.th.convergeMs = th.value(QStringLiteral("convergeMs")).toDouble(s.th.convergeMs);
    s.th.convergeWindowS = th.value(QStringLiteral("convergeWindowS")).toDouble(s.th.convergeWindowS);
    s.th.propagationBaseMs = th.value(QStringLiteral("propagationBaseMs")).toDouble(s.th.propagationBaseMs);
    int idx = 0;
    for (const QJsonValue &v : o.value(QStringLiteral("clients")).toArray()) {
        const QJsonObject c = v.toObject();
        ClientSpec cs;
        cs.name = c.value(QStringLiteral("name")).toString(QStringLiteral("c%1").arg(idx));
        cs.latencyMs = c.value(QStringLiteral("latencyMs")).toDouble(cs.latencyMs);
        cs.jitterMs = c.value(QStringLiteral("jitterMs")).toDouble(cs.jitterMs);
        cs.lossP = c.value(QStringLiteral("lossP")).toDouble(cs.lossP);
        cs.clockSkewPpm = c.value(QStringLiteral("clockSkewPpm")).toDouble(cs.clockSkewPpm);
        cs.clockOffsetMs = c.value(QStringLiteral("clockOffsetMs")).toDouble(1000.0 * (idx + 1) + 0.5);
        cs.mediaSkewPpm = c.value(QStringLiteral("mediaSkewPpm")).toDouble(cs.mediaSkewPpm);
        cs.seekLatencyMs = c.value(QStringLiteral("seekLatencyMs")).toDouble(cs.seekLatencyMs);
        cs.posJitterMs = c.value(QStringLiteral("posJitterMs")).toDouble(cs.posJitterMs);
        s.clients.push_back(cs);
        ++idx;
    }
    for (const QJsonValue &v : o.value(QStringLiteral("events")).toArray()) {
        const QJsonObject e = v.toObject();
        Event ev;
        ev.atS = e.value(QStringLiteral("at")).toDouble();
        ev.action = e.value(QStringLiteral("action")).toString();
        ev.client = e.value(QStringLiteral("client")).toInt(0);
        ev.value = e.value(QStringLiteral("value")).toDouble();
        s.events.push_back(ev);
    }
    if (s.clients.isEmpty()) {
        err = QStringLiteral("scenario has no clients");
        return false;
    }
    for (const Event &e : s.events) {
        if (e.client < 0 || e.client >= s.clients.size()) {
            err = QStringLiteral("event client index out of range");
            return false;
        }
    }
    out = s;
    return true;
}

}  // namespace sim
