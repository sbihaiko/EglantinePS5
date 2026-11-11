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
| `0912-routed-global-atomics` | `EDEN_NX2_GLOBAL_ATOMICS=1` | off | the fork's own earlier `0065-ocarina-global-reduction` (SPIR-V part) |
| driver: `tools/patch-radv-submit-thread.py` | `RADV_PS5_SUBMIT_THREAD=1` (`RADV_PS5_SUBMIT_SPIN_US`, default 2000) | off | PlayTendoh mesa 0008, by way of Vampinity `lead-mesh-14` |

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
