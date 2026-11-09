#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepares everything the PS5 build needs besides Eden itself (make prepare), after the pinned
# inputs are in place (make deps): this checkout's build cache, Eden's source tree, the Payload
# SDK, FFmpeg for the PS5, the native packaging tool, the runtime libc.prx, the libSceAgcDriver
# link stub and the isolated RADV driver. Each step is skipped when its result already exists.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
step() { echo "== $*"; }

# Thousands of small files: keep the build cache on a native Linux filesystem, one per checkout.
cache_parent="${XDG_CACHE_HOME:-$HOME/.cache}"
if [[ ! -f .local/headless-cache ]] || [[ ! -d "$(cat .local/headless-cache)" ]]; then
    scratch="$cache_parent/ps5-eden-headless.$(printf '%s' "$root" | sha256sum | cut -c1-12)"
    mkdir -p .local "$scratch"
    printf '%s\n' "$root" > "$scratch/owner"
    printf '%s\n' "$scratch" > .local/headless-cache
fi
scratch=$(cat .local/headless-cache)
[[ "$(cat "$scratch/owner")" == "$root" ]] || { echo "Build cache $scratch belongs to another checkout" >&2; exit 1; }

eden="$scratch/source"
# The emulator source is the Eglantine submodule; a tree synced from another commit of it is
# synced again (Eden's package cache, downloads only, is kept).
bash tools/sync-eden-source.sh "$eden"
# Optional: seed Eden's package cache from another checkout's (CI reuses a development cache).
if [[ -n ${EDEN_CPM_CACHE_SEED:-} && ! -d $eden/.cache/cpm ]]; then
    step "Eden package cache from $EDEN_CPM_CACHE_SEED"
    mkdir -p "$eden/.cache"
    cp -a "$EDEN_CPM_CACHE_SEED" "$eden/.cache/cpm"
fi
# The FFmpeg source Eden pins, where Eden's package cache keeps it.
ffmpeg_source="$eden/.cache/cpm/ffmpeg/c7b5f1537d"
if [[ ! -f $ffmpeg_source/configure ]]; then
    step "FFmpeg source"
    mkdir -p "$(dirname "$ffmpeg_source")"
    cp -a .deps/ffmpeg-c7b5f1537d "$ffmpeg_source"
fi

if [[ ! -f $scratch/sdk/.complete ]]; then
    step "Payload SDK"
    mkdir -p "$scratch/sdk"
    cp -a ../ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk/target "$scratch/sdk/"
    touch "$scratch/sdk/.complete"
fi
# .with-dav1d: built with the AV1 decoder (an older cache rebuilds once).
if [[ ! -f $scratch/ffmpeg-native/install/lib/libavcodec.a || ! -f $scratch/ffmpeg-native/install/.with-dav1d ]]; then
    step "FFmpeg for the PS5"
    bash tools/build-headless-ffmpeg.sh
fi
step "Native packaging tool"
bash tools/build-native-tool.sh
# The app's runtime libc.prx: the boilerplate's clean-room build, checked against its recorded digest.
if [[ ! -f ../ps5-native-app-boilerplate/runtime/libc.prx ]]; then
    step "Runtime libc.prx"
    bash ../ps5-native-app-boilerplate/tools/rebuild-libc.sh
fi
step "libSceAgcDriver link stub"
bash tools/build-agc-driver-stub.sh
bash tools/build-common-dialog-stub.sh
# The driver's display code carries this repository's adaptation (tools/patch-radv-wsi.py): a
# driver built with another version of it is built again (only the changed file compiles).
# So is a driver built from another Mesa revision than tools/deps.json pins.
radv_release=../mihawk-vulkan-review/.deps/native/radv-release
mesa_pin=$(python3 -c 'import json; print(next(i["commit"] for i in json.load(open("tools/deps.json"))["items"] if i["name"] == "ps5-mesa"))')
if [[ -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
    built=$(sed -n 's/^revision: //p' "$radv_release/PROVENANCE.txt" 2>/dev/null || true)
    if [[ $built != "$mesa_pin" ]]; then
        step "RADV: built from Mesa ${built:0:12}, pinned ${mesa_pin:0:12}"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-disk-cache.py --check "$radv_release/EDEN_DISK_CACHE_SHA256"; then
        step "RADV: the elevated-app shader cache adaptation changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-memory-report.py --check "$radv_release/EDEN_MEMORY_REPORT_SHA256"; then
        step "RADV: the per-purpose memory report changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-trim.py --check "$radv_release/EDEN_MEMORY_TRIM_SHA256"; then
        step "RADV: giving back device memory no resource uses changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-pieces.py --check "$radv_release/EDEN_MEMORY_PIECES_SHA256"; then
        step "RADV: driver memory from 2 MiB pieces changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-gs-compute.py --check "$radv_release/EDEN_GS_COMPUTE_SHA256"; then
        step "RADV: geometry-shader compute passes for dropped draws changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-wsi.py --check "$radv_release/EDEN_WSI_SHA256"; then
        step "RADV: the display code's adaptation changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    fi
fi
# The cached driver is useless without the Mesa tree its headers come from: headless/nir_backend.cmake
# reads nir.h out of .deps/work/radv-src, and the CI cache restores the built driver only.
if [[ -f build/radv-isolated/libvulkan_radeon.ps5.a &&
      ! -d ../mihawk-vulkan-review/.deps/work/radv-src/src/compiler/nir ]]; then
    step "RADV: the pinned Mesa source tree is missing"
    bash tools/build-radv-dependencies.sh
    rm -f build/radv-isolated/libvulkan_radeon.ps5.a
fi
if [[ ! -f build/radv-isolated/libvulkan_radeon.ps5.a ]]; then
    if [[ ! -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
        step "RADV (Mesa) for the PS5 - this takes a while"
        bash tools/build-radv-dependencies.sh
    fi
    step "Isolating RADV beside the OpenGL Mesa"
    python3 -B tools/isolate-radv.py
fi
step "Prepared: $scratch"
