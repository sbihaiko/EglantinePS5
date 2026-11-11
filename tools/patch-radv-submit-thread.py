#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Submissions on a thread of their own in the pinned console RADV (opt-in: RADV_PS5_SUBMIT_THREAD=1).

sceAgcDriverSubmitDcb keeps the submitting thread in the console's kernel for a while (agc_ms in
the "radv/ps5 submissions" line: about 130 ms/s in Ocarina of Time's gameplay, run12). With the
variable set, a submission is still planned, numbered, given ring space and copied into the ring
on the submitting thread, under the submit lock; a thread of the driver's own then flushes the
words and hands them to AGC in the order they were made. Sync objects are signalled by sequence
number and ring space comes back as the marker passes it, both as before. The thread drains before
anything else reaches AGC (the clock packet, the tessellation ring and off-chip settings), and a
refused submission makes every later submit and CPU wait report the device lost.

RADV_PS5_SUBMIT_SPIN_US (2000 by default, 0-100000) is how long the thread looks for the next
submission before it sleeps. The thread names itself RadvSubmit and asks the app to place it
(eden_thread_named, weak), so EDEN_PIN_RadvSubmit / EDEN_PS5_PLACE apply to it.

Port of Vampinity/Eglantine-PS5- tools/patch-radv-series/lead-mesh-14-radv-submit-thread.patch
(itself a port of PlayTendoh's patches/mesa 0008, source reference 80a816a05c), adapted to our
Mesa pin: our tree has no submission dump metadata or submit line struct, and the suspend point
already runs on the platform's kick thread. Measured there: Ocarina walking 23.9 -> 25.2 fps,
present fence wait 13.9 -> 6.5 ms (viejunerd/Eglantine-PS5- #21, run H1, base PS5 13.60).
Patches the build export, never the Mesa source checkout.
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
MARK = 'EglantinePS5 submit thread'

SUBMITTER = r'''/* Submissions handed to a thread of their own (RADV_PS5_SUBMIT_THREAD=1, ''' + MARK + r''').
 *
 * sceAgcDriverSubmitDcb keeps the submitting thread in the console's kernel
 * for a while. With the variable set, a submission is planned, numbered, given
 * ring space and copied into the ring where it is made, as before; the thread
 * then flushes its words and hands them to AGC, in the order they were made.
 * A sync object is signalled by the sequence number, which the GPU reaches
 * only once the thread has submitted it, and ring space comes back as the
 * marker passes it, as before. RADV_PS5_SUBMIT_SPIN_US (2000 by default) is
 * how long the thread looks for the next submission before it sleeps. */
#define RADV_PS5_SUBMITTER_JOBS 256u

/* The app's: places and names a thread like its own (EglantinePS5, absent elsewhere). */
extern void eden_thread_named(const char *name) __attribute__((weak));

struct radv_ps5_submit_job {
   uint32_t *words;
   uint32_t count;
   volatile uint32_t *marker;
   uint32_t marker_value;
};

static struct {
   bool enabled;
   /* PAUSEs the thread spins for before it sleeps: about 20 ns each. */
   uint32_t spins;
   struct radv_ps5_submit_job jobs[RADV_PS5_SUBMITTER_JOBS];
   /* The next job the thread submits (only the thread writes it), and the
    * next free slot (written under the queue's submit lock). */
   uint32_t head;
   uint32_t tail;
   /* The thread sleeps, or is about to, on wanted. */
   uint32_t sleeping;
   /* Submitters waiting on done: for room, or for the thread to catch up. */
   uint32_t waiting;
   /* The first result AGC refused a submission with; the device is lost from
    * then on. */
   int32_t error;
   mtx_t lock;
   cnd_t wanted;
   cnd_t done;
} radv_ps5_submitter;
/* The platform's flush and AGC totals use plain +=: the thread's submissions
 * and the ten-second snapshot take this. */
static simple_mtx_t radv_ps5_submit_api_lock = SIMPLE_MTX_INITIALIZER;
static once_flag radv_ps5_submitter_once = ONCE_FLAG_INIT;

static int
radv_ps5_submitter_body(void *unused)
{
   (void)unused;
   u_thread_setname("RadvSubmit");
   if (eden_thread_named)
      eden_thread_named("RadvSubmit");
   uint32_t head = 0;
   for (;;) {
      for (uint32_t spin = radv_ps5_submitter.spins; spin && head == p_atomic_read(&radv_ps5_submitter.tail); spin--)
         __builtin_ia32_pause();
      if (head == p_atomic_read(&radv_ps5_submitter.tail)) {
         mtx_lock(&radv_ps5_submitter.lock);
         p_atomic_set(&radv_ps5_submitter.sleeping, 1);
         /* The flag before the look at the tail: a submitter stores the tail,
          * then looks at the flag, so one of the two sees the other's store. */
         __sync_synchronize();
         while (head == p_atomic_read(&radv_ps5_submitter.tail))
            cnd_wait(&radv_ps5_submitter.wanted, &radv_ps5_submitter.lock);
         p_atomic_set(&radv_ps5_submitter.sleeping, 0);
         mtx_unlock(&radv_ps5_submitter.lock);
      }
      const struct radv_ps5_submit_job job = radv_ps5_submitter.jobs[head % RADV_PS5_SUBMITTER_JOBS];
      int failure = 0;
      if (!p_atomic_read(&radv_ps5_submitter.error)) {
         simple_mtx_lock(&radv_ps5_submit_api_lock);
         const int result = radv_ps5_submit(job.words, job.count, job.marker, job.marker_value);
         simple_mtx_unlock(&radv_ps5_submit_api_lock);
         if (result != 0) {
            p_atomic_set(&radv_ps5_submitter.error, result);
            failure = result;
         }
      }
      head++;
      p_atomic_set(&radv_ps5_submitter.head, head);
      /* The head before the look at the waiters, as above. */
      __sync_synchronize();
      if (p_atomic_read(&radv_ps5_submitter.waiting)) {
         mtx_lock(&radv_ps5_submitter.lock);
         cnd_broadcast(&radv_ps5_submitter.done);
         mtx_unlock(&radv_ps5_submitter.lock);
      }
      if (failure)
         fprintf(stderr, "radv/ps5: sceAgcDriverSubmitDcb failed: 0x%08x (on the submit thread)\n",
                 (unsigned)failure);
   }
   return 0;
}

static void
radv_ps5_submitter_start(void)
{
   if (!debug_get_bool_option("RADV_PS5_SUBMIT_THREAD", false))
      return;
   const int64_t spin_us = CLAMP(debug_get_num_option("RADV_PS5_SUBMIT_SPIN_US", 2000), 0, 100000);
   radv_ps5_submitter.spins = (uint32_t)spin_us * 50;
   if (mtx_init(&radv_ps5_submitter.lock, mtx_plain) != thrd_success)
      goto failed_lock;
   if (cnd_init(&radv_ps5_submitter.wanted) != thrd_success) {
      mtx_destroy(&radv_ps5_submitter.lock);
      goto failed_lock;
   }
   if (cnd_init(&radv_ps5_submitter.done) != thrd_success) {
      cnd_destroy(&radv_ps5_submitter.wanted);
      mtx_destroy(&radv_ps5_submitter.lock);
      goto failed_lock;
   }
   thrd_t thread;
   if (thrd_create(&thread, radv_ps5_submitter_body, NULL) != thrd_success) {
      cnd_destroy(&radv_ps5_submitter.done);
      cnd_destroy(&radv_ps5_submitter.wanted);
      mtx_destroy(&radv_ps5_submitter.lock);
      fprintf(stderr, "radv/ps5: submissions stay on the submitting thread: the submit thread was refused\n");
      return;
   }
   thrd_detach(thread);
   radv_ps5_submitter.enabled = true;
   fprintf(stderr, "radv/ps5: submissions on a thread of their own (RADV_PS5_SUBMIT_THREAD), which looks for the next "
                   "one for %" PRId64 " us before it sleeps\n", spin_us);
   return;
failed_lock:
   fprintf(stderr, "radv/ps5: submissions stay on the submitting thread: the submit thread's lock was refused\n");
}

/* Waits until the submit thread has submitted every job before until. */
static void
radv_ps5_submitter_wait(uint32_t until)
{
   if ((int32_t)(until - p_atomic_read(&radv_ps5_submitter.head)) <= 0)
      return;
   mtx_lock(&radv_ps5_submitter.lock);
   p_atomic_inc(&radv_ps5_submitter.waiting);
   /* The count before the look at the head; the thread stores the head, then
    * looks at the count. */
   __sync_synchronize();
   while ((int32_t)(until - p_atomic_read(&radv_ps5_submitter.head)) > 0)
      cnd_wait(&radv_ps5_submitter.done, &radv_ps5_submitter.lock);
   p_atomic_dec(&radv_ps5_submitter.waiting);
   mtx_unlock(&radv_ps5_submitter.lock);
}

/* Every job submitted to AGC: before anything else reaches AGC, or an AGC
 * setting changes for the next submissions. Called with the submit lock held,
 * so no job is added meanwhile. */
static void
radv_ps5_submitter_drain(void)
{
   if (radv_ps5_submitter.enabled)
      radv_ps5_submitter_wait(p_atomic_read(&radv_ps5_submitter.tail));
}

/* Hands count words to the submit thread, after the jobs before them. Called
 * with the submit lock held. Returns what AGC refused an earlier one with, or
 * 0. */
static int
radv_ps5_submitter_push(uint32_t *words, uint32_t count, volatile uint32_t *marker, uint32_t marker_value)
{
   if (p_atomic_read(&radv_ps5_submitter.error))
      return p_atomic_read(&radv_ps5_submitter.error);
   const uint32_t tail = p_atomic_read(&radv_ps5_submitter.tail);
   /* All the slots in use: the thread is RADV_PS5_SUBMITTER_JOBS submissions
    * behind, so it is running, not asleep. */
   radv_ps5_submitter_wait(tail - RADV_PS5_SUBMITTER_JOBS + 1);
   radv_ps5_submitter.jobs[tail % RADV_PS5_SUBMITTER_JOBS] = (struct radv_ps5_submit_job){
      .words = words,
      .count = count,
      .marker = marker,
      .marker_value = marker_value,
   };
   /* The job before the tail, and the tail before the look at the thread's
    * flag. */
   __sync_synchronize();
   p_atomic_set(&radv_ps5_submitter.tail, tail + 1);
   __sync_synchronize();
   if (p_atomic_read(&radv_ps5_submitter.sleeping)) {
      mtx_lock(&radv_ps5_submitter.lock);
      cnd_signal(&radv_ps5_submitter.wanted);
      mtx_unlock(&radv_ps5_submitter.lock);
   }
   return p_atomic_read(&radv_ps5_submitter.error);
}

/* Never true with the option off. */
bool
radv_ps5_submission_failed(void)
{
   return p_atomic_read(&radv_ps5_submitter.error) != 0;
}

'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_cs.c': [
        ('#include "util/u_math.h"\n#include "sid.h"\n',
         '#include "util/u_math.h"\n#include "util/u_debug.h"\n#include "util/u_thread.h"\n#include "sid.h"\n'),
        ('/* ------------------------------------------------------------- submission */\n\n',
         '/* ------------------------------------------------------------- submission */\n\n' + SUBMITTER),
        # The clock packet goes to AGC directly: after every job before it.
        ('   simple_mtx_lock(&queue->submit_lock);\n   *value = UINT64_MAX;\n',
         '   call_once(&radv_ps5_submitter_once, radv_ps5_submitter_start);\n'
         '   simple_mtx_lock(&queue->submit_lock);\n'
         '   radv_ps5_submitter_drain();\n'
         '   if (radv_ps5_submission_failed()) {\n'
         '      simple_mtx_unlock(&queue->submit_lock);\n'
         '      return 0;\n'
         '   }\n'
         '   *value = UINT64_MAX;\n'),
        ('   uint64_t times[3];\n   radv_ps5_submit_times(times);\n',
         '   uint64_t times[3];\n'
         '   if (radv_ps5_submitter.enabled)\n'
         '      simple_mtx_lock(&radv_ps5_submit_api_lock);\n'
         '   radv_ps5_submit_times(times);\n'
         '   if (radv_ps5_submitter.enabled)\n'
         '      simple_mtx_unlock(&radv_ps5_submit_api_lock);\n'),
        ('   /* Waits need only their signals submitted: this queue runs in order. */\n',
         '   call_once(&radv_ps5_submitter_once, radv_ps5_submitter_start);\n'
         '   if (radv_ps5_submission_failed())\n'
         '      return VK_ERROR_DEVICE_LOST;\n\n'
         '   /* Waits need only their signals submitted: this queue runs in order. */\n'),
        ('   simple_mtx_lock(&queue->submit_lock);\n   /* AGC owns the tessellation factor ring',
         '   simple_mtx_lock(&queue->submit_lock);\n'
         '   if (radv_ps5_submission_failed()) {\n'
         '      simple_mtx_unlock(&queue->submit_lock);\n'
         '      return VK_ERROR_DEVICE_LOST;\n'
         '   }\n'
         '   /* AGC owns the tessellation factor ring'),
        ('      radv_ps5_queue_wait_seq(queue, queue->submitted_seq, OS_TIMEOUT_INFINITE);\n'
         '      const int set = radv_ps5_set_tess_factor_ring(',
         '      radv_ps5_submitter_drain();\n'
         '      if (!radv_ps5_queue_wait_seq(queue, queue->submitted_seq, OS_TIMEOUT_INFINITE)) {\n'
         '         simple_mtx_unlock(&queue->submit_lock);\n'
         '         return VK_ERROR_DEVICE_LOST;\n'
         '      }\n'
         '      const int set = radv_ps5_set_tess_factor_ring('),
        ('   if (ctx->tess_factor_ring_va && !queue->hs_offchip_param_set) {\n',
         '   if (ctx->tess_factor_ring_va && !queue->hs_offchip_param_set) {\n'
         '      radv_ps5_submitter_drain();\n'
         '      if (radv_ps5_submission_failed()) {\n'
         '         simple_mtx_unlock(&queue->submit_lock);\n'
         '         return VK_ERROR_DEVICE_LOST;\n'
         '      }\n'),
        ('      submitted = radv_ps5_submit(words + start, end - start, queue->marker, (uint32_t)seq);\n',
         '      if (radv_ps5_submitter.enabled)\n'
         '         submitted = radv_ps5_submitter_push(words + start, end - start, queue->marker, (uint32_t)seq);\n'
         '      else\n'
         '         submitted = radv_ps5_submit(words + start, end - start, queue->marker, (uint32_t)seq);\n'),
        ('      fprintf(stderr, "radv/ps5: sceAgcDriverSubmitDcb failed: 0x%08x\\n", (unsigned)submitted);\n',
         '      if (!radv_ps5_submitter.enabled)\n'
         '         fprintf(stderr, "radv/ps5: sceAgcDriverSubmitDcb failed: 0x%08x\\n", (unsigned)submitted);\n'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_sync.c': [
        ('   mtx_lock(&queue->sync_lock);\n   for (;;) {\n      uint32_t ready = 0;\n',
         '   mtx_lock(&queue->sync_lock);\n'
         '   for (;;) {\n'
         '      /* A submission the submit thread had refused never completes (' + MARK + '). */\n'
         '      if (radv_ps5_submission_failed()) {\n'
         '         mtx_unlock(&queue->sync_lock);\n'
         '         return VK_ERROR_DEVICE_LOST;\n'
         '      }\n'
         '      uint32_t ready = 0;\n'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c': [
        ('   uint64_t last = completed;\n   for (;;) {\n      completed = radv_ps5_queue_poll(queue);\n',
         '   uint64_t last = completed;\n'
         '   for (;;) {\n'
         '      /* A submission the submit thread had refused never completes (' + MARK + '). */\n'
         '      if (radv_ps5_submission_failed())\n'
         '         return false;\n'
         '      completed = radv_ps5_queue_poll(queue);\n'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.h': [
        ('/* Completion: the sequence the GPU has run up to, read from the marker. */\n',
         '/* The submit thread\'s first AGC failure; never true with RADV_PS5_SUBMIT_THREAD off\n'
         ' * (' + MARK + '). */\n'
         'bool radv_ps5_submission_failed(void);\n\n'
         '/* Completion: the sequence the GPU has run up to, read from the marker. */\n'),
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
            raise SystemExit(f'{relative}: pinned anchor changed: {old.splitlines()[0]!r}')
        text = text.replace(old, new)
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV submit thread: submissions on a thread of their own (RADV_PS5_SUBMIT_THREAD=1)')
