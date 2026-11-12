# Ocarina of Time: what the sibling forks taught us

Console: base PS5 (CFI-2014), firmware 11.20. Core pin `3140648a` with the parent's patch series.
Each item below is behind an environment switch (`env.txt` in the app folder, read at start).

## In this build

| patch | switch | default | source |
|---|---|---|---|
| `0901-color-output-mask` | `EDEN_NX2_COLOR_OUTPUT_MASK=0` turns it off | on | Lichii #322/#353 (`79c151694a`) |
| `0902-amd-shader-fixes` | `EDEN_NX2_AMD_SHADER_FIXES=1` | off | mja00/Lichii `c418e4f477`, `5e8d409f81`, `913ba475da`, by way of danueecitizen `0413-white-03` |
| `0903-storage-size-word` | `EDEN_NX2_STORAGE_SIZE_WORD=0` turns it off | on | viejunerd `15cde68` (core patch 0082) |
| `0904-texture-collector-own-bytes` | `EDEN_NX2_TC_OWN_BYTES=1` | off | ours |
| `0905-batch-fence-submissions` | `EDEN_NX2_FENCE_FLUSH_US=500` turns it on (0908 made 0 the default) | off | Lichii `660e5f0039`, `313463cd27` (rdenno) |
| `0906-profile-submit-and-gpu-wait` | `EDEN_NX2_GPU_PROFILE=1` | off | ours |
| `0907-profile-split-drain-sites` | `EDEN_NX2_GPU_PROFILE=1` | off | ours |
| `0908-fence-batching-off-by-default` | — | — | ours |
| `0909-fence-work-on-split-back` | none | on | Lichii `e3d8e9c605` (#70), fence part |
| `0910-count-only-counters` | `EDEN_NX2_COUNTERS=1` (`EDEN_NX2_FAST_DRAW_PROBE` picks the probed methods) | off | ours |
| `0911-nan-guard-and-invariant-position` | `EDEN_NX2_NAN_GUARD=1`, `EDEN_NX2_POSITION_INVARIANT=1` | off | notquiteog/Dragoon `395547c` (Shakoquo/Hookshot `f04461bc9`) |
| `0913-destroy-unmap-all` | `EDEN_NX2_DESTROY_UNMAP_ALL=1` (the log line is always on) | off | Lichii #1199 (`bcb37c49f7`) |
| `0914-refresh-reused-device-ranges` | `EDEN_NX2_REFRESH_REUSED=1` (the log line is always on) | off | Lichii #1236, PantACRO4life `46e7920081` |
| `0915-tc-past-critical` | `EDEN_NX2_TC_PAST_CRITICAL=1` | off | Lichii #1306 |
| `0916-tc-padded-square-depth` | `EDEN_NX2_TC_PADDED_SQUARE=1` | off | Lichii #1314, commit 1 |
| `0917-tc-superseded-images` | `EDEN_NX2_TC_SUPERSEDED=1` | off | Lichii #1314, commit 2 |
| `0918-tc-square-depth-unscaled` | `EDEN_NX2_TC_SQUARE_DEPTH_UNSCALED=1` | off | Lichii #1314, commit 3 |
| `0920-flash-fixes` | `EDEN_NX2_FLASH_FIXES=1` | off | exploitizm/Eglantine `e0ce856` (Lichii #679 items 2 and 3, sharkiix `858c115541`, `45d3b1b84b`) |
| `0921-pending-poll-us` | `EDEN_NX2_PENDING_POLL_US=50` (1-999 µs; unset or 0 keeps 1 ms) | off | Lichii #446 (`acb9c18d6`) |
| `0922-skip-loop-writes` | `EDEN_NX2_SKIP_LOOP_WRITES=1` (split front only) | off | Lichii #178 (`7e2a9fe20`) with #488 (`5ba52f772`), without #488's restart part |
| `0923-dma-read-reported` | `EDEN_NX2_DMA_READ_REPORTED=1` (risk: a stale occlusion result for a frame) | off | Lichii #1032 (closed unmerged), on 0027's `EverReported` condition |
| `0924-upload-batch` | `EDEN_NX2_UPLOAD_BATCH=1` | off | Lichii #1245 |
| `0925-dispatch-barrier-inline` | `EDEN_NX2_DISPATCH_BARRIER_INLINE=1` | off | Lichii #1246 |
| `0926-device-alloc` | none of its own: `dev_alloc_n`, `dev_alloc_ms` and `dev_alloc_mib` join the `EDEN_STALL` line, and the timing is a no-op without `EDEN_NX2_STALL_REPORT` | — | ours: b3-A's pure `split_wait` stalls had no back-side cost to explain them |
| `0927-oot-texture-pool-x2` | `EDEN_NX2_OOT_TEXTURE_POOL_X2=1` | off | Lichii #1313 (RsMan-Dev): the store that sizes Ocarina's graphics allocator becomes a call that doubles x8 first, and the routine goes in the executable padding after the text segment. The pool is too small for the guard house once the resolution grows |
| `0928-back-profile` | `EDEN_NX2_BACK_PROFILE=1` | off | ours, from the review of the pool-exhaustion stalls: the split's back thread records its phase (idle, execute, submit, fence) and the `EDEN_STALL` line ends with `back_phase=` and how long it has been in it, so a stall says whether the back was idle, submitting or waiting on the GPU while the front waited seconds |
| `0929-mem-giveback` | none of its own: with `EDEN_NX2_STALL_REPORT` on, the `EDEN_STALL` line carries `mem_release_ms` and `mem_pageout_ms` | — | ours: the review's second alternative, guest map/unmap thrash. Time the guest spends giving memory back (`DecommitMemoryRange`, `PageOutRange`), so a stall that lands on those transitions is visible |
| `0930-oot-texture-pool-has-patch` | same switch as 0927 | off | ours: the loader calls `PatchNSO` only when `HasNSOPatch` says yes, so 0927 alone never ran. Build 7 shipped without this and the switch did nothing (no `TEXTURE_POOL` line in `eden_log.txt`) |
| `0931-heap-holders-giveback` | `EDEN_NX2_HEAP_GIVEBACK=1` | off | mihawk-99/PS5_Lichii `70b85df49d` patch 0230 ("three heap holders give their memory back", measured at 1,735 MiB live in play): a shader IR pool keeps at most 4 MiB between shaders (OoT held 113 MiB in six pools after the boot build), an upload scratch buffer over 8 MiB is given back every 256 frames and the ASTC scan buffer after a scan over 8 MiB (64 + 56 MiB were kept from the save load on) |
| app: `src/memory_pages.cpp` (`ReserveOver`) | `EDEN_PS5_RESERVE_OVER=1` (unset keeps the two calls) | off | parent PR #95: a mapping becomes reserved address space in one call, so the range is never free for a moment while another mapping can land in it (`DecommitMemoryRange`, `PageOutRange`, the rollbacks of `CommitMemoryRange`/`PageInRange`, the fastmem window's `UnmapChunks`) |
| driver: `tools/patch-radv-submit-thread.py` | `RADV_PS5_SUBMIT_THREAD=1` (`RADV_PS5_SUBMIT_SPIN_US`, default 2000) | off | PlayTendoh mesa 0008, by way of Vampinity `lead-mesh-14` |
| driver: `tools/patch-radv-retire.py` | `RADV_PS5_RETIRE=1`; `RADV_PS5_RETIRE_SLACK=<n>` (default 0) holds each freed buffer n submissions past the newest one at its destroy, for command lists the split's front recorded but the back had not submitted yet | off | mihawk-99/PS5_Mesa `f5610fe270` |
| driver: `tools/radv-patches/0f86253f-mesh-ring.patch` | none: an entry of the mesh shaders' scratch ring is 64 KiB instead of 160 KiB, so the ring is 128 MiB instead of 320 MiB. Take the patch file out to go back; the driver says on stderr when a shader keeps more than an entry | always on | mihawk-99/PS5_Mesa `0f86253f` |
| driver: `tools/patch-radv-free-log.py` | `RADV_PS5_FREE_LOG=1` (`RADV_PS5_FREE_LOG_LINES`, default 200 lines per kind per 10 s): `radv/ps5 free-log` lines for frees and allocations of 16 MiB or more, pieces, sparse binds, replaced queue buffers and trimmed granules; lifts the trim and pieces line caps | off | ours |
| app: `headless/heap_arenas.inc` (`eden_heap_trim`) | `EDEN_PS5_HEAP_NO_GIVEBACK=1` (the heap never gives pieces or arenas back; large blocks of their own still go back when freed) | off | ours |

- **Colour output mask.** A render target the fragment shader does not write keeps its contents.
  AMD writes undefined values (NaN) there, and bloom spreads them into white glow.
- **AMD shader fixes.** Storage buffers bind at 4 bytes on RADV, so 8 and 16-byte accesses go word
  by word where the vector view would read the wrong words; min, max and clamp use the NaN-defined
  forms.
- **Storage size word.** Kokiri Forest passes the float 1.0 (`0x3F800000`) where a size is
  expected, which asked for a 1 GiB buffer. A word above 256 MiB is bound as a bare pointer (up to
  8 MiB) and reported as `EDEN_SSBO_SIZE_WORD` in `stderr.log`.
- **Collector own bytes.** The device memory figure on the PS5 covers the whole shared pool, so it
  read 4.1-6.1 GiB against 1.0-1.6 GiB of images and the collector stayed in aggressive mode all
  through gameplay (up to 7,500 images created and 1,800 downloaded per 600 frames). With the
  switch the collector counts its own images; set `EDEN_NX2_TC_EXPECTED_MIB` and
  `EDEN_NX2_TC_CRITICAL_MIB` to match.

- **Front waits by call site.** `split drain` used to lump every front wait together. With the
  profile on, each wait now counts under its call site: `split full` (ring full), `drain: macro
  parameters`, `drain: sync fence`, `drain: DMA read`, `drain: command end`, `drain: puller`,
  `drain: CPU read`, `drain: list`, `drain: fence release` and `drain: macro flush`.
- **Fence work on the split's back.** run12 crashed (SIGSEGV, front GPU thread, 158 s) in
  `BufferCacheRuntime::CopyBuffer` under `ReleaseFences`: the front drained the back and then
  recorded into the scheduler, while the back's idle hook (`Scheduler::FlushDeferred`) could swap
  the chunk. `GpuSplit::Run` now sends the puller's fence releases, a GP entry's sync fence and
  the macro DMA read flush to the back, and waits. Batching (0905) stays off until a run shows it
  stable with this patch.

- **Count-only counters.** The profile reads the clock twice per zone, about 2.6 million times a
  second on the split's back (26 ns a read here: an estimated 135 ms a second), so it slows what
  it measures. `EDEN_NX2_COUNTERS=1` counts without the clock and logs every 10 s: the sets of
  fast draw breaks per draw (and how many draws only `EDEN_NX2_FAST_DRAW_PROBE` methods broke,
  default `0x54c,0x556`), the back's ring entries by kind and its deferred calls by thunk, and the
  executable 3D methods it runs. Thunk addresses are absolute: subtract the logged anchor, add
  the address of `Nx2CountersAnchor` in the symbols ELF (`llvm-nm -C`), then symbolize. Measure FPS
  with neither switch set.

- **NaN guard and invariant position.** Bright flashes and white glow on AMD have two causes the
  colour output mask does not cover. A float colour output holding NaN spreads through bloom, blur
  and downsample averages: with `EDEN_NX2_NAN_GUARD=1` NaN becomes 0 and the rest is clamped to
  +-1e30. The colour pass drawn again with depth EQUAL after the depth pre-pass can be rejected in
  spots when AMD computes a slightly different position, showing the bright sky that bloom blows
  up: `EDEN_NX2_POSITION_INVARIANT=1` decorates `gl_Position` Invariant. GrowingBlackBeans
  measured both with the colour output masks (viejunerd/Eglantine-PS5- #21): flashes from a few
  per walk to nearly none, no FPS cost. The first boot after a change builds the new pipelines.

- **On this branch (`pr99-test`): the parent's PR #99 and #100 (core 0070, 0072-0078).** 0070
  tracks storage buffers natively and routes global atomics, so 0912 is gone; it also reads 8 and
  16-byte storage accesses word by word whenever the binding alignment is below their size, so
  0902 keeps only its float min/max and memory parts. 0903 still applies. The parent measured
  OoT at ~25 FPS in Link's house with #99 (from ~40) and #100 taking back part of that.

- **Routed global atomics.** In every Ocarina session 8 compute pipelines fail at load with
  `SPIR-V Instruction EmitGlobalAtomicIAdd32 is not implemented`, right after `Storage buffer failed
  to track, using global memory fallbacks`, so those passes never run. With
  `EDEN_NX2_GLOBAL_ATOMICS=1` a 32-bit global atomic the storage buffer pass could not route walks
  the bound NVN storage buffers, as the global load and store fallbacks already do, and runs on the
  word that holds its address (INC, DEC and the float add through the storage CAS loops); with no
  buffer matching, the operation is dropped. A 32-bit atomic add in a compute pass is the usual
  shape of a luminance histogram, so this is the lead for the flashes the NaN guard did not stop.
  This is the SPIR-V part of the fork's earlier 0065, dropped on 2026-11-10 only because the
  parent's 0068 changed the same lines; 0068 replaces its Ampere lowering part.
  On the console (2026-11-11, with 0911 on): no pipeline fails any more, but 5 of the shaders
  have no bound NVN storage buffer at all (`Global atomic with no bound NVN storage buffer:
  dropped`), so their atomics still go nowhere, and the flashes are unchanged. The address
  these shaders use is not one the storage buffer pass can track; parent PR #99 (native
  storage buffer tracking) is the next thing to try.

- **Ranges into freed memory (0913, 0914).** Lichii #849 found Ocarina of Time drawing meshes
  whose index buffers held an actor pack and texture blocks: NaN that bloom spreads into a white
  wash, and white areas after area changes. Two ways the GPU can still read memory the game has
  freed and reused as heap: a range a sub 211 mapped into the memory at another address, which
  sub 102 left mapped (0913 unmaps it), and a device range reused for new pages at the same CPU
  address, which still points at the old ones (0914 maps it again; heap ranges are only logged).
  Both log what they find even with their switch off, so one session with them off says whether
  either path happens at all: look for `nx2:gpu 102 destroy of memory` and `pages point at old
  memory` in `stderr.log`. The release half of #1236 is already how this registry works: sub 102
  releases the device range once no mapping uses it.
- **Collect faster past the critical memory (0915).** Past `EDEN_NX2_TC_CRITICAL_MIB` the
  collector deletes one more image per 2 MiB over it (up to 1024 more a frame), and at 1 GiB over
  it deletes images 3 frames after their last use instead of 10. Lichii saw the cache grow 4 GiB
  in two seconds and the driver crash while pots broke in Hyrule Castle. With 2560/3328 the
  cache peaked at 2135 MB on our route, so this guards later areas, not the route.
- **Shadow map churn (0916-0918, Lichii #1314).** Ocarina of Time draws its local shadow maps
  into depth targets padded to whole GOBs (800x793) and samples them as squares (793x793), so each
  map was two images and a copy, for every new size while the camera moves; it also moves its
  render targets within a pool every frame, and the old images wait for the age-based collector.
  0916 looks a padded depth target up as its square; 0917 deletes, at the frame boundary, a
  bad-overlapping image unused for 2 frames once newer images cover all of it and nothing is lost
  (no GPU data, or every cover cleared its whole surface after the image's last write; images with
  a partial readback pending are kept); 0918 leaves a square depth-only pass unscaled. Lichii, on
  an RTX 4080: the guard house went from 4-7 to 53-59 fps with all three, and none of the three
  alone helped, so test them together. Risk: 0917 deletes GPU-written images without a download
  when a full clear covered them; that rests on `ClearOverwritesWhole`, ported because this pin
  lacks Lichii #817. 0918 does nothing at 1x resolution.
- **Empty flush submits** (Lichii #1329, Fritchii EXP61): already in the port.
  `tools/prepare-vulkan-port.py` makes `FlushCommands` return when a submission has taken every
  draw since the last one (`recorded_tick`).

- **Submit thread (driver).** `sceAgcDriverSubmitDcb` keeps the submitting thread in the kernel
  for a while; in run11 the driver spent 135-220 ms a second in `agc`. With the switch, RADV
  numbers a submission and copies it into the ring as before, then hands it to a thread of its own
  (`RadvSubmit`), which submits in order. Clock reads and AGC setting changes wait for the thread
  first; a refused submission loses the device instead of hanging a wait. The app names and places
  the thread like its own (`EDEN_PS5_PLACE=RadvSubmit=<cpu>`). GrowingBlackBeans measured it on
  Vampinity's base (viejunerd/Eglantine-PS5- #21, run H1, firmware 13.60, Handheld): walking 23.9
  to 25.2 FPS, walking 1% low 94 to 82 ms, present fence wait 13.9 to 6.5 ms. A host stress test
  (four submitters, 200,000 jobs, ThreadSanitizer) kept the order and found no race.
  Here (8cc7018, base PS5, firmware 11.20, Docked, 2026-11-11; one session off, two on, same
  route): 1-3 FPS more in most 5 s windows (house 19-20 against 17-19, after it 26 against 23-24,
  Kokiri 34-38 against 36-37), but longer stalls (10 s and 6 s, then 4 s and 2 s, against two of
  1.4 s off), each at a driver request for 512 MiB that came back nearly empty while the app packed
  guest memory (14 and 6 packing passes, against 6 off). The second session ended at 240 s with the system's crash screen and no crash
  file of the app. A third session on (no memory packing at all) had no stall over 0.7 s and no
  crash, at the same FPS gain: the stalls follow the memory packing, which varies per session,
  more than the switch. Left off until the crash is explained (the watcher now keeps the kernel
  log).

- **Batched submissions.** In run11 the back GPU thread made 850-1,540 queue submissions a
  second, one per fence and per command list end, and the driver spent 135-220 ms a second in
  `agc` and 190-250 ms in `suspend` on them. With the patch, a fence or the end of a command list
  on the split's back submits only when the last submission is older than
  `EDEN_NX2_FENCE_FLUSH_US` microseconds; otherwise the work goes with the next submission. The
  back submits deferred work before it sleeps and when another thread asks
  (`Scheduler::WaitSubmitted`). Only the back submits; `InnerFence::Wait` on another thread asks
  and waits, up to 2 s (`Fence tick N not submitted in 2 s` in the log). The end of a command
  list is deferred in `tools/prepare-vulkan-port.py`, which replaces
  `RasterizerVulkan::FlushCommands`. This pin has no command list page copies, so that part of
  `313463cd27` is left out. Vampinity measured 40-54 white flashes with a 500 us batch and none
  with 0: compare both values when judging bloom.
- **Profile zones.** `submit` is the time `Scheduler::Flush` takes on the recording thread and
  `GPU wait` the time it waits for a GPU tick; both were part of `other` before. With
  `EDEN_NX2_TC_OWN_BYTES=1` the log also prints the device figure every 600 frames, which is the
  query that makes the driver print `RADV_PS5_MEMORY` (`RADV_PS5_MEMORY_REPORT=1`).

## Open investigations

- **Bloom.** The glow around lights and the sky is blown out in every session since gameplay
  was first reached on this base, and no switch so far has changed it: the colour output mask
  (0901), `EDEN_NX2_AMD_SHADER_FIXES=1`, the NaN guard and invariant position (0911), routed
  global atomics (0912), and the 2560/3328 texture cache limits. Suspect: auto-exposure. Under
  0912, 5 of the 8 compute shaders with global atomics still drop them (no bound NVN storage
  buffer), so the exposure value the tonemapper reads is never updated. Next: parent PR #99 +
  #100 (branch `pr99-test`), which says it fixes exposure; if the bloom stays, the cause is not
  exposure.
- **Flashes.** The whole image flashes bright for one frame, often ("lightning on a rainy
  night"). Unchanged by every switch listed for bloom. Not texture re-decode alone: the larger
  texture cache removed the long stalls and the packing passes, and the flashes stayed. Next:
  PR #99 + #100, then Lichii #1199 and #1236 (unmap freed GPU ranges: OoT drew from freed heap,
  giving white flashes after area changes).
- **GPU page fault with the submit thread.** `RADV_PS5_SUBMIT_THREAD=1` crashed 2 of 4 sessions
  with an async GPU page fault right after a RADV trim; 0 of 4 crashed without it. Suspect: a
  deferred submission still references memory the trim released. Keep the switch off until the
  trim waits for the submit thread.

- **Performance.** Every sibling survey also looks for FPS. Base PS5, Docked, best env so far
  (0912 + 2560/3328): Link's house 49 -> 27, after the house 19-33, Kokiri 38-39. Candidates:
  - parent PR #100 (0076-0078): takes back part of PR #99's CPU cost (15.7 -> 20.2 outdoors on
    the parent's Pro); on `pr99-test`.
  - Lichii #1329: skip empty `FlushCommands` submits (+6% there); needs the change in
    `tools/prepare-vulkan-port.py`, which replaces that function.
  - Lichii #1056: macros on the back thread (front waits 17.9k/s -> 0, +8% there). viejunerd cut
    drains 17k -> 3.5k/s with no FPS change, so the gain on our base is uncertain.
  - The submit thread (+2-4 FPS mid-route), once the trim waits for it.
  - Thread layout (run B: +2-7 after the house), in the env since run B; one session each.
  - viejunerd's texture switches (`GPU_TRANSCODE_MIB=256`, `TC_KEEP_ASYNC=300`,
    `AREA_EVICT=40`): not on our pin; they need porting.
  - Lichii #1314 (texture cache) and #1334 (pipeline rebuild): open upstream, not yet read.

- **The pool runs out, and the largest block goes first (build 6, 2026-11-11).** Two sessions
  after `RADV_PS5_RETIRE_SLACK=1` had zero page faults and zero crashes — b5-A's unmapped read did
  not return, and both 150 MiB merge precursors happened again without one. What ends those
  sessions is memory, not a fault: the largest contiguous free block falls from 10,060 to 728 to
  140 to 3-18 MiB within about 90 s, and FPS follows it down (60 -> 10 -> 0.4, stalls of 2-8 s,
  every one of them `split_wait`, sometimes with `tc_upload`+`staging_create`). Five things the
  numbers say:
  1. **Placement, not total.** Guest (4.4-5.3 GiB) + driver plateau (2.7 GiB) + app heap
     (1.6-1.8 GiB) is the whole 9.7 GiB pool, and the driver's live set plateaus by t=90 s while
     the guest gives ~800 MiB back at times. Nothing grows without bound; the free *run* collapses
     early and stays collapsed.
  2. **The texture cache is not the consumer.** `EDEN_NX2_TC_EXPECTED_MIB`/`TC_CRITICAL_MIB` cut to
     1536/2304 (log confirms the override) changed nothing: the same 92 buffers of 16 MiB in VRAM
     (2.7 GiB) and the same collapse in ~3 min.
  3. **The stall is not in the app's `Commit`.** `dev_alloc_n=0` in every long stall, so no
     `vk::Buffer::Commit` ran; a 2 MiB-piece fallback there would have shown as a large
     `dev_alloc_ms`.
  4. **Correlation has to be built at the source.** `EDEN_STALL` and the driver's free-log use
     different clocks and cannot be aligned afterwards, so the instruments are on the code paths:
     `EDEN_NX2_BACK_PROFILE` (0928: `back_phase=` and its age) and `mem_release_ms`/
     `mem_pageout_ms` (0929).
  5. **Not a PS5-only symptom.** Lichii #1299 (4 GB desktop GPU) and #871 (AMD device loss at the
     movie-to-3D handover) describe the same class, which points at how large paths behave under
     pressure rather than at the console.
  Build 7 (`b7-memory`, run 43101496075) carries two candidate fixes, both off by default:
  `EDEN_NX2_OOT_TEXTURE_POOL_X2` (0927, Lichii #1313: the game's own graphics pool starts twice as
  large) and `EDEN_PS5_RESERVE_OVER` (parent PR #95: a mapping becomes reserved address space in
  one call, no window for another mapping). Judged by time-to-pool-exhaustion (baseline ~3 min) and
  the first stall over 1 s (baseline 2.7 s).

Build 11 (`e7e6381`, run 43143919613) adds a `caller=` field to the free log's lines of 16 MiB and
over, so the allocation that collapses the run is named at the source instead of guessed. Its
session was not run: the console was off. Build 11 also carries 0931, which ports mihawk's three
heap holders (shader IR pool, upload scratch, ASTC scan buffer) behind `EDEN_NX2_HEAP_GIVEBACK=1`:
that patch measured 1,735 MiB live in play after giving the three back, and it is the cheapest
answer yet to "placement, not total" above.

## Looked at, not ported

- **Null image on a memo hit** (Lichii `c2e293ec8`, #910). This pin already gates
  `PrepareImageView` on the null view.
- **Profile of GPU waits by zone** (Lichii `986a2b684`, #965). It builds on the stutter trace and
  drain sites this pin does not have; `0906` adds the two zones we need.
- **Native storage buffer tracking** (parent PR #99, patches 0070 and 0072-0075). It fixes
  lighting and tree leaves; the parent measured 25.2 FPS in Link's house and ~14.6 outdoors
  against ~40 and ~24 before it. Left out until FPS on a base PS5 allows it.
- **Occlusion counter pause** (Vampinity `0123-t39`). It hung an AMD host at save select
  (Lichii #121).
- **Staging cap** (exploitizm `fbb8b92306`). The staging caches held 7.75 MiB when run7 closed,
  so they are not what fills the pool.
- **Area eviction** (Vampinity `0274-mem-02`), **cold guest memory** (exploitizm `e952049757`).
  Both need adapting; viejunerd reports SSD swap did not help.

## Runs so far

See issue #1 of this fork for run3 to run11.
