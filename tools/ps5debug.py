#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only client for ps5debug-NG (https://github.com/Pharaoh2k/ps5debug-NG), the debugger
payload on the console's TCP port 744: the process list, a process's virtual memory map as the
kernel holds it, and process memory. This gives the console's own view of a game's memory, next to
the counters the app prints (EDEN_MEMORY, EDEN_GUEST_POOLS, EDEN_SVC_MEMORY).

  python3 tools/ps5debug.py info                     # payload, firmware, foreground app
  python3 tools/ps5debug.py ps                       # processes (name, pid)
  python3 tools/ps5debug.py maps app [--json]        # the foreground app's map, summed by name
  python3 tools/ps5debug.py read app 0xADDR 4096 -o dump.bin
  python3 tools/ps5debug.py watch app --every 10 --seconds 300 --out DIR
  python3 tools/ps5debug.py mark 'race starts'      # a line in the kernel log (tools/klog.py)

`app` is the foreground application (EglantinePS5 while a game runs); a decimal pid works too.
Everything but `mark` only reads. `watch` writes DIR/maps.jsonl (one map per sample) and
DIR/maps-summary.txt (MiB by mapping name over time, with the growth from the first sample). It
can run next to any console runner without a lease of its own; start it after the game is up and
stop it (--seconds) well before the runner closes the game: reading the map of an exiting process
took the console off the network twice (2026-11-09).

Protocol: PROTOCOL.md of the ps5debug-NG repository (v1.3). Requests are a 12-byte header
(magic 0xFFAABBCC, command, body length) and a body, little-endian; status words come back
bit-swapped (success is 0x80000000 on the wire).
"""
import argparse
import collections
import json
import socket
import struct
import sys
import time
from pathlib import Path

PORT = 744
MAGIC = 0xFFAABBCC
SUCCESS = 0x80000000
STATUS_NAMES = {0xF0000001: 'error', 0xF0000003: 'data null', 0xF0000004: 'already debugging',
                0xF0000005: 'invalid index'}

CMD_VERSION = 0xBD000001
CMD_FW_VERSION = 0xBD000500
CMD_BRANDING = 0xBD000501
CMD_PROC_LIST = 0xBDAA0001
CMD_PROC_READ = 0xBDAA0002
CMD_PROC_MAPS = 0xBDAA0004
CMD_PROC_INFO = 0xBDAA000A
CMD_CONSOLE_PRINT = 0xBDDD0003
CMD_FOREGROUND_APP = 0xBDDD0006

MAP_ENTRY = struct.Struct('<32sQQQH')  # name, start, end, offset, prot: 58 bytes
PROT = ((1, 'r'), (2, 'w'), (4, 'x'))
MIB = 1 << 20
AUX = '(aux)'  # page-table-walk entries, inside other mappings


class Ps5DebugError(RuntimeError):
    pass


class Ps5Debug:
    """One connection to the command server; each call is one request and its reply."""

    def __init__(self, host, port=PORT, timeout=15):
        self.sock = socket.create_connection((host, port), timeout=timeout)

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _recv(self, size):
        data = bytearray()
        while len(data) < size:
            chunk = self.sock.recv(min(size - len(data), 1 << 20))
            if not chunk:
                raise Ps5DebugError('connection closed by ps5debug-NG')
            data += chunk
        return bytes(data)

    def _u32(self):
        return struct.unpack('<I', self._recv(4))[0]

    def _request(self, command, body=b''):
        self.sock.sendall(struct.pack('<III', MAGIC, command, len(body)) + body)

    def _status(self, what):
        status = self._u32()
        if status != SUCCESS:
            raise Ps5DebugError(f'{what}: {STATUS_NAMES.get(status, hex(status))}')

    def version(self):
        self._request(CMD_VERSION)
        return self._recv(self._u32()).decode(errors='replace')

    def branding(self):
        self._request(CMD_BRANDING)
        return self._recv(self._u32()).split(b'\0')[0].decode(errors='replace')

    def firmware(self):
        self._request(CMD_FW_VERSION)
        value = struct.unpack('<H', self._recv(2))[0]
        return f'{value // 100}.{value % 100:02d}'

    def processes(self):
        self._request(CMD_PROC_LIST)
        self._status('process list')
        count = self._u32()
        data = self._recv(36 * count)
        return [(data[i:i + 32].split(b'\0')[0].decode(errors='replace'),
                 struct.unpack_from('<i', data, i + 32)[0]) for i in range(0, len(data), 36)]

    def foreground(self):
        """pid, title ID, content ID, name, version of the foreground app (pid 0: none)."""
        self._request(CMD_FOREGROUND_APP)
        self._status('foreground app')
        data = self._recv(140)
        text = lambda a, b: data[a:b].split(b'\0')[0].decode(errors='replace')
        return {'pid': struct.unpack_from('<I', data)[0], 'title_id': text(4, 20), 'content_id': text(20, 84),
                'name': text(84, 124), 'version': text(124, 140)}

    def info(self, pid):
        self._request(CMD_PROC_INFO, struct.pack('<I', pid))
        self._status(f'process info of {pid}')
        data = self._recv(188)
        text = lambda a, b: data[a:b].split(b'\0')[0].decode(errors='replace')
        return {'pid': struct.unpack_from('<I', data)[0], 'name': text(4, 44), 'path': text(44, 108),
                'title_id': text(108, 124), 'content_id': text(124, 188)}

    def maps(self, pid):
        """The process's VM map: dicts with name, start, end, offset, prot ('rwx')."""
        self._request(CMD_PROC_MAPS, struct.pack('<I', pid))
        self._status(f'memory map of {pid}')
        count = self._u32()
        data = self._recv(MAP_ENTRY.size * count)
        entries = []
        for name, start, end, offset, prot in MAP_ENTRY.iter_unpack(data):
            entries.append({'name': name.split(b'\0')[0].decode(errors='replace'), 'start': start, 'end': end,
                            'offset': offset, 'prot': ''.join(c if prot & bit else '-' for bit, c in PROT)})
        return entries

    def mark(self, text):
        """Writes a line into the kernel log, to line up a run's phases with it."""
        data = text.encode() + b'\n'
        self._request(CMD_CONSOLE_PRINT, struct.pack('<I', len(data)))
        self.sock.sendall(data)
        self._status('kernel log line')

    def read(self, pid, address, length):
        self._request(CMD_PROC_READ, struct.pack('<IQI', pid, address, length))
        self._status(f'read {length} bytes at {address:#x} of {pid}')
        return self._recv(length)


