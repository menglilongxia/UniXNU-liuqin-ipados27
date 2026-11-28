#!/usr/bin/env python3
"""Pull and decode Xiaomi Pad 6 Pro (liuqin) UniXNU SHLOG001 v5 persistent UFS boot logs.

Matches UniXNU-sheng/package/BootLog.c and scripts/read-sheng-log.py binary layout
(last 8192 x 4096-byte blocks = 32 MiB of userdata, read-only).

Modes:
  1. Decode an already-dumped 32 MiB log window:
       python scripts/read-liuqin-log.py --decode log.bin --out E:\\unixnu-liuqin-userdata\\logs
  2. Pull read-only over ADB from TWRP/OrangeFox/rooted Android recovery:
       python scripts/read-liuqin-log.py --adb --out E:\\unixnu-liuqin-userdata\\logs
  3. Pull over USB CDC-ACM serial from a Linux log reader boot image (requires pyserial):
       python scripts/read-liuqin-log.py --port COM7 --out E:\\unixnu-liuqin-userdata\\logs
"""
import argparse
import base64
import gzip
import hashlib
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

BLOCK = 4096
LOG_BLOCKS = 8192
HEADER = struct.Struct('<8sIIQQ8I6I32s')
FIELDS = (
    'magic', 'version', 'header_bytes', 'boot_id', 'first_lba', 'window_blocks',
    'msg_first', 'msg_blocks', 'msg_bytes', 'slot_first', 'slot_blocks', 'slot_count',
    'slots_written', 'uptime_s', 'rounds', 'write_errors', 'gpt_open', 'gpt_last',
    'state', 'tag',
)
EXPORT = struct.Struct('<4I')
EXPORT3 = struct.Struct('<I')
DIAG4 = struct.Struct('<II')
MSG5 = struct.Struct('<4I')


def parse_header(blob: bytes) -> dict:
    if len(blob) < BLOCK:
        raise SystemExit(f'log window too short ({len(blob)} bytes)')
    h = dict(zip(FIELDS, HEADER.unpack_from(blob)))
    if h['magic'] != b'SHLOG001':
        raise SystemExit(f'unexpected magic {h["magic"]!r} (expected b"SHLOG001"; did the tablet boot with log=1?)')
    off = HEADER.size
    if h['version'] >= 2:
        (h['export_first'], h['export_blocks'], h['export_bytes'], h['export_generation']) = EXPORT.unpack_from(blob, off)
        off += EXPORT.size
    if h['version'] >= 3:
        (h['export_below'],) = EXPORT3.unpack_from(blob, off)
        off += EXPORT3.size
    if h['version'] >= 4:
        (h['diag_first'], h['diag_bytes']) = DIAG4.unpack_from(blob, off)
        off += DIAG4.size
    if h['version'] >= 5:
        (h['msg_mode'], h['msg_resyncs'], h['msg_head_lo'], h['msg_head_hi']) = MSG5.unpack_from(blob, off)
    else:
        h['msg_mode'] = 0
        h['msg_resyncs'] = 0
        h['msg_head_lo'] = h['msg_bytes']
        h['msg_head_hi'] = 0
    h['tag_str'] = h['tag'].split(b'\0', 1)[0].decode('utf-8', 'replace') or 'liuqin'
    return h


def decode_msgbuf(blob: bytes, h: dict) -> bytes:
    start = h['msg_first'] * BLOCK
    cap = h['msg_blocks'] * BLOCK
    area = blob[start:start + cap]
    if h.get('msg_mode', 0) == 1:
        head = (h['msg_head_hi'] << 32) | h['msg_head_lo']
        if head <= cap:
            return area[:head]
        pos = head % cap
        return area[pos:] + area[:pos]
    return area[:min(h['msg_bytes'], cap)]


def decode_slots(blob: bytes, h: dict) -> list[str]:
    count = h['slot_count']
    written = h['slots_written']
    if not count or not written:
        return []
    keep = min(written, count)
    first_idx = written - keep
    slots = []
    slot_bytes = h['slot_blocks'] * BLOCK
    for idx in range(first_idx, written):
        off = (h['slot_first'] + (idx % count) * h['slot_blocks']) * BLOCK
        raw = blob[off:off + slot_bytes].split(b'\0', 1)[0]
        slots.append(raw.decode('utf-8', 'replace'))
    return slots


