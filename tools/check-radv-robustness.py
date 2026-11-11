#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The driver robustness patch (geometry-shader compute sinks) is wired in.

Checks that build-radv-dependencies.sh applies and stamps it, that prepare-build.sh rebuilds the
driver when a stamp is missing or stale, and, when the driver export is on this machine, that it
applies to a copy of it and that running it again changes nothing.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
prepare = (root / 'tools/prepare-build.sh').read_text()
build = (root / 'tools/build-radv-dependencies.sh').read_text()
scripts = {'patch-radv-gs-compute.py': 'EDEN_GS_COMPUTE_SHA256',
           'patch-radv-submit-thread.py': 'EDEN_SUBMIT_THREAD_SHA256'}

for name, stamp_name in scripts.items():
    script = root / 'tools' / name
    assert f'python3 "$root/tools/{name}"\n' in build, f'{name} is not applied'
    assert build.index(f'python3 "$root/tools/{name}"\n') < build.rindex('build-radv.sh" release'), name
    assert f'tools/{name}" --digest \\\n    > "$vulkan/.deps/native/radv-release/{stamp_name}"' in build, name
    assert f'tools/{name} --check "$radv_release/{stamp_name}"' in prepare, name
    with tempfile.TemporaryDirectory() as folder:
        stamp = Path(folder) / 'stamp'
        check = lambda tool=script: subprocess.run([sys.executable, '-B', str(tool), '--check', str(stamp)]).returncode == 0
        assert not check(), f'{name}: a missing stamp passed'
        stamp.write_bytes(subprocess.check_output([sys.executable, '-B', str(script), '--digest']))
        assert check(), f'{name}: its own stamp failed'
        changed = Path(folder) / name
        changed.write_text(script.read_text() + '\n# changed\n')
        assert not check(changed), f'{name}: a changed script passed the old stamp'

export = root.parent / 'mihawk-vulkan-review/.deps/work/radv-src/src/amd/vulkan'
if export.is_dir():
    with tempfile.TemporaryDirectory() as folder:
        tree = Path(folder) / 'radv-src'
        shutil.copytree(export, tree / 'src/amd/vulkan')
        for name in scripts:
            subprocess.run([sys.executable, '-B', str(root / 'tools' / name), str(tree)], check=True,
                           stdout=subprocess.DEVNULL)
        before = {p: p.read_bytes() for p in tree.rglob('*') if p.is_file()}
        for name in scripts:
            subprocess.run([sys.executable, '-B', str(root / 'tools' / name), str(tree)], check=True,
                           stdout=subprocess.DEVNULL)
        assert before == {p: p.read_bytes() for p in tree.rglob('*') if p.is_file()}, 'patches are not idempotent'
        vulkan = tree / 'src/amd/vulkan'
        assert 'const uint64_t null_sink = radv_gs_compute_upload' in (vulkan / 'radv_cmd_buffer.c').read_text()
        assert 'block->ro_sink;\n      p->count_buffer' in (vulkan / 'cl/radv_gs_compute.cl').read_text()
        assert 'radv_ps5_submitter_push(' in (vulkan / 'winsys/ps5/radv_ps5_cs.c').read_text()
    print('RADV robustness: wired in, stamped, applies to the export and is idempotent')
else:
    print('RADV robustness: wired in and stamped (no driver export here to apply to)')
