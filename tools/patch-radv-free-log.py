#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Log lines to match a GPU page fault's address against what the driver freed (opt-in: RADV_PS5_FREE_LOG=1).

Ocarina of Time ends after about 10 minutes with an async GPU page fault (a TCP read of an
unmapped page in the RADV region, 0x40_xxxx_xxxx) or a GPU hang, each within about 10 s of a
large release. With the variable set, stderr.log gets, each line starting
"radv/ps5 free-log t=<ms>":

- free: every buffer of 16 MiB or more destroyed, with its address range, whether it was in
  2 MiB pieces, its RADV priority, and the newest submission against the one the GPU has run
  (submitted > completed: the GPU may still read it);
- alloc: every buffer of 16 MiB or more created, the same fields (pieces=1 is the pieced case);
- sparse: each sparse bind and unbind, the range and the backing address;
- replace: the queue's scratch, compute scratch, ES/GS and GS/VS rings and descriptor buffer
  when radv_update_preamble_cs replaces them (old and new range; the old one is freed at once);
- trim: each granule the trim gives back; the trim summary line and the pieces line lose their
  24 and 4 line caps.

Each kind gets RADV_PS5_FREE_LOG_LINES lines (default 200) per 10 s; the lines dropped are counted
in the next window. Unset, nothing changes. Applied after patch-radv-trim.py (and
patch-radv-retire.py, whose anchors it does not touch). Patches the build export, never the Mesa
source checkout.
"""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else
          root.parent / 'mihawk-vulkan-review/.deps/work/radv-src')
MARK = 'EglantinePS5 free log'

DECLARE = '''/* ''' + MARK + ''' (tools/patch-radv-free-log.py): RADV_PS5_FREE_LOG=1. */
enum radv_ps5_free_log_kind {
   RADV_PS5_FREE_LOG_MEMORY,
   RADV_PS5_FREE_LOG_SPARSE,
   RADV_PS5_FREE_LOG_REPLACE,
   RADV_PS5_FREE_LOG_TRIM,
   RADV_PS5_FREE_LOG_KINDS,
};
bool radv_ps5_free_log_on(void);
/* True when the line may be printed (the switch is on and the kind has budget left). */
bool radv_ps5_free_log_allow(enum radv_ps5_free_log_kind kind);
uint64_t radv_ps5_free_log_ms(void);
'''

DEFINE = '''
/* ''' + MARK + ''' (tools/patch-radv-free-log.py): lines to match a GPU fault's address against
 * what the driver freed. RADV_PS5_FREE_LOG=1 turns them on; each kind gets
 * RADV_PS5_FREE_LOG_LINES (200) lines per 10 s. */
bool
radv_ps5_free_log_on(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = getenv("RADV_PS5_FREE_LOG");
      enabled = value && value[0] == '1' && value[1] == '\\0';
   }
   return enabled;
}

uint64_t
radv_ps5_free_log_ms(void)
{
   return (uint64_t)(os_time_get_nano() / INT64_C(1000000));
}

bool
radv_ps5_free_log_allow(enum radv_ps5_free_log_kind kind)
{
   static const char *const names[RADV_PS5_FREE_LOG_KINDS] = {"memory", "sparse", "replace", "trim"};
   static simple_mtx_t lock = SIMPLE_MTX_INITIALIZER;
   static int64_t window_ns[RADV_PS5_FREE_LOG_KINDS];
   static uint32_t used[RADV_PS5_FREE_LOG_KINDS], dropped[RADV_PS5_FREE_LOG_KINDS];
   static uint32_t budget;
   if (!radv_ps5_free_log_on() || kind >= RADV_PS5_FREE_LOG_KINDS)
      return false;
   simple_mtx_lock(&lock);
   if (!budget) {
      const char *text = getenv("RADV_PS5_FREE_LOG_LINES");
      budget = text ? (uint32_t)strtoul(text, NULL, 10) : 200u;
      if (!budget)
         budget = 1;
   }
   const int64_t now = os_time_get_nano();
   if (!window_ns[kind] || now - window_ns[kind] >= INT64_C(10000000000)) {
      if (dropped[kind])
         fprintf(stderr, "radv/ps5 free-log t=%" PRIu64 " %u %s lines dropped in the last 10 s\\n",
                 (uint64_t)(now / INT64_C(1000000)), dropped[kind], names[kind]);
      window_ns[kind] = now;
      used[kind] = 0;
      dropped[kind] = 0;
   }
   const bool allowed = used[kind] < budget;
   if (allowed)
      used[kind]++;
   else
      dropped[kind]++;
   simple_mtx_unlock(&lock);
   return allowed;
}
'''

BO_LOG = '''/* ''' + MARK + ''' (tools/patch-radv-free-log.py): a buffer of 16 MiB or more created or freed. */
static void
radv_ps5_free_log_bo(const char *what, const struct radv_ps5_bo *bo)
{
   if (bo->memory.bytes < (UINT64_C(16) << 20) || !radv_ps5_free_log_allow(RADV_PS5_FREE_LOG_MEMORY))
      return;
   struct radv_ps5_queue *const queue = bo->ws ? bo->ws->queue : NULL;
   const uint64_t submitted = queue ? p_atomic_read(&queue->submitted_seq) : 0;
   const uint64_t completed = queue ? radv_ps5_queue_poll(queue) : 0;
   const uint64_t va = (uint64_t)(uintptr_t)bo->memory.cpu;
   fprintf(stderr,
           "radv/ps5 free-log t=%" PRIu64 " %s va=0x%" PRIx64 "-0x%" PRIx64 " (%" PRIu64 " MiB) sparse=%d pieces=%d "
           "priority=%u domain=0x%x submitted=%" PRIu64 " completed=%" PRIu64 "\\n",
           radv_ps5_free_log_ms(), what, va, va + bo->memory.bytes, bo->memory.bytes >> 20, (int)bo->sparse,
           bo->memory.pieces != NULL, (unsigned)bo->report_priority, (unsigned)bo->base.initial_domain, submitted,
           completed);
}

/* The queue replaced one of its buffers (radv_update_preamble_cs); the old one is destroyed next. */
void
radv_ps5_free_log_replace(const char *what, const struct radeon_winsys_bo *old_bo, const struct radeon_winsys_bo *new_bo)
{
   if (!old_bo || !radv_ps5_free_log_allow(RADV_PS5_FREE_LOG_REPLACE))
      return;
   const struct radv_ps5_bo *const old_ps5 = (const struct radv_ps5_bo *)old_bo;
   struct radv_ps5_queue *const queue = old_ps5->ws ? old_ps5->ws->queue : NULL;
   const uint64_t submitted = queue ? p_atomic_read(&queue->submitted_seq) : 0;
   const uint64_t completed = queue ? radv_ps5_queue_poll(queue) : 0;
   fprintf(stderr,
           "radv/ps5 free-log t=%" PRIu64 " replace %s old=0x%" PRIx64 "+0x%" PRIx64 " new=0x%" PRIx64 "+0x%" PRIx64
           " submitted=%" PRIu64 " completed=%" PRIu64 "\\n",
           radv_ps5_free_log_ms(), what, old_bo->va, old_bo->size, new_bo ? new_bo->va : 0, new_bo ? new_bo->size : 0,
           submitted, completed);
}

'''

REPLACE_DECLARE = '''/* ''' + MARK + ''' (winsys/ps5/radv_ps5_bo.c): RADV_PS5_FREE_LOG=1 logs a queue buffer replaced. */
void radv_ps5_free_log_replace(const char *what, const struct radeon_winsys_bo *old_bo,
                               const struct radeon_winsys_bo *new_bo);

'''


def queue_ring(name, label):
    return ('''   if (%s_bo != queue->%s_bo) {
      if (queue->%s_bo) {
''' % (name, name, name), '''   if (%s_bo != queue->%s_bo) {
      if (queue->%s_bo) {
         radv_ps5_free_log_replace("%s", queue->%s_bo, %s_bo);
''' % (name, name, name, label, name, name))


edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.h': [(
        'uint64_t radv_ps5_eden_trim(uint64_t want);\n',
        'uint64_t radv_ps5_eden_trim(uint64_t want);\n' + DECLARE)],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''#else
#include <sys/mman.h>
#endif
''', '''#else
#include <sys/mman.h>
#endif
''' + DEFINE),
        ('''   static uint32_t told;
   if (told++ < 4)
      fprintf(stderr, "radv/ps5: %" PRIu64 " MiB in %u pieces of 2 MiB (no run that long in the pool)\\n",
              out->bytes >> 20, granules);''', '''   static uint32_t told;
   if (radv_ps5_free_log_on()) {
      if (radv_ps5_free_log_allow(RADV_PS5_FREE_LOG_MEMORY))
         fprintf(stderr, "radv/ps5 free-log t=%" PRIu64 " pieces %" PRIu64 " MiB in %u pieces of 2 MiB va=0x%" PRIx64
                 "-0x%" PRIx64 " (no run that long in the pool)\\n", radv_ps5_free_log_ms(), out->bytes >> 20,
                 granules, (uint64_t)(uintptr_t)at, (uint64_t)(uintptr_t)at + out->bytes);
   } else if (told++ < 4)
      fprintf(stderr, "radv/ps5: %" PRIu64 " MiB in %u pieces of 2 MiB (no run that long in the pool)\\n",
              out->bytes >> 20, granules);'''),
        ('''         memory->pieces[g] = -1;
         returned += RADV_PS5_LARGE_BYTES;
''', '''         memory->pieces[g] = -1;
         returned += RADV_PS5_LARGE_BYTES;
         if (radv_ps5_free_log_allow(RADV_PS5_FREE_LOG_TRIM))
            fprintf(stderr, "radv/ps5 free-log t=%" PRIu64 " trim granule va=0x%" PRIx64 "-0x%" PRIx64 " idle_ms=%u\\n",
                    radv_ps5_free_log_ms(), (uint64_t)(uintptr_t)at, (uint64_t)(uintptr_t)at + RADV_PS5_LARGE_BYTES,
                    memory->idle_ms[g] ? now - memory->idle_ms[g] : 0u);
'''),
        ('''   if ((returned || held_back) && told++ < 24)''',
         '''   if ((returned || held_back) &&
       (radv_ps5_free_log_on() ? radv_ps5_free_log_allow(RADV_PS5_FREE_LOG_TRIM) : told++ < 24))'''),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c': [
        ('''static VkResult
radv_ps5_buffer_create(''', BO_LOG + '''static VkResult
radv_ps5_buffer_create('''),
        ('''   p_atomic_add(&radv_ps5_allocated_bytes, bo->memory.bytes);
''', '''   p_atomic_add(&radv_ps5_allocated_bytes, bo->memory.bytes);
   if (radv_ps5_free_log_on())
      radv_ps5_free_log_bo("alloc", bo);
'''),
        ('''   if (!bo)
      return;
   if (bo->sparse) {
''', '''   if (!bo)
      return;
   if (radv_ps5_free_log_on() && !bo->imported)
      radv_ps5_free_log_bo("free", bo);
   if (bo->sparse) {
'''),
        ('''   if (!done)
      fprintf(stderr, "radv/ps5: sparse %s of %" PRIu64 " bytes at %p failed\\n", bo ? "bind" : "unbind", size,
              (void *)at);
''', '''   if (!done)
      fprintf(stderr, "radv/ps5: sparse %s of %" PRIu64 " bytes at %p failed\\n", bo ? "bind" : "unbind", size,
              (void *)at);
   if (radv_ps5_free_log_allow(RADV_PS5_FREE_LOG_SPARSE))
      fprintf(stderr, "radv/ps5 free-log t=%" PRIu64 " sparse %s va=0x%" PRIx64 "-0x%" PRIx64 " backing=0x%" PRIx64
              " ok=%d\\n", radv_ps5_free_log_ms(), bo ? "bind" : "unbind", (uint64_t)(uintptr_t)at,
              (uint64_t)(uintptr_t)at + size,
              bo ? (uint64_t)(uintptr_t)radv_ps5_bo(bo)->memory.cpu + bo_offset : 0, (int)done);
'''),
    ],
    'src/amd/vulkan/radv_radeon_winsys.h': [(
        '''static inline uint64_t
radv_buffer_get_va(const struct radeon_winsys_bo *bo)''',
        REPLACE_DECLARE + '''static inline uint64_t
radv_buffer_get_va(const struct radeon_winsys_bo *bo)''')],
    'src/amd/vulkan/radv_queue.c': [
        queue_ring('scratch', 'scratch'),
        queue_ring('compute_scratch', 'compute scratch'),
        queue_ring('esgs_ring', 'esgs ring'),
        queue_ring('gsvs_ring', 'gsvs ring'),
        ('''   if (descriptor_bo != queue->descriptor_bo) {
      if (queue->descriptor_bo)
         radv_bo_destroy(device, NULL, queue->descriptor_bo);
''', '''   if (descriptor_bo != queue->descriptor_bo) {
      if (queue->descriptor_bo) {
         radv_ps5_free_log_replace("descriptor", queue->descriptor_bo, descriptor_bo);
         radv_bo_destroy(device, NULL, queue->descriptor_bo);
      }
'''),
    ],
}

# Each file is patched once: its first replacement carries the mark, or a comment does.
changed = False
for relative, replacements in edits.items():
    path = source / relative
    text = path.read_text()
    if MARK in text:
        continue
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: pinned anchor changed ({text.count(old)}x): {old.strip().splitlines()[0]!r}')
        text = text.replace(old, new)
    if MARK not in text:
        text = text.rstrip('\n') + '\n/* ' + MARK + ' (tools/patch-radv-free-log.py) */\n'
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV free log: frees, pieces, sparse binds, replaced queue buffers and trims (RADV_PS5_FREE_LOG=1)')
