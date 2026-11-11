// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

namespace {
constexpr u64 Page = u64{1} << Tegra::Nx2WrittenPageBits;
} // Anonymous namespace

TEST_CASE("An image the GPU writes marks every page it covers",
          "[video_core][nx2_image_written_pages]") {
    Tegra::Nx2ClearGpuWrittenPages();
    // A 4x1 RGBA32F target (64 bytes) inside one page.
    Tegra::Nx2MarkImageGpuWritten(40 * Page + 0x2c0, 64);
    REQUIRE(Tegra::Nx2IsGpuWritten(40 * Page));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(39 * Page));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(41 * Page));
    // Pokopia's 256x3 RGBA32F sky image (12 KiB, its CPU reads it at +0x1000), page aligned, and
    // the same size starting mid-page.
    Tegra::Nx2MarkImageGpuWritten(50 * Page, 256 * 3 * 16);
    REQUIRE(Tegra::Nx2IsGpuWritten(50 * Page + 0x1000));
    REQUIRE(Tegra::Nx2IsGpuWritten(52 * Page + Page - 1));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(53 * Page));
    Tegra::Nx2MarkImageGpuWritten(70 * Page + 8, 256 * 3 * 16);
    REQUIRE(Tegra::Nx2IsGpuWritten(73 * Page));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(74 * Page));
}

TEST_CASE("Marking an image again, or an empty one, changes nothing else",
          "[video_core][nx2_image_written_pages]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkImageGpuWritten(60 * Page, 0);
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(60 * Page));
    Tegra::Nx2MarkImageGpuWritten(60 * Page, Page);
    Tegra::Nx2MarkImageGpuWritten(60 * Page, Page);
    REQUIRE(Tegra::Nx2IsGpuWritten(60 * Page));
    REQUIRE_FALSE(Tegra::Nx2IsGpuWritten(61 * Page));
    // A range already partly marked still gets its other pages.
    Tegra::Nx2MarkImageGpuWritten(60 * Page, 2 * Page);
    REQUIRE(Tegra::Nx2IsGpuWritten(61 * Page));
}

TEST_CASE("Images are marked once, sparse ones again after a download",
          "[video_core][nx2_image_written_pages]") {
    REQUIRE(Tegra::Nx2ImagePagesNeedMarking(false, false, false));
    REQUIRE(Tegra::Nx2ImagePagesNeedMarking(false, false, true));
    REQUIRE_FALSE(Tegra::Nx2ImagePagesNeedMarking(true, false, false));
    REQUIRE_FALSE(Tegra::Nx2ImagePagesNeedMarking(true, false, true));
    REQUIRE(Tegra::Nx2ImagePagesNeedMarking(true, true, false));
    REQUIRE_FALSE(Tegra::Nx2ImagePagesNeedMarking(true, true, true));
}