def resolve_pid(client, target):
    if target != 'app':
        return int(target, 0)
    app = client.foreground()
    if not app['pid']:
        raise Ps5DebugError('no foreground application')
    return app['pid']


def summarize(entries):
    """Bytes per mapping name of the accessible mappings (names in the kernel map are the
    allocation names the process gave). Left out: '(aux)' entries (ps5debug-NG's page-table walk,
    inside other mappings) and no-access ('---') reservations and guards, which hold no memory
    (one of them spans the whole user address space)."""
    sizes = collections.Counter()
    for entry in entries:
        if entry['name'] != AUX and entry['prot'] != '---':
            sizes[entry['name'] or '(anonymous)'] += entry['end'] - entry['start']
    return sizes


def reserved(entries):
    """Bytes of no-access mappings below the user address space's top (the catch-all is skipped)."""
    return sum(e['end'] - e['start'] for e in entries
               if e['name'] != AUX and e['prot'] == '---' and e['end'] - e['start'] < 1 << 44)


def print_summary(entries, top):
    sizes = summarize(entries)
    total = sum(sizes.values())
    aux = sum(1 for entry in entries if entry['name'] == AUX)
    print(f'{len(entries) - aux} mappings (+{aux} page-table-walk entries inside them): '
          f'{total / MIB:.0f} MiB accessible, {reserved(entries) / MIB:.0f} MiB reserved without access')
    for name, size in sizes.most_common(top):
        print(f'{size / MIB:10.1f} MiB  {name}')


