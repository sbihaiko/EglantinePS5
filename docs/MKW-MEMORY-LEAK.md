# Mario Kart World: where the 12 GiB pool goes, and what is never given back

Status: **findings, and the first fixes** (2026-11-08; see "Fixed" at the end). The crash at the end of every long MKW session at 1x is the
pool running out (`VK_ERROR_OUT_OF_DEVICE_MEMORY`, or a code-2 SIGSEGV with the largest free
block at 1 MiB). This file records what holds the pool over a session and which parts never come
back, measured on the console with the diagnostic branch `diag/mkw-leak`.

## How it was measured

- Build: `main` at e573a23 (core patches 0001–0050), scripted input, plus the diagnostic branch:
  - `EDEN_MEMORY_PARTS` every 10 s: the driver's real buffer bytes (`radv_ps5_allocated_bytes`)
    and the guest memory the service holds.
  - `EDEN_CRAM_COLD` (`EDEN_CRAM_COLD_REPORT=1`) now also counts held 2 MiB slots that look
    all-zero.
- Runs: `tools/mkw-bench-console.py --events` from the title, L+R, menus, Grand Prix, into the race
  (1x and 0.5x). The texture cache's 600-frame summaries and the allocator's per-heap lines were
  kept with `EDEN_LOG_FILTER=*:Warning Render.Vulkan:Info HW.GPU:Info`.
- Bench folders on the dev machine: `.local/bench/b82-leak-1x`, `b83-leak-05x`,
  `b84-leakdiag-05x`, plus the earlier race runs `b69-mme-race05`.

## What holds the pool

0.5x, diagnostic build (MiB; t counted from the first sample after boot):

| t | Pool largest free | Game memory held | Driver buffers (real) | Eden heap | Game memory unwritten for 60 s |
|---|---|---|---|---|---|
| +23 s | 9,600 | 1,553 | 248 | 1,088 | 0 |
| +33 s | 6,718 | 2,859 | 1,628 | 1,216 | – |
| +43 s | 4,540 | 3,618 | 2,819 | 1,388 | 8 |
| +53 s | 2,984 | 4,205 | 3,662 | 1,452 | – |
| +63 s | 1,939 | 4,314 | 4,470 | 1,612 | 658 |
| +73 s | 1,872 | 4,560 | 3,432 | 1,644 | – |

1x, the same title → race path (20 s samples):

| Pool largest free | Game memory held | Game memory mapped by its kernel | Eden heap |
|---|---|---|---|
| 9.7 GB | 1.6 GB | 8.6 GB | 1.0 GB |
| 4.6 GB | 3.6 GB | 9.1 GB | 1.3 GB |
| 2.4 GB | 4.3 GB | 9.2 GB | 1.4 GB |
| 1.8 GB (title ends) | 5.0 GB | 9.4 GB | 1.5 GB |
| 1.2 GB (race loading) | **6.1 GB** | 9.6 GB | 1.5 GB |

## Findings

### 1. The game's memory: written once, held for good (the largest and the one that only grows)

- MKW's kernel maps ~9.5 GB for the game at boot. Eden backs a 2 MiB slot with real memory the first
  time the game writes it (`src/guest_memory_service.inc`). It gives a slot back only when the
  game returns that memory to its kernel (`returns_free`: 532 slots, ~1 GB, in a whole session).
- MKW frees and reuses memory inside its own heap, which the kernel (and so Eden) never sees.
  When the title scene ends, its data stays held, and the race's data goes into slots the game
  had not written before. Game memory held grows **1.5 → 6.1 GB** from boot through the race
  start in every run, and is the one figure that never goes down (only the LZ4 packer shrinks it).
  In the one session that lasted (b69, 420 s) it levelled off at ~5.5 GB.
- It is real data, not cleared memory: all-zero slots stayed at 4–8 MiB, and sampled cold slots
  compress to 0.29.
- A good part of it goes cold: 658 MiB unwritten for 60 s while still on the title, and the
  packer compressed 1,364 MiB unwritten for 60 s or more once the race was loading (b82).

On the console this is real hardware memory for the game. On the PS5 it shares one pool with
Eden's copies of the same textures (finding 2), Eden's heap and the driver, and that sum does not
fit.

### 2. Freed GPU memory comes back scattered: the largest free block does not recover

