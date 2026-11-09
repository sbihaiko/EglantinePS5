# Shared PS5 experiments

Use `python3 tools/console-lease.py --host 192.168.0.190 run --wait -- COMMAND`
for a complete deploy/input/measurement experiment. Omit `--wait` to return 75
when another session owns or is queued for the console.

The OS advisory lock lives at
`~/.cache/eglantineps5-console/192.168.0.190/console.lock`. `status` tests that
lock; `owner.json` alone is not live ownership. `note TEXT` appends findings to
`messages.jsonl` in the same directory. A second OS lock queues waiting sessions
so repeated immediate reacquisition cannot starve them. The child inherits the
console lock descriptor and retains ownership if its launcher exits.

All sessions must adopt the helper; it cannot protect against scripts that
bypass it. Do not remove either lock file. Use one lease for deployment,
scripted input, capture, evidence collection, and any restoration. Restore only
when the installed executable still matches the experiment's candidate.

For process memory maps and the kernel log of a run, see `docs/CONSOLE-DEBUG-TOOLS.md`
(`tools/ps5debug.py`, `tools/klog.py`).
