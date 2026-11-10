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

## Looked at, not ported

- **Fence flush deferral** (Djihads80 `660e5f0039df`, Portablelle UB-042 `6105037`). Submission
  count is not proven to limit FPS here (~49 submits per frame at 36 FPS, 60 at 17 FPS). A safe
  port keeps the deferred flush on the recording thread and never flushes from `InnerFence::Wait`.
  Vampinity measured 40-54 white flashes with a 500 us batch and none with 0.
- **Occlusion counter pause** (Vampinity `0123-t39`). It hung an AMD host at save select
  (Lichii #121).
- **Staging cap** (exploitizm `fbb8b92306`). The staging caches held 7.75 MiB when run7 closed,
  so they are not what fills the pool.
- **Area eviction** (Vampinity `0274-mem-02`), **cold guest memory** (exploitizm `e952049757`).
  Both need adapting; viejunerd reports SSD swap did not help.

## Runs so far

See issue #1 of this fork for run3 to run8.