- When the title scene goes, the texture cache deletes ~2,700 images and the driver frees
  **1,038 MiB** (+63 → +73 s). The pool's largest free block **does not grow** (1,939 → 1,872 MiB).
- The driver's buffers come from 2 MiB pieces once the pool has no long run left
  (`tools/patch-radv-pieces.py`). Freeing them returns 2 MiB holes between the guest slots and
  heap pieces, not a block a later large allocation can use. The overlay's M figure (the largest
  free block) stays down after the title for this reason.

### 3. The texture collector reads twice the driver's memory, and churns

- The PS5 driver reports its buffers as both `allocated_vram` and `allocated_vram_vis`, and RADV's
  APU budget adds the two (`radv_get_memory_budget_properties`). Eden's
  `Device::GetDeviceMemoryUsage` subtracts only the allocator's unused space
  (`AllocatedUsage`), so the collector's "usage" is about twice the driver's real memory.
- So the collector sits at or above its critical limit (4 GiB) while the driver really holds
  3.2–3.4 GB. In 600 frames during the title's end it created 5,451 images and deleted 5,648
  (3,614 of them costly re-decodes). On the title before that it logged `scanned=0` for long
  stretches: every image had been used within the aggressive age, so it had nothing to delete,
  while the double-counted figure kept it in aggressive mode.

### 4. Eden's heap grows and never gives back

- 736 MiB → 1,772 MiB over a session (in use 481 → 1,267 MiB), "0 MiB given back" throughout.
  The heap-site report covers only ~500 MiB of it, so the growing allocations are not
  identified yet.

### Not the cause

- The driver's own memory: it is almost all the allocator's blocks (3,175 MiB of blocks against
  3,432 MiB of driver buffers), and it is freed when images are deleted.
- The texture cache keeping title textures: it deletes them (941 → 100 images when the title ends).

## Also seen in these runs (separate problems)

- **Freeze:** twice (b74 run 1 at ~75 s, b84 at ~100 s) MKW stopped presenting and every log
  stopped, with no crash report and no "GPU has not reached submission" line. Both builds were
  `main` after #80.
- **GPU hang without the transfer split:** b82 (1x, race loading) logged "the GPU has not reached
  submission" and device lost at 171 s on a build without `patch-radv-transfer-split.py`. So the
  split was not the only cause of the hangs PR #77 attributed to it.

## What would fix it, in order of payoff

1. **Give back the game's freed memory.** Release slots the game no longer uses without waiting for
   its kernel:
   - slots covered by GPU mappings the game unmaps (`Nx2GpuMemoryRegistry::Unmap`), whose data the
     game must write again before the GPU reads it;
   - cold slots under pressure: pack them, as now, or drop them when they are covered by texture
     cache images that hold the same data.
2. **Count the driver once.** Make `GetDeviceMemoryUsage` subtract the duplicated
   `allocated_vram_vis`, or read `radv_ps5_allocated_bytes` as `EDEN_MEMORY_PARTS` does, so the
   collector stops churning at a false critical.
3. **Keep the pool unfragmented.** Take the driver's big blocks before the guest's 2 MiB slots
   scatter the pool, or let the guest memory service hand 2 MiB pieces back to the driver in
   runs.
4. **Find the heap growth.** Extend the heap-site report to cover all allocations.

## Fixed (branch fix/mkw-memory-leaks)

### The race-load "out of memory" was one impossible index buffer (core patch 0056)

What ended 1x sessions as the race loaded was not the gradual growth above but a single request.
Switch 2 titles leave the index limit at 0, and the count/first registers sometimes hold values no
draw uses (1.4–2.9 billion indices). The 32-bit span arithmetic wrapped to a 1.3–2.9 GB "index
buffer"; the buffer cache merged up to 101 buffers into it, RADV built it from up to 1,372 2 MiB
pieces, and a staging buffer of the same size was asked for at once ("recover asked=2768MiB"). The
pool ran out on the spot, even with 2.4 GB free.

Core patch 0056 works out the span in 64 bits, treats an index limit below the start as no
bound, caps an index binding at 128 MiB, and drops an indexed draw whose indices reach past the
cap. Capping alone was not enough: run on the GPU, a 559 million index draw kept the queue past
the driver's 10 s limit and the device was lost.

