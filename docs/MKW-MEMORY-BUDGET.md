# Mario Kart World: where the console's memory goes

Status, 2026-11-05: every title-screen run at 0.5x ends within 120–221 s. All three crash types
come from one cause: the title's single 12 GiB direct-memory pool runs out. The pool holds guest
RAM, the driver's GPU memory, Eden's heap, and everything else at once. The crash types are:

- `VK_ERROR_OUT_OF_DEVICE_MEMORY`
- `VK_ERROR_DEVICE_LOST`
- `SIGABRT` on a guest CPU thread

Measured with `tools/mkw-bench-console.py` (docs/MKW-BENCHMARK.md).

## Accounting at the crash

From a diagnostics build (`EDEN_MKW_DIAGNOSTICS=ON`, `MKW_MEMORY` every 5 s) with the driver
report on (`RADV_PS5_MEMORY_REPORT=1`):

| Seconds | Guest RAM | Eden heap (+ large) | Eden tables | Driver | Largest free |
|---|---|---|---|---|---|
| 25 | 1.74 GB | 1.04 + 0.17 GB | 0.22 GB | (loading) | 8.09 GB |
| 55 | 4.28 GB | 1.28 + 0.42 GB | 0.24 GB | ~3.9 GB | 1.59 GB |
| 85 | 4.60 GB | 1.38 + 0.42 GB | 0.24 GB | ~4.0 GB | 0.63 GB |
| 145 | 4.88 GB | 1.51 + 0.42 GB | 0.24 GB | 4.22 GB | **4 MiB** |

About 1 GB more is JIT code caches, thread stacks and libraries. The driver's 4.22 GB breaks down
by RADV priority as follows:

- 3.87 GB application memory (VMA)
- 0.26 GB descriptor and upload buffers. This grows through the whole run: 31 → 256 MiB, 822
  objects.
- 0.09 GB shaders

A VMA dump at 80 s shows 3.79 GB of blocks holding 3.26 GB of allocations:

- 0.95 GB render targets, including the game's 8192×8192×68 virtual-texture arrays
- 0.58 GB sampled textures
- 0.60 GB sparse bindings
- 0.53 GB buffers
- 0.53 GB empty space inside blocks

## What does not help

- **The texture collector.** It already measures the pool's largest free block
  (`graphics_usage_from_pool`) and evicts at its hardest when the pool runs short. At the crash
  it scans and finds nothing evictable: the images are in use.
  - Lower limits (`EDEN_NX2_TC_EXPECTED_MIB=4096`, `EDEN_NX2_TC_CRITICAL_MIB=4608`): crash at
    193 s, against 190 s with the defaults.
  - Much lower limits (3584/4096): crash at 133 s and 18 FPS, from re-upload churn.
- **ASTC→BC3.** Switch 2 ASTC is already re-compressed to BC3 on this pool
  (`UseBc3ForSwitch2OnSmallCards`).
- **ProsperoEden's guest-memory states (Zero/Shown).** They save CPU page tables, not memory.
  Eglantine's guest service is already newer here: it keeps a 512 MiB reserve.

## Freed guest pages give their memory back (2026-11-07)

Two bugs kept guest pages the game had freed in host memory until the guest reused them:

- **The derived kernel pool and page heap were never built.** `headless/CMakeLists.txt` set the
  core's sources to a list with `k_memory_manager.cpp` and `k_page_heap.cpp` derived, and the next
  line overwrote it with `kernel.cpp`'s list. The original files were built, so the free hook and the
  optimized-allocation fill rule never ran (`returns_free=0` on every console run).
- **The free hook only saw single frees of a whole 2 MiB slot.** Pages come back in pieces. The hook
  now sits where the guest kernel's page heap merges free buddies: once a free block of 2 MiB or more
  forms, its slots go back. `EDEN_GUEST_FREE_RETURN=0` turns this off.

Measured on MKW 1x (2 interleaved runs of 240 s each). With returns on, `returns_free` was 384–481
and `returns_clear` was 0. Before, the slots had come back later, when the guest cleared them on
reallocation (`returns_clear` 264).

| | Returns on | Off (`=0`) |
|---|---|---|
| Guest memory held at the end | 4.4–4.5 GB | 4.0 GB (at 130 s) |
| FPS | 24.6 / 26.4 | 19.3 / 24.1 |
| Crash at | 163 / 180 s | 130 / 180 s |

