// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_funcs.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// Whether TextureCache::PrepareImageView prepares the view's image. Neither the null view, whose
/// image has no valid format and stays CpuModified (uploading it ends in UNREACHABLE, Lichii
/// #904/#910), nor a buffer view, which has no image, is prepared.
template <typename ImageViews>
[[nodiscard]] bool PreparesImage(const ImageViews& image_views, ImageViewId id) noexcept {
    return id && id != NULL_IMAGE_VIEW_ID && !image_views[id].IsBuffer();
}

/// Whether sampling the image now needs FillImageViews' work on it: an upload from guest memory
/// (CpuModified, whatever part of it is dirty), a newer alias to copy in (SynchronizeAliases runs
/// while alias_maybe_newer is set), or a replaced unwritten sparse image FillImageViews deletes.
[[nodiscard]] inline bool SamplingChangesImage(const ImageBase& image) noexcept {
    return True(image.flags & ImageFlagBits::CpuModified) ||
           (!image.aliased_images.empty() && image.alias_maybe_newer) ||
           (True(image.flags & ImageFlagBits::UnwrittenSparse) &&
            True(image.flags & ImageFlagBits::Remapped));
}

/// Whether sampling any of the views now needs a resolve that prepares their images (fast draw
/// delta draws keep the previous resolve's views and prepare none).
template <typename Views, typename ImageViews, typename Images>
[[nodiscard]] bool SampledViewsChange(const Views& views, const ImageViews& image_views,
                                      const Images& images) noexcept {
    return std::ranges::any_of(views, [&](const auto& view) {
        return PreparesImage(image_views, view.id) &&
               SamplingChangesImage(images[image_views[view.id].image_id]);
    });
}

} // namespace VideoCommon
