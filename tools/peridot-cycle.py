#!/usr/bin/env python3
"""Temporary diagnostic boot -> automatic recovery return -> console capture.

The image must contain sweetdiagreset=30..3600 and sweetpanicreset=1.
Start with exactly one phone in root recovery. Sets only the 32-byte BCB
boot-recovery command; never flashes a partition or replaces a boot image.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import subprocess
import sys
import time


def run(args, timeout=15, check=True):
    return subprocess.run(args, timeout=timeout, check=check, capture_output=True)


def timed_cmdline(blob, timeout):
    """Validate the packed payload before issuing any device command."""
    if len(blob) < 1652 or blob[:8] != b'ANDROID!':
        raise ValueError('Expected Android boot image')
    kernel = struct.unpack_from('<I', blob, 8)[0]
    page = struct.unpack_from('<I', blob, 36)[0]
    version = struct.unpack_from('<I', blob, 40)[0]
    if version != 2 or page not in (2048, 4096, 8192, 16384):
        raise ValueError('Unsupported boot image header')
    offset = page + ((kernel + page - 1) // page) * page
    if offset + 0x2a8 > len(blob) or blob[offset:offset + 8] != b'SWEETXNU':
        raise ValueError('Missing complete XNU payload header')
    field = blob[offset + 0x48:offset + 0x2a8]
    if b'\0' not in field:
        raise ValueError('Unterminated payload command line')
    command = field.split(b'\0', 1)[0].decode('ascii')
    args = {}
    for token in command.split():
        if '=' not in token:
            continue
        key, value = token.split('=', 1)
        if key in args:
            raise ValueError(f'Duplicate payload argument: {key}')
        args[key] = value
    seconds = int(args.get('sweetdiagreset', '0'), 0)
    if not 30 <= seconds <= 3600 or args.get('sweetpanicreset') != '1':
        raise ValueError('Automatic cycle requires timed reset and panic reset')
    if args.get('peridotmanual') == '1':
        raise ValueError('Manual-return image cannot use the automatic cycle')
    if timeout < seconds + 60:
        raise ValueError('Cycle timeout must allow reset plus 60 seconds for recovery')
    return command


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('image', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--timeout', type=int, default=420)
    a = ap.parse_args()
    image = a.image.resolve(strict=True)
    blob = image.read_bytes()
    command_line = timed_cmdline(blob, a.timeout)
    a.out.mkdir(parents=True, exist_ok=True)
    rows = [l.split() for l in run(['adb', 'devices']).stdout.decode().splitlines()[1:] if l.strip()]
    if len(rows) != 1 or rows[0][1] != 'recovery':
        sys.exit('Need exactly one phone already in root recovery')
    serial = rows[0][0]
    adb, fastboot = ['adb', '-s', serial], ['fastboot', '-s', serial]
    if run(adb + ['shell', 'id -u']).stdout.strip() != b'0':
        sys.exit('Recovery shell must be root')
    pull = Path(__file__).resolve().with_name('sweet-log-pull.py')
    before = a.out / 'console.before.txt'
    baseline = run([sys.executable, str(pull), '-s', serial, str(before)], timeout=60, check=False)
    (a.out / 'baseline-result.txt').write_bytes(baseline.stdout + baseline.stderr)
    before_sha = hashlib.sha256(before.read_bytes()).hexdigest() if baseline.returncode == 0 else None
    original = run(adb + ['exec-out', 'dd if=/dev/block/by-name/misc bs=32 count=1 2>/dev/null']).stdout
    if len(original) != 32:
        sys.exit('Could not back up the BCB command')
    (a.out / 'bcb-command.before').write_bytes(original)
    command = a.out / 'bcb-boot-recovery.bin'
    command.write_bytes(b'boot-recovery'.ljust(32, b'\0'))
    run(adb + ['push', str(command), '/tmp/peridot-cycle-bcb.bin'])
    run(adb + ['shell', 'dd if=/tmp/peridot-cycle-bcb.bin of=/dev/block/by-name/misc '
               'bs=32 count=1 conv=notrunc 2>/dev/null && sync'])
    run(adb + ['reboot', 'bootloader'])
    print('Waiting for fastboot...', flush=True)
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if serial in run(['fastboot', 'devices'], check=False).stdout.decode():
            break
        time.sleep(1)
    else:
        sys.exit('Fastboot did not appear; BCB backup is in the output directory')
    boot = run(fastboot + ['boot', str(image)], timeout=45)
    (a.out / 'fastboot-boot.txt').write_bytes(boot.stdout + boot.stderr)
    metadata = {'image': str(image), 'sha256': hashlib.sha256(blob).hexdigest(),
                'cmdline': command_line}
    (a.out / 'boot.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('Temporary boot accepted; waiting for diagnostic reset...', flush=True)
    start = time.monotonic()
    while time.monotonic() - start < a.timeout:
        state = run(adb + ['get-state'], check=False).stdout.strip()
        if state == b'recovery':
            # Recovery reconfigures its USB gadget during early init. Wait
            # until it has run for 20 s before starting a large log transfer.
            probe = run(adb + ['shell', 'cat /proc/uptime'], check=False)
            try:
                ready = probe.returncode == 0 and float(probe.stdout.split()[0]) >= 20
            except (ValueError, IndexError):
                ready = False
            if ready:
                for _ in range(3):
                    r = run([sys.executable, str(pull), '-s', serial, str(a.out / 'console.txt')], timeout=60, check=False)
                    (a.out / 'pull-result.txt').write_bytes(r.stdout + r.stderr)
                    if r.returncode == 0:
                        console = a.out / 'console.txt'
                        after_sha = hashlib.sha256(console.read_bytes()).hexdigest()
                        text = console.read_text(encoding='utf-8', errors='replace')
                        same = before_sha is not None and after_sha == before_sha
                        current_args = 'boot-args = ' + command_line in text
                        result = {'before_sha256': before_sha, 'after_sha256': after_sha,
                                  'unchanged_disk_log': same, 'current_userland_args': current_args,
                                  'current_userland_log_verified': current_args and not same}
                        (a.out / 'freshness.json').write_text(json.dumps(result, indent=2) + '\n')
                        if same:
                            sys.exit('Recovery returned, but disk console is unchanged from before boot; '
                                     'this is a stale log, not this image\'s hardware result')
                        if not current_args:
                            sys.exit('Captured changed kernel log without current userland boot arguments; '
                                     'inspect it as an incomplete boot, not a successful graphics cycle')
                        print(f'Captured console: {a.out / "console.txt"}', flush=True)
                        return 0
                    time.sleep(3)
                sys.exit('Recovery returned but log pull failed; see pull-result.txt')
        time.sleep(2)
    sys.exit('No automatic recovery return; manual fastboot/recovery reboot is needed')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as e:
        sys.exit(str(e))