The crash stays: the guest really writes ~4.5 GB that it keeps. Freed memory came back on
reallocation before as well, just later.

## Research, 2026-11-07: what fills the pool, and packing cold guest memory

**Where the guest's memory is** (`EDEN_GUEST_POOLS`, every 20 s). All of it is the game's own
application pool: 4.7 GB at 180 s and still growing ~10 MiB/s. The GPU-only regions (system
non-secure pool) hold 0 MiB. A smaller memory layout does not boot: with
`EDEN_NX2_MEMORY_LAYOUT_GB=10` or `8`, MKW jumps to address 0 at 30 s.

**Eden's heap** holds 1.9–2.5 GB, of which 1.4–1.7 GB is in use. Its trim only gives back 32 MiB
pieces with no block in them. The free space inside pieces belongs to the SDK's mspaces, so it
cannot be given back.

**Cold guest memory** (`EDEN_CRAM_COLD_REPORT=1`). At ~160 s, 2.5 GB of 4.4 GB held had not been
written for 60 s or more. Sampled with LZ4, it compresses to **27%** (~0.8 ms per 2 MiB).

### Packing (`EDEN_CRAM_PACK=1`; off by default since 2026-11-08)

Off by default: in sessions that reached a race, packing was followed by garbage GPU draws
(1.8-2.9 billion indices) and GPU hangs, which never happened with it off; see
`docs/MKW-MEMORY-LEAK.md`. The comparison below ran on the title screen only.


- **Pack:** while the pool's largest free block is under 1.5 GiB, the scanner LZ4-compresses Own
  slots unwritten for 60 s or more (scanning every 5 s while short). It does up to 512 MiB per
  pass, coldest first. The packed data goes to a store of 2 MiB direct-memory blocks outside
  Eden's heap, the slot's block goes back, and the slot maps the zero block with no access.
- **Unpack:** any access faults, and the handler unpacks the slot into a new block. It
  decompresses in a separate view and only then maps the block at the slot.
- **Thrash guard:** slots that come back wait twice as long before being packed again.
- **First version bug:** a slot mapped readable for a moment showed zeroes, and a guest thread
  jumped to address 0.

Packing frees 2 MiB blocks all over the pool. The PS5 RADV winsys took each buffer as one run, so
its 64 MiB VMA blocks still failed with ~650 MiB free in 2 MiB blocks. `tools/patch-radv-pieces.py`
builds such a buffer from 2 MiB pieces at its device-region granules.

| MKW 1x, 2 runs of 300 s each | Packing | `EDEN_CRAM_PACK=0` |
|---|---|---|
| Out of device memory at | **203 / 244 s** | 163 / 120 s |
| FPS | 23.9 / 26.3 | 20.9 / 18.9 |
| Packed (stored) | 0.72–1.06 GB (140–185 MiB) | — |
| Unpacked again | 25–56% of packs | — |

The crash moves ~80 s later.

### Memory on demand, and the trims (final state)

- **Recovery on demand.** `eden_memory_recover(bytes)` first asks the driver to trim, then packs
  the coldest slots (unwritten ≥ 10 s). Three places call it:
  - the RADV winsys, when direct memory is refused;
  - VMA allocations (core patch `0023`);
  - Eden's heap commits, through a variant that never waits (the heap may hold its growth lock).

  The guest reserve is 128 MiB while packing runs (`EDEN_CRAM_RESERVE_MIB`).
- **Heap arenas.** Per-thread arenas held 802 MiB with 368 MiB live, 44 of them empty: arenas stay
  with the thread records that made them. A thread that would make a new arena now takes the
  emptiest one in the process if it is under half full. That is 100–220 MiB less heap
  (`EDEN_HEAP_ARENA_REUSE=0` turns it off). `EDEN_HEAP_SLACK` in the memory report shows the split.
- **Driver trim** (`tools/patch-radv-trim.py`). VMA blocks held 27–880 MiB more than their
  allocations (holes left by evicted images); smaller VMA blocks did not help.
  - The winsys counts, per 2 MiB granule of a buffer object, the images and buffers bound over it
    (counted at bind, released at destroy).
  - `radv_ps5_eden_trim` gives back granules with nothing bound and maps the zero block there. It
    gives back 130–330 MiB per pass.
  - A bind over a given-back granule takes a new piece first, or fails with
    `VK_ERROR_OUT_OF_DEVICE_MEMORY` so the app recovers memory and binds again.
