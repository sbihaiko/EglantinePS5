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
| `0905-batch-fence-submissions` | `EDEN_NX2_FENCE_FLUSH_US=0` turns it off (default 500) | on | Lichii `660e5f0039`, `313463cd27` (rdenno) |
| `0906-profile-submit-and-gpu-wait` | `EDEN_NX2_GPU_PROFILE=1` | off | ours |

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