def save_decoded(blob: bytes, out_root: Path) -> Path:
    h = parse_header(blob)
    run_dir = out_root / f"{h['tag_str']}-{h['boot_id']:016x}"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / 'log.bin').write_bytes(blob)

    header_lines = [f'{k}={h[k]!r}' for k in FIELDS if k != 'tag']
    header_lines.append(f"tag={h['tag_str']!r}")
    for extra in ('msg_mode', 'msg_resyncs', 'msg_head_lo', 'msg_head_hi'):
        header_lines.append(f'{extra}={h[extra]!r}')
    (run_dir / 'header.txt').write_text('\n'.join(header_lines) + '\n', encoding='utf-8')

    msg = decode_msgbuf(blob, h).replace(b'\0', b'')
    (run_dir / 'msgbuf.txt').write_bytes(msg)

    slots = decode_slots(blob, h)
    (run_dir / 'slots.txt').write_text('\n---\n'.join(slots) + ('\n' if slots else ''), encoding='utf-8')
    if slots:
        (run_dir / 'latest.txt').write_text(slots[-1] + '\n', encoding='utf-8')

    print(f'DECODED -> {run_dir}')
    print(f"  tag={h['tag_str']} uptime={h['uptime_s']}s rounds={h['rounds']} slots={h['slots_written']} "
          f"state={h['state']} errors={h['write_errors']} msg_bytes={len(msg)}")
    if slots:
        print('\n=== LATEST SLOT ===')
        print(slots[-1])
    return run_dir


def pull_via_adb(adb_bin: str, serial: str | None) -> bytes:
    base = [adb_bin] + (['-s', serial] if serial else [])
    dev = subprocess.check_output(base + ['shell', 'readlink -f /dev/block/by-name/userdata'], text=True).strip()
    if not dev.startswith('/dev/block/'):
        raise SystemExit(f'Could not resolve userdata block device via adb: {dev!r}')
    sectors = int(subprocess.check_output(base + ['shell', f'blockdev --getsz {dev}'], text=True).strip())
    blocks_4k = sectors // 8
    skip = blocks_4k - LOG_BLOCKS
    print(f'Pulling {LOG_BLOCKS} x 4 KiB blocks from {dev} (skip={skip}, total_4k={blocks_4k}) read-only...')
    cmd = f'dd if={dev} bs=4096 skip={skip} count={LOG_BLOCKS} 2>/dev/null | gzip -1 | base64'
    b64 = subprocess.check_output(base + ['shell', cmd], text=True)
    blob = gzip.decompress(base64.b64decode(''.join(b64.split())))
    if len(blob) != LOG_BLOCKS * BLOCK:
        raise SystemExit(f'Short read from adb: {len(blob)} bytes (expected {LOG_BLOCKS * BLOCK})')
    return blob


def pull_via_serial(port: str, reboot_fastboot: bool) -> bytes:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit('pyserial is required for --port mode: pip install pyserial') from exc
    with serial.Serial(port, 115200, timeout=2) as ser:
        ser.reset_input_buffer()
        ser.write(b'\n')
        time.sleep(0.3)
        ser.reset_input_buffer()
        marker_b, marker_e = '===LIUQINLOG_BEGIN===', '===LIUQINLOG_END==='
        ser.write(f'echo {marker_b}; gzip -c /run/log.bin | base64; echo {marker_e}\n'.encode())
        buf = bytearray()
        deadline = time.time() + 120
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                if marker_e.encode() in buf:
                    break
        text = buf.decode('ascii', 'ignore')
        i0 = text.find(marker_b)
        i1 = text.find(marker_e, i0 + len(marker_b))
        if i0 < 0 or i1 < 0:
            raise SystemExit('Timed out waiting for log payload over serial')
        b64 = ''.join(text[i0 + len(marker_b):i1].split())
        blob = gzip.decompress(base64.b64decode(b64))
        if reboot_fastboot:
            ser.write(b'/bin/rebootbl\n')
    return blob


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--decode', type=Path, help='Decode an already-saved 32 MiB log.bin file')
    src.add_argument('--adb', action='store_true', help='Pull last 32 MiB of userdata read-only via adb (TWRP/root)')
    src.add_argument('--port', help='Pull /run/log.bin over USB CDC-ACM serial port (e.g. COM7)')
    ap.add_argument('--adb-bin', default=shutil.which('adb') or 'adb', help='Path to adb executable')
    ap.add_argument('--serial', help='Optional ADB device serial')
    ap.add_argument('--no-reboot', action='store_true', help='In --port mode, skip /bin/rebootbl after pulling')
    ap.add_argument('--out', type=Path, default=Path('out/logs'), help='Output directory for decoded runs')
    args = ap.parse_args()

    if args.decode:
        blob = args.decode.read_bytes()
    elif args.adb:
        blob = pull_via_adb(args.adb_bin, args.serial)
    else:
        blob = pull_via_serial(args.port, not args.no_reboot)
    print(f'Log window SHA-256: {hashlib.sha256(blob).hexdigest()}')
    save_decoded(blob, args.out)


if __name__ == '__main__':
    main()
