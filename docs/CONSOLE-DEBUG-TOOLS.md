# Console debugging tools: ps5debug-NG and klogsrv

Two payloads on the test console give a view of the app from outside it, next to the
counters the app prints itself (`EDEN_MEMORY`, `EDEN_GUEST_POOLS`, `EDEN_SVC_MEMORY`, `EDEN_FD`):

| Payload | Port | Repository | Client here |
|---|---|---|---|
| ps5debug-NG 1.3.2 | TCP 744 | https://github.com/Pharaoh2k/ps5debug-NG (protocol: `PROTOCOL.md`) | `tools/ps5debug.py` |
| klogsrv | TCP 3232 | https://github.com/ps5-payload-dev/klogsrv | `tools/klog.py` |

Both are loaded on the console already (`python3 tools/ps5debug.py ps` lists `klogsrv.elf`). If a
reboot drops them, send each ELF to the ELF loader on port 9021, as `~/scratch/nup-console/services.sh`
does for ps5vkctl. ps5debug-NG also serves the kernel log on 3232 when klogsrv is not loaded.

## ps5debug-NG: processes and memory maps

```sh
python3 tools/ps5debug.py info                 # payload, firmware, foreground app
python3 tools/ps5debug.py ps                   # processes
python3 tools/ps5debug.py maps app             # the foreground app's map, MiB by mapping name
python3 tools/ps5debug.py maps app --json      # every mapping: name, start, end, offset, prot
python3 tools/ps5debug.py watch app --every 10 --seconds 300 --out .local/maps/RUN
python3 tools/ps5debug.py read app 0xADDR 0x1000 -o dump.bin
python3 tools/ps5debug.py mark 'race starts'   # a line in the kernel log
```

`app` is the foreground application: `eboot.bin`, title PPSA98515, while EglantinePS5 runs. Only `mark`
writes anything (one kernel log line). `watch` can run next to a console runner while that runner
holds the console lease: start it once the game is up. It stops when the process exits.
It writes `maps.jsonl` (the whole map per sample) and `maps-summary.txt` (MiB per mapping name over
time and the growth from the first sample).

**Do not read the map of a process while it exits.** Twice (2026-11-09) the console went off the
network, needing a reboot, when a runner closed MKW while `watch` was sampling it every 10 s. Both
times the kernel log ended at the exit's `FMEM` table. `watch` now checks that the process is still
listed before each sample, but that only narrows the window. Give `watch` a `--seconds` that ends
well before the runner closes the game, and don't sample a warm-up run the runner will close.

Reading the map:

- `---` (no access) entries are reservations and guard pages and hold no memory. They are
  counted separately as "reserved". One catch-all entry spans the whole user address space and
  is skipped.
- `(aux)` entries come from ps5debug-NG's page-table walk and lie inside other mappings, so they are
  not counted.
- Names are the allocation names the process gave (`SceAgcDriver`, `gl0`, `executable`,
  `main stack`, ...); unnamed memory is `(anonymous)`.

EglantinePS5 at its launcher (main ae4a39a, 2026-11-09) holds 255 mappings: 662 MiB accessible
(444 MiB anonymous, 128 MiB executable) and 6088 MiB reserved.

## klogsrv: the kernel log

```sh
python3 tools/klog.py                                     # follow, timestamped
python3 tools/klog.py --out klog.txt --seconds 300 --quiet
python3 tools/klog.py --grep 'DMEM|BAPM|PAGE_FAULT|EglantinePS5' --out klog.txt
```

Each line gets the host time and the seconds since the start. klogsrv keeps no history, so start the
tool before the launch you want to see. Several clients can read at once; the console runners
already save it as `klog.txt` in their output.

Useful lines:

| Line | Meaning |
|---|---|
| `[KERNEL] INFO: DMEM size: 0x348000000` | direct memory given to the app: 0x348000000 = 13440 MiB with PS5 Pro memory (param.json attribute3 0x8480040), 0x2fc000000 without |
| `[BAPM] CPU/GPU Frequency: High CPU Frequency` | the app's clock mode |
| `### GFX(pipe0) Game. proc gen:3 ... pid:N` | the app's process generation and pid |
| `EXEC /app0/eboot.bin [user], vm#1, dmem#1 ...` | the app process starting |
| `[EglantinePS5 diag] ...` | the app's own early diagnostics |
| `GPU_FAULT_PAGE_FAULT_ASYNC` | the GPU touched unmapped memory; a crash follows |
| `[SceShellCore] FMEM a/ b c/ d NAME` | ShellCore's per-process memory table in MiB (two used/peak pairs; which pool each pair is has not been checked) |