- **Texture cache ceiling for Switch 2 titles** (core patch `0024`). The thresholds are capped at
  3.5/4 GiB (expected/critical); the environment variables still override them.

| MKW 1x | Ended at |
|---|---|
| none of the above | 113–180 s |
| packing + RADV pieces | 203 / 244 s |
| + recovery, arena reuse, driver trim | 281 / 291 / 300+ s |
| + texture cache ceiling (default) | 241 / 281 / 294 / 360+ / 420+ s |

**Dead ends** (measured):
- A smaller memory layout (10/8 GB): MKW does not boot.
- 16 MiB VMA blocks: the holes come from freed images, not from the block size.
- A 3/3.5 GiB texture cache: 8–11 s re-upload stalls.
- 0.75x resolution: 244 / 231 s, no gain.
- Recovering memory on the reserve keeper while writes wait: 8–15 s stalls.

**It still runs out at 4–7 minutes:** the guest's ~4.4 GB, the driver's ~4 GB and Eden's ~1.5 GB
fill the 12 GiB pool. ProsperoEden's MKW figures are from a PS5 Pro, whose pool is ~2 GB larger.
## Dense GPU-only regions backed lazily (ProsperoEden 0029/0031)

Core patch `0022-dense-lazy-gpu-regions.patch`. nx2:gpu 214 GPU-only regions of 16 MiB or more
get memory as the GPU writes them, like sparse regions already did.
`EDEN_NX2_DENSE_LAZY_MIB=<n>` moves the threshold, and `0` backs them whole again.

Measured on 2026-11-07 (MKW 1.8.0, title, 1x, interleaved, `Service:Info` log in a separate run):

| | Lazy (default) | Whole (`=0`) |
|---|---|---|
| Regions backed whole | 75 small ones, 31 MiB | 82, **8,830 MiB** |
| Guest kernel allocation failures | none | one 1,453 MiB region got no backing and stayed unmapped |
| FPS (2 runs of 240 s) | 20.5 / 23.4 | 26.2 / 18.6 |
| Out of device memory at | 163 / 173 s | 171 / 113 s |
| Pool free at 150 s | ~550 MiB | ~470 MiB |

**It does not fix the crash on this core.** Unlike ProsperoEden, Eglantine never clears a
GPU-only backing ("touching 300 MB would commit it on the host"), and guest memory takes host
memory only when written. A dense region backed whole already costs the console little more
than the lazy one. What the patch does change is the guest kernel side. Backed whole, the
regions used 8.8 GB of the guest kernel's system pool, and the largest one could not be
allocated: the GPU's writes to that region had nowhere to go. Backed lazily, every region
gets a mapping.

The crash's cause is therefore still the host-side growth listed below, not the GPU-only regions.

## The driver's own heap figure counts memory twice

The PS5 winsys reports its whole allocation total for both of its heaps. On MKW, VMA's
`heapUsage` was 7.8 GB with 3.5 GB of VMA blocks, while the per-priority report shows the
driver's true total near 4 GB. Pool-based usage is the default, so this does not change the
collector's decisions. Read the per-priority report rather than `VMA heap N: usage`.

## Where the remaining savings are

1. **Guest RAM: 4.9 GB, still growing at the crash.** The game maps 9.2 GB of its 12 GiB
   layout. Only written 2 MiB slots are held. The 512 MiB guest reserve sits idle while
   graphics allocations fail.
2. **Driver descriptor and upload buffers** grow without bound (≈ +1.5 MiB/s). Check whether
   descriptor pools or command-buffer upload buffers are ever trimmed.
3. **VMA internal fragmentation** (~0.5 GB) and the 0.6 GB of sparse bindings.
4. **Eden's heap** (1.9 GB) grows ~2 MiB/s through the title.

## Tools

- `tools/patch-radv-memory-report.py`: applied to the pinned RADV at driver build time. It
  keeps live bytes and objects per RADV priority. With `RADV_PS5_MEMORY_REPORT=1` in `env.txt`,
  it prints `RADV_PS5_MEMORY pool_mib=… available_mib=… driver_mib=… pN=MiB/count` to stderr at
  most every 10 s. Without the variable it only maintains the counters.
- `EDEN_MKW_DIAGNOSTICS=ON` builds print `MKW_MEMORY` (guest, heap, tables, largest free) every
  5 s.