### Packing is off by default (`EDEN_CRAM_PACK=1` turns it on)

Every GPU hang ("the GPU has not reached submission N in 10 s", then `VK_ERROR_DEVICE_LOST`, and
several times the console dropping off the network) came in a session that packed, seconds after a
packing burst. The impossible draws above came only in sessions that packed (none in the
packing-off runs). Some path reads the game's memory without faulting on a packed slot, so the
emulated GPU read garbage. Keeping packing's freed blocks for unpacking
(`EDEN_CRAM_UNPACK_KEEP_MIB`, default 256) removed the waits on unpacks (3.5 GB packed, 206
unpacks, no hang in 4 runs), but a later run still hung after a 506 MiB pass with 3 unpacks. So
packing stays off until it skips memory the GPU path uses. The keep and the tunables
`EDEN_CRAM_PACK_BELOW_MIB` / `EDEN_CRAM_PACK_COLD_S` stay for when it is turned on.

### Tried and dropped: giving back destroyed GPU memory

Returning the whole 2 MiB slots of GPU memory the game destroys (nx2:gpu 102) freed only ~219 MiB
a session, and the game still reads some of that memory. With it, sessions died of guest faults
("ActorLoading", "ARes Loading N jumped to an unmapped address": 1 of 2 runs, and 2 of 2 in
another set); without it, 0 of 2.

## Result

MKW 1x, title into a race, on the final build (packing off, patch 0056):

