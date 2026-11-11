#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A freed buffer keeps its memory until the GPU has run what was submitted before (opt-in: RADV_PS5_RETIRE=1).

The PS5 winsys unmaps a buffer, or a sparse range, and releases its direct memory the moment it is
destroyed. The GPU runs submissions behind the CPU, so a read it still had to make of that memory
hits an unmapped page ("Unmapped page access", a TCP read in the RADV region at 0x40_xxxx_xxxx).
RADV itself frees that way: radv_update_preamble_cs destroys the old scratch, ring and descriptor
buffers at once when a pipeline needs larger ones. On Linux the amdgpu kernel keeps a freed
buffer's pages until the fences of the submissions using it signal.

With the variable set, destroying a buffer or a sparse range notes the newest submission made so
far (queue->submitted_seq, set before the submit thread takes it) and gives the memory and the
range back only once the GPU has run it; until then the range stays reserved, so no other buffer
is placed there. Held memory is given back on later creates and destroys, and an allocation that
fails waits for the GPU to finish with it and tries again before reporting out of memory (the
wait gives up when the GPU stops making progress, radv_ps5_queue_wait_seq). Unset, a destroy
releases at once, as before.

Adapted to the trim (tools/patch-radv-trim.py), which this runs after: a held buffer leaves the
trim's tracked list before its record is copied, so the trim never reads a freed buffer object
and never gives back a granule of memory the GPU may still read.