def watch(args, client):
    pid = resolve_pid(client, args.pid)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    samples = []
    started = time.monotonic()
    with (out / 'maps.jsonl').open('a') as log:
        while True:
            elapsed = time.monotonic() - started
            try:
                # Reading the map of a process that is exiting has taken the console down twice
                # (kernel log ends at the exit's FMEM table): only sample a listed process.
                if pid not in {p for _, p in client.processes()}:
                    raise Ps5DebugError(f'process {pid} has exited')
                entries = client.maps(pid)
            except (Ps5DebugError, OSError) as error:
                print(f'{elapsed:6.0f} s  stopped: {error}', file=sys.stderr)
                break
            log.write(json.dumps({'t': round(elapsed, 1), 'time': time.time(), 'pid': pid, 'maps': entries}) + '\n')
            log.flush()
            sizes = summarize(entries)
            samples.append((elapsed, sizes))
            print(f'{elapsed:6.0f} s  {len(entries)} mappings, {sum(sizes.values()) / MIB:.0f} MiB accessible, '
                  f'{reserved(entries) / MIB:.0f} MiB reserved', flush=True)
            if args.seconds and elapsed >= args.seconds:
                break
            time.sleep(max(0.0, args.every - (time.monotonic() - started - elapsed)))
    if not samples:
        return
    first, last = samples[0][1], samples[-1][1]
    names = sorted(set(first) | set(last), key=lambda n: -max(s.get(n, 0) for _, s in samples))[:args.top]
    lines = [f'pid {pid}, {len(samples)} samples every {args.every:g} s; accessible MiB per mapping name',
             '      t ' + ''.join(f'{n[:14]:>15}' for n in names) + '          total']
    for elapsed, sizes in samples:
        lines.append(f'{elapsed:7.0f} ' + ''.join(f'{sizes.get(n, 0) / MIB:15.0f}' for n in names)
                     + f'{sum(sizes.values()) / MIB:15.0f}')
    lines.append('growth  ' + ''.join(f'{(last.get(n, 0) - first.get(n, 0)) / MIB:+15.0f}' for n in names)
                 + f'{(sum(last.values()) - sum(first.values())) / MIB:+15.0f}')
    (out / 'maps-summary.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='192.168.0.190')
    parser.add_argument('--port', type=int, default=PORT)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('info')
    commands.add_parser('ps').add_argument('--json', action='store_true')
    maps = commands.add_parser('maps')
    maps.add_argument('pid', help="'app' (the foreground application) or a pid")
    maps.add_argument('--json', action='store_true', help='every mapping, as JSON')
    maps.add_argument('--top', type=int, default=25)
    read = commands.add_parser('read')
    read.add_argument('pid')
    read.add_argument('address', type=lambda v: int(v, 0))
    read.add_argument('length', type=lambda v: int(v, 0))
    read.add_argument('-o', '--output', required=True)
    commands.add_parser('mark').add_argument('text')
    watching = commands.add_parser('watch')
    watching.add_argument('pid')
    watching.add_argument('--every', type=float, default=10)
    watching.add_argument('--seconds', type=float, default=0, help='0: until the process goes away')
    watching.add_argument('--out', required=True)
    watching.add_argument('--top', type=int, default=8)
    args = parser.parse_args()
    try:
        with Ps5Debug(args.host, args.port) as client:
            if args.command == 'info':
                print(client.branding())
                print('protocol', client.version(), '- firmware', client.firmware())
                app = client.foreground()
                print('foreground:', f"{app['name']} {app['title_id']} pid {app['pid']}" if app['pid'] else 'none')
            elif args.command == 'ps':
                processes = client.processes()
                if args.json:
                    print(json.dumps([{'name': n, 'pid': p} for n, p in processes], indent=1))
                else:
                    for name, pid in processes:
                        print(f'{pid:6d}  {name}')
            elif args.command == 'maps':
                entries = client.maps(resolve_pid(client, args.pid))
                if args.json:
                    print(json.dumps(entries, indent=1))
                else:
                    print_summary(entries, args.top)
            elif args.command == 'read':
                pid = resolve_pid(client, args.pid)
                data = bytearray()
                for offset in range(0, args.length, 1 << 20):
                    data += client.read(pid, args.address + offset, min(1 << 20, args.length - offset))
                Path(args.output).write_bytes(data)
                print(f'{len(data)} bytes from {args.address:#x} -> {args.output}')
            elif args.command == 'mark':
                client.mark(args.text)
            elif args.command == 'watch':
                watch(args, client)
    except (Ps5DebugError, OSError) as error:
        sys.exit(f'ps5debug: {error}')


if __name__ == '__main__':
    main()
