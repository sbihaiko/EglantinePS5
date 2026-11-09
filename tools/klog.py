#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Kernel log of the console, from klogsrv (https://github.com/ps5-payload-dev/klogsrv) on TCP
port 3232. ps5debug-NG serves the same stream on 3232 when klogsrv is not loaded.

  python3 tools/klog.py                                   # follow the log, timestamped
  python3 tools/klog.py --out klog.txt --seconds 300      # save it as well
  python3 tools/klog.py --grep 'PAGE_FAULT|DMEM|BAPM|PPSA98515' --out klog.txt

Every line gets the host time and the seconds since the start ("12:01:02 +35.2 ..."), so it can
be matched to a runner's log. The syslog priority prefix ("<118>") is dropped unless --raw.
klogsrv keeps no history: it only sends what the kernel logs while a client is connected, so
start this before the game. It serves any number of clients at once (the console runners
also read it, into their klog.txt). A dropped connection is retried until --seconds is up.

Lines worth knowing for EglantinePS5 (see also the console runners' klog.txt):
  GPU_FAULT_PAGE_FAULT_ASYNC ...       the GPU touched unmapped memory (a crash follows)
  [BAPM] CPU/GPU Frequency, proc gen   the app's clock mode and generation at launch
  DMEM size                            direct memory given to the app (Pro memory: 0x348000000)
"""
import argparse
import re
import socket
import sys
import time

PORT = 3232
PRIORITY = re.compile(rb'^<\d+>')


def follow(args):
    pattern = re.compile(args.grep) if args.grep else None
    output = open(args.out, 'a', buffering=1, encoding='utf-8') if args.out else None
    started = time.monotonic()
    deadline = started + args.seconds if args.seconds else None
    pending = b''
    connected = False
    try:
        while deadline is None or time.monotonic() < deadline:
            try:
                with socket.create_connection((args.host, args.port), timeout=5) as client:
                    client.settimeout(1)
                    if not connected:
                        print(f'# kernel log of {args.host}:{args.port}', file=sys.stderr)
                    connected = True
                    while deadline is None or time.monotonic() < deadline:
                        try:
                            data = client.recv(65536)
                        except socket.timeout:
                            continue
                        if not data:
                            raise OSError('closed by the console')
                        *lines, pending = (pending + data).split(b'\n')
                        for raw in lines:
                            if not args.raw:
                                raw = PRIORITY.sub(b'', raw)
                            text = raw.rstrip(b'\r').decode('utf-8', errors='replace')
                            stamped = f'{time.strftime("%H:%M:%S")} +{time.monotonic() - started:.1f} {text}'
                            if output:
                                output.write(stamped + '\n')
                            if pattern is None or pattern.search(text):
                                if output is None or pattern is not None or not args.quiet:
                                    print(stamped, flush=True)
            except OSError as error:
                print(f'# {time.strftime("%H:%M:%S")} kernel log connection: {error}; retrying', file=sys.stderr)
                time.sleep(2)
    except KeyboardInterrupt:
        pass
    finally:
        if output:
            output.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='192.168.0.190')
    parser.add_argument('--port', type=int, default=PORT)
    parser.add_argument('--out', help='append every line to this file')
    parser.add_argument('--seconds', type=float, default=0, help='stop after this long (0: until Ctrl-C)')
    parser.add_argument('--grep', help='print only lines matching this regular expression (the file gets all)')
    parser.add_argument('--quiet', action='store_true', help='with --out and no --grep: print nothing')
    parser.add_argument('--raw', action='store_true', help='keep the <priority> prefix')
    follow(parser.parse_args())


if __name__ == '__main__':
    main()
