# Jumping the port onto Lichii's core (November 2026)

Branch `core-jump-bare`: the Eglantine submodule moves from `3140648a` to `d2eaae4f3` (Lichii's
`nx2`, 1,137 commits newer) and the 65-patch series is dropped. What the new core already does
replaces most of them; the rest is either ported into the port itself or set aside until it can be.

## What the siblings taught

- **premohq/PlayTendoh**, `patches/eglantine-lichii-next/` (0011–0021): the same jump, already done.
  All 69 Vulkan anchors fit Lichii; where Lichii now does the same work itself, Lichii's is kept —
  the compute read barrier, the 512-draw submission cap, the scratch-buffer release, the 16 MiB VMA
  blocks, the pipeline cache writes, the hardened proxy sockets and BSD service, and **the garbage
  collector**. Their 0019 moves the port's 54 remaining core patches to
  `tools/eglantine-patches-pending/` and pins the submodule to `d2eaae4`.
  Our branch took the same decisions independently for the collector and the network code, and
  applied 0012–0018 and 0020 directly.
- **heydemoura/EglantinePS5**, `docs/LICHII-PORTS.md` and commits `573b76a1`, `9f783dac`, `8b3fcd74`:
  the parent ports our patch numbers onto its own pin, which names what matters on the console.
  - 0061/0062 (upload skips) and 0063/0064 (fast draw, texture memo, buffer sync early-outs) are the
    live wins (+5% and +7%). `StalePages::PAGE_SIZE` is `PAGE_BYTES` because the PS5 SDK defines
    `PAGE_SIZE` — our branch carries the same fix as an overlay in `stale_pages.h`.
  - 0054 (Ocarina of Time's compute shaders) and 0009 (software HMMA): both are **in the new core**.
    `lower_to_maxwell.h` lowers `UIMAD.WIDE.U32` with an immediate, uniform IMAD with a
    constant-buffer operand and the UIADD3 carries, and HMMA is expanded in software (0x23c).
    Ocarina's title screen needs no patch on this core.
  - **0068 (Ocarina of Time gameplay) needs no port either**, measured 2026-11-10 rather than assumed:
    the parent's `ampere_ocarina_gameplay` cases were compiled against this core's
    `lower_to_maxwell.h` and all 26 gameplay forms lower (ISETP.EX with an input predicate, FRND from
    a constant buffer, TXQ masks, RED.F32, S2R 132, ATOMS.POPC.INC, SHF/USHF 64-bit, F2I of `-Rb`,
    TLD.LZ with a texel offset, TLD4 arrays and depth compares, TEX with a uniform handle, TXD with
    fewer than four channels, REDUX.MIN.S32, USEL under a `UP0` guard, a mirrored UR move, guest
    R247, LEA.HI `-Rb`, SURED non-ADD, MUFU of a negated uniform), and the per-tap gather proof
    accepts the 1080p upscaler's `TLD4...PTP` and rejects nonconstant offsets. Its second half is
    superseded: `BypassOotNetwork` skips only the prepass and network keys, so other tensor programs
    — the gameplay upscaler among them — run. Do not cherry-pick `eb1efeeb`: it only adds the patch
    file, and this branch has no patch series to apply it to.
  - **0069 (UR28-UR62 mirrors, branch `fix/oot-high-uniforms`) is in the new core** as Lichii #241:
    `LowerToMaxwellWith`, `LoweredProgram::high_uniform_registers` and the retry with an aligned
    base. Measured 2026-11-10: MUFU.RCP, F2I and VOTEU naming UR28 or UR62 lower; UR64 is rejected.
  - **0065-0067 (branch `perf/lichii-949`) are in the new core** as Lichii #949 (`a4f6287327`):
    uniform buffer keeps (`uniform_keep`, about +5% at Bananza's gate on the parent's console),
    descriptor replay, and the drain-free semaphore acquires the parent could not port. 0067's rule
    holds here: `main` applies `env.txt` after static initializers run, so a switch read at
    namespace scope never sees it. The only such switch in this core is
    `EDEN_NX2_DELTA_DESCRIPTORS_VERIFY` (diagnostic; `vk_graphics_pipeline.cpp`); set it in the
    launch environment, not `env.txt`.
  - 0070 (native storage buffer tracking, branch `fix/oot-native-storage`) is still WIP upstream;
    check it against this core when it lands.
- **premohq/PlayTendoh 0022-0023**: CMake pin relaxations for Lichii's GPU fence wait, shutdown,
  named macro callback and graphics pipeline build lock. Nothing to take: our branch already keys
  the pipeline lock on `PipelineBuild` (`pipeline_wait.cmake`), leaves `macro_native.cmake` out, and
  does not build the OpenGL path those pins guard.

## Survey of sibling forks, 2026-11-10

Forks of heydemoura/EglantinePS5 pushed since 2026-11-03 with commits of their own (10), and forks
of zortos293/Eglantine pushed since 2026-11-08 that are ahead of `nx2` (76 of 458, 2,501 commits),
filtered for Ocarina of Time, AMD and RADV work. Taken into this branch:

- **ASTC edge blocks stay inside the image** (`headless/astc_bounds.cmake`; Nolar-Hack/Lichii
  `eb518660ec`, after CheezerWheezer/Lichii `132cc46c9e`). The decoder wrote every texel of a block
  that reaches past the image's right or bottom edge; on AMD those stores land in other images
  (black blocks, a crosshatch in Ocarina, then a GPU fault). The decoder is compiled again from a
  bounded copy. The unswizzle shaders the same commit bounds are OpenGL only here.
- **A deleted buffer leaves every channel's bindings** (`prepare-vulkan-port.py`, buffer_cache.h;
  g1i7ch/lichii-ocarina `c3442ea23e`). Ocarina uses several GPU channels, and only the current one
  was cleared, so another could resolve the stale id to a buffer that reused the slot.

