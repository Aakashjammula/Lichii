// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>

namespace VideoCommon::Nx2Perf {

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

/// The frame period the game asks for (nx2:vi2 present word 12 at the speed limit), 0 while
/// unknown or unpaced. The host frame pacer (vk_host_pacer.h) never holds frames longer.
inline std::atomic<long long> g_guest_frame_interval_ns{0};

/// How long the host frame pacer had to hold the last frame before its refresh (negative: it
/// was late), or kHostPaceOff. The game's present pacing (nx2_gsv.h) starts frames later while
/// the pacer holds them longer than it needs, so they wait less in the queue (input lag).
inline constexpr long long kHostPaceOff = -(1LL << 62);
inline std::atomic<long long> g_host_pace_slack_ns{kHostPaceOff};

} // namespace VideoCommon::Nx2Perf
