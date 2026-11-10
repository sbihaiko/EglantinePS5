#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check terrain argument initialization, append preservation and pass expiry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
# The counters this check reads are a core patch (0008): a core without the header has nothing to
# check, and bananza_terrain_check.cpp does not compile against it.
if not (root / 'Eglantine/src/video_core/bananza_terrain_compat.h').exists():
    print('Bananza terrain: skipped, the core has no bananza_terrain_compat.h')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='eden-bananza-terrain-') as directory:
    executable = Path(directory) / 'terrain-check'
    subprocess.run(['c++', '-std=c++20', '-O2', '-I' + str(root / 'Eglantine/src'),
                    str(root / 'headless/bananza_terrain_check.cpp'), '-o', str(executable)],
                   check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('Bananza terrain: counter initialization, append preservation, pass expiry and rejection checks PASS')
