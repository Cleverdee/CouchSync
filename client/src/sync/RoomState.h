// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace couchsync {

enum class WaitMode { Wait, NoWait };

// The subset of the protocol `state` message (docs/PROTOCOL.md, "Room state")
// that playback sync needs. Units follow the protocol: positions in seconds,
// server times in milliseconds.
struct RoomState {
    std::int64_t seq = 0;
    bool playing = false;
    double position = 0;    // s
    double serverTime = 0;  // server ms; may be in the future (scheduled start)
    double rate = 1.0;
    bool autoPaused = false;
    bool hostOnlyControls = false;  // policy.controls == "host"
    WaitMode waitMode = WaitMode::Wait;

    // Protocol formula:
    //   playing && t > serverTime ? position + (t - serverTime) / 1000 * rate : position
    // i.e. the position holds while serverTime is in the future.
    double expectedPosition(double serverNowMs) const
    {
        return (playing && serverNowMs > serverTime)
                   ? position + (serverNowMs - serverTime) / 1000.0 * rate
                   : position;
    }
};

}  // namespace couchsync
