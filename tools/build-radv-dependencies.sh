#!/usr/bin/env bash
# Build isolated, pinned RADV dependencies; never deploy or replace the old SDK.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
refs=$(dirname "$root")
export PS5_MESA_FORK="$refs/mihawk-mesa-review"
export PS5_PAYLOAD_SDK_FORK="$refs/mihawk-sdk-review"
vulkan="$refs/mihawk-vulkan-review"
# The three checkouts must sit at the revisions tools/deps.json pins (the only place they are set).
pin() { python3 -c 'import json, sys; print(next(i["commit"] for i in json.load(open(sys.argv[1]))["items"] if i["name"] == sys.argv[2]))' "$root/tools/deps.json" "$1"; }
for check in "ps5-vulkan-tools $vulkan" "ps5-mesa $PS5_MESA_FORK" "ps5-payload-sdk-fork $PS5_PAYLOAD_SDK_FORK"; do
    read -r name path <<< "$check"
    [[ $(git -C "$path" rev-parse HEAD) == "$(pin "$name")" ]] ||
        { echo "$path is not at the $name revision pinned in tools/deps.json" >&2; exit 1; }
done
# Memory-bounded parallel jobs (tools/build-jobs.sh) unless RADV_BUILD_JOBS says otherwise: hosted CI runners have a few cores
# and too little memory for 24 compilers (.github/workflows/release.yml sets it).
jobs=${RADV_BUILD_JOBS:-$("$root/tools/build-jobs.sh")}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo "RADV_BUILD_JOBS must be a positive number: $jobs" >&2; exit 2; }
export BUILD_JOBS=$jobs CMAKE_BUILD_PARALLEL_LEVEL=$jobs
mkdir -p "$root/build/radv-tools"
# Upstream calls ninja directly; enforce the same bounded parallelism everywhere.
ninja=$(command -v ninja) || { echo 'ninja not found' >&2; exit 1; }
printf '#!/bin/sh\nexec %s -j%s "$@"\n' "$ninja" "$jobs" > "$root/build/radv-tools/ninja"
chmod +x "$root/build/radv-tools/ninja"
export NINJA="$root/build/radv-tools/ninja"
command -v ccache >/dev/null
# Cross compilers do not get Meson's automatic native ccache detection.
# Keep this two-line build-only adaptation reproducible on a fresh checkout.
python3 - "$vulkan/tooling/radv/ps5-cross.ini" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
for lang, compiler in [('c', 'prospero-clang'), ('cpp', 'prospero-clang++')]:
    plain = f"{lang} = sdk / 'bin/{compiler}'"
    cached = f"{lang} = ['ccache', sdk / 'bin/{compiler}']"
    assert plain in s or cached in s, 'Unexpected upstream cross compiler configuration'
    s = s.replace(plain, cached)
p.write_text(s)
PY
bash "$vulkan/tools/setup-native-dependencies.sh"
bash "$vulkan/tools/build-radv.sh" release
python3 "$root/tools/patch-radv-wsi.py"
python3 "$root/tools/patch-radv-disk-cache.py"
python3 "$root/tools/patch-radv-memory-report.py"
python3 "$root/tools/patch-radv-pieces.py"
python3 "$root/tools/patch-radv-trim.py"
# Mihawk PS5_Mesa f5610fe270: freed buffers kept until the GPU has run what was submitted before
# (RADV_PS5_RETIRE=1; off by default). After the trim: a held buffer leaves its list.
python3 "$root/tools/patch-radv-retire.py"
python3 "$root/tools/patch-radv-gs-compute.py"
# PlayTendoh's submit thread, by way of Vampinity (RADV_PS5_SUBMIT_THREAD=1; off by default).
python3 "$root/tools/patch-radv-submit-thread.py"
# ProsperoEden's patch-radv-transfer-split.py (splitting before transfers and barriers) is not
# taken: with it, Mario Kart World hung the GPU under memory pressure (PR #77).
bash "$vulkan/tools/build-radv.sh" release
sha256sum "$vulkan/.deps/work/radv-src/src/vulkan/wsi/wsi_common_videoout.c" \
    > "$vulkan/.deps/native/radv-release/EDEN_WSI_SHA256"

python3 "$root/tools/patch-radv-disk-cache.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_DISK_CACHE_SHA256"
python3 "$root/tools/patch-radv-memory-report.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_MEMORY_REPORT_SHA256"
python3 "$root/tools/patch-radv-pieces.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_MEMORY_PIECES_SHA256"
python3 "$root/tools/patch-radv-trim.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_MEMORY_TRIM_SHA256"
python3 "$root/tools/patch-radv-retire.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_RETIRE_SHA256"
python3 "$root/tools/patch-radv-gs-compute.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_GS_COMPUTE_SHA256"
python3 "$root/tools/patch-radv-submit-thread.py" --digest \
    > "$vulkan/.deps/native/radv-release/EDEN_SUBMIT_THREAD_SHA256"
