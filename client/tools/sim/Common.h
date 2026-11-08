// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace sim {

// The simulator's ground-truth clock (real elapsed time on this machine), in ms.
inline double realNowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Deterministic RNG (xorshift64*).
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed = 1) : s(seed ? seed : 1) {}
    double uni()  // [0, 1)
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<double>((s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
    }
    double sym(double a) { return (uni() * 2 - 1) * a; }  // [-a, a)
};

}  // namespace sim
