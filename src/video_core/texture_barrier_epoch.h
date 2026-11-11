// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCommon {

/// Counts the guest's texture barriers (Maxwell3D fragment_barrier, invalidate_texture_data_cache
/// and tiled_cache_barrier). Texture reads are ordered after earlier colour writes only across
/// one; TextureCache::SynchronizeAliases uses it to bound feedback copies.
inline std::atomic<u64> texture_barrier_epoch{};

inline void NoteTextureBarrier() {
    texture_barrier_epoch.fetch_add(1, std::memory_order_relaxed);
}

} // namespace VideoCommon
