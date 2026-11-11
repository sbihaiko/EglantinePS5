<p align="center">
  <img src="sce_sys/icon0.png" width="128" alt="EglantinePS5 icon">
</p>

<h1 align="center">EglantinePS5: Ocarina of Time on a base PS5</h1>

This fork of [EglantinePS5](https://github.com/heydemoura/EglantinePS5) records what it has learned
about running *The Legend of Zelda: Ocarina of Time* (Switch 2 `.nup`, title id `0400486024dec000`,
update v131072, which is 2.0.0) on a retail PS5. For install, features, settings, and the app
manual, read the [parent's README](https://github.com/heydemoura/EglantinePS5#readme).

EglantinePS5 is an unofficial PS5 port of [Eglantine](https://github.com/Djihads80/Eglantine), a
Switch 2 capable fork of [Eden](https://github.com/eden-emulator/mirror). All credit for the emulator
core belongs to the Eglantine and Eden projects and their contributors. EglantinePS5 is not
affiliated with or endorsed by the Eglantine team, the Eden team, or Sony.

Every result below comes from one console: a base PS5 (CFI-2014, not a Pro) on firmware 11.20,
Docked for the fixed-route sessions. The core pin is `3140648a` with the parent's patch series.

## What this fork is

- **`main`**: the parent's core pin and patch series, plus the fork's `09xx` patches, each behind an
  `env.txt` switch.
- **`pr99-test`**: `main` plus the parent's PR #99 and #100 (core patches 0070 and 0072-0078), and
  0913-0918. 0912 is dropped there, because 0070 routes global atomics itself. Build `0f537b0` is
  the one the latest sessions ran.
- The session-by-session record is [issue #1](https://github.com/sbihaiko/EglantinePS5/issues/1).
  What the sibling forks taught, and what was ported or left out, is in
  [`docs/OOT-SIBLING-PORTS.md`](docs/OOT-SIBLING-PORTS.md).
- The complete source is in this repository. To build it, see [docs/BUILDING.md](docs/BUILDING.md).

## Status against the goal

As of 2026-11-11, build `0f537b0`, session b2-B. FPS is in 5 s windows, frame generation off.

| criterion | target | status |
|---|---|---|
| Bloom | normal, as the player judges it | **met** (b2-A and b2-B) |
| Flashes | none, as the player judges it | not met: still present, much weaker |
| Stability | no crash in 2 sessions of 20+ min | not met: b2-B crashed at about 10 min (GPU page fault); without `skip` the GPU hangs (b2-C) |
| FPS in Link's house | 30 or more | **met**: 30-34 |
| FPS after the house | 30 or more | not met: 22-29 |
| FPS in Kokiri Forest | 38 or more | not met: 13-34 |
| Merge | the work on the fork's `main`, with docs | not yet: it lives on `pr99-test` |

## What we learned

### Bloom and exposure

- From the first gameplay session until the PR #99 base, the glow around lights and the sky was blown
  out. Nothing changed it: the colour output mask (0901), the AMD shader fixes (0902), the NaN guard
  and invariant position (0911), routed global atomics (0912), the 2560/3328 texture cache limits,
  and `skip_game_aa=false` (run8).
- On the PR #99 and #100 base the bloom is gone. The player reports a much better image and, for the
  first time, a forest visible without the haze. `Storage buffer failed to track, using global
  memory fallbacks` went from about 200 lines to 0, and the 8 compute pipelines that failed with
  `EmitGlobalAtomicIAdd32 is not implemented` now build without 0912.
- Explanation (unconfirmed): a 32-bit atomic add in a compute pass is the usual shape of a luminance
  histogram. Those passes never ran, so the tonemapper read an exposure value nobody updated. Under
  0912, 5 of the 8 shaders still had no bound NVN storage buffer, so their atomics went nowhere. PR
  #99's own description says "exposure meter values unchanged", yet the image changed here.
- The mesh skip is an unlikely cause: 0071 on `main` always skipped task and mesh draws, and the
  bloom stayed. Session b2-C was meant to isolate it, but hung before gameplay.

### One-frame flashes

- The whole image flashes bright for one frame, often. The player's words: "lightning on a rainy
  night".
- Ruled out on this core: NaN colour outputs and depth-EQUAL rejection (0911 on, shaders rebuilt,
  flashes unchanged), 0901, 0902, and 0912. Texture re-decode alone is ruled out too: the larger
  texture cache removed the long stalls and the packing passes, and the flashes stayed.
- On the PR #99 base they are weaker: mild if any in b2-A, still present but much weaker in b2-B.
- The 0913 and 0914 diagnostics (`destroy of memory … still mapped`, `pages point at old memory`)
  fired 0 times in both b2 sessions. Lichii #1199 and #1236 fixed white flashes caused by drawing
  from freed memory after area changes; that situation did not occur on this route.
- Vampinity measured 40-54 white flashes with a 500 µs fence batch and none with 0, so keep
  `EDEN_NX2_FENCE_FLUSH_US=0` when judging flashes.
- The cause of the remaining flashes is unknown.

### GPU hang versus GPU page fault

Two failures look alike to the player (the game stops) and are not the same:

| | GPU hang | async GPU page fault |
|---|---|---|
| signature | stderr: `radv/ps5: the GPU has not reached submission N (at N-1) in 10 s`, then `VK_ERROR_DEVICE_LOST` on GPUFencingThread | kernel log only: `GPU Protection fault … client:TCP(8) access:Read`, `Unmapped page access`, `GPU_FAULT_PAGE_FAULT_ASYNC`, no thread information |
| app crash file | the app logs the device loss | none; the system shows its crash screen and writes a coredump |
| seen in | early builds near the end of the opening movie; pr99-1 (133 s) and b2-C (289 s) at the first scene load | 0911 session (about 164 s), probably submit 2 (240 s), b2-B (about 10 min) |

**The early hang.** On 2026-11-09 the device was lost at the same point near the end of the opening
movie in every run, about four to seven minutes after launch. The fork fixed, in turn, native shaders
the lowering rejected (`0x98e` RED.E and `0x819` SHF.R.U64), SPIR-V global atomics that were stubs,
and 56 more rejections in 15 forms (a backport of the Ampere frontend). Every rejection was gone and
the stall still came at the same point: those were real gaps, not the trigger. From build `e6cf433`
(the parent's series plus 0071 and two port-side fixes) the movie ends and gameplay starts with no
device loss. Which change removed the hang was not isolated.

**The mesh-only draws hang the GPU on this console.** With `EDEN_OOT_MESH_DRAWS` unset on
`pr99-test`, draws with a task stage are skipped and mesh-only draws are issued. Both sessions in
that mode hung at the first scene load after the title: pr99-1 (timeout at 133 s; it logged `task
mesh draw groups=421` at 122 s) and b2-C (`the GPU has not reached submission 47858 (at 47857) in
10 s`, then `VK_ERROR_DEVICE_LOST` at 289 s, after which the app restarted itself). Both sessions
with `skip` (b2-A, b2-B) got past that point. pr99-1 left no page fault in the kernel log.
Native shader errors (`Native QMD failed`, `Native compute shader rejected`, `Native graphics shader
rejected`) appear in the sessions that survived too, so they do not tell a hang from survival. The
parent's 0075 says these draws render the tree leaves correctly in its console bisect; why they hang
here is open.

**The page fault.**

- 0911 session: address `0x0000004061609000`, all 216 waves at one shader PC. The last stderr line
  was a RADV trim (`trim gave back 86 MiB (asked 512; … 266 MiB unused for less than 2000 ms
  kept)`). The address lies outside every guest range in the log, so it looks like a host-side RADV
  allocation that was freed or trimmed (unconfirmed).
- b2-B: address `0x0000004030e06000`, same signature. The last stderr events were
  `EDEN_STAGING_EXACT 112 MiB made for a transfer of 104 MiB`, then `MKW_BUFFER_MERGE …
  size=156893184` (150 MiB at guest `7fc50000`-`891f0000`), then a 150 MiB staging upload.
- The RADV submit thread was on in the first two page faults (0911 and, probably, submit 2). b2-B
  faulted with it off, which weakens that suspicion.
- RADV holds trimmed memory for 2000 ms (`RADV_PS5_TRIM_QUARANTINE_MS`), because giving it back at
  once caused this same fault on Mario Kart World. Suspect (unconfirmed): a command list recorded
  earlier still reads memory that was trimmed or freed. b2-C ran with 10000 ms but hung before it
  could test it; session D tests it again.

### FPS and the texture cache

- **Memory.** A base PS5 gives the app 12,224 MiB of direct memory against 13,440 MiB on a Pro. The
  parent's 40.6 FPS in Link's house was most likely measured on a Pro.
- **The collector's figure.** The texture cache's device memory figure covers the whole shared pool:
  it read 4.1-6.1 GiB against 1.0-1.6 GiB of images, so the collector evicted in aggressive mode all
  through gameplay. `EDEN_NX2_TC_OWN_BYTES=1` (0904) makes it count its own images.
- **The budget.** Without own bytes, 2560/3328 thrashed (run7, up to 7,500 images made per 600
  frames). With own bytes, 2048/2560 ran out of memory at gameplay start (run10), and 1024/1408 held
  21 min at about 18 FPS (run11). On later builds, own bytes with 2560/3328 removed the long stalls
  and the packing passes, and gave the best FPS so far.
- **Long stalls follow memory packing.** Stall length tracked the guest memory packing passes
  (`EDEN_CRAM_PACK recover`): 0 passes gave no stall, 6 gave 1.4-4 s, 11-14 gave 6-10 s. RADV asked
  for 512 MiB 16-24 times a session and got back 0-206 MiB.
- **Resolution.** 0.75x made memory worse (out of memory at 111 s). A likely reason, not checked in
  the code: rescaled copies of render targets kept next to the originals.
- **CPU.** In run11 the front GPU thread was about 100% busy and waited 557-640 ms a second for the
  back, so the split behaved like one core. Submission count is not the limit: fence batching (0905)
  cut submissions, gave no FPS, and crashed (run12), so it stays off.
- **Measured gains.** The RADV submit thread gave 2-4 FPS mid-route; it stays off while the page
  fault is open. The thread layout gave 2-7 FPS after the house, in one session each (a lead, not a
  result).
- **The PR #99 cost.** The parent measured 25.2 FPS in Link's house and about 14.6 outdoors with PR
  #99, against about 40 and 24 before; PR #100 takes back part of it (15.7 to 20.2 outdoors on the
  parent's Pro). Here Kokiri fell from 38-39 (0912 build) to 13-36 (b2-A). Cause (unconfirmed): the
  passes that never ran before now run.
- **0913-0918.** b2-B against b2-A: house 30-34 against 22-29, after the house 22-29 against 4-19,
  Kokiri 13-34 and steadier against 13-36. Lichii measured 4-7 to 53-59 FPS in the Castle Town guard
  house with the three #1314 switches together. Which of the six switches gave the gain here is not
  isolated. Stutters of 1-4 s remain at the heaviest points.

### The missing door

- Link's house door has never rendered, in any build. It can still be opened.
- Suspect (unconfirmed): it is a task or mesh draw. 0071 on `main` skips every task and mesh draw,
  and so does `EDEN_OOT_MESH_DRAWS=skip`. Patch 0075 says the task program (`1a55fa57a335b963`)
  still reads its instance records through pointers the storage tracking does not bind for graphics
  stages yet; with `draw` its draws come out as bright stretched triangles.
- b2-C ran with the switch unset, which issues mesh-only draws, to test whether the door is one. It
  hung before the house, so the test did not happen.

### Tooling

- **`env.txt` lives in the app folder**, `/data/homebrew/PPSA98515/env.txt`, not
  `/data/eglantineps5/config/env.txt`. The app reads it once at start, so close the app with the PS
  button after a change. Each applied line is printed on stdout as `EDEN_ENV NAME=VALUE`; no such
  line means nothing was applied. Switches written to `config/env.txt` before 2026-11-10 never took
  effect, so those earlier sessions did not test them.
- **Stream the kernel log for the whole session**: klogsrv on TCP 3232, read with `tools/klog.py`
  (see `docs/CONSOLE-DEBUG-TOOLS.md`). klogsrv keeps no history. An async GPU fault has no thread,
  so the app's signal handler never runs; without the stream the crash looks like the app vanishing.
  The system writes a coredump under `devlog/system/sce_coredumps.0/PPSA98515_<id>/`.
- **Hang watchdog.** When presents stop, the app prints `EDEN_HANG no_present_s=…` to stderr
  (`EDEN_HANG_WATCHDOG_SECS`, default 10, 0 turns it off). The Eden log has a size cap and once
  stopped writing at 95.6 s, so read stderr for late events.
- **Install checks.** The console turns an uploaded SELF `eboot.bin` into an ELF, and ftpsrv serves
  it back decrypted. Hashes never match the ZIP; check an install with `stat` in the shell or with
  the build's strings, and install with `tools/install-ftp.py`.
- **Logs** rotate at the next launch; copy them as soon as the process exits.
- **Profiling slows what it measures** (an estimated 135 ms a second in clock reads). Measure FPS
  with `EDEN_NX2_GPU_PROFILE` and `EDEN_NX2_COUNTERS` unset.
- **CI.** GitHub can disable Actions on a fork on its own: the API still reports them enabled and a
  dispatch answers 422. Only the button on the repository's Actions tab turns them back on. A push to
  `main` builds nothing; `release.yml` runs on a `v*` tag, a pull request, or a manual dispatch.
- **Compile ahead.** On an early build, before gameplay, turning it off gave 5 of 12 windows above
  40 ms against none with it on.

## Settings that work today

These are the b2-B lines: the best image and FPS so far, though the session crashed at about 10 min.
They need build `0f537b0` from `pr99-test`; on `main`, the 0070 and 0913-0918 switches do not exist.
`RADV_PS5_SUBMIT_THREAD` stays off. Keep `EDEN_OOT_MESH_DRAWS=skip`: without it the GPU hangs at the
first scene load.

```ini
# Texture cache counts its own images (0904), with limits for a base PS5
EDEN_NX2_TC_OWN_BYTES=1
EDEN_NX2_TC_EXPECTED_MIB=2560
EDEN_NX2_TC_CRITICAL_MIB=3328
# Driver memory report (RADV_PS5_MEMORY lines)
RADV_PS5_MEMORY_REPORT=1
# Fence batching off (0905, 0908)
EDEN_NX2_FENCE_FLUSH_US=0
# AMD shader fixes (0902)
EDEN_NX2_AMD_SHADER_FIXES=1
# Thread layout (run B)
EDEN_PS5_PLACE=CPUCore_4=1/CPUCore_5=3/VulkanWorker=12
EDEN_SECONDARY_MASK=2a0
# NaN guard and invariant position (0911)
EDEN_NX2_NAN_GUARD=1
EDEN_NX2_POSITION_INVARIANT=1
# Reach of a table pointer binding, in MiB (parent 0070)
EDEN_NX2_TABLE_POINTER_MIB=256
# GPU ranges into freed memory (0913, 0914)
EDEN_NX2_DESTROY_UNMAP_ALL=1
EDEN_NX2_REFRESH_REUSED=1
# Texture cache churn (0915-0918)
EDEN_NX2_TC_PAST_CRITICAL=1
EDEN_NX2_TC_PADDED_SQUARE=1
EDEN_NX2_TC_SUPERSEDED=1
EDEN_NX2_TC_SQUARE_DEPTH_UNSCALED=1
# Skip every task and mesh draw (parent 0075)
EDEN_OOT_MESH_DRAWS=skip
```

The file holds one `NAME=VALUE` per line; a line that starts with `#` is a comment. The fork
generates each session's `env.txt` with a script rather than filtering an earlier log by hand.

## Patch series and switches

The fork's patches live in `tools/eglantine-patches/`. Each is behind a switch, off by default,
except 0901, 0903, and 0909, which are on.

| patch | switch | default | what it does | source |
|---|---|---|---|---|
| 0901 colour output mask | `EDEN_NX2_COLOR_OUTPUT_MASK=0` turns it off | on | keeps targets the fragment shader does not write (AMD writes NaN there) | Lichii #322, #353 |
| 0902 AMD shader fixes | `EDEN_NX2_AMD_SHADER_FIXES=1` | off | 8 and 16-byte storage accesses word by word; NaN-defined min, max, clamp. On `pr99-test` only the float and memory parts remain | mja00/Lichii, by way of danueecitizen |
| 0903 storage size word | `EDEN_NX2_STORAGE_SIZE_WORD=0` turns it off | on | a size word above 256 MiB is bound as a bare pointer (Kokiri passes 1.0f as a size) | viejunerd `15cde68` |
| 0904 collector own bytes | `EDEN_NX2_TC_OWN_BYTES=1` | off | the collector counts its own images, not the shared pool | ours |
| 0905 batch fence submissions | `EDEN_NX2_FENCE_FLUSH_US=500` turns it on | off | defers fence and command list submissions | Lichii `660e5f0039`, `313463cd27` |
| 0906 profile submit, GPU wait | `EDEN_NX2_GPU_PROFILE=1` | off | adds the `submit` and `GPU wait` zones | ours |
| 0907 profile drain sites | `EDEN_NX2_GPU_PROFILE=1` | off | counts the front's waits by call site | ours |
| 0908 batching off by default | none | n/a | makes 0 the 0905 default | ours |
| 0909 fence work on the back | none | on | runs the front's fence releases on the split's back; fixes the run12 crash | Lichii `e3d8e9c605` (#70), fence part |
| 0910 count-only counters | `EDEN_NX2_COUNTERS=1` | off | counts fast-draw breaks, ring entries, and deferred calls without the clock | ours |
| 0911 NaN guard, invariant position | `EDEN_NX2_NAN_GUARD=1`, `EDEN_NX2_POSITION_INVARIANT=1` | off | NaN to 0 in float colour outputs; `gl_Position` Invariant | notquiteog/Dragoon `395547c` (Shakoquo/Hookshot `f04461bc9`) |
| 0912 routed global atomics | `EDEN_NX2_GLOBAL_ATOMICS=1` | off | routes unrouted 32-bit global atomics to bound NVN buffers; `main` only | the fork's earlier 0065 |
| 0913 destroy unmap all | `EDEN_NX2_DESTROY_UNMAP_ALL=1` | off (always logs) | unmaps a destroyed memory's other GPU ranges | Lichii #1199 |
| 0914 refresh reused ranges | `EDEN_NX2_REFRESH_REUSED=1` | off (always logs) | maps again a reused device range whose pages are stale | Lichii #1236, refresh half |
| 0915 past critical | `EDEN_NX2_TC_PAST_CRITICAL=1` | off | collects more images, sooner, the further past critical | Lichii #1306 |
| 0916 padded square depth | `EDEN_NX2_TC_PADDED_SQUARE=1` | off | looks up a GOB-padded depth target (800x793) as its square | Lichii #1314, 1 of 3 |
| 0917 superseded images | `EDEN_NX2_TC_SUPERSEDED=1` | off | deletes images whose memory went wholly to newer images | Lichii #1314, 2 of 3 |
| 0918 square depth unscaled | `EDEN_NX2_TC_SQUARE_DEPTH_UNSCALED=1` | off | leaves square depth-only passes (shadow maps) unscaled | Lichii #1314, 3 of 3 |
| driver submit thread | `RADV_PS5_SUBMIT_THREAD=1` | off | submits on a `RadvSubmit` thread (`tools/patch-radv-submit-thread.py`) | PlayTendoh mesa 0008, by way of Vampinity `lead-mesh-14` |

0913-0918 are on `pr99-test` only. Switches from the parent that matter here:

- `EDEN_OOT_MESH_DRAWS` (0071 on `main`, 0075 on `pr99-test`). On `pr99-test`: unset skips draws
  with a task stage and issues mesh-only draws (tree leaves); `skip` skips every task and mesh draw;
  `draw` issues them all, which gives bright stretched triangles from task program
  `1a55fa57a335b963`. The log line `task draws are skipped` prints once per process, not once per
  scene load. On `main`, unset skips them all.
- `EDEN_NX2_TABLE_POINTER_MIB` (0070): the reach of a table pointer binding in MiB, 1 to 1023; the
  default is just under 1 GiB.
- `RADV_PS5_TRIM_QUARANTINE_MS` (`tools/patch-radv-trim.py`): how long trimmed memory waits before
  it goes back, default 2000.

## Test method

- **Fixed route**: Continue in Link's house, stand 60 s, leave, and walk one lap of Kokiri Forest.
- **FPS** comes from `EDEN_VULKAN_FRAME` in 5 s windows, with frame generation and the profiler off.
- **One build, many switches.** A build carries every ready candidate behind its own switch, off by
  default, and sessions differ only in `env.txt`. A change with no switch (PR #99) gets a build of
  its own.
- **The player judges the image**: bloom, flashes, and the door.
- **Evidence**: the kernel log streams for the whole session, and logs are copied when the app exits.
- **One session per change is a lead, not a result.**

## Session log

Getting to gameplay (2026-11-09 to 2026-11-10):

| session | build or change | result |
|---|---|---|
| first crashes | pinned core | `VK_ERROR_DEVICE_LOST` near the end of the opening movie (267-442 s); native shaders rejected |
| 0055 v1-v4 | RED.E and SHF lowering, global atomic router | rejections and stubs gone; device lost at 261-283 s anyway |
| 0066 | Ampere frontend backport | 0 rejected shaders (56 before); presents stop at 242 s |
| run3 | `e6cf433`, parent's series plus 0071 | no crash; about 30 FPS falling to 12 after about 6 min; stalls of 3-12 s |
| run4 | 0.75x | out of memory, SIGSEGV at 111 s |
| run7 | 2560/3328 | about 36 FPS, then a silent close with no crash file |
| run8 | run7 plus `skip_game_aa=false` | bloom unchanged; 17-24 FPS; stalls of 6-10 s |
| run10 | `bbb3f02`, own bytes 2048/2560 | out of memory at gameplay start (495 s) |
| run11 | `bbb3f02`, own bytes 1024/1408, profile on | 21 min, no crash, about 18 FPS |
| run12 | `19dddc5`, fence batching 500 µs | 17.9-18.7 FPS; SIGSEGV on the front GPU thread at 158 s |

Fixed route (2026-11-11). FPS in 5 s windows; an arrow shows the start and end of the 60 s in the
house.

| session | build | `env.txt` change | house | after the house | Kokiri | stalls and end |
|---|---|---|---|---|---|---|
| run A | `8cc7018` | own bytes 1024/1408, memory report, batching off, AMD fixes | 28 → 17 | 21-24 | 36-37 | stalls 1.4 s |
| submit 1 | `8cc7018` | run A plus submit thread | 26 → 19 | 26 | 34-37 | stalls 10 s, 6 s |
| submit 2 | `8cc7018` | same | 28 → 20 | 20-26 | 37-38 | system crash at 240 s |
| submit 3 | `8cc7018` | same | 27 → 18 | 25-28 | 36-37 | stalls 0.7 s |
| run B | `8cc7018` | run A plus thread layout | 29 → 21 | 23-31 | 36-40 | closed by hand |
| 0911 | `aafdaa4` | run B plus submit thread, NaN guard, invariant position | 36 → 20 | 20-31 | 36-39 | GPU page fault at about 164 s |
| 0912 | 0912 build | 0911, 0912, 2560/3328 | 49 → 27 | 19-33 | 38-39 | best FPS before PR #99 |
| pr99-1 | `faf52dc` | none (mesh-only draws issued) | n/a | n/a | n/a | GPU hang at 133 s, first scene load |
| b2-A | `0f537b0` | `EDEN_OOT_MESH_DRAWS=skip` | 22-29 | 4-19 | 13-36 | bloom gone; ended at about 236 s, no crash file |
| b2-B | `0f537b0` | b2-A plus 0913-0918 switches | 30-34 | 22-29 | 13-34 | stutters of 1-4 s; GPU page fault at about 10 min |
| b2-C | `0f537b0` | b2-B without `skip`, quarantine 10000 ms | n/a | n/a | n/a | GPU hang at 289 s, first scene load |

The 0912 row comes from `docs/OOT-SIBLING-PORTS.md`, which does not name its sessions.

## Open questions

1. Does the bloom fix come from the PR #99 base or from the mesh skip?
2. What causes the remaining one-frame flashes?
3. What causes the async GPU page fault, now that it happens with the submit thread off?
4. Why do the mesh-only draws hang the GPU here, when the parent's bisect found they draw the leaves
   correctly?
5. Is the door a mesh-only draw, or a task draw that waits on pointer binding for graphics stages?
   With mesh-only draws hanging the GPU, it cannot yet be tested.
6. Which of 0913-0918 gave the b2-B gain, and what reaches 38 FPS in Kokiri? Candidates in
   `docs/OOT-SIBLING-PORTS.md`: Lichii #1329 and #1056, viejunerd's texture switches, and Lichii
   #1334.

**Session D**, next: the b2-B lines (with `skip`) plus `RADV_PS5_TRIM_QUARANTINE_MS=10000`, to test
whether a longer trim quarantine stops the async GPU page fault. If it does not,
`GPU_FAULT_PAGE_FAULT_ASYNC` shows up in the kernel log again.

## Links

- [Issue #1](https://github.com/sbihaiko/EglantinePS5/issues/1): the session-by-session record.
- [`docs/OOT-SIBLING-PORTS.md`](docs/OOT-SIBLING-PORTS.md): sibling ports, open investigations, and
  what was left out.
- [The parent's README](https://github.com/heydemoura/EglantinePS5#readme): install, features, and
  the app manual.

## Project foundation

- Built on the [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  the native foundation ProsperoLight also uses: application structure, runtime, packaging, and
  homebrew deployment.
- Graphics are powered by [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), the native
  PS5 rendering layer the Eden graphics backend uses.
- Vulkan is powered by Mihawk's [PS5 Mesa](https://github.com/mihawk-99/PS5_Mesa) and
  [PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan). Mihawk's Mesa/RADV driver runs the Vulkan
  renderer. Many thanks to Mihawk for this work and for [all of the PS5 projects](https://github.com/mihawk-99)
  behind it.
- Thanks to [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) by mpereiraesaa, an
  experimental Vulkan graphics and compute API for native PS5 homebrew.
- The emulator core is [Eglantine](https://github.com/Djihads80/Eglantine), a Switch 2 capable fork
  of [Eden](https://github.com/eden-emulator/mirror).

## Credits

Built with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) by John Törnblom
(ps5-payload-dev). Third-party components, authors, and licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Thanks

Testers: **Jones** ([X](https://x.com/Jonesskulls), [GitHub](https://github.com/AgentJonesy)),
**Phronesis**, **szampan** ([GitHub](https://github.com/heni0xyz)), **BunkinBacon**, **Cold**
([GitHub](https://github.com/JMUtechnologies)), **Bauls Deep** ([GitHub](https://github.com/bawsdeep)),
and **OverkillLabs**.

Code contributors:

- **Mihawk** ([GitHub](https://github.com/mihawk-99)): besides the driver, per-cheat selection, the
  fix for the crash when a game with cheats stops, the driver's shader cache on the console, a fix
  for games without control data, and one source for the driver build's versions.
- **quatrixone** ([GitHub](https://github.com/quatrixone)): the Performance settings and their
  seven switches, also per game, and a much faster first compilation of a game's code.
- **Martin Pham** ([GitHub](https://github.com/MartinPham)): starting a game from its path, which
  home screen forwarders use.
- **matschi95** ([GitHub](https://github.com/matschi95)): the RomM integration and save sync, and
  the fix for the stutter that log writes caused.
- **v0ltfault** ([GitHub](https://github.com/v0ltfault)): why every game ended at its first picture
  on system software 7.40, with a fix and a write-up.
- **gyn7561** ([GitHub](https://github.com/gyn7561)): frame generation on the PS5, its settings, and
  its log lines.
- **Bauls Deep** ([GitHub](https://github.com/bawsdeep)): the Nix shell for the build environment.
- **lunar-me** ([GitHub](https://github.com/lunar-me)): a README correction.
- **EIREXE** ([GitHub](https://github.com/EIREXE)): the RNG seed per game, which lets mods such as
  HewDraw Remix work.

## Elevation credits

Filesystem elevation uses [PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon), created
by ArkSama. EglantinePS5 follows the cooperative owned-root design of
[EglantinePS5's Lapy fork](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon), based on
[mpereiraesaa's cooperative helper](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon). The build
pins that source, builds its one-shot helper without local changes, verifies the upstream manifest,
and ships the resulting ELF. The donor-release and firmware 13.60 fixes were merged upstream in
[PR #48](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/48) and
[PR #49](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/49); the pinned `57fcc19` helper
also carries the target-lifetime and accounting changes of
[PR #50](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/50). It still requires an attended
qualification run on firmware 6.02.

## License

Copyright © 2026 heydemoura. Based on ProsperoEden, Copyright © 2026 BlackBearReloaded. Licensed
under GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party components keep their own licenses.
Binary releases are built from the tagged source in this repository.

## Disclaimer

- **No affiliation.** This is an independent homebrew project, not affiliated with, endorsed by, or
  sponsored by Sony Interactive Entertainment, nor affiliated with or endorsed by the Eden project.
  "PlayStation", "PS5", and related marks are trademarks of Sony Interactive Entertainment Inc.
- **No proprietary material.** No Sony SDK, firmware, encryption keys, or decrypted system modules
  are included.
- **No warranty.** Provided "as is", without warranty of any kind, to the extent permitted by law.
  See sections 15 and 16 of the GPL.
- **Use at your own risk.** Running homebrew requires a modified console, which may void its
  warranty, breach the platform's terms of service, or cause data loss.
- **Legal use only.** Use it only with hardware, accounts, and content you own. This project does not
  support or enable piracy.

## AI assistance

The parent project was developed with AI assistance from OpenAI tools, Anthropic tools, or both.
This fork's Ocarina of Time work, its patches, and this README were written with Claude Code
(Anthropic), with every console result measured on real hardware.
