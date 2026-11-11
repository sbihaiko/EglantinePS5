// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"

// The Legend of Zelda: Ocarina of Time's anti-aliasing (after upstream Eglantine f96cded333 by
// ProphetWild, re-derived for this core). After tonemapping (an A8B8G8R8 image at the internal
// resolution), the game runs a tensor (HMMA) network in nine compute passes and an output pass that
// upscales the network's result to the output resolution and sharpens it hard. With the bypass on,
// the network's passes are skipped and the output pass is replaced by a bilinear upscale of the
// tonemapped image: no sharpening halos, and on the PS5 none of the software HMMA cost.
//
// On by default; EDEN_OOT_ANTI_ALIASING=game (Settings > Video > "Ocarina of Time anti-aliasing"
// at Game) runs the game's own network.
//
// The program keys and handle offsets are those of the game's update 131072 (2.0.0);
// another update needs them re-checked (EDEN_OOT_NETWORK_TRACE=1 logs the passes to stderr).
namespace VideoCommon::OotNetwork {

constexpr u64 ProgramId = 0x0400486024DEC000ULL;

/// Native program key (Shader::Ampere::NativeProgramKey) of the output pass.
constexpr u64 OutputProgramKey = 0x6093c9143ba0904dULL;
/// Native program keys of the network's nine passes, in the order the game dispatches them. Only
/// these are skipped: gameplay runs other tensor (HMMA) compute programs that are not part of the
/// anti-aliasing (6f9367bd41dc0b58, a 40x45 pass writing a 1080p image), and skipping every tensor
/// program left the world black under the HUD.
constexpr std::array<u64, 9> NetworkPassKeys{
    0x2f5791cea2a85397ULL, 0x313d724f3e214203ULL, 0xdf2c83cf2ad70790ULL,
    0x1fdb95ea2565f49cULL, 0x0be2f024aea21a6cULL, 0x0618cc4a3ac8f33aULL,
    0xe27336d38f1cc8f4ULL, 0xecebb02dec6faa17ULL, 0x2a366b38546a4d5eULL,
};

constexpr bool IsNetworkPass(u64 native_key) {
    return std::find(NetworkPassKeys.begin(), NetworkPassKeys.end(), native_key) !=
           NetworkPassKeys.end();
}

static_assert(IsNetworkPass(0x2f5791cea2a85397ULL) && IsNetworkPass(0x2a366b38546a4d5eULL));
// The gameplay upscaler and the output pass (replaced, not skipped) are not network passes.
static_assert(!IsNetworkPass(0x6f9367bd41dc0b58ULL) && !IsNetworkPass(OutputProgramKey));

/// Constant buffer 0 offsets of the output pass's handles: the tonemapped image it reads and the
/// output image it writes.
constexpr u32 TonemappedHandleOffset = 0xa8;
constexpr u32 OutputHandleOffset = 0xc8;

/// The latest output pass was replaced; the network's passes are skipped only while it is, so a
/// failed replacement brings the game's network back on the next frame.
inline std::atomic<bool> output_replaced{};

/// Thrown out of the compute shader lookup when a dispatch of the network is skipped or replaced.
struct DispatchReplaced {};

inline bool BypassEnabled(u64 program_id) {
    if (program_id != ProgramId) {
        return false;
    }
    // Read each time: the launcher sets it from Settings before each game it starts.
    const char* value = std::getenv("EDEN_OOT_ANTI_ALIASING");
    return value == nullptr || std::strcmp(value, "game") != 0;
}

/// Task/mesh draws. The game culls its vegetation on the GPU: compute programs write per-type task
/// counts and instance records that its task and mesh programs read. With native storage buffer
/// tracking the culling runs and the mesh-only pipelines draw the trees' leaves correctly, but the
/// task program (1a55fa57a335b963, every task pipeline) still reads its instance records through
/// pointers it loads from memory that the tracking does not bind for graphics stages yet, and its
/// draws come out as bright stretched triangles flickering across the scene (a console bisect of
/// the 37 task/mesh programs, rejecting halves, found only this one). Draws of pipelines with a
/// task stage are skipped; mesh-only draws are issued. EDEN_OOT_MESH_DRAWS=draw issues every
/// task/mesh draw (for work on the task program), =skip skips them all as before.
inline bool SkipMeshDraw(u64 program_id, bool has_task_stage) {
    if (program_id != ProgramId) {
        return false;
    }
    static const int mode = [] {
        const char* value = std::getenv("EDEN_OOT_MESH_DRAWS");
        if (value != nullptr && std::strcmp(value, "draw") == 0) return 1;
        if (value != nullptr && std::strcmp(value, "skip") == 0) return 2;
        return 0;
    }();
    return mode == 2 || (mode == 0 && has_task_stage);
}

} // namespace VideoCommon::OotNetwork
