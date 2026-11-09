#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Device memory that no bound resource uses goes back to the console's pool when it is short.

VMA keeps its 64 MiB blocks while images in them are freed: on Mario Kart World the blocks held
27-880 MiB more than their allocations (the holes evicted textures leave), in the pool the CPU
shares with the GPU. The PS5 winsys now counts, for each 2 MiB granule of a buffer object, the
images and buffers bound over it (vkBind*Memory and their destruction, the one way any of them
reaches memory). radv_ps5_eden_trim(want) gives back granules with no resource on them: the
granule's direct memory is released and the shared zero block mapped in its place. A bind over a
given-back granule takes a new 2 MiB piece first; a bind that finds none fails with
VK_ERROR_OUT_OF_DEVICE_MEMORY (the app asks for memory back and binds again). Sparse resources,
imported and placed memory are left alone. Applied after patch-radv-pieces.py (pieces are its
2 MiB granules). Patches the build export, never the Mesa checkout.
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
MARK = 'EglantinePS5 memory trim'

TRIM = r'''
/* ''' + MARK + r''' (tools/patch-radv-trim.py): per 2 MiB granule of a buffer object, the
 * resources bound over it; granules with none go back when the app asks (radv_ps5_eden_trim). */
static int radv_ps5_trim_lock;
static bool
radv_ps5_trim_trylock(void)
{
   return p_atomic_cmpxchg(&radv_ps5_trim_lock, 0, 1) == 0;
}
static void
radv_ps5_trim_acquire(void)
{
   while (!radv_ps5_trim_trylock())
      thrd_yield();
}
static void
radv_ps5_trim_release(void)
{
   p_atomic_set(&radv_ps5_trim_lock, 0);
}
#define RADV_PS5_TRIM_TRACKED 8192
static struct radv_ps5_memory *radv_ps5_trim_tracked[RADV_PS5_TRIM_TRACKED];
static uint32_t radv_ps5_trim_count;
static uint64_t radv_ps5_trim_returned, radv_ps5_trim_retaken;

/* A granule goes back only after no resource has been on it for this long: Eden destroys an image
 * when its texture cache evicts it, while command lists recorded before (the split GPU thread runs
 * frames behind) may still read or write it. Giving its memory back at once left the GPU writing
 * through the zero block: GPU_FAULT_PAGE_FAULT_ASYNC on Mario Kart World 30 s after the first trim.
 * RADV_PS5_TRIM_QUARANTINE_MS overrides (default 2000). */
static uint32_t
radv_ps5_trim_now_ms(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   const uint32_t ms = (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
   return ms ? ms : 1;
}
static uint32_t
radv_ps5_trim_quarantine_ms(void)
{
   static uint32_t value;
   if (!value) {
      const char *text = getenv("RADV_PS5_TRIM_QUARANTINE_MS");
      value = text ? (uint32_t)strtoul(text, NULL, 10) : 2000u;
      if (!value)
         value = 1;
   }
   return value;
}

static bool
radv_ps5_trim_eligible(const struct radv_ps5_memory *memory)
{
   return memory->cpu && !memory->window_replay && memory->bytes >= RADV_PS5_LARGE_BYTES &&
          memory->bytes % RADV_PS5_LARGE_BYTES == 0 && (uintptr_t)memory->cpu % RADV_PS5_LARGE_BYTES == 0 &&
          (memory->physical >= 0 || memory->pieces) &&
          (memory->physical < 0 || memory->physical % (int64_t)RADV_PS5_LARGE_BYTES == 0);
}

/* The memory's granules as pieces (the trim lock is held). */
static bool
radv_ps5_trim_pieces(struct radv_ps5_memory *memory)
{
   if (memory->pieces)
      return true;
   const uint64_t granules = memory->bytes / RADV_PS5_LARGE_BYTES;
   int64_t *pieces = calloc(granules, sizeof(*pieces));
   if (!pieces)
      return false;
   for (uint64_t i = 0; i < granules; ++i)
      pieces[i] = memory->physical + (int64_t)(i * RADV_PS5_LARGE_BYTES);
   memory->pieces = pieces;
   memory->physical = -1;
   return true;
}

static void
radv_ps5_trim_untrack(struct radv_ps5_memory *memory)
{
   if (!memory->uses)
      return;
   radv_ps5_trim_acquire();
   for (uint32_t i = 0; i < radv_ps5_trim_count; ++i) {
      if (radv_ps5_trim_tracked[i] == memory) {
         radv_ps5_trim_tracked[i] = radv_ps5_trim_tracked[--radv_ps5_trim_count];
         break;
      }
   }
   free(memory->uses);
   memory->uses = NULL;
   free(memory->idle_ms);
   memory->idle_ms = NULL;
   radv_ps5_trim_release();
}

bool
radv_ps5_memory_use(struct radv_ps5_memory *memory, uint64_t offset, uint64_t size, int delta)
{
   if (size == 0 || offset >= memory->bytes)
      return true;
   radv_ps5_trim_acquire();
   if (!radv_ps5_trim_eligible(memory)) {
      radv_ps5_trim_release();
      return true;
   }
   const uint64_t granules = memory->bytes / RADV_PS5_LARGE_BYTES;
   if (!memory->uses) {
      if (delta < 0 || radv_ps5_trim_count == RADV_PS5_TRIM_TRACKED ||
          !(memory->uses = calloc(granules, sizeof(*memory->uses)))) {
         radv_ps5_trim_release();
         return true;
      }
      if (!(memory->idle_ms = calloc(granules, sizeof(*memory->idle_ms)))) {
         free(memory->uses);
         memory->uses = NULL;
         radv_ps5_trim_release();
         return true;
      }
      radv_ps5_trim_tracked[radv_ps5_trim_count++] = memory;
   }
   const uint64_t first = offset / RADV_PS5_LARGE_BYTES;
   const uint64_t last = MIN2((offset + size - 1) / RADV_PS5_LARGE_BYTES, granules - 1);
   bool ok = true;
   for (uint64_t g = first; g <= last; ++g) {
      if (delta < 0) {
         if (memory->uses[g] && --memory->uses[g] == 0)
            memory->idle_ms[g] = radv_ps5_trim_now_ms();
         continue;
      }
      if (memory->pieces && memory->pieces[g] < 0) {
         int64_t physical = -1;
         if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), RADV_PS5_LARGE_BYTES,
                                           RADV_PS5_LARGE_BYTES, RADV_PS5_DIRECT_TYPE, &physical) != 0 ||
             !radv_ps5_map_fixed(memory->cpu + g * RADV_PS5_LARGE_BYTES, RADV_PS5_LARGE_BYTES, physical)) {
            if (physical >= 0)
               sceKernelReleaseDirectMemory(physical, RADV_PS5_LARGE_BYTES);
            /* Undo this bind's counts: it fails, and its resource will not be destroyed as bound. */
            for (uint64_t back = first; back < g; ++back)
               if (memory->uses[back])
                  memory->uses[back]--;
            ok = false;
            break;
         }
         memory->pieces[g] = physical;
         radv_ps5_trim_retaken += RADV_PS5_LARGE_BYTES;
      }
      if (memory->uses[g] < UINT16_MAX)
         memory->uses[g]++;
   }
   radv_ps5_trim_release();
   return ok;
}

/* Gives back granules no resource is bound over, up to `want` bytes (0: all); returns how much.
 * Never waits: it may be asked from an allocation that holds the lock. */
uint64_t
radv_ps5_eden_trim(uint64_t want)
{
   if (!radv_ps5_trim_trylock())
      return 0;
   const int64_t zero = radv_ps5_zero_block();
   const uint32_t now = radv_ps5_trim_now_ms(), quarantine = radv_ps5_trim_quarantine_ms();
   uint64_t returned = 0, held_back = 0;
   for (uint32_t i = 0; i < radv_ps5_trim_count && zero >= 0 && (!want || returned < want); ++i) {
      struct radv_ps5_memory *memory = radv_ps5_trim_tracked[i];
      const uint64_t granules = memory->bytes / RADV_PS5_LARGE_BYTES;
      for (uint64_t g = 0; g < granules && (!want || returned < want); ++g) {
         if (memory->uses[g] || (memory->pieces && memory->pieces[g] < 0))
            continue;
         if (memory->idle_ms[g] && now - memory->idle_ms[g] < quarantine) {
            held_back += RADV_PS5_LARGE_BYTES;
            continue;
         }
         if (!radv_ps5_trim_pieces(memory))
            break;
         uint8_t *const at = memory->cpu + g * RADV_PS5_LARGE_BYTES;
         if (!radv_ps5_map_fixed(at, RADV_PS5_LARGE_BYTES, zero))
            continue;
         if (sceKernelReleaseDirectMemory(memory->pieces[g], RADV_PS5_LARGE_BYTES) != 0) {
            /* Kept: the system would not take a part back. Its own memory goes back in place. */
            if (!radv_ps5_map_fixed(at, RADV_PS5_LARGE_BYTES, memory->pieces[g]))
               abort();
            continue;
         }
         memory->pieces[g] = -1;
         returned += RADV_PS5_LARGE_BYTES;
      }
   }
   radv_ps5_trim_returned += returned;
   static uint32_t told;
   if ((returned || held_back) && told++ < 24)
      fprintf(stderr, "radv/ps5: trim gave back %" PRIu64 " MiB (asked %" PRIu64 "; %" PRIu64 " MiB back, %" PRIu64
              " MiB taken again so far; %" PRIu64 " MiB unused for less than %u ms kept)\n", returned >> 20,
              want >> 20, radv_ps5_trim_returned >> 20, radv_ps5_trim_retaken >> 20, held_back >> 20, quarantine);
   radv_ps5_trim_release();
   return returned;
}

void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (memory->window_replay) {'''

