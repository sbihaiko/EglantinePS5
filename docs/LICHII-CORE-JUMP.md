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
