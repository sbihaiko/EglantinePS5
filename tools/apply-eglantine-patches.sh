#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies the patches in tools/eglantine-patches/ to the Eglantine submodule's working tree, which is
# what the build reads. The Zortos core is referenced directly by the submodule;
# this hook supports optional local patches. Safe to run again: applied patches are left alone.
# --revert takes them out (make update does, before it moves the submodule).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mode=${1:-apply}
shopt -s nullglob
patches=("$root"/tools/eglantine-patches/*.patch)
((${#patches[@]})) || exit 0

# Later patches change lines earlier ones added (0005 over 0004, for one), so once the whole set is
# in, an earlier patch can no longer be taken out on its own and `git apply --reverse --check`
# cannot tell that it is in. Instead, find how many of the patches, in order, the working tree
# already has: the files they touch are compared with the submodule's commit plus the first k
# patches, built in a temporary index (the working tree is not touched).
if [[ $mode != --revert ]]; then
    index=$(mktemp)
    trap 'rm -f "$index"' EXIT
    # A read loop, not mapfile: macOS ships bash 3.2, and this script is run there too.
    touched=()
    while IFS= read -r entry; do
        touched+=("$entry")
    done < <(for patch in "${patches[@]}"; do git -C Eglantine apply --numstat "$patch"; done | cut -f3 | sort -u)
    applied=-1
    GIT_INDEX_FILE=$index git -C Eglantine read-tree HEAD
    for ((k = 0; k <= ${#patches[@]}; k++)); do
        if ((k)); then
            GIT_INDEX_FILE=$index git -C Eglantine apply --cached "${patches[k - 1]}" 2>/dev/null || break
        fi
        GIT_INDEX_FILE=$index git -C Eglantine diff --quiet -- "${touched[@]}" && applied=$k
    done
    if ((applied >= 0)); then
        for patch in "${patches[@]:applied}"; do
            name=$(basename "$patch")
            if git -C Eglantine apply --check "$patch" >/dev/null 2>&1; then
                git -C Eglantine apply "$patch"
                echo "Eglantine patch applied: $name"
            else
                echo "Eglantine patch does not apply: $name (Eglantine moved; make the patch again)" >&2
                exit 1
            fi
        done
        exit 0
    fi
fi

# The working tree is not the commit plus some of the patches in order (local changes, or a
# revert): patch by patch.
if [[ $mode == --revert ]]; then
    reversed=()
    while IFS= read -r entry; do
        reversed+=("$entry")
    done < <(printf '%s\n' "${patches[@]}" | sort -r)
    patches=("${reversed[@]}")
fi
for patch in "${patches[@]}"; do
    name=$(basename "$patch")
    if git -C Eglantine apply --reverse --check "$patch" >/dev/null 2>&1; then
        if [[ $mode == --revert ]]; then
            git -C Eglantine apply --reverse "$patch"
            echo "Eglantine patch taken out: $name"
        fi
    elif [[ $mode == --revert ]]; then
        echo "Eglantine patch not in: $name"
    elif git -C Eglantine apply --check "$patch" >/dev/null 2>&1; then
        git -C Eglantine apply "$patch"
        echo "Eglantine patch applied: $name"
    else
        echo "Eglantine patch does not apply: $name (Eglantine moved; make the patch again)" >&2
        exit 1
    fi
done