edits = {
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.h': [(
        '''   int64_t *pieces;
};
''',
        '''   int64_t *pieces;
   /* Per 2 MiB granule: resources bound over it (''' + MARK + '''). */
   uint16_t *uses;
   /* Per 2 MiB granule: when its last resource went (ms, CLOCK_MONOTONIC; 0 never bound). */
   uint32_t *idle_ms;
};

bool radv_ps5_memory_use(struct radv_ps5_memory *memory, uint64_t offset, uint64_t size, int delta);
uint64_t radv_ps5_eden_trim(uint64_t want);
''')],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''
void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (memory->window_replay) {''', TRIM),
        ('''   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
''', '''   radv_ps5_trim_untrack(memory);
   const int32_t unmapped = sceKernelMunmap(memory->cpu, memory->bytes);
'''),
        ('''      for (uint64_t i = 0; i < memory->bytes / RADV_PS5_LARGE_BYTES; ++i)
         sceKernelReleaseDirectMemory(memory->pieces[i], RADV_PS5_LARGE_BYTES);''',
         '''      for (uint64_t i = 0; i < memory->bytes / RADV_PS5_LARGE_BYTES; ++i)
         if (memory->pieces[i] >= 0)
            sceKernelReleaseDirectMemory(memory->pieces[i], RADV_PS5_LARGE_BYTES);'''),
        # Host build: nothing is given back.
        ('''radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (radv_ps5_window_contains(''', '''radv_ps5_memory_free_host(struct radv_ps5_memory *memory);

bool
radv_ps5_memory_use(struct radv_ps5_memory *memory, uint64_t offset, uint64_t size, int delta)
{
   (void)memory, (void)offset, (void)size, (void)delta;
   return true;
}

uint64_t
radv_ps5_eden_trim(uint64_t want)
{
   (void)want;
   return 0;
}

void
radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
   radv_ps5_memory_free_host(memory);
}

void
radv_ps5_memory_free_host(struct radv_ps5_memory *memory)
{
   if (!memory->cpu)
      return;
   if (radv_ps5_window_contains('''),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_bo.c': [(
        '''static void *
radv_ps5_buffer_map(''',
        '''/* ''' + MARK + ''': a resource bound over [va, va + size) of the buffer object (delta 1), or
 * destroyed (-1). False: no memory could be taken for it. */
bool
radv_ps5_bo_use(struct radeon_winsys_bo *base, uint64_t va, uint64_t size, int delta)
{
   struct radv_ps5_bo *bo = (struct radv_ps5_bo *)base;
   if (!bo || bo->sparse || bo->imported || bo->placed)
      return true;
   const uint64_t start = (uint64_t)(uintptr_t)bo->memory.cpu;
   if (va < start)
      return true;
   return radv_ps5_memory_use(&bo->memory, va - start, size, delta);
}

static void *
radv_ps5_buffer_map(''')],
    'src/amd/vulkan/radv_radeon_winsys.h': [(
        '''static inline uint64_t
radv_buffer_get_va(const struct radeon_winsys_bo *bo)''',
        '''/* ''' + MARK + ''' (winsys/ps5/radv_ps5_bo.c). */
bool radv_ps5_bo_use(struct radeon_winsys_bo *bo, uint64_t va, uint64_t size, int delta);

static inline uint64_t
radv_buffer_get_va(const struct radeon_winsys_bo *bo)''')],
    'src/amd/vulkan/radv_image.h': [(
        '''struct radv_image_binding {
   /* Set when bound */
   struct radeon_winsys_bo *bo;''',
        '''struct radv_image_binding {
   /* Set when bound */
   struct radeon_winsys_bo *bo;
   /* Counted over its granules (''' + MARK + ''') */
   bool eden_used;''')],
    'src/amd/vulkan/radv_buffer.h': [(
        '''   /* Set when bound */
   struct radeon_winsys_bo *bo;
   uint64_t range;
};''',
        '''   /* Set when bound */
   struct radeon_winsys_bo *bo;
   uint64_t range;
   /* Counted over its granules (''' + MARK + ''') */
   bool eden_used;
};''')],
    'src/amd/vulkan/radv_image.c': [
        ('''      const uint64_t addr = radv_buffer_get_va(mem->bo);

      radv_bind_image_memory(device, image, bind_idx, mem->bo, addr, offset, reqs.memoryRequirements.size);''',
         '''      const uint64_t addr = radv_buffer_get_va(mem->bo);

      if (!radv_ps5_bo_use(mem->bo, addr + offset, reqs.memoryRequirements.size, 1)) {
         if (status)
            *status->pResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      radv_bind_image_memory(device, image, bind_idx, mem->bo, addr, offset, reqs.memoryRequirements.size);
      image->bindings[bind_idx].eden_used = true;'''),
        ('''   for (uint32_t i = 0; i < ARRAY_SIZE(image->bindings); i++) {
      if (!image->bindings[i].addr)
         continue;
''', '''   for (uint32_t i = 0; i < ARRAY_SIZE(image->bindings); i++) {
      if (!image->bindings[i].addr)
         continue;
      if (image->bindings[i].eden_used)
         radv_ps5_bo_use(image->bindings[i].bo, image->bindings[i].addr, image->bindings[i].range, -1);
'''),
    ],
    'src/amd/vulkan/radv_buffer.c': [
        ('''      buffer->bo = mem->bo;
      buffer->vk.device_address = radv_buffer_get_va(mem->bo) + pBindInfos[i].memoryOffset;
      buffer->range = reqs.memoryRequirements.size;''',
         '''      if (!radv_ps5_bo_use(mem->bo, radv_buffer_get_va(mem->bo) + pBindInfos[i].memoryOffset,
                           reqs.memoryRequirements.size, 1)) {
         if (status)
            *status->pResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      buffer->bo = mem->bo;
      buffer->vk.device_address = radv_buffer_get_va(mem->bo) + pBindInfos[i].memoryOffset;
      buffer->range = reqs.memoryRequirements.size;
      buffer->eden_used = true;'''),
        ('''   if ((buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) && buffer->bo)
      radv_bo_destroy(device, &buffer->vk.base, buffer->bo);
''', '''   if ((buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) && buffer->bo)
      radv_bo_destroy(device, &buffer->vk.base, buffer->bo);
   else if (buffer->eden_used)
      radv_ps5_bo_use(buffer->bo, buffer->vk.device_address, buffer->range, -1);
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
        if old not in text and new in text:
            continue  # already applied (a file patched before this script marked it)
        if text.count(old) != 1:
            raise SystemExit(f'{relative}: pinned anchor changed ({text.count(old)}x): {old.strip().splitlines()[0]!r}')
        text = text.replace(old, new)
    path.write_text(text)
    changed = True
if changed:
    receipt = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if source.resolve() == (root.parent / 'mihawk-vulkan-review/.deps/work/radv-src').resolve() and receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print('RADV memory trim: granules no bound resource uses go back when the app asks')