Already here, by our core or our port: Lichii #273, #295, #337, #351, #410, #655, #692, #721,
#820, #949, #1060-#1062 (all merged before `d2eaae4f3`); exploitizm's AMD white-flash fixes,
sparse-unbacked images and core pinning; danueecitizen's T33-T36 except `FRONT_BATCH` (an opt-in
test with no measured gain); laovnex's JIT cache shares; Sebaneg's fiber stack drop (our core no
longer allocates it, so our REPLACE at `CMakeLists.txt` for `std::array<u8, DEFAULT_STACK_SIZE>`
matches nothing).

Worth measuring on the console before any code:

- `EDEN_NX2_FLUSH_DRAWS=4096` and `EDEN_NX2_FENCE_FLUSH_US=5000` (mja00/Lichii `3985ac21c6`: about
  960 submissions a second down to 22 in Kokiri Forest).
- `robustness2=on` (mja00/Lichii `6571c04166`: out-of-bounds fog reads turned the sky white on AMD).
- The guest memory scanner's `scan_ms` in the `EDEN_CRAM` lines: laovnex stops it for 176-401 ms
  hitches every 20 s, but here `ScanPass` also runs `ColdPass`, so it cannot simply be turned off.
- Lichii #909 (open): skips the once-a-frame 64 KiB native bank uploads for Ocarina, about 11 MB a
  frame in Kokiri Forest.

Candidates for later ports: Sebaneg's GPU thread stack in the freeze report (`31961efac0`) and
service work kept off the full GPU queue (`6f8df7a2dd`, a Switch 2 intro deadlock).

## What the jump changed in the port

- `vulkan-gc`: our collector rewrite and the batched download are gone (they keyed on
  `ImageFlagBits::PartialDownload`, `partial_download_images`, `PartialDownloadBands`,
  `DownloadRowBands`, `partial_downloads_seen` from the old series). The core's collector, which
  keys pending transcodes by shape and reuses them, is used as it stands.
- `PendingTranscode` no longer stores `.bytes`: the transcode bound counts 4 bytes a pixel for every
  level and layer from the shape.
- `nx2_gsv.h`: the SDK's `nanoseconds::rep` and `Common::s64` are different integer types, so the
  port compiles its own copy with a cast. `FileEnvironment::Deserialize` takes the file end now.
  `RealVfsFilesystem::OpenFileFromEntry` takes `(path, size, perms)`.
- NUP: the core ships `NupContainer` and `AppLoader_NUP`; `metadata_bridge` builds the control.nacp
  and icon from them instead of the port's reader (core patch 0001).
- Set aside, gated on the header each needs, and kept on disk for the port that brings them back:
  the NIR backend (0015), the readback dependency diagnostics and guest memory statistics (0004),
  the read-trap check (0020), and the NUP and memory checks themselves.
