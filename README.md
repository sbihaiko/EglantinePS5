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

> [!TIP]
> **This work runs through [Ariel](https://github.com/sbihaiko/ariel)**, a PS5 payload shell built
> for AI agents. Ariel lets the agent behind these sessions see the console, follow a test session,
> and bring a frozen console back without anyone at the pad. Every change that matters still needs
> the owner's signature. See [How Ariel speeds up the work](#how-ariel-speeds-up-the-work).

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

## How Ariel speeds up the work

Each round has the same steps: build on Actions, install, write `env.txt`, the player runs the
route, the logs come back, and the result is written up. Only one of those steps needs the player:
the route at the pad. The goal is for an agent to do everything else, and to do it safely on a
console with kernel privileges. [Ariel](https://github.com/sbihaiko/ariel) is what makes that
possible. It is a fork of [ps5-payload-dev/shsrv](https://github.com/ps5-payload-dev/shsrv) with
an agent port on TCP 2324: one command per connection, and every answer ends in `exit <status>`,
so the agent branches on a word, not on prose.

- **Seeing needs no approval.** With only the access token, the agent can run `ps`, `procstat`,
  `tail`, `grep`, `cat`, `stat`, `dmesg`, and `sysinfo`. That covers what the loop needs:
  confirming the app is closed before `env.txt` changes, checking an install by the eboot's size
  (hashes never match; see [Tooling](#tooling)), and following `stderr.log` and `heap.log` while
  the player is on the route.
- **Changing needs the owner's key.** Writes, `kill`, `launch`, `shutdown`, and payload installs
  answer `approval_required` with a challenge that names the exact action. The owner's signer signs
  that one action, once. The agent never holds the key, so text injected into a log or a web page
  cannot turn into an action. That is what makes it safe to leave the agent watching a session
  alone.
- **Recovery without the pad.** On 2026-11-11 a GPU hang froze the console, with the controller
  stuck. The agent asked Ariel for a `shutdown`, the owner signed the challenge, and the console
  powered off about two minutes later. Nobody had to hold the power button. The plain shell has no
  power-off command, and its `suspend` did nothing. Ariel's `shutdown` goes through the console's
  own path, which unmounts the removable drives first.
- **One tool surface.** `ariel-mcp` exposes the same commands as MCP tools, so a client such as
  Claude Code discovers what it may ask instead of being taught the grammar. The server holds no
  key and never signs.

Files still move over ftpsrv (TCP 2121), and the kernel log streams from klogsrv (TCP 3232). Ariel
is the agent's way to see the console and its safe way back from a freeze.

## Status against the goal

As of 2026-11-12. Builds on this page: `0f537b0` (`pr99-test`), build 3 (`30d1655`), build 4
(`9e519f5`), build 5 (`06873bf`), build 6 (`144cced`, on `main`), and on branch `b7-memory` build 7
(`a71f5fb`), 10 (`855c8f9`), 11 (`e7e6381`), 12 (`355fa35`), 13 (`37c35d9`), 14
(`4a5dfa4`) and 15 (`eb9f3ac`). FPS is in 5 s windows, frame generation off.

| criterion | target | status |
|---|---|---|
| Bloom | normal, as the player judges it | met on the PR #99 base (b2-A, b2-B); the player has not judged it since the builds returned to `main`, which does not carry PR #99 |
| Flashes | none, as the player judges it | not met: still present, much weaker than before 0911 |
| Stability | no crash in 2 sessions of 20+ min | **half met**: b6-A ran 27 min with no page fault and no crash, under `RADV_PS5_RETIRE_SLACK=1`; the second session (b6-B) ended by hand at 6 min after the pool ran out, not by a fault |
| FPS in Link's house | 30 or more | met at the start of a session: 29-35 |
| FPS after the house | 30 or more | not met: 20-31, and 2-18 once the pool is exhausted. The best run so far is b10-D (build 10, build 6's env): 60 FPS held for 13 min, then 20-29 and 9.3 when the pool fell to 6 MiB |
| FPS in Kokiri Forest | 38 or more | not met: 13-38, following the pool |
| Merge | the work on the fork's `main`, with docs | not yet: builds 1-6 are on `main`; build 7, the 0927-0930 patches and `ReserveOver` live on `b7-memory` |

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
  could test it. Session D ran with 10000 ms and still faulted at about 9 min (address
  `0x000000403cc1b000`), so the quarantine is ruled out.
- b2-B and b2-D both faulted right after the same 150 MiB buffer (guest `7fc50000`-`891f0000`) was
  created a second time. In b2-D the driver had just built a 288 MiB allocation from 144 pieces of
  2 MiB (`no run that long in the pool`).
- b2-D2 froze with no page fault and no RADV timeout. The kernel could not suspend the app when it
  was closed (`GPU_FAULT_SUSPENDPOINT_TIMEOUT_IN_SUSPEND_ASYNC`), so the GPU had work it could not
  stop. About 10 s earlier the heap gave memory back for the first time in the session (`gave back
  32 MiB: the pool is short`).
- b2-D3 marked the memory pool short 26 times. Its two longest stalls (7.5 s and 17.6 s) came with
  the largest free block at 2 MiB and the guest memory compressor packing memory to make room.
- Reading (unconfirmed): every long session dies within seconds of a memory release or re-creation,
  under memory pressure. Build 3 tests this with `RADV_PS5_RETIRE` (keep freed memory until the GPU
  has run past it), `EDEN_PS5_HEAP_NO_GIVEBACK`, and `RADV_PS5_FREE_LOG`.

### FPS and the texture cache

- **The budget is not what exhausts the pool.** Under pressure the largest free run collapses with
  any budget, and cutting it to 1536/2304 changed nothing (b6-C). See *The pool runs out first*.
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

### The profiler costs 3.6x on the back thread

`EDEN_NX2_BACK_PROFILE=1` (0928) ran in every b10 session that looked slow, and no earlier build had
it, so "build 10 is slow" and "the back profile is slow" were the same observation (b10-A/B/C against
b10-D, 2026-11-11). With the switch on a frame took 134 ms: `split_wait` 67-82 ms while the back
thread was in `execute` with nothing else accounted. The cause was in 0928 itself: the `Execute` mark
was built inside the loop over ring entries, and an entry is one GPU method write, not a draw, so
every one paid two `steady_clock` reads and four atomic stores. `bb74083` sets the mark once per
batch. Sessions with the switch on must never be compared with sessions with it off.

### The pool runs out first

Build 6 settled what ends these sessions, and it is not a fault. With `RADV_PS5_RETIRE_SLACK=1`
there was no GPU page fault and no crash in two sessions (b6-A, 27 min, and its continuation), and
both 150 MiB `MKW_BUFFER_MERGE` precursors happened again with no fault after them. What ends them
is memory:

| quantity (b6-A) | start | mid | end |
|---|---|---|---|
| pool, largest contiguous free block | 10,060 MiB | 728 -> 140 MiB | 3-18 MiB |
| guest (`application held`) | 100 MiB | 4,923 -> 5,169 MiB | 5,313 MiB, oscillating to 4,389 |
| emulator heap holds | 1,008 MiB | 1,640 MiB | 1,793 MiB |
| RADV live buffers of 16 MiB or more | 1 block | 105 -> 91 blocks | 92 blocks / 2.7 GiB, a plateau from t=90 s |

Five things follow, and three earlier readings are now discarded:

- **Placement, not total.** Guest + driver plateau + app heap is about 9.7 GiB, the whole pool. The
  driver's live set stops growing by t=90 s and the guest gives ~800 MiB back at times; nothing
  leaks. The free *run* collapses early and stays collapsed, so every 16 MiB request lands in the
  pieces path or waits.
- **Discarded: the texture cache exhausts the pool.** `EDEN_NX2_TC_EXPECTED_MIB` and
  `TC_CRITICAL_MIB` cut to 1536/2304 (the log confirms `expected 1536 MiB, critical 2304 MiB,
  minimum 1024 MiB`) changed nothing: the same 92 buffers of 16 MiB and the same collapse in about
  3 minutes (b6-C).
- **Discarded: the app's `Commit` is where the stall time goes.** `dev_alloc_n=0` in every long
  stall, so no `vk::Buffer::Commit` ran then; a 2 MiB-piece fallback would have shown a large
  `dev_alloc_ms`.
- **Discarded: a driver leak.** The live set is a plateau, and the guest's own figure rises and
  falls.
- **Correlation has to be built at the source.** `EDEN_STALL` and the driver's free log use
  different clocks with no shared origin, so the instruments are on the code paths:
  `EDEN_NX2_BACK_PROFILE` (0928) and `mem_release_ms`/`mem_pageout_ms` (0929).
- **Not a PS5-only symptom.** Lichii #1299 (4 GB desktop GPU) and #871 (AMD device loss at the
  movie-to-3D handover) describe the same class.

What the 22 stalls of b7-A looked like: all `split_wait`, sometimes with `tc_upload` and
`staging_create` beside it, the worst 7.3 s, while the largest free block sat at 2 MiB.

#### One site allocates all the blocks (build 12, 2026-11-12)

The free log of build 12 prints the return address that asked for each buffer. In session b12-A,
all **134 allocations of 16 MiB or more came from one single address**, 3,762 MiB cumulative, and
**102 of them were exactly one 16 MiB block**. A block of that size is a VMA block: the build 10
A/B (`EDEN_PS5_VMA_BLOCK_MIB=8` and `=32`) showed the free log's sizes following that variable, and
both directions were worse than the 16 MiB default.

**The single caller says less than it looks like.** Every `vkAllocateMemory` enters the driver
through the same function, so one caller is what the instrument must show whatever the emulator
asked for: it proves the blocks are VMA blocks, not which consumer asks for them. Naming the
consumer needs a call site above VMA — an app-side instrument, not a driver one.

That is also why the 0931 result reads the way it does. Patch 0931 ports mihawk's three heap holders (shader IR pool,
upload scratch, ASTC scan buffer). Its A/B, both sessions on build 12 with the same env otherwise:
b12-A without the switch had 84 windows, median 30.4 FPS, quartiles 59.9 / 59.7 / 25.5 / 30.2,
4 stalls, 0 faults; b12-B with `EDEN_NX2_HEAP_GIVEBACK=1` had 51 windows, median 30.3, quartiles
59.9 / 21.1 / 26.9 / 31.1, 3 stalls, 0 faults. The switch does what it says — 96 MiB given back over
31 lines — and moves nothing, because the holders it trims are not the one that allocates the blocks.

The address itself could not be named: the symbol table the release publishes does not cover that
region of the driver. Build 13 adds `self=`, `req=` and `flags=` to the same line, so that
`caller - self` cancels the base the image is loaded at, and a `req=` small against the block size
proves a suballocation without needing any symbol at all.

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

Builds 3 to 7 add, all off by default: the 0920-0925 switches, `RADV_PS5_RETIRE=1` with
`RADV_PS5_RETIRE_SLACK=1`, `RADV_PS5_FREE_LOG=1` and `EDEN_NX2_STALL_REPORT=400`. Build 7 also
enables `EDEN_PS5_RESERVE_OVER=1` (a mapping becomes reserved in one call) and
`EDEN_NX2_OOT_TEXTURE_POOL_X2=1`; the latter needs 0930, which build 7 does not carry, so the switch
did nothing there. 0928 and 0929 are diagnostic and first ship in build 8.

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
| 0919 stall report | `EDEN_NX2_STALL_REPORT=400` | off | prints `EDEN_STALL` for each frame over the given ms, with where its time went, and `EDEN_AUDIO_GAP` for each audio cut over 50 ms; changes nothing else | ours, diagnostic |
| driver retire | `RADV_PS5_RETIRE=1` | off | keeps a freed buffer's memory until the GPU has finished the work submitted before the free; a failed allocation waits for the GPU and retries (`tools/patch-radv-retire.py`) | Mihawk PS5_Mesa `f5610fe270`, plus a fix for our trim |
| heap no give-back | `EDEN_PS5_HEAP_NO_GIVEBACK=1` | off | the app heap gives nothing back when the pool is short; blocks of 32 MiB or more are still returned when freed | ours |
| driver free log | `RADV_PS5_FREE_LOG=1` | off | logs frees and allocations of 16 MiB or more, pieced allocations, sparse binds, regrown scratch and descriptor buffers, and trimmed granules, each with a time and submission numbers; an allocation line also carries `caller=`, `self=`, `req=` and `flags=`, which name the site that asked for the buffer (`tools/patch-radv-free-log.py`) | ours, diagnostic |
| driver submit thread | `RADV_PS5_SUBMIT_THREAD=1` | off | submits on a `RadvSubmit` thread (`tools/patch-radv-submit-thread.py`) | PlayTendoh mesa 0008, by way of Vampinity `lead-mesh-14` |
| 0920 flash fixes | `EDEN_NX2_FLASH_FIXES=1` | off | Lichii #679's items 2 and 3 for the one-frame flashes | exploitizm/Eglantine `e0ce856`, sharkiix |
| 0921 pending poll | `EDEN_NX2_PENDING_POLL_US=50` | off | polls a pending submission after 50 us instead of 1 ms | Lichii #446 |
| 0922 skip loop writes | `EDEN_NX2_SKIP_LOOP_WRITES=1` | off | skips constant-buffer writes inside a loop (split front only) | Lichii #178 with #488 |
| 0923 DMA read reported | `EDEN_NX2_DMA_READ_REPORTED=1` | off | treats an already-reported DMA read as reported; risk: a stale occlusion result for a frame | Lichii #1032 (closed), on 0027 |
| 0924 upload batch | `EDEN_NX2_UPLOAD_BATCH=1` | off | batches texture uploads | Lichii #1245 |
| 0925 dispatch barrier inline | `EDEN_NX2_DISPATCH_BARRIER_INLINE=1` | off | inlines the dispatch barrier | Lichii #1246 |
| 0926 device alloc | `EDEN_NX2_STALL_REPORT` only | n/a | `dev_alloc_n/ms/mib` on the `EDEN_STALL` line: the driver's allocation time for a `vk::Buffer` | ours, diagnostic |
| 0927 OoT texture pool x2 | `EDEN_NX2_OOT_TEXTURE_POOL_X2=1` | off | the store that sizes the game's graphics allocator doubles x8 first; the routine goes in the executable padding after the text segment, guarded by the `main` build id | Lichii #1313, RsMan-Dev |
| 0928 back profile | `EDEN_NX2_BACK_PROFILE=1` | off | the split's back records idle/execute/submit/fence; `EDEN_STALL` ends with `back_phase=` and its age | ours, diagnostic |
| 0929 memory give-back | `EDEN_NX2_STALL_REPORT` only | n/a | `mem_release_ms` and `mem_pageout_ms`: the guest's time giving memory back | ours, diagnostic |
| 0930 texture pool reached | same switch as 0927 | off | `HasNSOPatch` answers for the built-in patch, without which the loader never calls `PatchNSO` | ours |
| 0931 heap holders give back | `EDEN_NX2_HEAP_GIVEBACK=1` | off | a shader IR pool keeps at most 4 MiB between shaders; an upload scratch over 8 MiB is given back every 256 frames and the ASTC scan buffer after one scan. Measured: 96 MiB given back, frame rate unchanged (b12-A versus b12-B) | mihawk-99/PS5_Lichii `70b85df49d` patch 0230 |
| 0932 scrub all channels | `EDEN_NX2_SCRUB_ALL_CHANNELS=1` | off | a deleted buffer scrubs its range in every channel that still binds it, each with its own dirty flags, not only the channel the flags name | Lichii #1459 (`1207061`) |
| 0933 transient targets | `EDEN_NX2_RT_EVICT_PRESSURE=<n>` | off | under pressure, a render target the GPU never wrote gives its memory back after n frames instead of waiting for the collector | mihawk-99 0227 |
| 0936 VMA block stats | `EDEN_NX2_VMA_STATS=1` | off | prints `eden vma-stats` for every block made and given back, with its type, size and memory, and the allocator's totals every 600 frames | ours, diagnostic |
| 0937 pool bridge | none | n/a | `src/video_core/host_pool.h`: the core asks the application's pool (free bytes, short, critical, recover) instead of the port script injecting its `performance.h` into the core; nothing installed reads as "not known", so a build without a pool behaves as before | ours |
| 0940 image recycling | `EDEN_NX2_IMAGE_RECYCLE_MIB=<n>` | 256 | images of 4 MiB and more are kept when destroyed and handed out again for an identical create info (bc250 `65c6b779`), so Ocarina's per-frame depth and HDR targets stop taking fresh device memory; the kept images are given back first when an allocation fails, and none is kept while the pool is short | bc250 `65c6b779` |
| driver retire slack | `RADV_PS5_RETIRE_SLACK=<n>` | 0 | holds each freed buffer n submissions past the newest one at its destroy; 1 covers a list the split's front recorded but the back had not submitted | ours, on Mihawk `f5610fe270` |

Build 14 never compiled (0932 and 0936 had errors); 0932, 0933, 0936, 0937 and 0939 land in
build 15, which ran once (session b15-A, 2026-11-12): 60 fps in the opening, then from t=432 s
the pool's largest free block fell to 3 MiB with the guest holding 4968 MiB of 9679 mapped and
the trim giving back 0 MiB, and the frame loop fell to 0.2 fps with 6-15 s gaps. That is the
build 10 collapse reached earlier, not a crash. 0938 (pool-critical collector) is not needed
here: `tools/prepare-vulkan-port.py` already makes the collector follow the pool's short and
critical state in the build tree. 0940 lands in build 16 and has no session yet.

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

## What each change bought

FPS in 5 s windows, frame generation off, on the same console (base PS5, Docked). Only the rows
marked **A/B** compare two sessions that differ in that one thing; the rest are separate sessions
on the fixed route, so they show a direction, not a number to add up.

| change | commit or patch | how it was measured | FPS effect |
|---|---|---|---|
| texture cache 2560/3328 | `env`: `TC_EXPECTED_MIB`/`TC_CRITICAL_MIB` | run3 versus run7 | about 30 falling to 12, versus about 36: the larger cache removed the long stalls |
| routed global atomics | 0912 (`EDEN_NX2_GLOBAL_ATOMICS=1`) | 0912 build versus the builds before it | the best FPS before PR #99: house 49 -> 27, after the house 19-33, Kokiri 38-39 |
| PR #99 + #100 | core patches 0070, 0072-0078 | the parent's own measurement, not ours | 15.7 -> 20.2 outdoors on the parent's PS5 Pro; on this console the PR #99 base held 22-29 in the house |
| thread layout (pin CPU cores) | `env`: `EDEN_PS5_PLACE`, `EDEN_SECONDARY_MASK` | run A versus run B, same build | +2 to +7 after the house |
| `RADV_PS5_RETIRE_SLACK=1` | `tools/patch-radv-retire.py` | b5-A (fault) versus b6-A (none) | none: it buys stability, not frames — 27 min with no page fault and no crash |
| **back profile** | 0928 (`EDEN_NX2_BACK_PROFILE=1`) | **A/B**: b10-A/b10-B against b10-D, same build | **costs about 8x**: 7.1-7.8 FPS with it on, 60 with it off. Diagnostic only, never on for play |
| **OoT texture pool x2** | 0927 (`EDEN_NX2_OOT_TEXTURE_POOL_X2=1`) | **A/B**: b10-A against b10-B (both with the back profile on) | no gain (7.1 versus 7.8); the pool doubling did not survive its own test |
| `ReserveOver` | parent PR #95 (`EDEN_PS5_RESERVE_OVER=1`) | **A/B**: b6-D (build 6) against b7-B (build 7), both on build 6's env, so the switch off on both | none measurable, and the refactor is innocent: 23.2 / 29.6 / 28.4 against 28.9 / 27.0 / 26.5 / 27.8 |
| **VMA block size** | `EDEN_PS5_VMA_BLOCK_MIB` 8 / 16 / 32 | **A/B**, three sessions | 20-25 / 60 until the collapse / 30-35. The 16 MiB default is the best of the three |
| **heap holders give back** | 0931 (`EDEN_NX2_HEAP_GIVEBACK=1`) | **A/B**: b12-A against b12-B, same build and env otherwise | none: median 30.4 versus 30.3, the same collapse point. It gives 96 MiB back and that is all |
| frame generation | `framegen.txt` `0 2` (FSR3) | not tested yet | would double the frames on screen, not the game's own rate |

Two things this table says out loud:

- **The only thing that ever produced 60 FPS was the run where the back profile was off** (b10-D,
  build 10 with build 6's env): 60 for 13 minutes, then the pool collapsed. No individual patch in
  the series has been shown to be worth a frame on its own except the routed atomics and the larger
  texture cache.
- **Every same-session A/B so far has been a negative result.** Block size, pool doubling, heap
  give-back and `ReserveOver` all came back at noise or worse. What is left is memory pressure
  itself, which the collapse numbers point at.

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
| b2-D | `0f537b0` | b2-B plus quarantine 10000 ms | 31-34 | 25-31 | 15-37 | GPU page fault at about 9 min |
| b2-D2 | `0f537b0` | same (the app restarted itself) | n/a | 29-33 | n/a | freeze with the GPU stuck at about 10 min |
| b2-D3 | `0f537b0` | same | 31-34 | n/a | 16-36 | stalls of 7.5 s and 17.6 s, pool exhausted; closed by hand |
| b3-A | `30d1655` | b2-D without the quarantine; plus `RADV_PS5_RETIRE=1`, `EDEN_PS5_HEAP_NO_GIVEBACK=1`, `EDEN_NX2_STALL_REPORT=400`, `RADV_PS5_FREE_LOG=1` | 26-34 | 21-32 | 15-38 | no page fault in 17 min; six `EDEN_HANG` of 10.4-20.6 s; after about 10 min mostly 0-2 FPS; pool at 2 MiB; closed by hand |
| b4-A | `9e519f5` | b3-A without `NO_GIVEBACK`, plus the six 0920-0925 switches | 29-31 | 16-31 | n/a | app gone after `EDEN_HANG` of 10.6 and 20.6 s at frame 19387; no page fault; pool at 2-8 MiB |
| b4-A2 | `9e519f5` | same | n/a | 0.1-27 | n/a | 21 stalls, the worst 9.1 s, all pure `split_wait`; 114 `EDEN_HANG`; unplayable after about 8 min |
| b5-A | `06873bf` | same env | 28-35 | 20-27 | n/a | **no `EDEN_HANG`**; 2 stalls, worst 1.6 s; median 41.1 FPS; GPU page fault at `0x4060629000` about 8 min in, on a 64 MiB block freed 97 ms earlier; the app restarted itself |
| b6-A | `144cced` | b5 env plus `RADV_PS5_RETIRE_SLACK=1` | 29-35 | 22-38 | n/a | 27 min, **no page fault and no crash**; pool exhausted (largest block 4 MiB); 5 stalls, the worst 8.8 s; closed by hand |
| b6-B | `144cced` | same | n/a | 18-28 | n/a | second session; pool at 4 MiB and 21 stalls by 8 min; closed by hand |
| b6-C | `144cced` | same, thresholds cut to `TC_EXPECTED_MIB=1536`, `TC_CRITICAL_MIB=2304` | n/a | 7.5-28 | n/a | the cut changed nothing: pool at 7 MiB and the same 10/16 FPS windows in about 3 min |
| b7-A | `a71f5fb` | b6 env plus `EDEN_PS5_RESERVE_OVER=1` and `EDEN_NX2_OOT_TEXTURE_POOL_X2=1` | n/a | 1.0-27.2 | n/a | 64 windows in 6 min, **no fault and no crash**; 22 stalls, the worst 7.3 s, all `split_wait`; pool at 2 MiB. The 0927 switch did nothing in this build (see below) |
| b6-D | `144cced` | b6 env unchanged (rerun for comparison) | n/a | 23.2 → 29.6 → 28.4 | n/a | pool 5066 → 1574 MiB in the second the screen opens; one 606 ms stall |
| b7-B | `a71f5fb` | b6 env, so 0927 and ReserveOver off | n/a | 28.9 → 27.0 → 26.5 → 27.8 | n/a | same as b6-D; pool 5076 → 1524 → 1208 MiB |
| build 8 | — | 0928-0930 | n/a | n/a | n/a | does not compile: 0928 names `EnterBackPhase` before its definition |
| build 9 | — | 0928-0930, first error fixed | n/a | n/a | n/a | does not compile: the generated scheduler uses `StallLedger` without its header |
| b10-A | `855c8f9` | b7 env: back-profile, pool x2 and ReserveOver on | n/a | 7.1 | n/a | pool 5 GiB → 1.4 GiB in one second at the screen |
| b10-B | `855c8f9` | pool x2 off | n/a | 7.8 | n/a | the same collapse without the switch |
| b10-C | `855c8f9` | b10-B plus `EDEN_NX2_STALL_REPORT=100` | n/a | 7.8 | n/a | 66 frames of 134 ms: `split_wait` 67-82 ms, `gpuw_wait` 27-29, `fence_wait` 23-30, `tc_upload` 0, `dev_alloc_n` 0 |
| b10-D | `855c8f9` | b6 env, so back-profile off | 60 | 60 until t≈745 s, then 20-29 and 9.3 | n/a | 14 min, **no fault and no crash**; 117 of 157 windows at 55-60 FPS; pool 5978 MiB flat for 13 min, then 6 MiB; 9 stalls, the worst 7.6 s |
| b12-A | `355fa35` | b6 env (`EDEN_NX2_HEAP_GIVEBACK` absent) | n/a | 59.9 / 59.7 / 25.5 / 30.2 by quarter | n/a | 84 windows, median 30.4, 4 stalls, 0 faults; the free log names one caller for all 134 allocations of 16 MiB and over |
| b12-B | `355fa35` | b6 env plus `EDEN_NX2_HEAP_GIVEBACK=1` | n/a | 59.9 / 21.1 / 26.9 / 31.1 by quarter | n/a | 51 windows, median 30.3, 3 stalls, 0 faults; 96 MiB given back, the collapse unchanged |

The 0912 row comes from `docs/OOT-SIBLING-PORTS.md`, which does not name its sessions.

## Open questions

1. Does the bloom fix come from the PR #99 base or from the mesh skip?
2. What causes the remaining one-frame flashes?
3. **Open.** Which driver path creates the 16 MiB blocks? Build 12 showed one caller for all of
   them, build 13 carries the fields that name it. Until it is named, the levers on this front are
   the shape of the pressure (block size, when the guest gives memory back), not the holder.
4. **Answered, provisionally.** The page fault was a free while a submission still read the
   buffer. b3-A (retire, no fault in 17 min) and b6-A/b6-B (`RADV_PS5_RETIRE_SLACK=1`, no fault in
   27 min and 8 min, with both 150 MiB merge precursors) point at the same mechanism: a buffer the
   split's front recorded was freed before the back submitted the list that names it.
5. Why do the mesh-only draws hang the GPU here, when the parent's bisect found they draw the leaves
   correctly?
6. Is the door a mesh-only draw, or a task draw that waits on pointer binding for graphics stages?
   With mesh-only draws hanging the GPU, it cannot yet be tested.
7. Which of 0913-0918 gave the b2-B gain, and what reaches 38 FPS in Kokiri? Candidates in
   `docs/OOT-SIBLING-PORTS.md`: Lichii #1329 and #1056, viejunerd's texture switches, and Lichii
   #1334.
8. **Answered.** The back was in `execute` (not in a fence wait and not allocating) during every
   slow frame measured (b10-C, `back_phase=execute`, `back_phase_ms` under 1 ms, `dev_alloc_n=0`),
   and the guest gave nothing back through `DecommitMemoryRange` or `PageOutRange`
   (`mem_release_ms=0`, `mem_pageout_ms=0` in every line). The front waits on a back thread that is
   slow, not blocked.
9. **Half answered.** What allocates the buffers of exactly 16 MiB that plateau in VRAM (92 of
   them, 1.47 GiB)? They are not texture-cache images (cutting the cache changed nothing), and the
   app's `Commit` is not on the path. Build 12's free log proves the blocks are VMA blocks (one
   caller for all 134 allocations of 16 MiB and over) but not the consumer: every Vulkan allocation
   enters the driver through that same function. The next step is above VMA — the device memory
   callbacks `vulkan_device.cpp` leaves null (`.pDeviceMemoryCallbacks`), which name each block's
   type and size, plus VMA's own per-pool statistics, which say whether the blocks are full or
   mostly empty. That decides between one consumer and fragmentation.
10. Does the game's own texture pool (`EDEN_NX2_OOT_TEXTURE_POOL_X2`, Lichii #1313) change how much
   the guest holds? Build 10 confirmed that 0930 makes the loader reach the patch
   (`ApplyOotTexturePool: ... the graphics allocator's initial pool is doubled` in `eden_log.txt`).
   With the switch on (b10-A) and off (b10-B) the collapse was the same, so the doubling is not the
   lever; b10-D's 13 healthy minutes had it off.

**What b3-A settled.** Guest memory compression is not a cause of the stalls: `cram_unpack_ms` was
0.0 in all 51 `EDEN_STALL` lines. The stalls are the front GPU thread waiting for the back
(`split_wait_ms` 283 s of the 286 s of frame time summed over those lines); the measured back-side
costs are `tc_upload` 29 s, `staging_create` 21 s, `pipe` 20 s and `fence_wait` 7 s. Several late
stalls are pure `split_wait` with no back-side category to explain them, so the back is blocked
somewhere the report does not see, most likely in memory allocation.

What the pool did, from `EDEN_MEMORY` and the free log:

| | largest free block | guest memory |
|---|---|---|
| start | 10028 MiB | 3744 MiB |
| house | 4852 MiB | |
| Kokiri | 1600 → 1190 MiB | |
| about 9 min | 526 → 255 MiB, `short=1` starts | |
| after 10 min | 1-32 MiB, mostly 2 MiB | 4769 MiB |

The driver made 1213 allocations from 2 MiB pieces, and the RADV trim gave back 3008 MiB while
2446 MiB of it was taken again. Retire and no-giveback both hold memory longer, so together they may
bring the pressure sooner; b2-D3 reached the same 2 MiB state without them, so the pressure is not
new.

**Build 4 session**, next: b3-A without `EDEN_PS5_HEAP_NO_GIVEBACK`, plus `EDEN_NX2_FLASH_FIXES=1`,
`EDEN_NX2_PENDING_POLL_US=50`, `EDEN_NX2_SKIP_LOOP_WRITES=1`, `EDEN_NX2_DMA_READ_REPORTED=1`,
`EDEN_NX2_UPLOAD_BATCH=1` and `EDEN_NX2_DISPATCH_BARRIER_INLINE=1`. Six switches at once, so a gain
is theirs and not one switch's; the next session splits them.

**What build 4 settled.** Two sessions (b4-A, b4-A2, both at the same `env.txt`) went the way of
build 3: the pool's largest free block fell to 1-8 MiB, `short=1` came up constantly, and the stalls
were again pure `split_wait` (2.1, 2.1, 3.8, 2.3, 2.3 and 9.1 s) with no back-side category. The
first session ended with the app gone at frame 19387 after `EDEN_HANG` of 10.6 and 20.6 s, with no
page fault, no `VK_ERROR_DEVICE_LOST` and no new crash index entry. The second ended the same way at
frame 21834. The four stalls the report caught were short (0.7-1.2 s) and dominated by `fence_wait`
(1111 of 1176 ms), which is not what build 3 showed. Whatever the six switches give, they do not
give memory back.

**What build 5 settled.** Build 5 (`06873bf`) added `0926`, which times the driver's memory
allocation into `dev_alloc_ms`, and mihawk's `0f86253f`, which cuts the mesh shaders' scratch ring
from 320 MiB to 128 MiB.

| | build 4 (b4-A2) | build 5 (b5-A) |
|---|---|---|
| `EDEN_HANG` | 114 | **0** |
| stalls | 21, up to 9.1 s | 2, up to 1.6 s |
| median FPS | 21.0 | **41.1** |
| largest free block at the end | 1-8 MiB | 36-59 MiB |
| ending | app gone, no fault | **GPU page fault** |

The ring is the biggest single win so far on smoothness: no hangs at all, and the pool lasted
further. It does not stop the drain, and it does not stop the page fault.

`0926` answered its question in the negative, which is worth as much: the stall at t=534 s is
`split_wait=1523 ms` with `dev_alloc_ms=0.0` and `dev_alloc_n=0`. The driver was not allocating
during it, so the split's back half waits somewhere else.

**The page fault has a smoking gun now.** The fault address `0x4060629000` sits inside a 64 MiB
block, `0x4060600000`-`0x4064600000`, that the free log shows being **allocated and freed 97 ms
before the fault**, on the memory-recovery path (`priority=14`), with `submitted=669657` equal to
`completed=669657`:

```
t=20364749 alloc 0x4060600000-0x4064600000 (64 MiB) submitted=669656 completed=669656
t=20364846 free  0x4060600000-0x4064600000 (64 MiB) priority=14 submitted=669657 completed=669657
```

The faulting client is `TCP` (a shader read), so the driver believed the GPU was done with the
block and the GPU was still reading it: a use-after-free in the driver's own accounting, not the
pieced allocation the earlier hypothesis blamed. That same address was allocated and freed three
times in the 30 s before (16 MiB, 48 MiB, 64 MiB), which is what a hot address under pressure looks
like. `RADV_PS5_RETIRE=1` does not cover this path: it holds buffers freed the ordinary way, not the
granules the recovery path hands back.

**Build 6**, next: make the recovery path wait for the GPU before it releases a granule. Record, on
release, the submission that last used the block and the device's real completion value, and hold
the block until the device passes it. That is the fix the fault now points at, and it is the one the
goal has been circling since b2-B.

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