| Runs | Length | Result |
|---|---|---|
| 4 | 240 s | all ran the full 240 s (no out-of-memory, no GPU hang; the guest fault only in the runs that gave back destroyed memory) |
| 2 | 330 s | ended at 241 s and 292 s, the pool exhausted (largest free block 2–16 MiB, game memory 6.0–6.2 GB, Eden's heap 1.7 GB) |

Before: every 1x session that reached the race load ended within 131–211 s, by out-of-memory
(the impossible buffer), a GPU hang (packing), or a guest fault.

## Still open

- **The game's memory and Eden's copies still fill the pool.** At 240 s the largest free block is
  12–337 MiB, with 5.7–6.2 GB of game memory held. Making packing safe is the way to give that back.
- Findings 2–4 above (scattered free memory, the collector's double count, heap growth) are
  unchanged.

## Packing safety (branch diag/pack-safety)

MKW 1x, early packing (`EDEN_CRAM_PACK=1 EDEN_CRAM_PACK_BELOW_MIB=4096 EDEN_CRAM_PACK_COLD_S=20`).

**Packing itself is correct.**
- `EDEN_CRAM_PACK_KEEP=1` keeps a copy of every packed slot and compares it on unpack: 1,627
  unpacks compared, 0 differed.
- The shared zero block was never written (`zero_block_dirty=0`).
- Garbage persists with the GPU split off (`EDEN_NX2_SPLIT=0`).

**GPU threads no longer unpack.**
- `EDEN_CRAM_UNPACKERS` counts unpacks by thread name.
- Before: draws past the index cap appeared only in runs where the command reader ("GPU") unpacked.
- Skipping slots with pages in the GPU caches cut that down but did not stop it: a slot can be
  reached before it is cached.
- Core patch 0059 now reports every page DeviceMemoryManager maps for the GPU. Slots holding
  any such page are never packed.
- With 0059 (b107): no unpack by a GPU thread, no GPU hang, and 354–518 MiB packed (it was
  0.4–1.9 GB when everything cold was packable).

**The garbage draws are not packing's.**
- Even with no GPU unpack, these runs still showed the race-load event:
  - 16 indexed draws of ~1.1–1.4 billion indices, dropped by 0056.
  - The draw macro at 0x1e6 failing with "instruction limit exceeded" every ~0.1 s.
  - Then a slideshow (0.1–0.7 fps), corrupted textures, or a device lost.
- One count was 1065353216 = 0x3F800000, the float 1.0: the macro is fed a float where the game
  puts a count.
- The same event appears with packing off, just less often: b94 1 of 3 runs, b93 1 of 2.
- Ruled out as its cause:

| Run | Change | Result |
|---|---|---|
| b103 | GPU split off | 2/2 runs with the event |
| b108 | MME read-ahead off (`EDEN_NX2_DMA_READ_AHEAD=0`) | 2/3 runs with the event |
| b109 | native MME macros off (`EDEN_NX2_MME_NATIVE=0`) | every run with the event; prime device lost |

**Next:**
- Find where the macro's parameters come from at that point: the command list words around the
  0x1e6 calls, compared between a good run and a bad one.
- Packing is safe to turn back on only after that.

## Without PS5 Pro memory (2026-11-09, branch fix/mkw-nonpro-memory)

`param.json` attribute3 0x80040 instead of 0x8480040: the kernel gives the app `DMEM size: 0x2fc000000`
(12,224 MiB) instead of 0x348000000 (13,440 MiB). MKW 1x, scripted input from the title into a race
(`tools/mkw-leak-console.py`), build = main + #86 + #87 + #83 (core 0056) + SVC counters (core 0057).

**Every run ends at race load, 161 s after launch (121 s after loading), out of memory.**
Different allocations fail each time (a buffer, an image, `vkEndCommandBuffer`), with the pool's
largest free block at 1 MiB. No impossible draws and no macro failures: it is plain exhaustion.

| Run | Change | Largest free before the end | Game memory held | Driver | Eden heap | Result |
|---|---|---|---|---|---|---|
| run2 | baseline | 331 MiB | 5.9 GB | 2.5 GB | 1.6 GB | OOM at 161 s |
| run3 | + clear freed pieces (below) | 20 MiB | 5.55 GB | 3.2 GB | – | OOM at 161 s |
| run4 | + #84 heap release, #88 packing (GPU-reachable slots never packed) | 499 MiB | 5.45 GB | – | – | OOM at 161 s; 86 MiB packed |

### Why game memory grows: the game writes fresh pages of its 8 GB heap

- SVC counters (core 0057, `EDEN_SVC_MEMORY`): one `SetHeapSize` (8,162 MiB) at ~36 s, then
  ~3.3k `MapPhysicalMemory` / ~4.7k `UnmapPhysicalMemory` calls (net ~1 GiB), mostly in the first
  2.5 minutes. The game does **not** reserve everything at startup.
- ps5debug-NG (`tools/ps5debug.py watch`) shows the same from the console's side. The 20 GiB guest
  range at 0x8000000000 has `rw-` mappings exactly where Eden holds a slot: 12 MiB at 30 s, 5,140 MiB at
  240 s, matching `EDEN_MEMORY` (5,157 MiB).
- Diffing the samples by slot: after 100 s, 150–260 MiB of slots that had never been written appear
  every 20 s. They come in a few regions that move upward (13.0→13.5 GiB, 15.x GiB, 18.0→18.5 GiB of
  guest memory), and almost none are given back. The game's allocators keep moving into untouched
  heap pages as it streams data (on the title loop alone). On a Switch 2 the whole heap is real
  memory, so this costs nothing there; here every first write takes 2 MiB from the shared pool.

### Freed pieces: tried, small effect

The guest kernel's page heap reported only free blocks of 2 MiB or more. ~640 MiB of the 1.7 GB the
game unmapped came back in smaller pieces and stayed held. `GuestMemoryFreedPart` now clears such
pieces in held slots (`EDEN_GUEST_FREE_ZERO=0` turns it off), for the scanner to give back. run3
cleared 992 MiB of freed pages, but the game maps those pages again within seconds. Whole slots
returned stayed ~207, and game memory held at the end was ~300 MiB lower. It is kept (harmless), but
it is not the fix.

### Packing with GPU-reachable slots excluded packs almost nothing

MKW maps nearly all of its memory for the GPU, so core 0059's rule left 86 MiB to pack (run4).
Earlier runs without the rule packed 1.0–1.3 GB (into 0.2–0.3 GB), i.e. ~1 GB saved, the only lever
of that size.

### ps5debug-NG caution

Twice the console went off the network (a reboot, ~20 min) when the runner closed MKW while
`watch` was reading its map. The tool now checks the process is still listed, and the runs here
sample only the measured run and stop 50 s before the runner closes the game.

### Packing GPU-reachable slots too: MKW reaches the race without Pro memory, not reliably

`EDEN_CRAM_PACK_GPU=1` (packing starts below 3 GiB largest free, slots cold for 30 s). These are now
the defaults on this branch (`EDEN_CRAM_PACK=0` / `EDEN_CRAM_PACK_GPU=0` turn them off). 1.0–1.8 GB
packed into 0.15–0.4 GB per session. Seven 500 s runs from the title into a Grand Prix, without
Pro memory:

| Run | Result |
|---|---|
| run5 | whole first race (lap 3/3 at 341 s, ~31 fps), into the second; 6 fps while it loads (pool at 2–4 MiB) |
| run6-1 | race, then out of memory at 422 s (JIT code cache could not grow) |
| run6-2 | race-load event (macro 0x21a "instruction limit exceeded"), then the game read float data as a pointer and panicked at 165 s |
| run7 default-1 | out of memory at race load (171 s), ~1 GB packed |
| run7 default-2 | whole first race, into the second |
| run7 nozero-1 (`EDEN_GUEST_FREE_ZERO=0`) | app gone (PS5 home screen) by 341 s; the runner missed it |
| run7 nozero-2 | black screen from race load, pool at 0 |

Without packing: 0 of 3 reached the race. With it: 3 of 7, and 2 of 7 lasted ~300 s into it.
Where the pool is at the end (run6-1, 422 s): game 4.0 GB (+0.37 GB packed store), driver 3.8 GB
(texture cache at 6.4 GB "usage" against a 4 GB critical, with nothing it can delete), Eden heap
1.77 GB (1.3 GB in use, +16 MiB every 20 s), the unpack reserve 256 MiB, the guest reserve 512 MiB.

What is left, by size: the driver's 3.8 GB (texture working set and render targets at 1x), Eden's heap
(1.3 GB in use and growing), and the race-load peak (~1.5 GB within seconds, faster than 30 s cold
packing can answer).

Earlier packing (`EDEN_CRAM_PACK_BELOW_MIB=5120 EDEN_CRAM_PACK_COLD_S=15`, run8, 2 runs): 2.4 GB packed
into 0.5 GB. Both reached the race; one was gone (home screen) between 241 and 441 s, the other ran
out at 296 s (JIT code cache). Packing more of the game does not close the gap: what is left is the
driver (~3.8 GB) and Eden's heap (~1.5–1.8 GB).

### JIT code cache under pressure (run9, run10)

The JIT aborted when its next 16 MiB step was refused ("JIT code cache: the console has no memory for its next part"),
even after packing gave memory back (`recover asked=14MiB packed=14MiB`): the pool's free memory was
scattered, with no 16 MiB run. Now a refused step asks the guest memory service for memory back
(up to 3 times), and each step is taken as eight 2 MiB pieces. run10 (3 runs): no JIT abort. Two
runs ended at 407 and 415 s (~250 s into the race), when a heap allocation was refused (SIGSEGV at
0x199). The third, like run9's third and both "app gone" runs before it, ended after the race-load
macro event (0x1e6 / 0x21a "instruction limit exceeded", at 243–291 s): frames stop and the app is
gone.

So without Pro memory two things are left:
1. **The pool runs dry ~250 s into a race.** That is the game (~4.5 GB held plus ~0.4 GB packed),
   the driver (~3.3–3.8 GB) and Eden's heap (~1.6 GB). Packing more does not help (run8). The
   driver and the heap have to shrink.
2. **The macro event.** It comes at race loads, more often with packing and a short pool. Every
   "app gone" run ended with it. It is the open item from #88: find where 0x1e6's parameters come
   from when it fails.

### Idle blocks as a last resort (run11)

Recovery (`eden_memory_recover`, used by the driver, the heap and now the JIT) only packed slots
cold for 10 s, and the heap's non-waiting version got nothing while the scanner held the packing
lock. Now, when packing falls short, the blocks kept for unpacking (256 MiB) and the guest's reserve
(128 MiB) go back to the pool. run11, 3 runs: two ran the whole 500 s (race 1 to lap 3/3, then
race 2: ~340 s of racing without Pro memory), and none ran out of memory. The give-back was not
needed in them (packing covered every request), so those two are not proof of it. The third ended
after the macro event at 254 s (app gone).

Tally of GPU-packing runs since the JIT fix (run10, run11): 2 whole runs, 2 out of memory (heap,
before the give-back), 2 ended by the macro event. **The macro event is now what ends most runs.**