Port of mihawk-99/PS5_Mesa f5610fe270 ("radv/ps5: a freed buffer keeps its memory until the GPU
has run what was submitted before"). Mihawk measured: "9 of about 70 such sessions that ran short
of memory ended in this GPU fault; with this change, 11 sessions (8 of them short of memory) ran
without one, at the same frame rate". Patches the build export, never the Mesa source checkout.
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
MARK = 'EglantinePS5 retire'

RETIRE = r'''/* ''' + MARK + r''' (tools/patch-radv-retire.py, mihawk-99/PS5_Mesa f5610fe270): a
 * buffer freed while the GPU may still run submissions made before the free keeps its memory and
 * its range until they have run, as the amdgpu kernel keeps a freed buffer's pages until its fences
 * signal. Unmapped at once, a read the GPU had still to make faults it (the title is ended), and
 * the range may meanwhile go to another buffer. RADV_PS5_RETIRE=1 turns it on. */
struct radv_ps5_retired {
   struct radv_ps5_memory memory;
   uint64_t seq;
   bool sparse;
};
static simple_mtx_t radv_ps5_retired_lock = SIMPLE_MTX_INITIALIZER;
static struct util_dynarray radv_ps5_retired;

static bool
radv_ps5_retire_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = getenv("RADV_PS5_RETIRE");
      enabled = value && value[0] == '1' && value[1] == '\0';
      if (enabled)
         fprintf(stderr, "radv/ps5: freed buffers kept until the GPU has run what was submitted before "
                         "(RADV_PS5_RETIRE=1)\n");
   }
   return enabled;
}

/* How many submissions past the one that was newest at the destroy a buffer waits for.
 * Zero keeps mihawk's rule. One covers the case b5-A showed: the split's front records
 * commands that name a buffer, the back submits them later, and the destroy happens while
 * submitted_seq already equals the GPU's completed value, so the buffer is released before
 * the list that reads it is even submitted. RADV_PS5_RETIRE_SLACK=1 waits for one more. */
static uint64_t
radv_ps5_retire_slack(void)
{
   static int slack = -1;
   if (slack < 0) {
      const char *value = getenv("RADV_PS5_RETIRE_SLACK");
      slack = 0;
      if (value) {
         for (const char *p = value; *p >= '0' && *p <= '9' && slack < 64; p++)
            slack = slack * 10 + (*p - '0');
      }
      if (slack)
         fprintf(stderr, "radv/ps5: a freed buffer waits %d submissions past the newest at its "
                         "destroy (RADV_PS5_RETIRE_SLACK=%d)\n", slack, slack);
   }
   return (uint64_t)slack;
}

static void
radv_ps5_memory_release(struct radv_ps5_memory *memory, bool sparse)
{
   if (sparse)
      radv_ps5_vrange_release(memory);
   else
      radv_ps5_memory_free(memory);
}

/* Gives back what the GPU has finished with; the newest submission the rest
 * waits for, or 0 if nothing is held. */
static uint64_t
radv_ps5_retired_free(struct radv_ps5_queue *queue)
{
   simple_mtx_lock(&radv_ps5_retired_lock);
   const uint64_t completed = radv_ps5_queue_poll(queue);
   uint64_t newest = 0;
   unsigned kept = 0;
   util_dynarray_foreach (&radv_ps5_retired, struct radv_ps5_retired, entry) {
      if (entry->seq <= completed) {
         radv_ps5_memory_release(&entry->memory, entry->sparse);
      } else {
         newest = MAX2(newest, entry->seq);
         *util_dynarray_element(&radv_ps5_retired, struct radv_ps5_retired, kept++) = *entry;
      }
   }
   radv_ps5_retired.size = kept * sizeof(struct radv_ps5_retired);
   simple_mtx_unlock(&radv_ps5_retired_lock);
   return newest;
}

static void
radv_ps5_memory_retire(struct radv_ps5_queue *queue, struct radv_ps5_memory *memory, bool sparse)
{
   if (!queue || !radv_ps5_retire_enabled()) {
      radv_ps5_memory_release(memory, sparse);
      return;
   }
   /* Out of the trim's list before the record is copied: the trim keeps pointers to it. */
   if (!sparse)
      radv_ps5_memory_hold(memory);
   const uint64_t seq = p_atomic_read(&queue->submitted_seq) + radv_ps5_retire_slack();
   if (seq <= radv_ps5_queue_poll(queue)) {
      radv_ps5_memory_release(memory, sparse);
   } else {
      simple_mtx_lock(&radv_ps5_retired_lock);
      const struct radv_ps5_retired entry = {.memory = *memory, .seq = seq, .sparse = sparse};
      util_dynarray_append(&radv_ps5_retired, entry);
      simple_mtx_unlock(&radv_ps5_retired_lock);
   }
   radv_ps5_retired_free(queue);
}

/* After an allocation failed: waits for the GPU to finish with the memory held
 * for it and gives it back; false if nothing was held (always, unset). */
static bool
radv_ps5_retired_wait(struct radv_ps5_queue *queue)
{
   if (!queue || !radv_ps5_retire_enabled())
      return false;
   const uint64_t newest = radv_ps5_retired_free(queue);
   if (!newest || !radv_ps5_queue_wait_seq(queue, newest, OS_TIMEOUT_INFINITE))
      return false;
   radv_ps5_retired_free(queue);
   return true;
}

'''

MEMORY_REPORTS = '''/* Memory reports (VK_EXT_device_memory_report) name a buffer by an id that is
 * never 0 and never reused. */
'''

HOLD = '''
/* ''' + MARK + ''' (tools/patch-radv-retire.py): memory whose release waits for the GPU leaves the
 * trim's list first (the record is copied, and the trim must not give back what the GPU may read). */
void
radv_ps5_memory_hold(struct radv_ps5_memory *memory)
{
   radv_ps5_trim_untrack(memory);
}
'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.h': [(
        'uint64_t radv_ps5_eden_trim(uint64_t want);\n',
        'uint64_t radv_ps5_eden_trim(uint64_t want);\n'
        '/* ' + MARK + ': out of the trim\'s list before its release waits for the GPU. */\n'
        'void radv_ps5_memory_hold(struct radv_ps5_memory *memory);\n')],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''
void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (memory->window_replay) {''', HOLD + '''
void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (memory->window_replay) {'''),
        # Host build: the trim tracks nothing.
        ('''void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   radv_ps5_memory_free_host(memory);
}
''', '''void
radv_ps5_memory_hold(struct radv_ps5_memory *memory)
{
   (void)memory;
}

void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   radv_ps5_memory_free_host(memory);
}
'''),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c': [
        (MEMORY_REPORTS, RETIRE + MEMORY_REPORTS),
        ('''      if (!radv_ps5_vrange_reserve(size, flags & RADEON_FLAG_32BIT,
                                   (flags & RADEON_FLAG_REPLAYABLE) || replay_address, replay_address,
                                   &bo->memory)) {
''', '''      bool reserved;
      do {
         reserved = radv_ps5_vrange_reserve(size, flags & RADEON_FLAG_32BIT,
                                            (flags & RADEON_FLAG_REPLAYABLE) || replay_address, replay_address,
                                            &bo->memory);
      } while (!reserved && radv_ps5_retired_wait(ws->queue));
      if (!reserved) {
'''),
        ('''   if (replayable ? !radv_ps5_memory_alloc_replayable(size, alignment, window32, replay_address, &bo->memory)
                  : !radv_ps5_memory_alloc(size, alignment, window32, &bo->memory)) {
''', '''   bool allocated;
   do {
      allocated = replayable ? radv_ps5_memory_alloc_replayable(size, alignment, window32, replay_address, &bo->memory)
                             : radv_ps5_memory_alloc(size, alignment, window32, &bo->memory);
   } while (!allocated && radv_ps5_retired_wait(ws->queue));
   if (!allocated) {
'''),
        ('''   if (bo->sparse) {
      radv_ps5_vrange_release(&bo->memory);
      free(bo);
      return;
   }
''', '''   if (bo->sparse) {
      radv_ps5_memory_retire(ws->queue, &bo->memory, true);
      free(bo);
      return;
   }
'''),
        ('''   radv_ps5_memory_free(&bo->memory);
   free(bo);
}
''', '''   radv_ps5_memory_retire(ws->queue, &bo->memory, false);
   free(bo);
}
'''),
    ],
}

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
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV retire: freed buffers kept until the GPU has run what was submitted before (RADV_PS5_RETIRE=1)')
