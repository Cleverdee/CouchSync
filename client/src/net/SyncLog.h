// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <cstdio>

// --log-sync support: one global switch, lines go to stderr.
namespace couchsync {

inline bool &syncLogEnabled()
{
    static bool enabled = false;
    return enabled;
}

inline void syncLog(const QString &line)
{
    if (!syncLogEnabled())
        return;
    std::fprintf(stderr, "[sync] %s\n", line.toLocal8Bit().constData());
    std::fflush(stderr);
}

}  // namespace couchsync
